#include "nav_state.h"

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "nav";

static nav_state_t g_nav;
static SemaphoreHandle_t g_lock;

// Route chunks arrive over BLE and a varint can straddle a chunk boundary, so
// the raw bytes are buffered whole and decoded once at ROUTE_END.
static uint8_t *g_route_raw;
static uint32_t g_route_raw_len;
static uint32_t g_route_raw_cap;
static uint32_t g_route_expect_pts;

#define LOCK()   xSemaphoreTake(g_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(g_lock)

bool nav_init(void)
{
    memset(&g_nav, 0, sizeof(g_nav));
    g_lock = xSemaphoreCreateMutex();
    return g_lock != NULL;
}

void nav_set_link(bool up)
{
    LOCK();
    g_nav.link_up = up;
    UNLOCK();
}

void nav_set_fix(const msg_gps_fix_t *m, uint32_t now_ms)
{
    LOCK();
    nav_fix_t *f = &g_nav.fix;

    f->lat = (double)m->lat_e7 * 1e-7;
    f->lon = (double)m->lon_e7 * 1e-7;
    f->epoch_s = m->epoch_s;
    f->rx_ms = now_ms;
    f->valid = true;

    f->speed_mps = (m->speed_cm_s == GPS_UNKNOWN_U16)
                 ? 0.0f : (float)m->speed_cm_s * 0.01f;

    f->accuracy_m = (m->accuracy_dm == GPS_UNKNOWN_U16)
                  ? 0.0f : (float)m->accuracy_dm * 0.1f;

    // Hold the previous heading when the phone reports none. Snapping to
    // north every time you stop at a light would be worse than slightly
    // stale, and there is no magnetometer to fall back on.
    if (m->bearing_cdeg != GPS_UNKNOWN_U16 && m->bearing_cdeg < 36000)
        f->bearing_deg = (float)m->bearing_cdeg * 0.01f;

    UNLOCK();
}

// ---- route ---------------------------------------------------------------

