#include "map_render.h"

#include <string.h>
#include <math.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "style.h"
#include "../map/tile_cache.h"

static const char *TAG = "map_render";

#define XFORM_POINTS  16384   // largest single feature we will draw
#define MAX_RINGS     256
#define TILE_MARGIN_PX 64

bool map_render_init(map_renderer_t *r, rgb565_t *fb, int16_t w, int16_t h)
{
    memset(r, 0, sizeof(*r));
    surface_init(&r->surf, fb, w, h);

    r->xform_cap = XFORM_POINTS;
    r->xform = heap_caps_malloc((size_t)r->xform_cap * 2 * sizeof(int16_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r->rings_cap = MAX_RINGS;
    r->rings = heap_caps_malloc((size_t)r->rings_cap * sizeof(raster_ring_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!r->xform || !r->rings || !raster_scratch_init(&r->scratch, 4096)) {
        ESP_LOGE(TAG, "renderer scratch allocation failed");
        map_render_deinit(r);
        return false;
    }
    return true;
}

void map_render_deinit(map_renderer_t *r)
{
    if (r->xform) heap_caps_free(r->xform);
    if (r->rings) heap_caps_free(r->rings);
    raster_scratch_free(&r->scratch);
    r->xform = NULL;
    r->rings = NULL;
}

// ---- geometry transform --------------------------------------------------

// Transform one feature's points into r->xform starting at `dst_pt`, clamping
// to int16 screen range. Returns points written; fills the screen-space bbox.
static uint32_t xform_feature(map_renderer_t *r, const map_tile_t *t,
                              const etil_feature_t *f, const map_affine_t *m,
                              uint32_t dst_pt,
                              int32_t bbox[4])
{
    uint32_t n = f->point_count;
    if (dst_pt + n > r->xform_cap) {
        n = (r->xform_cap > dst_pt) ? (r->xform_cap - dst_pt) : 0;
    }
    if (n == 0) return 0;

    const int16_t *src = t->pts + (size_t)f->first_point * 2;
    int16_t *dst = r->xform + (size_t)dst_pt * 2;

    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;

    for (uint32_t i = 0; i < n; i++) {
        int32_t sx, sy;
        map_affine_apply(m, src[i * 2], src[i * 2 + 1], &sx, &sy);

        // Clamp well outside the screen rather than wrapping int16. Keeps
        // long ways that leave the viewport pointing the right direction.
        if (sx < -8192) sx = -8192; else if (sx > 8191) sx = 8191;
        if (sy < -8192) sy = -8192; else if (sy > 8191) sy = 8191;

        dst[i * 2]     = (int16_t)sx;
        dst[i * 2 + 1] = (int16_t)sy;

        if (sx < x0) x0 = sx;
        if (sx > x1) x1 = sx;
        if (sy < y0) y0 = sy;
        if (sy > y1) y1 = sy;
    }

    bbox[0] = x0; bbox[1] = y0; bbox[2] = x1; bbox[3] = y1;
    return n;
}

static inline bool bbox_visible(const surface_t *s, const int32_t b[4], int32_t pad)
{
    return !(b[2] + pad < s->clip_x0 || b[0] - pad >= s->clip_x1 ||
             b[3] + pad < s->clip_y0 || b[1] - pad >= s->clip_y1);
}

// ---- passes --------------------------------------------------------------

static void draw_area_class(map_renderer_t *r, const map_view_t *v,
                            tile_slot_t **tiles, uint32_t n_tiles,
                            enum style_class cls)
{
    const style_def_t *def = style_get(cls);
    if (v->display_zoom < def->min_zoom) return;

    for (uint32_t ti = 0; ti < n_tiles; ti++) {
        const map_tile_t *t = &tiles[ti]->tile;
        if (t->empty) continue;

        map_affine_t m;
        map_affine_for_tile(v, t->id.z, t->id.x, t->id.y, t->extent, &m);

        for (uint8_t li = 0; li < t->layer_count; li++) {
            const etil_layer_t *L = &t->layers[li];
            if (L->kind != ETIL_POLYGON || L->style_class != cls) continue;

            uint32_t fi = L->first_feature;
            uint32_t end = fi + L->feature_count;

            while (fi < end) {
                // Gather this polygon's rings: exterior plus any holes.
                uint32_t dst_pt = 0, n_rings = 0;
                int32_t poly_bbox[4] = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };

                for (;;) {
                    const etil_feature_t *f = &t->features[fi];
                    int32_t bb[4];
                    uint32_t n = xform_feature(r, t, f, &m, dst_pt, bb);

                    if (n >= 3 && n_rings < r->rings_cap) {
                        r->rings[n_rings].pts = r->xform + (size_t)dst_pt * 2;
                        r->rings[n_rings].n_pts = n;
                        n_rings++;
                        if (bb[0] < poly_bbox[0]) poly_bbox[0] = bb[0];
                        if (bb[1] < poly_bbox[1]) poly_bbox[1] = bb[1];
                        if (bb[2] > poly_bbox[2]) poly_bbox[2] = bb[2];
                        if (bb[3] > poly_bbox[3]) poly_bbox[3] = bb[3];
                        dst_pt += n;
                    }

                    bool more = f->ring_continues && (fi + 1 < end);
                    fi++;
                    if (!more) break;
                }

                if (n_rings && bbox_visible(&r->surf, poly_bbox, 0)) {
                    raster_fill_rings(&r->surf, &r->scratch, r->rings, n_rings,
                                      def->fill);
                    r->last_points += dst_pt;
                }
            }
        }
    }
}

static void draw_line_class(map_renderer_t *r, const map_view_t *v,
                            tile_slot_t **tiles, uint32_t n_tiles,
                            enum style_class cls, bool casing_pass)
{
    const style_def_t *def = style_get(cls);
    uint8_t w = style_width(cls, v->display_zoom);
    if (w == 0) return;

    rgb565_t colour;
    int32_t width;

    if (casing_pass) {
        if (def->casing_px == 0) return;
        colour = def->casing;
        width = w + def->casing_px * 2;
    } else {
        colour = def->fill;
        width = w;
    }

    for (uint32_t ti = 0; ti < n_tiles; ti++) {
        const map_tile_t *t = &tiles[ti]->tile;
        if (t->empty) continue;

        map_affine_t m;
        map_affine_for_tile(v, t->id.z, t->id.x, t->id.y, t->extent, &m);

        for (uint8_t li = 0; li < t->layer_count; li++) {
            const etil_layer_t *L = &t->layers[li];
            if (L->kind != ETIL_LINE || L->style_class != cls) continue;

            for (uint32_t k = 0; k < L->feature_count; k++) {
                const etil_feature_t *f = &t->features[L->first_feature + k];
                int32_t bb[4];
                uint32_t n = xform_feature(r, t, f, &m, 0, bb);
                if (n < 2) continue;
                if (!bbox_visible(&r->surf, bb, width)) continue;

                raster_polyline(&r->surf, r->xform, n, width, colour);
                r->last_points += n;
            }
        }
    }
}

static void draw_route(map_renderer_t *r, const map_view_t *v,
                       const nav_state_t *nav)
{
    const nav_route_t *rt = &nav->route;
    if (!rt->has_route || rt->n_pts < 2 || !rt->world_pts) return;

    map_affine_t m;
    map_affine_for_world_zoom(v, ROUTE_REF_ZOOM, &m);

    // The route can be far longer than the transform scratch, so walk it in
    // overlapping windows (one point of overlap keeps the joins continuous).
    const uint32_t window = r->xform_cap;
    uint32_t i = 0;

    int32_t base_w = style_width(SC_ROAD_PRIMARY, v->display_zoom);
    if (base_w < 6) base_w = 6;
    int32_t route_w = base_w + 3;

    while (i + 1 < rt->n_pts) {
        uint32_t n = rt->n_pts - i;
        if (n > window) n = window;

        int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        for (uint32_t k = 0; k < n; k++) {
            int32_t sx, sy;
            map_affine_apply(&m, rt->world_pts[(i + k) * 2],
                             rt->world_pts[(i + k) * 2 + 1], &sx, &sy);
            if (sx < -8192) sx = -8192; else if (sx > 8191) sx = 8191;
            if (sy < -8192) sy = -8192; else if (sy > 8191) sy = 8191;
            r->xform[k * 2]     = (int16_t)sx;
            r->xform[k * 2 + 1] = (int16_t)sy;
            if (sx < x0) x0 = sx;
            if (sx > x1) x1 = sx;
            if (sy < y0) y0 = sy;
            if (sy > y1) y1 = sy;
        }

        int32_t bb[4] = { x0, y0, x1, y1 };
        if (bbox_visible(&r->surf, bb, route_w)) {
            raster_polyline(&r->surf, r->xform, n, route_w + 4, style_route_casing());
            raster_polyline(&r->surf, r->xform, n, route_w, style_route_fill());
            r->last_points += n * 2;
        }

        if (n < window) break;
        i += window - 1;  // overlap by one so segments join up
    }
}

static void draw_puck(map_renderer_t *r, const map_view_t *v,
                      const nav_state_t *nav)
{
    if (!nav->fix.valid) return;

    int32_t sx, sy;
    map_latlon_to_screen(v, nav->fix.lat, nav->fix.lon, &sx, &sy);

    // Accuracy halo, only when it is actually meaningful on screen.
    if (nav->fix.accuracy_m > 0.0f) {
        double mpp = merc_meters_per_px(nav->fix.lat, v->display_zoom);
        if (mpp > 0.0) {
            int32_t rad = (int32_t)(nav->fix.accuracy_m / mpp);
            if (rad > 8 && rad < 400)
                raster_ring(&r->surf, sx, sy, rad, 1, style_puck_fill());
        }
    }

    // In heading-up mode the map rotates and the puck stays pointing up, so
    // the cone is drawn relative to the residual angle.
    float ang = nav->fix.bearing_deg * (float)M_PI / 180.0f + v->rotation_rad;
    float ca = cosf(ang), sa = sinf(ang);

    const int32_t R = 11;
    int32_t tipx = sx + (int32_t)(sa * (R + 9));
    int32_t tipy = sy - (int32_t)(ca * (R + 9));

    raster_thick_segment(&r->surf, sx, sy, tipx, tipy, 7, style_puck_casing());
    raster_thick_segment(&r->surf, sx, sy, tipx, tipy, 4, style_puck_fill());

    raster_disc(&r->surf, sx, sy, R, style_puck_casing());
    raster_disc(&r->surf, sx, sy, R - 3, style_puck_fill());
}

// ---- frame ---------------------------------------------------------------

void map_render_frame(map_renderer_t *r, const map_view_t *v,
                      const nav_state_t *nav)
{
    int64_t t_start = esp_timer_get_time();
    r->last_points = 0;

    surface_reset_clip(&r->surf);
    surface_fill(&r->surf, style_background());

    tile_id_t ids[MAP_MAX_VISIBLE_TILES];
    int n_ids = map_visible_tiles(v, TILE_MARGIN_PX, ids, MAP_MAX_VISIBLE_TILES);

    tile_slot_t *pinned[MAP_MAX_VISIBLE_TILES];
    uint32_t n_tiles = tile_cache_pin_set(ids, (uint32_t)n_ids, pinned,
                                          MAP_MAX_VISIBLE_TILES);
    r->last_tiles = n_tiles;

    if (n_tiles) {
        const enum style_class *order;

        uint32_t n_area = style_area_order(&order);
        for (uint32_t i = 0; i < n_area; i++)
            draw_area_class(r, v, pinned, n_tiles, order[i]);

        // Casings for every road class first, then fills for every class.
        // Doing it per-class instead would leave dark seams across junctions.
        uint32_t n_road = style_road_order(&order);
        for (uint32_t i = 0; i < n_road; i++)
            draw_line_class(r, v, pinned, n_tiles, order[i], true);
        for (uint32_t i = 0; i < n_road; i++)
            draw_line_class(r, v, pinned, n_tiles, order[i], false);
    }

    draw_route(r, v, nav);
    draw_puck(r, v, nav);

    tile_cache_unpin(pinned, n_tiles);

    r->last_us = (uint32_t)(esp_timer_get_time() - t_start);
}
