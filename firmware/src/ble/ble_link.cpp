// GATT server on NimBLE-Arduino.
//
// Written against the NimBLE-Arduino 2.x API (callbacks take NimBLEConnInfo&).
// If you pin 1.x instead, the three callback signatures below are what change.
//
// The credit scheme is the important part: the phone writes with
// WRITE_NO_RESPONSE, which has no backpressure of its own, so we grant it a
// bounded number of writes and top up as we consume them. Without this the
// reassembly buffer overruns on a busy link and roughly a third of packets
// vanish.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <string.h>

#include "ble_link.h"
#include "proto.h"
#include "../map/tile_cache.h"
#include "../map/tile_codec.h"
#include "../map/nav_state.h"

#define LOGI(fmt, ...) Serial.printf("[ble] " fmt "\n", ##__VA_ARGS__)

// Headroom over the ~10-20 KB a filtered urban z14 tile should encode to.
// Measured tiles with buildings still included reached 97 KB, so this is
// deliberately generous - a rejected tile is a blank screen, and PSRAM is
// the one resource this board has plenty of.
#define MAX_TILE_BYTES   (128 * 1024)
#define CREDIT_WINDOW    32
#define CREDIT_REFILL_AT 12

static NimBLEServer         *g_server;
static NimBLECharacteristic *g_rx;
static NimBLECharacteristic *g_tx;

static volatile bool g_connected;
static uint16_t g_conn_handle;
static uint16_t g_mtu = 23;
static uint16_t g_tx_seq;
static uint8_t  g_consumed;

static ble_link_stats_t g_stats;

static struct {
    bool      active;
    tile_id_t id;
    uint8_t  *buf;
    uint32_t  cap;
    uint32_t  len;
    uint32_t  total;
    uint16_t  chunks;
    uint16_t  got;
} g_rx_tile;

// ---- notify helpers ------------------------------------------------------

static bool notify_raw(uint8_t type, const void *payload, uint16_t len)
{
    if (!g_connected || !g_tx) return false;
    if (len > g_mtu - 3 - PKT_HDR_LEN) return false;

    uint8_t buf[512];
    pkt_hdr_t h;
    h.type = type;
    h.flags = 0;
    h.seq = g_tx_seq++;
    h.len = len;
    memcpy(buf, &h, PKT_HDR_LEN);
    if (len && payload) memcpy(buf + PKT_HDR_LEN, payload, len);

    g_tx->setValue(buf, PKT_HDR_LEN + len);
    return g_tx->notify();
}

static void grant_credits(uint16_t n)
{
    if (notify_raw(MSG_CREDITS, &n, sizeof(n))) g_stats.credits_granted += n;
}

static void maybe_refill_credits(void)
{
    if (++g_consumed >= CREDIT_REFILL_AT) {
        grant_credits(g_consumed);
        g_consumed = 0;
    }
}

// ---- tile reassembly -----------------------------------------------------

static void tile_abort(void)
{
    g_rx_tile.active = false;
    g_rx_tile.len = 0;
    g_rx_tile.got = 0;
}