bool nav_route_begin(uint32_t n_pts, uint32_t dist_m, uint32_t dur_s)
{
    if (n_pts == 0 || n_pts > NAV_MAX_ROUTE_PTS) {
        ESP_LOGW(TAG, "rejecting route of %lu points", (unsigned long)n_pts);
        return false;
    }

    LOCK();
    g_nav.route.has_route = false;   // hide the old route while rebuilding
    g_nav.route.total_dist_m = dist_m;
    g_nav.route.total_dur_s = dur_s;

    if (g_nav.route.cap_pts < n_pts) {
        if (g_nav.route.world_pts) heap_caps_free(g_nav.route.world_pts);
        g_nav.route.world_pts = heap_caps_malloc((size_t)n_pts * 2 * sizeof(int32_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        g_nav.route.cap_pts = g_nav.route.world_pts ? n_pts : 0;
    }
    g_nav.route.n_pts = 0;

    // Worst case 5 bytes per varint, two per point.
    uint32_t cap = n_pts * 10 + 16;
    if (g_route_raw_cap < cap) {
        if (g_route_raw) heap_caps_free(g_route_raw);
        g_route_raw = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        g_route_raw_cap = g_route_raw ? cap : 0;
    }
    g_route_raw_len = 0;
    g_route_expect_pts = n_pts;

    bool ok = g_nav.route.world_pts && g_route_raw;
    UNLOCK();

    if (!ok) ESP_LOGE(TAG, "route buffers alloc failed (%lu pts)", (unsigned long)n_pts);
    return ok;
}

bool nav_route_append(const uint8_t *data, uint32_t len)
{
    LOCK();
    bool ok = g_route_raw && (g_route_raw_len + len <= g_route_raw_cap);
    if (ok) {
        memcpy(g_route_raw + g_route_raw_len, data, len);
        g_route_raw_len += len;
    }
    UNLOCK();
    return ok;
}

static inline bool rd_varint(const uint8_t **p, const uint8_t *end, uint32_t *out)
{
    uint32_t v = 0;
    int shift = 0;
    const uint8_t *q = *p;
    while (q < end) {
        uint8_t b = *q++;
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *p = q; *out = v; return true; }
        shift += 7;
        if (shift > 28) return false;
    }
    return false;
}

static inline int32_t zz(uint32_t v) { return (int32_t)((v >> 1) ^ (~(v & 1) + 1)); }

void nav_route_commit(void)
{
    LOCK();

    if (!g_route_raw || !g_nav.route.world_pts) { UNLOCK(); return; }

    const uint8_t *p = g_route_raw;
    const uint8_t *end = g_route_raw + g_route_raw_len;

    int32_t lat_e6 = 0, lon_e6 = 0;
    uint32_t n = 0;

    while (n < g_route_expect_pts && n < g_nav.route.cap_pts) {
        uint32_t a, b;
        if (!rd_varint(&p, end, &a)) break;
        if (!rd_varint(&p, end, &b)) break;
        lat_e6 += zz(a);
        lon_e6 += zz(b);

        // Project once, here, into world pixels at the reference zoom.
        double lat = (double)lat_e6 * 1e-6;
        double lon = (double)lon_e6 * 1e-6;
        g_nav.route.world_pts[n * 2]     = (int32_t)lrint(merc_lon_to_wx(lon, ROUTE_REF_ZOOM));
        g_nav.route.world_pts[n * 2 + 1] = (int32_t)lrint(merc_lat_to_wy(lat, ROUTE_REF_ZOOM));
        n++;
    }

    g_nav.route.n_pts = n;
    g_nav.route.has_route = (n >= 2);
    g_route_raw_len = 0;

    UNLOCK();

    ESP_LOGI(TAG, "route committed: %lu of %lu points",
             (unsigned long)n, (unsigned long)g_route_expect_pts);
}

void nav_route_clear(void)
{
    LOCK();
    g_nav.route.has_route = false;
    g_nav.route.n_pts = 0;
    g_nav.route.has_maneuver = false;
    g_nav.route.remain_m = 0;
    g_nav.route.remain_s = 0;
    g_route_raw_len = 0;
    UNLOCK();
}

void nav_set_maneuver(const nav_maneuver_t *m)
{
    LOCK();
    g_nav.route.next_maneuver = *m;
    g_nav.route.has_maneuver = (m->kind != MANEUVER_NONE);
    UNLOCK();
}

void nav_set_progress(uint32_t eta_epoch_s, uint32_t remain_m, uint32_t remain_s)
{
    LOCK();
    g_nav.route.eta_epoch_s = eta_epoch_s;
    g_nav.route.remain_m = remain_m;
    g_nav.route.remain_s = remain_s;
    UNLOCK();
}

void nav_snapshot(nav_state_t *out)
{
    LOCK();
    *out = g_nav;
    UNLOCK();
}

uint32_t nav_dist_to_maneuver(void)
{
    LOCK();

    if (!g_nav.route.has_route || !g_nav.route.has_maneuver || !g_nav.fix.valid) {
        UNLOCK();
        return 0;
    }

    uint32_t idx = g_nav.route.next_maneuver.pt_index;
    if (idx >= g_nav.route.n_pts) { UNLOCK(); return 0; }

    // Straight-line from the fix to the maneuver point, plus the polyline
    // length between. Good enough at the ranges that matter (< 2 km) and it
    // updates every frame instead of every GPS packet.
    double mpp = merc_meters_per_px(g_nav.fix.lat, ROUTE_REF_ZOOM);

    double px = merc_lon_to_wx(g_nav.fix.lon, ROUTE_REF_ZOOM);
    double py = merc_lat_to_wy(g_nav.fix.lat, ROUTE_REF_ZOOM);

    // Nearest route vertex ahead of us, bounded to keep this cheap.
    uint32_t start = 0;
    double best = 1e30;
    uint32_t scan_lo = (idx > 256) ? idx - 256 : 0;
    for (uint32_t i = scan_lo; i <= idx; i++) {
        double dx = g_nav.route.world_pts[i * 2] - px;
        double dy = g_nav.route.world_pts[i * 2 + 1] - py;
        double d2 = dx * dx + dy * dy;
        if (d2 < best) { best = d2; start = i; }
    }

    double total = sqrt(best);
    for (uint32_t i = start; i + 1 <= idx; i++) {
        double dx = g_nav.route.world_pts[(i + 1) * 2] - g_nav.route.world_pts[i * 2];
        double dy = g_nav.route.world_pts[(i + 1) * 2 + 1] - g_nav.route.world_pts[i * 2 + 1];
        total += sqrt(dx * dx + dy * dy);
    }

    UNLOCK();
    return (uint32_t)(total * mpp);
}
