#include "tile_cache.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "tile_cache";

static tile_slot_t g_slots[TILE_CACHE_SLOTS];
static SemaphoreHandle_t g_lock;
static uint32_t g_tick;
static tile_cache_stats_t g_stats;

#define LOCK()   xSemaphoreTake(g_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(g_lock)

bool tile_cache_init(uint32_t bytes_budget)
{
    g_lock = xSemaphoreCreateMutex();
    if (!g_lock) return false;

    memset(g_slots, 0, sizeof(g_slots));
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.bytes_budget = bytes_budget;
    g_tick = 0;

    ESP_LOGI(TAG, "cache: %d slots, %lu KB budget",
             TILE_CACHE_SLOTS, (unsigned long)(bytes_budget / 1024));
    return true;
}

void tile_cache_deinit(void)
{
    if (!g_lock) return;
    LOCK();
    for (int i = 0; i < TILE_CACHE_SLOTS; i++) {
        if (g_slots[i].state == TC_READY) map_tile_free(&g_slots[i].tile);
        memset(&g_slots[i], 0, sizeof(g_slots[i]));
    }
    UNLOCK();
    vSemaphoreDelete(g_lock);
    g_lock = NULL;
}

// Caller must hold the lock.
static tile_slot_t *find_locked(tile_id_t id)
{
    for (int i = 0; i < TILE_CACHE_SLOTS; i++) {
        if (g_slots[i].state != TC_FREE && tile_id_eq(&g_slots[i].id, &id))
            return &g_slots[i];
    }
    return NULL;
}

static void free_slot_locked(tile_slot_t *s)
{
    if (s->state == TC_READY) {
        g_stats.bytes_used -= map_tile_bytes(&s->tile);
        map_tile_free(&s->tile);
    }
    memset(s, 0, sizeof(*s));
}

// Least-recently-used unpinned slot, preferring READY over pending so we do
// not throw away an in-flight request.
static tile_slot_t *evict_candidate_locked(void)
{
    tile_slot_t *best = NULL;
    for (int i = 0; i < TILE_CACHE_SLOTS; i++) {
        tile_slot_t *s = &g_slots[i];
        if (s->state == TC_FREE || s->pins > 0) continue;
        if (s->state == TC_REQUESTED) continue;
        if (!best || s->last_used < best->last_used) best = s;
    }
    return best;
}

static tile_slot_t *alloc_slot_locked(uint32_t need_bytes)
{
    for (int i = 0; i < TILE_CACHE_SLOTS; i++) {
        if (g_slots[i].state == TC_FREE) {
            // Still have to respect the byte budget.
            while (g_stats.bytes_used + need_bytes > g_stats.bytes_budget) {
                tile_slot_t *v = evict_candidate_locked();
                if (!v) break;
                free_slot_locked(v);
                g_stats.evictions++;
            }
            return &g_slots[i];
        }
    }

    tile_slot_t *v = evict_candidate_locked();
    if (!v) return NULL;
    free_slot_locked(v);
    g_stats.evictions++;

    while (g_stats.bytes_used + need_bytes > g_stats.bytes_budget) {
        tile_slot_t *w = evict_candidate_locked();
        if (!w) break;
        free_slot_locked(w);
        g_stats.evictions++;
    }
    return v;
}

tile_slot_t *tile_cache_peek(tile_id_t id)
{
    LOCK();
    tile_slot_t *s = find_locked(id);
    UNLOCK();
    return s;
}

bool tile_cache_mark_requested(tile_id_t id, uint32_t now_ms)
{
    LOCK();
    tile_slot_t *s = find_locked(id);
    if (s) {
        s->state = TC_REQUESTED;
        s->requested_ms = now_ms;
        s->last_used = ++g_tick;
        UNLOCK();
        return true;
    }
    s = alloc_slot_locked(0);
    if (!s) { UNLOCK(); return false; }

    s->id = id;
    s->state = TC_REQUESTED;
    s->requested_ms = now_ms;
    s->last_used = ++g_tick;
    s->retries = 0;
    s->pins = 0;
    UNLOCK();
    return true;
}

