#include "raster.h"

#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

#define SUBPX 16  // Q4 subpixel for edge stepping

static inline int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }

// ---- surface -------------------------------------------------------------

void surface_init(surface_t *s, rgb565_t *px, int16_t w, int16_t h)
{
    s->px = px;
    s->w = w;
    s->h = h;
    surface_reset_clip(s);
}

void surface_set_clip(surface_t *s, int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    s->clip_x0 = (int16_t)imax(0, x0);
    s->clip_y0 = (int16_t)imax(0, y0);
    s->clip_x1 = (int16_t)imin(s->w, x1);
    s->clip_y1 = (int16_t)imin(s->h, y1);
}

void surface_reset_clip(surface_t *s)
{
    s->clip_x0 = 0;
    s->clip_y0 = 0;
    s->clip_x1 = s->w;
    s->clip_y1 = s->h;
}

void surface_fill(surface_t *s, rgb565_t c)
{
    for (int16_t y = s->clip_y0; y < s->clip_y1; y++) {
        rgb565_t *row = s->px + (size_t)y * s->w + s->clip_x0;
        int n = s->clip_x1 - s->clip_x0;
        // 32-bit stores halve the PSRAM write transactions vs a byte loop.
        uint32_t pair = ((uint32_t)c << 16) | c;
        uint32_t *p32 = (uint32_t *)row;
        int n2 = n >> 1;
        while (n2--) *p32++ = pair;
        if (n & 1) row[n - 1] = c;
    }
}

// ---- the one pixel-touching primitive ------------------------------------

void raster_span(surface_t *s, int32_t y, int32_t x0, int32_t x1, rgb565_t c)
{
    if (y < s->clip_y0 || y >= s->clip_y1) return;
    if (x0 < s->clip_x0) x0 = s->clip_x0;
    if (x1 > s->clip_x1) x1 = s->clip_x1;
    if (x1 <= x0) return;

    rgb565_t *p = s->px + (size_t)y * s->w + x0;
    int32_t n = x1 - x0;

    // Align to a 32-bit boundary, then burst.
    if (((uintptr_t)p & 2) && n) { *p++ = c; n--; }
    uint32_t pair = ((uint32_t)c << 16) | c;
    uint32_t *p32 = (uint32_t *)p;
    int32_t n2 = n >> 1;
    while (n2--) *p32++ = pair;
    if (n & 1) ((rgb565_t *)p32)[0] = c;
}

// ---- lines ---------------------------------------------------------------

void raster_line(surface_t *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                 rgb565_t c)
{
    int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int32_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;

    for (;;) {
        if (x0 >= s->clip_x0 && x0 < s->clip_x1 &&
            y0 >= s->clip_y0 && y0 < s->clip_y1) {
            s->px[(size_t)y0 * s->w + x0] = c;
        }
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = err << 1;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Fill a convex quad given as 4 screen-space corners, by scanline.
static void fill_quad(surface_t *s, const int32_t qx[4], const int32_t qy[4],
                      rgb565_t c)
{
    int32_t ymin = qy[0], ymax = qy[0];
    for (int i = 1; i < 4; i++) {
        if (qy[i] < ymin) ymin = qy[i];
        if (qy[i] > ymax) ymax = qy[i];
    }
    ymin = imax(ymin, s->clip_y0);
    ymax = imin(ymax, s->clip_y1 - 1);
    if (ymin > ymax) return;

    for (int32_t y = ymin; y <= ymax; y++) {
        int32_t xl = INT32_MAX, xr = INT32_MIN;
        // Sample edges at the pixel centre.
        int32_t yc = y;
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) & 3;
            int32_t ax = qx[i], ay = qy[i], bx = qx[j], by = qy[j];
            if (ay == by) continue;
            if ((yc < ay && yc < by) || (yc >= ay && yc >= by)) continue;
            // Parametric x at this scanline.
            int32_t x = ax + (int32_t)(((int64_t)(bx - ax) * (yc - ay)) / (by - ay));
            if (x < xl) xl = x;
            if (x > xr) xr = x;
        }
        if (xl <= xr) raster_span(s, y, xl, xr + 1, c);
    }
}