static void handle_tile_start(const uint8_t *p, uint16_t len)
{
    if (len < sizeof(msg_tile_start_t)) return;

    msg_tile_start_t m;
    memcpy(&m, p, sizeof(m));

    if (m.total_len == 0 || m.total_len > MAX_TILE_BYTES) {
        LOGI("tile %u/%lu/%lu size %lu rejected", m.z,
             (unsigned long)m.x, (unsigned long)m.y, (unsigned long)m.total_len);
        tile_abort();
        return;
    }

    if (!g_rx_tile.buf) {
        g_rx_tile.buf = (uint8_t *)heap_caps_malloc(MAX_TILE_BYTES,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        g_rx_tile.cap = g_rx_tile.buf ? MAX_TILE_BYTES : 0;
        if (!g_rx_tile.buf) {
            LOGI("no PSRAM for tile reassembly");
            return;
        }
    }

    g_rx_tile.active = true;
    g_rx_tile.id.z = m.z;
    g_rx_tile.id.x = m.x;
    g_rx_tile.id.y = m.y;
    g_rx_tile.total = m.total_len;
    g_rx_tile.chunks = m.chunks;
    g_rx_tile.len = 0;
    g_rx_tile.got = 0;
}

static void handle_tile_chunk(const uint8_t *p, uint16_t len)
{
    if (!g_rx_tile.active || len < 2) return;

    uint16_t index;
    memcpy(&index, p, 2);
    const uint8_t *data = p + 2;
    uint16_t n = len - 2;

    // Chunks are sequential. A gap means one was lost and the CRC would fail
    // anyway, so bail now rather than assembling garbage.
    if (index != g_rx_tile.got) {
        LOGI("chunk out of order: got %u expected %u", index, g_rx_tile.got);
        tile_abort();
        return;
    }
    if (g_rx_tile.len + n > g_rx_tile.cap) { tile_abort(); return; }

    memcpy(g_rx_tile.buf + g_rx_tile.len, data, n);
    g_rx_tile.len += n;
    g_rx_tile.got++;
}

static void handle_tile_end(const uint8_t *p, uint16_t len)
{
    if (!g_rx_tile.active || len < 4) { tile_abort(); return; }

    uint32_t want;
    memcpy(&want, p, 4);

    uint32_t have = esp_rom_crc32_le(0, g_rx_tile.buf, g_rx_tile.len);
    if (have != want || g_rx_tile.len != g_rx_tile.total) {
        LOGI("tile %u/%lu/%lu bad: crc %08lx vs %08lx, %lu of %lu B",
             g_rx_tile.id.z, (unsigned long)g_rx_tile.id.x,
             (unsigned long)g_rx_tile.id.y,
             (unsigned long)have, (unsigned long)want,
             (unsigned long)g_rx_tile.len, (unsigned long)g_rx_tile.total);
        g_stats.tiles_crc_fail++;
        tile_cache_invalidate(g_rx_tile.id);
        tile_abort();
        return;
    }

    map_tile_t t;
    if (map_tile_decode(g_rx_tile.buf, g_rx_tile.len, g_rx_tile.id, &t)) {
        uint32_t feats = t.n_features, pts = t.n_points;
        bool stored = tile_cache_store(g_rx_tile.id, &t);
        if (!stored) map_tile_free(&t);
        g_stats.tiles_ok++;
        LOGI("tile %u/%lu/%lu ok: %lu B, %lu feats, %lu pts%s",
             g_rx_tile.id.z, (unsigned long)g_rx_tile.id.x,
             (unsigned long)g_rx_tile.id.y, (unsigned long)g_rx_tile.len,
             (unsigned long)feats, (unsigned long)pts,
             stored ? "" : " (CACHE FULL, dropped)");
    } else {
        LOGI("tile %u/%lu/%lu decode FAILED (%lu B)", g_rx_tile.id.z,
             (unsigned long)g_rx_tile.id.x, (unsigned long)g_rx_tile.id.y,
             (unsigned long)g_rx_tile.len);
        tile_cache_invalidate(g_rx_tile.id);
    }
    tile_abort();
}

// ---- dispatch ------------------------------------------------------------

static void handle_packet(uint8_t type, const uint8_t *p, uint16_t len)
{
    switch (type) {
    case MSG_HELLO:
        LOGI("phone hello, proto v%u", len ? p[0] : 0);
        break;

    case MSG_GPS_FIX:
        if (len >= sizeof(msg_gps_fix_t)) {
            msg_gps_fix_t m;
            memcpy(&m, p, sizeof(m));
            nav_set_fix(&m, millis());
        }
        break;

    case MSG_TILE_START: handle_tile_start(p, len); break;
    case MSG_TILE_CHUNK: handle_tile_chunk(p, len); break;
    case MSG_TILE_END:   handle_tile_end(p, len);   break;

    case MSG_TILE_EMPTY:
        if (len >= 9) {
            tile_id_t id;
            id.z = p[0];
            memcpy(&id.x, p + 1, 4);
            memcpy(&id.y, p + 5, 4);
            tile_cache_store_empty(id);
        }
        break;

    case MSG_ROUTE_START:
        if (len >= 12) {
            uint32_t n_pts, dist, dur;
            memcpy(&n_pts, p, 4);
            memcpy(&dist, p + 4, 4);
            memcpy(&dur, p + 8, 4);
            nav_route_begin(n_pts, dist, dur);
        }
        break;

    case MSG_ROUTE_CHUNK:
        if (len > 2) nav_route_append(p + 2, len - 2);
        break;

    case MSG_ROUTE_END:   nav_route_commit(); break;
    case MSG_ROUTE_CLEAR: nav_route_clear();  break;

    case MSG_MANEUVER:
        if (len >= sizeof(msg_maneuver_hdr_t)) {
            msg_maneuver_hdr_t h;
            memcpy(&h, p, sizeof(h));
            nav_maneuver_t m;
            memset(&m, 0, sizeof(m));
            m.kind = h.kind;
            m.exit_no = h.exit_no;
            m.dist_m = h.dist_m;
            m.pt_index = h.pt_index;
            uint16_t tl = h.text_len;
            if (tl > NAV_MANEUVER_TEXT - 1) tl = NAV_MANEUVER_TEXT - 1;
            if (sizeof(h) + tl <= len) memcpy(m.text, p + sizeof(h), tl);
            m.text[tl] = '\0';
            nav_set_maneuver(&m);
        }
        break;

    case MSG_NAV_STATE:
        if (len >= 13) {
            uint32_t eta, rm, rs;
            memcpy(&eta, p + 1, 4);
            memcpy(&rm, p + 5, 4);
            memcpy(&rs, p + 9, 4);
            nav_set_progress(eta, rm, rs);
        }
        break;

    default:
        break;
    }
}

// ---- NimBLE callbacks ----------------------------------------------------

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
        (void)info;
        NimBLEAttValue v = c->getValue();
        const uint8_t *buf = v.data();
        size_t len = v.length();

        if (len >= PKT_HDR_LEN) {
            pkt_hdr_t h;
            memcpy(&h, buf, PKT_HDR_LEN);
            if ((size_t)(PKT_HDR_LEN + h.len) <= len) {
                handle_packet(h.type, buf + PKT_HDR_LEN, h.len);
                g_stats.rx_bytes += len;
                g_stats.rx_packets++;
            }
        }
        maybe_refill_credits();
    }
};

