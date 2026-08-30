// esp-maps - a vector map navigator for the Waveshare ESP32-S3 AMOLED 1.8.
//
// Work splits across both cores:
//   loop()     - core 1 (where Arduino pins loopTask): renders and flushes
//   link_task  - core 0: works out which tiles are missing and asks the phone
//
// NimBLE runs on its own task and feeds the tile cache and nav state directly,
// so neither of the above ever blocks on the radio.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <math.h>
#include <string.h>

#include "bsp/board_pins.h"
#include "bsp/display.h"
#include "ble/ble_link.h"
#include "map/mercator.h"
#include "map/tile_cache.h"
#include "map/nav_state.h"
#include "render/map_render.h"
#include "render/style.h"
#include "ui/hud.h"

#define TILE_CACHE_BYTES (5 * 1024 * 1024)
#define RENDER_PERIOD_MS 50     // 20 fps ceiling
#define LINK_PERIOD_MS   400
#define TILE_REQ_TIMEOUT 8000

static map_renderer_t g_renderer;
static map_view_t     g_view;
static SemaphoreHandle_t g_view_lock;

static bool g_follow = true;
static bool g_heading_up = true;
static bool g_show_debug = true;
static bool g_ready = false;

static void view_snapshot(map_view_t *out)
{
    xSemaphoreTake(g_view_lock, portMAX_DELAY);
    *out = g_view;
    xSemaphoreGive(g_view_lock);
}

// ---- link task (core 0) --------------------------------------------------

static void link_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t last_view_ms = 0;

    for (;;) {
        if (ble_link_connected()) {
            map_view_t v;
            view_snapshot(&v);

            uint32_t now = millis();

            // Ask for a slightly wider area than we draw, so tiles land before
            // they are needed rather than popping in at the edge.
            tile_id_t want[MAP_MAX_VISIBLE_TILES];
            int n_want = map_visible_tiles(&v, 192, want, MAP_MAX_VISIBLE_TILES);

            tile_id_t missing[MAP_MAX_VISIBLE_TILES];
            uint32_t n_missing = tile_cache_missing(want, (uint32_t)n_want, now,
                                                   TILE_REQ_TIMEOUT, missing,
                                                   MAP_MAX_VISIBLE_TILES);

            if (n_missing) {
                for (uint32_t i = 0; i < n_missing; i++)
                    tile_cache_mark_requested(missing[i], now);
                ble_link_request_tiles(missing, n_missing);

                // Without this you cannot tell whether the firmware never
                // asked or the phone never answered - the two look identical
                // from a blank screen.
                double lat, lon;
                map_view_get_center(&v, &lat, &lon);
                Serial.printf("[link] requested %lu tiles at z%u, view %.5f,%.5f "
                              "(first %u/%lu/%lu)\n",
                              (unsigned long)n_missing, v.data_zoom, lat, lon,
                              missing[0].z, (unsigned long)missing[0].x,
                              (unsigned long)missing[0].y);
            }

            // Periodic cache summary, so a silent stall is visible.
            static uint32_t last_stat_ms = 0;
            if (now - last_stat_ms > 5000) {
                last_stat_ms = now;
                tile_cache_stats_t cs;
                tile_cache_get_stats(&cs);
                ble_link_stats_t bs;
                ble_link_get_stats(&bs);
                Serial.printf("[link] cache ready=%lu pending=%lu | rx %lu B in "
                              "%lu pkts | tiles ok=%lu crcfail=%lu\n",
                              (unsigned long)cs.tiles_ready,
                              (unsigned long)cs.tiles_pending,
                              (unsigned long)bs.rx_bytes,
                              (unsigned long)bs.rx_packets,
                              (unsigned long)bs.tiles_ok,
                              (unsigned long)bs.tiles_crc_fail);
            }

            if (now - last_view_ms > 1000) {
                double lat, lon;
                map_view_get_center(&v, &lat, &lon);
                ble_link_send_view(lat, lon, v.display_zoom,
                                   -v.rotation_rad * 180.0f / (float)M_PI);
                last_view_ms = now;
            }
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(LINK_PERIOD_MS));
    }
}

