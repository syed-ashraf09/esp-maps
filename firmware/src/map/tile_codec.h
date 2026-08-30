// ETIL tile decoding (see docs/PROTOCOL.md section 5.1).
//
// Decode happens once, when a tile arrives over BLE. The result is flat
// int16 arrays so the renderer, which walks this every frame, never parses
// varints on the hot path.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "mercator.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct {
    uint32_t first_point;   // index into map_tile_t.pts (in points, not bytes)
    uint16_t point_count;
    uint8_t  ring_continues; // this ring is followed by a hole of the same poly
} etil_feature_t;

typedef struct {
    uint8_t  kind;          // enum etil_kind
    uint8_t  style_class;
    uint16_t feature_count;
    uint32_t first_feature; // index into map_tile_t.features
} etil_layer_t;

typedef struct {
    tile_id_t id;
    uint16_t  extent;
    uint8_t   layer_count;
    bool      empty;        // positive "nothing here" answer, not a failure

    etil_layer_t   *layers;
    etil_feature_t *features;
    int16_t        *pts;    // interleaved x0,y0,x1,y1,...

    uint32_t n_features;
    uint32_t n_points;

    void   *blob;           // single backing allocation for the three arrays
    size_t  blob_size;
} map_tile_t;

// Decode `len` bytes of ETIL into `out`. Allocates one PSRAM block; free with
// map_tile_free. Returns false on malformed input (out is left zeroed).
bool map_tile_decode(const uint8_t *data, size_t len, tile_id_t id, map_tile_t *out);

// Build the "empty tile" marker - caches a negative result without allocating.
void map_tile_make_empty(map_tile_t *out, tile_id_t id);

void map_tile_free(map_tile_t *t);

// Total heap footprint of a decoded tile, for cache accounting.
static inline size_t map_tile_bytes(const map_tile_t *t)
{
    return t->blob_size;
}

#ifdef __cplusplus
}
#endif
