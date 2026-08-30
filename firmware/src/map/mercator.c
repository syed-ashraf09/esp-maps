#include "mercator.h"

#include <math.h>
#include <string.h>

#define MERC_MAX_LAT 85.05112877980659  // atan(sinh(pi)) - the Mercator cutoff
#define EARTH_CIRCUM_M 40075016.686

static inline double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline double world_size(int zoom)
{
    return (double)MERC_TILE_PX * (double)(1u << zoom);
}

// ---- scalar projection ---------------------------------------------------

double merc_lon_to_wx(double lon_deg, int zoom)
{
    lon_deg = clampd(lon_deg, -180.0, 180.0);
    return (lon_deg + 180.0) / 360.0 * world_size(zoom);
}

double merc_lat_to_wy(double lat_deg, int zoom)
{
    lat_deg = clampd(lat_deg, -MERC_MAX_LAT, MERC_MAX_LAT);
    double s = sin(lat_deg * M_PI / 180.0);
    return (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * world_size(zoom);
}

double merc_wx_to_lon(double wx, int zoom)
{
    return wx / world_size(zoom) * 360.0 - 180.0;
}

double merc_wy_to_lat(double wy, int zoom)
{
    double n = M_PI - 2.0 * M_PI * wy / world_size(zoom);
    return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

double merc_meters_per_px(double lat_deg, int zoom)
{
    return EARTH_CIRCUM_M * cos(lat_deg * M_PI / 180.0) / world_size(zoom);
}

// ---- view ----------------------------------------------------------------

void map_view_init(map_view_t *v, int16_t w, int16_t h)
{
    memset(v, 0, sizeof(*v));
    v->screen_w = w;
    v->screen_h = h;
    v->display_zoom = 16;
    v->data_zoom = map_data_zoom_for(16);
    v->rotation_rad = 0.0f;
    map_view_set_center(v, 0.0, 0.0);
}

void map_view_set_center(map_view_t *v, double lat_deg, double lon_deg)
{
    v->center_wx = merc_lon_to_wx(lon_deg, v->display_zoom);
    v->center_wy = merc_lat_to_wy(lat_deg, v->display_zoom);
}

void map_view_get_center(const map_view_t *v, double *lat_deg, double *lon_deg)
{
    if (lat_deg) *lat_deg = merc_wy_to_lat(v->center_wy, v->display_zoom);
    if (lon_deg) *lon_deg = merc_wx_to_lon(v->center_wx, v->display_zoom);
}

void map_view_set_zoom(map_view_t *v, uint8_t display_zoom)
{
    if (display_zoom < 2)  display_zoom = 2;
    if (display_zoom > 19) display_zoom = 19;
    if (display_zoom == v->display_zoom) return;

    // Rescale the centre so the same ground point stays put.
    double lat, lon;
    map_view_get_center(v, &lat, &lon);
    v->display_zoom = display_zoom;
    v->data_zoom = map_data_zoom_for(display_zoom);
    map_view_set_center(v, lat, lon);
}

void map_view_pan_px(map_view_t *v, int dx, int dy)
{
    // Screen delta -> world delta is the inverse rotation.
    float cs = cosf(-v->rotation_rad);
    float sn = sinf(-v->rotation_rad);
    v->center_wx -= (double)(dx * cs - dy * sn);
    v->center_wy -= (double)(dx * sn + dy * cs);

    double ws = world_size(v->display_zoom);
    if (v->center_wx < 0)   v->center_wx += ws;
    if (v->center_wx >= ws) v->center_wx -= ws;
    v->center_wy = clampd(v->center_wy, 0.0, ws);
}

uint8_t map_data_zoom_for(uint8_t display_zoom)
{
    if (display_zoom >= 17) return 14;
    if (display_zoom >= 15) return 13;
    if (display_zoom >= 13) return 12;
    return 10;
}

// ---- transforms ----------------------------------------------------------

// Shared tail: given the world-space origin offset of a coordinate system and
// its per-unit scale, produce the Q16 affine into screen space.
static void build_affine(const map_view_t *v, double ox, double oy, double scale,
                         map_affine_t *out)
{
    double cs = cos(v->rotation_rad);
    double sn = sin(v->rotation_rad);

    out->a = (int32_t)lrint(scale * cs * 65536.0);
    out->b = (int32_t)lrint(-scale * sn * 65536.0);
    out->c = (int32_t)lrint(scale * sn * 65536.0);
    out->d = (int32_t)lrint(scale * cs * 65536.0);

    double ex = ox * cs - oy * sn + v->screen_w * 0.5;
    double ey = ox * sn + oy * cs + v->screen_h * 0.5;

    out->e = (int64_t)llrint(ex * 65536.0);
    out->f = (int64_t)llrint(ey * 65536.0);
}

void map_affine_for_tile(const map_view_t *v, uint8_t z, uint32_t x, uint32_t y,
                         uint16_t extent, map_affine_t *out)
{
    // One tile-local unit spans this many display-zoom world pixels.
    double zdiff = ldexp(1.0, (int)v->display_zoom - (int)z);
    double scale = (double)MERC_TILE_PX * zdiff / (double)extent;

    // World position of the tile's origin corner, relative to the view centre.
    double ox = (double)x * MERC_TILE_PX * zdiff - v->center_wx;
    double oy = (double)y * MERC_TILE_PX * zdiff - v->center_wy;

    build_affine(v, ox, oy, scale, out);
}

void map_affine_for_world(const map_view_t *v, map_affine_t *out)
{
    build_affine(v, -v->center_wx, -v->center_wy, 1.0, out);
}

void map_affine_for_world_zoom(const map_view_t *v, uint8_t ref_zoom,
                               map_affine_t *out)
{
    double scale = ldexp(1.0, (int)v->display_zoom - (int)ref_zoom);
    build_affine(v, -v->center_wx, -v->center_wy, scale, out);
}

void map_latlon_to_screen(const map_view_t *v, double lat_deg, double lon_deg,
                          int32_t *sx, int32_t *sy)
{
    double wx = merc_lon_to_wx(lon_deg, v->display_zoom) - v->center_wx;
    double wy = merc_lat_to_wy(lat_deg, v->display_zoom) - v->center_wy;

    double cs = cos(v->rotation_rad);
    double sn = sin(v->rotation_rad);

    if (sx) *sx = (int32_t)lrint(wx * cs - wy * sn + v->screen_w * 0.5);
    if (sy) *sy = (int32_t)lrint(wx * sn + wy * cs + v->screen_h * 0.5);
}

void map_screen_to_latlon(const map_view_t *v, int32_t sx, int32_t sy,
                          double *lat_deg, double *lon_deg)
{
    double dx = (double)sx - v->screen_w * 0.5;
    double dy = (double)sy - v->screen_h * 0.5;

    double cs = cos(-v->rotation_rad);
    double sn = sin(-v->rotation_rad);

    double wx = v->center_wx + (dx * cs - dy * sn);
    double wy = v->center_wy + (dx * sn + dy * cs);

    if (lat_deg) *lat_deg = merc_wy_to_lat(wy, v->display_zoom);
    if (lon_deg) *lon_deg = merc_wx_to_lon(wx, v->display_zoom);
}

// ---- tile coverage -------------------------------------------------------

int map_visible_tiles(const map_view_t *v, int margin_px,
                      tile_id_t *out, int max_out)
{
    if (max_out <= 0) return 0;

    // Corners of the padded screen rect, un-rotated back into world space.
    double hw = v->screen_w * 0.5 + margin_px;
    double hh = v->screen_h * 0.5 + margin_px;
    double cs = cos(-v->rotation_rad);
    double sn = sin(-v->rotation_rad);

    const double corner[4][2] = {
        { -hw, -hh }, { hw, -hh }, { hw, hh }, { -hw, hh },
    };

    double min_wx = 1e30, max_wx = -1e30, min_wy = 1e30, max_wy = -1e30;
    for (int i = 0; i < 4; i++) {
        double dx = corner[i][0], dy = corner[i][1];
        double wx = v->center_wx + (dx * cs - dy * sn);
        double wy = v->center_wy + (dx * sn + dy * cs);
        if (wx < min_wx) min_wx = wx;
        if (wx > max_wx) max_wx = wx;
        if (wy < min_wy) min_wy = wy;
        if (wy > max_wy) max_wy = wy;
    }

    // Convert display-zoom world px to data-zoom tile indices.
    double zdiff = ldexp(1.0, (int)v->data_zoom - (int)v->display_zoom);
    double tile_span = (double)MERC_TILE_PX / zdiff;  // display px per data tile

    int64_t tx0 = (int64_t)floor(min_wx / tile_span);
    int64_t tx1 = (int64_t)floor(max_wx / tile_span);
    int64_t ty0 = (int64_t)floor(min_wy / tile_span);
    int64_t ty1 = (int64_t)floor(max_wy / tile_span);

    int64_t n = (int64_t)1 << v->data_zoom;
    if (ty0 < 0) ty0 = 0;
    if (ty1 > n - 1) ty1 = n - 1;

    int count = 0;
    for (int64_t ty = ty0; ty <= ty1 && count < max_out; ty++) {
        for (int64_t tx = tx0; tx <= tx1 && count < max_out; tx++) {
            int64_t wrapped = tx % n;      // antimeridian wrap
            if (wrapped < 0) wrapped += n;
            out[count].z = v->data_zoom;
            out[count].x = (uint32_t)wrapped;
            out[count].y = (uint32_t)ty;
            count++;
        }
    }
    return count;
}
