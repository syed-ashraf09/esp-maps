#include "tile_codec.h"

#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "../ble/proto.h"

static const char *TAG = "tile_codec";

// ---- varint primitives ---------------------------------------------------

// Reads a LEB128 varint. Advances *p. Returns false past `end` or on a
// pathologically long encoding.
static inline bool varint_read(const uint8_t **p, const uint8_t *end, uint32_t *out)
{
    uint32_t v = 0;
    int shift = 0;
    const uint8_t *q = *p;

    while (q < end) {
        uint8_t b = *q++;
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *p = q;
            *out = v;
            return true;
        }
        shift += 7;
        if (shift > 28) return false;  // would overflow uint32
    }
    return false;
}

static inline bool varint_skip(const uint8_t **p, const uint8_t *end)
{
    const uint8_t *q = *p;
    int n = 0;
    while (q < end) {
        uint8_t b = *q++;
        if (!(b & 0x80)) { *p = q; return true; }
        if (++n > 4) return false;
    }
    return false;
}

static inline int32_t zigzag_decode(uint32_t v)
{
    return (int32_t)((v >> 1) ^ (~(v & 1) + 1));
}

// ---- pass 1: measure -----------------------------------------------------

// Walks the payload without decoding coordinates, to size the allocation.
static bool measure(const uint8_t *p, const uint8_t *end, uint8_t layer_count,
                    uint32_t *n_features, uint32_t *n_points)
{
    uint32_t feats = 0, points = 0;

    for (uint8_t l = 0; l < layer_count; l++) {
        if ((size_t)(end - p) < sizeof(etil_layer_hdr_t)) return false;
        etil_layer_hdr_t lh;
        memcpy(&lh, p, sizeof(lh));
        p += sizeof(lh);

        for (uint16_t f = 0; f < lh.feature_count; f++) {
            uint32_t packed;
            if (!varint_read(&p, end, &packed)) return false;
            uint32_t pc = ETIL_PTCOUNT(packed);
            if (pc == 0 || pc > 65535) return false;

            // Two varints per point; skip rather than decode.
            for (uint32_t i = 0; i < pc * 2; i++) {
                if (!varint_skip(&p, end)) return false;
            }
            feats++;
            points += pc;
        }
    }

    *n_features = feats;
    *n_points = points;
    return true;
}

// ---- decode --------------------------------------------------------------

void map_tile_make_empty(map_tile_t *out, tile_id_t id)
{
    memset(out, 0, sizeof(*out));
    out->id = id;
    out->empty = true;
}

bool map_tile_decode(const uint8_t *data, size_t len, tile_id_t id, map_tile_t *out)
{
    memset(out, 0, sizeof(*out));
    out->id = id;

    if (len < sizeof(etil_hdr_t)) {
        ESP_LOGW(TAG, "tile %u/%lu/%lu too short (%u B)", id.z,
                 (unsigned long)id.x, (unsigned long)id.y, (unsigned)len);
        return false;
    }

    etil_hdr_t h;
    memcpy(&h, data, sizeof(h));

    if (h.magic[0] != ETIL_MAGIC0 || h.magic[1] != ETIL_MAGIC1) {
        ESP_LOGW(TAG, "bad magic %02x%02x", h.magic[0], h.magic[1]);
        return false;
    }
    if (h.version != ETIL_VERSION) {
        ESP_LOGW(TAG, "unsupported ETIL version %u", h.version);
        return false;
    }
    if (h.extent == 0 || h.extent > 8192) {
        ESP_LOGW(TAG, "implausible extent %u", h.extent);
        return false;
    }
    if (sizeof(etil_hdr_t) + h.payload_len > len) {
        ESP_LOGW(TAG, "payload_len %lu overruns buffer %u",
                 (unsigned long)h.payload_len, (unsigned)len);
        return false;
    }

    if (h.layer_count == 0) {
        map_tile_make_empty(out, id);
        return true;
    }

    const uint8_t *payload = data + sizeof(etil_hdr_t);
    const uint8_t *end = payload + h.payload_len;

    uint32_t n_features = 0, n_points = 0;
    if (!measure(payload, end, h.layer_count, &n_features, &n_points)) {
        ESP_LOGW(TAG, "malformed geometry in %u/%lu/%lu", id.z,
                 (unsigned long)id.x, (unsigned long)id.y);
        return false;
    }
    if (n_features == 0) {
        map_tile_make_empty(out, id);
        return true;
    }

    // One allocation backs all three arrays, laid out layers | features | pts.
    // Keeps PSRAM fragmentation down when tiles churn through the LRU.
    size_t sz_layers = (size_t)h.layer_count * sizeof(etil_layer_t);
    size_t sz_feats  = (size_t)n_features * sizeof(etil_feature_t);
    size_t sz_pts    = (size_t)n_points * 2 * sizeof(int16_t);
    size_t total = sz_layers + sz_feats + sz_pts;

    uint8_t *blob = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) {
        ESP_LOGE(TAG, "PSRAM alloc failed for %u B", (unsigned)total);
        return false;
    }

    out->blob = blob;
    out->blob_size = total;
    out->layers   = (etil_layer_t *)blob;
    out->features = (etil_feature_t *)(blob + sz_layers);
    out->pts      = (int16_t *)(blob + sz_layers + sz_feats);
    out->extent = h.extent;
    out->layer_count = h.layer_count;
    out->n_features = n_features;
    out->n_points = n_points;

    // ---- pass 2: fill ----
    const uint8_t *p = payload;
    uint32_t fi = 0, pi = 0;

    for (uint8_t l = 0; l < h.layer_count; l++) {
        etil_layer_hdr_t lh;
        memcpy(&lh, p, sizeof(lh));
        p += sizeof(lh);

        out->layers[l].kind = lh.kind;
        out->layers[l].style_class = lh.style_class;
        out->layers[l].feature_count = lh.feature_count;
        out->layers[l].first_feature = fi;

        for (uint16_t f = 0; f < lh.feature_count; f++) {
            uint32_t packed;
            varint_read(&p, end, &packed);  // measure() already validated
            uint32_t pc = ETIL_PTCOUNT(packed);

            out->features[fi].first_point = pi;
            out->features[fi].point_count = (uint16_t)pc;
            out->features[fi].ring_continues = (packed & ETIL_RING_CONTINUES) ? 1 : 0;
            fi++;

            int32_t cx = 0, cy = 0;
            for (uint32_t i = 0; i < pc; i++) {
                uint32_t vx, vy;
                varint_read(&p, end, &vx);
                varint_read(&p, end, &vy);
                cx += zigzag_decode(vx);
                cy += zigzag_decode(vy);
                out->pts[pi * 2 + 0] = (int16_t)cx;
                out->pts[pi * 2 + 1] = (int16_t)cy;
                pi++;
            }
        }
    }

    ESP_LOGD(TAG, "decoded %u/%lu/%lu: %lu feats, %lu pts, %u B",
             id.z, (unsigned long)id.x, (unsigned long)id.y,
             (unsigned long)n_features, (unsigned long)n_points, (unsigned)total);
    return true;
}

void map_tile_free(map_tile_t *t)
{
    if (t->blob) heap_caps_free(t->blob);
    memset(t, 0, sizeof(*t));
}
