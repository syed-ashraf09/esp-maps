// PSRAM-backed LRU cache of decoded tiles.
//
// Two tasks touch this: the BLE task stores arriving tiles, the render task
// reads them. Rather than hold a lock for the length of a frame, the render
// task pins the slots it needs under a brief lock, draws, then unpins.
// Eviction skips pinned slots, so a tile can never be freed mid-draw.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "tile_codec.h"

#ifdef __cplusplus
extern "C" {
#endif


#define TILE_CACHE_SLOTS 96

typedef enum {
    TC_FREE = 0,
    TC_REQUESTED,  // asked the phone, waiting
    TC_READY,      // decoded and drawable
    TC_NOTHING,    // phone said there is nothing here; negative cache
} tc_state_t;

typedef struct {
    tile_id_t  id;
    tc_state_t state;
    map_tile_t tile;
    uint32_t   last_used;
    uint32_t   requested_ms;
    uint16_t   pins;
    uint8_t    retries;
} tile_slot_t;

typedef struct {
    uint32_t slots_used;
    uint32_t tiles_ready;
    uint32_t tiles_pending;
    uint32_t bytes_used;
    uint32_t bytes_budget;
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
} tile_cache_stats_t;

bool tile_cache_init(uint32_t bytes_budget);
void tile_cache_deinit(void);

// Look up without changing LRU order. Returns NULL if absent.
tile_slot_t *tile_cache_peek(tile_id_t id);

// Mark a tile as requested so we do not ask twice. Returns false if the cache
// is full of pinned/pending entries.
bool tile_cache_mark_requested(tile_id_t id, uint32_t now_ms);

// Hand over a decoded tile. Takes ownership of `t` (including its blob).
bool tile_cache_store(tile_id_t id, map_tile_t *t);

// Record a positive "nothing to draw here" answer.
bool tile_cache_store_empty(tile_id_t id);

// Drop a tile so it will be requested again.
void tile_cache_invalidate(tile_id_t id);

// Pin the READY tiles for `ids`, writing their slots into `out`. Returns the
// number pinned. Every successful pin must be released with tile_cache_unpin.
uint32_t tile_cache_pin_set(const tile_id_t *ids, uint32_t n_ids,
                            tile_slot_t **out, uint32_t max_out);
void tile_cache_unpin(tile_slot_t **slots, uint32_t n);

// Tiles that are absent or stale-pending, i.e. worth (re-)requesting.
uint32_t tile_cache_missing(const tile_id_t *ids, uint32_t n_ids,
                            uint32_t now_ms, uint32_t timeout_ms,
                            tile_id_t *out, uint32_t max_out);

void tile_cache_get_stats(tile_cache_stats_t *out);

#ifdef __cplusplus
}
#endif