bool tile_cache_store(tile_id_t id, map_tile_t *t)
{
    uint32_t need = (uint32_t)map_tile_bytes(t);

    LOCK();
    tile_slot_t *s = find_locked(id);
    if (s) {
        if (s->pins > 0) {
            // Being drawn right now; drop the new copy rather than stall.
            UNLOCK();
            ESP_LOGD(TAG, "store of pinned %u/%lu/%lu skipped", id.z,
                     (unsigned long)id.x, (unsigned long)id.y);
            return false;
        }
        if (s->state == TC_READY) {
            g_stats.bytes_used -= map_tile_bytes(&s->tile);
            map_tile_free(&s->tile);
        }
    } else {
        s = alloc_slot_locked(need);
        if (!s) { UNLOCK(); return false; }
    }

    s->id = id;
    s->state = TC_READY;
    s->tile = *t;
    s->last_used = ++g_tick;
    s->pins = 0;
    s->retries = 0;
    g_stats.bytes_used += need;

    memset(t, 0, sizeof(*t));  // ownership moved into the cache
    UNLOCK();
    return true;
}

bool tile_cache_store_empty(tile_id_t id)
{
    LOCK();
    tile_slot_t *s = find_locked(id);
    if (!s) {
        s = alloc_slot_locked(0);
        if (!s) { UNLOCK(); return false; }
    } else if (s->state == TC_READY && s->pins == 0) {
        g_stats.bytes_used -= map_tile_bytes(&s->tile);
        map_tile_free(&s->tile);
    } else if (s->pins > 0) {
        UNLOCK();
        return false;
    }

    s->id = id;
    s->state = TC_NOTHING;
    s->last_used = ++g_tick;
    UNLOCK();
    return true;
}

void tile_cache_invalidate(tile_id_t id)
{
    LOCK();
    tile_slot_t *s = find_locked(id);
    if (s && s->pins == 0) free_slot_locked(s);
    UNLOCK();
}

uint32_t tile_cache_pin_set(const tile_id_t *ids, uint32_t n_ids,
                            tile_slot_t **out, uint32_t max_out)
{
    uint32_t n = 0;
    LOCK();
    for (uint32_t i = 0; i < n_ids && n < max_out; i++) {
        tile_slot_t *s = find_locked(ids[i]);
        if (s && s->state == TC_READY) {
            s->pins++;
            s->last_used = ++g_tick;
            out[n++] = s;
            g_stats.hits++;
        } else if (!s || s->state == TC_REQUESTED) {
            g_stats.misses++;
        }
    }
    UNLOCK();
    return n;
}

void tile_cache_unpin(tile_slot_t **slots, uint32_t n)
{
    LOCK();
    for (uint32_t i = 0; i < n; i++) {
        if (slots[i] && slots[i]->pins > 0) slots[i]->pins--;
    }
    UNLOCK();
}

uint32_t tile_cache_missing(const tile_id_t *ids, uint32_t n_ids,
                            uint32_t now_ms, uint32_t timeout_ms,
                            tile_id_t *out, uint32_t max_out)
{
    uint32_t n = 0;
    LOCK();
    for (uint32_t i = 0; i < n_ids && n < max_out; i++) {
        tile_slot_t *s = find_locked(ids[i]);
        if (!s) {
            out[n++] = ids[i];
        } else if (s->state == TC_REQUESTED &&
                   (now_ms - s->requested_ms) > timeout_ms) {
            // Request went unanswered - the phone may have missed it.
            out[n++] = ids[i];
        }
    }
    UNLOCK();
    return n;
}

void tile_cache_get_stats(tile_cache_stats_t *out)
{
    LOCK();
    uint32_t used = 0, ready = 0, pending = 0;
    for (int i = 0; i < TILE_CACHE_SLOTS; i++) {
        if (g_slots[i].state == TC_FREE) continue;
        used++;
        if (g_slots[i].state == TC_READY) ready++;
        else if (g_slots[i].state == TC_REQUESTED) pending++;
    }
    g_stats.slots_used = used;
    g_stats.tiles_ready = ready;
    g_stats.tiles_pending = pending;
    *out = g_stats;
    UNLOCK();
}