void raster_thick_segment(surface_t *s, int32_t x0, int32_t y0,
                          int32_t x1, int32_t y1, int32_t width, rgb565_t c)
{
    if (width <= 1) { raster_line(s, x0, y0, x1, y1, c); return; }

    int32_t dx = x1 - x0, dy = y1 - y0;
    if (dx == 0 && dy == 0) { raster_disc(s, x0, y0, width / 2, c); return; }

    // Perpendicular offset of half the width, in Q4 to keep thin roads even.
    int32_t len2 = dx * dx + dy * dy;
    int32_t len = 0;
    // Integer sqrt, good enough at these magnitudes.
    for (int32_t bit = 1 << 15; bit; bit >>= 1) {
        int32_t t = len | bit;
        if (t * t <= len2) len = t;
    }
    if (len == 0) len = 1;

    int32_t hw = (width * SUBPX) / 2;
    int32_t nx = (int32_t)(((int64_t)-dy * hw) / len);
    int32_t ny = (int32_t)(((int64_t)dx * hw) / len);

    int32_t qx[4] = {
        x0 + nx / SUBPX, x1 + nx / SUBPX, x1 - nx / SUBPX, x0 - nx / SUBPX,
    };
    int32_t qy[4] = {
        y0 + ny / SUBPX, y1 + ny / SUBPX, y1 - ny / SUBPX, y0 - ny / SUBPX,
    };
    fill_quad(s, qx, qy, c);
}

void raster_disc(surface_t *s, int32_t cx, int32_t cy, int32_t r, rgb565_t c)
{
    if (r <= 0) return;
    int32_t r2 = r * r;
    int32_t y0 = imax(cy - r, s->clip_y0);
    int32_t y1 = imin(cy + r, s->clip_y1 - 1);

    for (int32_t y = y0; y <= y1; y++) {
        int32_t dy = y - cy;
        int32_t w2 = r2 - dy * dy;
        if (w2 < 0) continue;
        int32_t w = 0;
        for (int32_t bit = 1 << 12; bit; bit >>= 1) {
            int32_t t = w | bit;
            if (t * t <= w2) w = t;
        }
        raster_span(s, y, cx - w, cx + w + 1, c);
    }
}

void raster_ring(surface_t *s, int32_t cx, int32_t cy, int32_t r,
                 int32_t thickness, rgb565_t c)
{
    if (r <= 0 || thickness <= 0) return;
    int32_t ro2 = r * r;
    int32_t ri = imax(0, r - thickness);
    int32_t ri2 = ri * ri;

    int32_t y0 = imax(cy - r, s->clip_y0);
    int32_t y1 = imin(cy + r, s->clip_y1 - 1);

    for (int32_t y = y0; y <= y1; y++) {
        int32_t dy = y - cy;
        int32_t wo2 = ro2 - dy * dy;
        if (wo2 < 0) continue;
        int32_t wo = 0;
        for (int32_t bit = 1 << 12; bit; bit >>= 1) {
            int32_t t = wo | bit;
            if (t * t <= wo2) wo = t;
        }
        int32_t wi2 = ri2 - dy * dy;
        if (wi2 <= 0) {
            raster_span(s, y, cx - wo, cx + wo + 1, c);
        } else {
            int32_t wi = 0;
            for (int32_t bit = 1 << 12; bit; bit >>= 1) {
                int32_t t = wi | bit;
                if (t * t <= wi2) wi = t;
            }
            raster_span(s, y, cx - wo, cx - wi + 1, c);
            raster_span(s, y, cx + wi, cx + wo + 1, c);
        }
    }
}

void raster_polyline(surface_t *s, const int16_t *pts, uint32_t n_pts,
                     int32_t width, rgb565_t c)
{
    if (n_pts < 2) {
        if (n_pts == 1 && width > 2)
            raster_disc(s, pts[0], pts[1], width / 2, c);
        return;
    }

    for (uint32_t i = 0; i + 1 < n_pts; i++) {
        int32_t ax = pts[i * 2],     ay = pts[i * 2 + 1];
        int32_t bx = pts[i * 2 + 2], by = pts[i * 2 + 3];

        // Cheap reject: whole segment outside the clip box.
        if ((ax < s->clip_x0 && bx < s->clip_x0) ||
            (ax >= s->clip_x1 && bx >= s->clip_x1) ||
            (ay < s->clip_y0 && by < s->clip_y0) ||
            (ay >= s->clip_y1 && by >= s->clip_y1)) {
            continue;
        }

        raster_thick_segment(s, ax, ay, bx, by, width, c);

        // Round join at the interior vertex, so corners do not show a notch.
        if (width > 3 && i + 2 < n_pts) {
            raster_disc(s, bx, by, width / 2, c);
        }
    }
}

// ---- polygon fill (even-odd, active edge table) ---------------------------

typedef struct {
    int32_t y_max;   // last scanline this edge is active on
    int32_t x;       // current x, Q4
    int32_t dxdy;    // x step per scanline, Q4
} aet_edge_t;

typedef struct {
    int32_t y_min;
    aet_edge_t e;
} sorted_edge_t;

