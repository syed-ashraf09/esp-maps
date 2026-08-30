// Web Mercator projection and the tile-local -> screen affine transform.
//
// The hot path (transforming tile geometry every frame) must not touch
// floating point per-point. So: doubles are used once per frame to build a
// per-tile 2x3 affine in Q16 fixed point, and every geometry point after that
// costs 2 multiply-adds per axis on integers.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


#define MERC_TILE_PX 256  // nominal pixels per tile at its own zoom

typedef struct {
    // View centre in "world pixels" at display_zoom. World spans
    // 256 * 2^zoom pixels, origin top-left at (-180 lon, +85.05 lat).
    double  center_wx;
    double  center_wy;
    uint8_t display_zoom;   // what the user sees
    uint8_t data_zoom;      // what we fetch (<= display_zoom, see PROTOCOL 7)
    float   rotation_rad;   // 0 = north up; heading-up sets this to -bearing
    int16_t screen_w;
    int16_t screen_h;
} map_view_t;

// Q16 affine: screen = (a*fx + b*fy + e) >> 16, (c*fx + d*fy + f) >> 16
typedef struct {
    int32_t a, b, c, d;
    int64_t e, f;
} map_affine_t;

// ---- scalar projection ---------------------------------------------------

double merc_lon_to_wx(double lon_deg, int zoom);
double merc_lat_to_wy(double lat_deg, int zoom);
double merc_wx_to_lon(double wx, int zoom);
double merc_wy_to_lat(double wy, int zoom);

// Ground metres per world pixel at a given latitude and zoom.
double merc_meters_per_px(double lat_deg, int zoom);

// ---- view helpers --------------------------------------------------------

void map_view_init(map_view_t *v, int16_t w, int16_t h);
void map_view_set_center(map_view_t *v, double lat_deg, double lon_deg);
void map_view_get_center(const map_view_t *v, double *lat_deg, double *lon_deg);
void map_view_set_zoom(map_view_t *v, uint8_t display_zoom);

// Pan by a screen-space delta, honouring current rotation.
void map_view_pan_px(map_view_t *v, int dx, int dy);

// Data zoom for a given display zoom, per PROTOCOL section 7.
uint8_t map_data_zoom_for(uint8_t display_zoom);

// ---- transforms ----------------------------------------------------------

// Build the affine mapping tile-local coords (0..extent) of tile (z,x,y) to
// screen pixels under the given view.
void map_affine_for_tile(const map_view_t *v, uint8_t z, uint32_t x, uint32_t y,
                         uint16_t extent, map_affine_t *out);

// Build the affine mapping world pixels at display_zoom to screen pixels.
// Used for the route polyline and position marker, which are not tile-local.
void map_affine_for_world(const map_view_t *v, map_affine_t *out);

// Same, but for coordinates stored at a fixed reference zoom. Lets the route
// be projected once on arrival and merely transformed each frame.
void map_affine_for_world_zoom(const map_view_t *v, uint8_t ref_zoom,
                               map_affine_t *out);

static inline void map_affine_apply(const map_affine_t *m, int32_t fx, int32_t fy,
                                    int32_t *sx, int32_t *sy)
{
    *sx = (int32_t)((((int64_t)m->a * fx) + ((int64_t)m->b * fy) + m->e) >> 16);
    *sy = (int32_t)((((int64_t)m->c * fx) + ((int64_t)m->d * fy) + m->f) >> 16);
}

// Project a lat/lon straight to screen. Convenience for markers; do not use
// per-point on bulk geometry.
void map_latlon_to_screen(const map_view_t *v, double lat_deg, double lon_deg,
                          int32_t *sx, int32_t *sy);

void map_screen_to_latlon(const map_view_t *v, int32_t sx, int32_t sy,
                          double *lat_deg, double *lon_deg);

// ---- tile coverage -------------------------------------------------------

#define MAP_MAX_VISIBLE_TILES 16

typedef struct {
    uint8_t  z;
    uint32_t x;
    uint32_t y;
} tile_id_t;

static inline bool tile_id_eq(const tile_id_t *a, const tile_id_t *b)
{
    return a->z == b->z && a->x == b->x && a->y == b->y;
}

// Fill `out` with the tiles at v->data_zoom whose extent intersects the
// (rotated) screen rectangle, plus `margin_px` of slack. Returns the count,
// capped at MAP_MAX_VISIBLE_TILES.
int map_visible_tiles(const map_view_t *v, int margin_px,
                      tile_id_t *out, int max_out);

#ifdef __cplusplus
}
#endif