// ---- startup -------------------------------------------------------------

static void log_memory(const char *when)
{
    Serial.printf("[esp-maps] %s: internal %u KB free, PSRAM %u KB free\n", when,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void setup()
{
    Serial.begin(115200);
    delay(300);   // let USB CDC enumerate before the first print

    Serial.printf("\n[esp-maps] starting, board rev %s\n",
                  ESPMAPS_BOARD_REV == ESPMAPS_REV_A ? "A (SH8601/FT3168)"
                                                     : "B (CO5300/CST816)");
    log_memory("boot");

    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 4 * 1024 * 1024) {
        Serial.println("[esp-maps] WARNING: PSRAM looks small or absent. Check "
                       "board_build.arduino.memory_type = qio_opi in platformio.ini");
    }

    if (!display_init()) {
        Serial.println("[esp-maps] display init failed - see docs/BOARD_BRINGUP.md");
        return;
    }

#if ESPMAPS_PANEL_SELFTEST
    // Runs before the renderer exists, so a black screen here means the panel
    // and not the map code.
    display_selftest();
#endif

    g_view_lock = xSemaphoreCreateMutex();
    if (!g_view_lock) { Serial.println("[esp-maps] view lock failed"); return; }

    if (!nav_init())                        { Serial.println("[esp-maps] nav init failed"); return; }
    if (!tile_cache_init(TILE_CACHE_BYTES)) { Serial.println("[esp-maps] cache init failed"); return; }

    if (!map_render_init(&g_renderer, display_framebuffer(),
                         BSP_LCD_H_RES, BSP_LCD_V_RES)) {
        Serial.println("[esp-maps] renderer init failed");
        return;
    }

    map_view_init(&g_view, BSP_LCD_H_RES, BSP_LCD_V_RES);
    map_view_set_zoom(&g_view, 17);
    style_set_theme(STYLE_DAY);

    // Somewhere recognisable to look at before the first fix arrives.
    map_view_set_center(&g_view, 51.5074, -0.1278);

    if (!ble_link_init()) { Serial.println("[esp-maps] BLE init failed"); return; }

    log_memory("after init");

    xTaskCreatePinnedToCore(link_task, "link", 4096, NULL, 4, NULL, 0);

    g_ready = true;
    Serial.printf("[esp-maps] up. advertising as \"%s\" - connect the phone app.\n",
                  ESPMAPS_ADV_NAME);
}

// ---- render loop (core 1) ------------------------------------------------

void loop()
{
    if (!g_ready) { delay(500); return; }

    static uint32_t next_frame = 0;
    uint32_t now = millis();
    if ((int32_t)(now - next_frame) < 0) {
        delay(1);
        return;
    }
    next_frame = now + RENDER_PERIOD_MS;

    nav_state_t nav;
    nav_snapshot(&nav);

    xSemaphoreTake(g_view_lock, portMAX_DELAY);
    if (g_follow && nav.fix.valid) {
        map_view_set_center(&g_view, nav.fix.lat, nav.fix.lon);
        g_view.rotation_rad = g_heading_up
            ? -nav.fix.bearing_deg * (float)M_PI / 180.0f
            : 0.0f;
    }
    map_view_t v = g_view;
    xSemaphoreGive(g_view_lock);

    map_render_frame(&g_renderer, &v, &nav);

    hud_debug_t dbg;
    memset(&dbg, 0, sizeof(dbg));
    dbg.show_debug = g_show_debug;
    if (g_show_debug) {
        tile_cache_stats_t cs;
        ble_link_stats_t bs;
        tile_cache_get_stats(&cs);
        ble_link_get_stats(&bs);
        dbg.frame_us = g_renderer.last_us;
        dbg.tiles_ready = cs.tiles_ready;
        dbg.tiles_pending = cs.tiles_pending;
        dbg.mtu = bs.mtu;
        dbg.rx_kb = bs.rx_bytes / 1024;
    }

    hud_draw(&g_renderer.surf, &v, &nav, nav_dist_to_maneuver(), &dbg);

    display_flush();
    display_wait_flush();
}