/**
 * Credits MUST be granted here and not only on connect.
 *
 * The phone subscribes to TX *after* connecting and after MTU exchange, so a
 * grant sent from onConnect or onMTUChange has nowhere to go - notifications
 * are not enabled yet and the packet is silently dropped. Without this
 * callback the phone never receives a single credit, never writes anything,
 * and the link looks perfectly healthy from both ends while moving zero bytes.
 */
class TxCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic *c, NimBLEConnInfo &info,
                     uint16_t subValue) override {
        (void)c; (void)info;
        if (subValue == 0) {
            LOGI("phone unsubscribed from TX");
            return;
        }
        LOGI("phone subscribed to TX - granting %u credits", CREDIT_WINDOW);
        g_consumed = 0;
        grant_credits(CREDIT_WINDOW);
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
        g_conn_handle = info.getConnHandle();
        g_connected = true;
        g_stats.connected = true;
        nav_set_link(true);
        LOGI("connected, handle %u", g_conn_handle);

        // Ask for a tight interval. 6..12 units is 7.5..15 ms; phones will
        // clamp this to whatever they allow, but asking matters.
        s->updateConnParams(g_conn_handle, 6, 12, 0, 400);

        // 2M PHY roughly doubles usable throughput where the phone supports it.
        ble_gap_set_prefered_le_phy(g_conn_handle,
                                    BLE_GAP_LE_PHY_2M_MASK,
                                    BLE_GAP_LE_PHY_2M_MASK,
                                    BLE_GAP_LE_PHY_CODED_ANY);

        g_consumed = 0;
        grant_credits(CREDIT_WINDOW);
    }

    void onDisconnect(NimBLEServer *s, NimBLEConnInfo &info, int reason) override {
        (void)info;
        LOGI("disconnected, reason 0x%x", reason);
        g_connected = false;
        g_stats.connected = false;
        g_mtu = 23;
        nav_set_link(false);
        tile_abort();
        NimBLEDevice::startAdvertising();
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo &info) override {
        (void)info;
        g_mtu = mtu;
        g_stats.mtu = mtu;
        LOGI("MTU now %u (payload %u)", mtu, mtu - 3 - PKT_HDR_LEN);
        // The phone waits for credits before streaming; re-grant here so a
        // late MTU negotiation cannot strand it.
        g_consumed = 0;
        grant_credits(CREDIT_WINDOW);
    }
};