bool raster_scratch_init(raster_scratch_t *sc, uint32_t max_edges)
{
    // Edge table + active index list + crossing list.
    size_t sz = (size_t)max_edges * sizeof(sorted_edge_t)
              + (size_t)max_edges * sizeof(uint16_t)
              + (size_t)max_edges * sizeof(int32_t);

    sc->mem = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sc->mem) {
        sc->size = 0;
        sc->max_edges = 0;
        return false;
    }
    sc->size = sz;
    sc->max_edges = max_edges;
    return true;
}

void raster_scratch_free(raster_scratch_t *sc)
{
    if (sc->mem) heap_caps_free(sc->mem);
    memset(sc, 0, sizeof(*sc));
}

static int cmp_sorted_edge(const void *a, const void *b)
{
    int32_t ya = ((const sorted_edge_t *)a)->y_min;
    int32_t yb = ((const sorted_edge_t *)b)->y_min;
    return (ya > yb) - (ya < yb);
}

static int cmp_i32(const void *a, const void *b)
{
    int32_t xa = *(const int32_t *)a, xb = *(const int32_t *)b;
    return (xa > xb) - (xa < xb);
}

void raster_fill_rings(surface_t *s, raster_scratch_t *sc,
                       const raster_ring_t *rings, uint32_t n_rings, rgb565_t c)
{
    if (!sc->mem || n_rings == 0) return;

    sorted_edge_t *edges = (sorted_edge_t *)sc->mem;
    uint16_t *active = (uint16_t *)(edges + sc->max_edges);
    int32_t  *xs = (int32_t *)(active + sc->max_edges);

    uint32_t n_edges = 0;
    int32_t poly_ymin = INT32_MAX, poly_ymax = INT32_MIN;

    for (uint32_t r = 0; r < n_rings; r++) {
        const int16_t *p = rings[r].pts;
        uint32_t n = rings[r].n_pts;
        if (n < 3) continue;

        for (uint32_t i = 0; i < n; i++) {
            uint32_t j = (i + 1) % n;   // implicit closing edge
            int32_t ax = p[i * 2], ay = p[i * 2 + 1];
            int32_t bx = p[j * 2], by = p[j * 2 + 1];

            if (ay == by) continue;             // horizontal edges contribute nothing
            if (ay > by) {                      // normalise to downward
                int32_t t;
                t = ax; ax = bx; bx = t;
                t = ay; ay = by; by = t;
            }
            if (by <= s->clip_y0 || ay >= s->clip_y1) continue;
            if (n_edges >= sc->max_edges) goto scan;  // over budget: draw what we have

            int32_t dy = by - ay;
            int32_t dxdy = (int32_t)(((int64_t)(bx - ax) * SUBPX) / dy);

            int32_t y_start = ay;
            int32_t x_start = ax * SUBPX;
            if (y_start < s->clip_y0) {         // clip entry into the surface
                x_start += dxdy * (s->clip_y0 - y_start);
                y_start = s->clip_y0;
            }

            edges[n_edges].y_min = y_start;
            edges[n_edges].e.y_max = by - 1;
            edges[n_edges].e.x = x_start;
            edges[n_edges].e.dxdy = dxdy;
            n_edges++;

            if (y_start < poly_ymin) poly_ymin = y_start;
            if (by - 1 > poly_ymax) poly_ymax = by - 1;
        }
    }

scan:
    if (n_edges == 0) return;

    qsort(edges, n_edges, sizeof(sorted_edge_t), cmp_sorted_edge);

    int32_t y0 = imax(poly_ymin, s->clip_y0);
    int32_t y1 = imin(poly_ymax, s->clip_y1 - 1);

    uint32_t next_edge = 0, n_active = 0;

    for (int32_t y = y0; y <= y1; y++) {
        // Admit edges starting on this scanline.
        while (next_edge < n_edges && edges[next_edge].y_min <= y) {
            if (edges[next_edge].e.y_max >= y && n_active < sc->max_edges)
                active[n_active++] = (uint16_t)next_edge;
            next_edge++;
        }
        // Retire finished edges.
        for (uint32_t i = 0; i < n_active; ) {
            if (edges[active[i]].e.y_max < y) active[i] = active[--n_active];
            else i++;
        }
        if (n_active == 0) {
            if (next_edge >= n_edges) break;
            continue;
        }

        uint32_t n_x = 0;
        for (uint32_t i = 0; i < n_active; i++) {
            aet_edge_t *e = &edges[active[i]].e;
            xs[n_x++] = e->x;
            e->x += e->dxdy;      // step for the next scanline
        }

        qsort(xs, n_x, sizeof(int32_t), cmp_i32);

        // Even-odd: fill between consecutive crossing pairs. Holes wound as
        // separate rings fall out of this for free.
        for (uint32_t i = 0; i + 1 < n_x; i += 2) {
            raster_span(s, y, xs[i] / SUBPX, xs[i + 1] / SUBPX + 1, c);
        }
    }
}