// ---- public API ----------------------------------------------------------

extern "C" {

bool ble_link_init(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    memset(&g_rx_tile, 0, sizeof(g_rx_tile));

    NimBLEDevice::init(ESPMAPS_ADV_NAME);
    NimBLEDevice::setMTU(517);
    NimBLEDevice::setPower(9);

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new ServerCallbacks());
    g_server->advertiseOnDisconnect(true);

    NimBLEService *svc = g_server->createService(ESPMAPS_SVC_UUID);

    g_rx = svc->createCharacteristic(
        ESPMAPS_RX_UUID,
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE);
    g_rx->setCallbacks(new RxCallbacks());

    g_tx = svc->createCharacteristic(ESPMAPS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
    g_tx->setCallbacks(new TxCallbacks());

    // In NimBLE 2.x services start with the server, not individually.
    g_server->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setName(ESPMAPS_ADV_NAME);
    adv->addServiceUUID(ESPMAPS_SVC_UUID);
    // The 128-bit UUID will not fit beside the name in the 31-byte advertising
    // payload, so it spills into the scan response.
    adv->enableScanResponse(true);
    adv->start();

    LOGI("advertising as \"%s\"", ESPMAPS_ADV_NAME);
    return true;
}

bool ble_link_connected(void) { return g_connected; }
uint16_t ble_link_mtu(void)   { return g_mtu; }

void ble_link_request_tiles(const tile_id_t *ids, uint32_t n)
{
    if (n == 0 || !g_connected) return;
    if (n > 16) n = 16;

    uint8_t payload[1 + 16 * sizeof(tile_req_entry_t)];
    payload[0] = (uint8_t)n;

    for (uint32_t i = 0; i < n; i++) {
        tile_req_entry_t e;
        e.z = ids[i].z;
        e.x = ids[i].x;
        e.y = ids[i].y;
        e.prio = (uint8_t)i;
        memcpy(payload + 1 + i * sizeof(e), &e, sizeof(e));
    }
    notify_raw(MSG_TILE_REQUEST, payload, 1 + n * sizeof(tile_req_entry_t));
}

void ble_link_send_view(double lat, double lon, uint8_t zoom, float bearing_deg)
{
    struct __attribute__((packed)) {
        int32_t lat_e7, lon_e7;
        uint8_t zoom;
        uint16_t bearing_cdeg;
    } p;
    p.lat_e7 = (int32_t)(lat * 1e7);
    p.lon_e7 = (int32_t)(lon * 1e7);
    p.zoom = zoom;
    p.bearing_cdeg = (uint16_t)(bearing_deg * 100.0f);
    notify_raw(MSG_VIEW, &p, sizeof(p));
}

void ble_link_send_dest(double lat, double lon)
{
    struct __attribute__((packed)) { int32_t lat_e7, lon_e7; } p;
    p.lat_e7 = (int32_t)(lat * 1e7);
    p.lon_e7 = (int32_t)(lon * 1e7);
    notify_raw(MSG_DEST, &p, sizeof(p));
}

void ble_link_get_stats(ble_link_stats_t *out)
{
    *out = g_stats;
    out->mtu = g_mtu;
    out->connected = g_connected;
}

}  // extern "C"
