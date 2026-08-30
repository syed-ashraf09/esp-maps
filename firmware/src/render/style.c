#include "style.h"

static style_theme_t g_theme = STYLE_DAY;

// Road widths scale with zoom off a base defined at z17. Q8 multipliers,
// indexed by display zoom - 10. Below z13 minor roads vanish entirely, which
// is what keeps a wide-area view from turning into grey mush.
static const uint16_t zoom_scale_q8[10] = {
//  z10  z11  z12  z13  z14  z15  z16  z17  z18  z19
     77,  90, 102, 115, 141, 179, 218, 256, 333, 435,
};

static const style_def_t g_style[STYLE__THEME_COUNT][SC__COUNT] = {
    // ---- STYLE_DAY -------------------------------------------------------
    [STYLE_DAY] = {
        [SC_BACKGROUND] = {
            .fill = MAP_RGB(0xF2, 0xEF, 0xE9), .casing = 0,
            .base_width = 0, .casing_px = 0, .min_zoom = 0, .is_line = false },
        [SC_LANDUSE_PARK] = {
            .fill = MAP_RGB(0xC9, 0xE3, 0xC1), .casing = 0,
            .base_width = 0, .casing_px = 0, .min_zoom = 12, .is_line = false },
        [SC_LANDUSE_RESIDENTIAL] = {
            .fill = MAP_RGB(0xEC, 0xE8, 0xE1), .casing = 0,
            .base_width = 0, .casing_px = 0, .min_zoom = 13, .is_line = false },
        [SC_LANDUSE_INDUSTRIAL] = {
            .fill = MAP_RGB(0xE6, 0xE1, 0xD8), .casing = 0,
            .base_width = 0, .casing_px = 0, .min_zoom = 13, .is_line = false },
        [SC_WATER] = {
            .fill = MAP_RGB(0xA5, 0xCF, 0xDD), .casing = 0,
            .base_width = 0, .casing_px = 0, .min_zoom = 0, .is_line = false },
        [SC_WATERWAY] = {
            .fill = MAP_RGB(0xA5, 0xCF, 0xDD), .casing = 0,
            .base_width = 2, .casing_px = 0, .min_zoom = 13, .is_line = true },
        [SC_BUILDING] = {
            .fill = MAP_RGB(0xDF, 0xDA, 0xD1), .casing = MAP_RGB(0xCE, 0xC8, 0xBE),
            .base_width = 0, .casing_px = 0, .min_zoom = 16, .is_line = false },

        // Motorways read as orange with a darker casing - the single strongest
        // cue that this is a road map and not a wireframe.
        [SC_ROAD_MOTORWAY] = {
            .fill = MAP_RGB(0xF9, 0xB0, 0x29), .casing = MAP_RGB(0xD9, 0x8B, 0x11),
            .base_width = 9, .casing_px = 2, .min_zoom = 6, .is_line = true },
        [SC_ROAD_TRUNK] = {
            .fill = MAP_RGB(0xFB, 0xC8, 0x4A), .casing = MAP_RGB(0xDA, 0xA2, 0x25),
            .base_width = 8, .casing_px = 2, .min_zoom = 8, .is_line = true },
        [SC_ROAD_PRIMARY] = {
            .fill = MAP_RGB(0xFF, 0xF0, 0xB8), .casing = MAP_RGB(0xD4, 0xC2, 0x8C),
            .base_width = 7, .casing_px = 2, .min_zoom = 10, .is_line = true },
        [SC_ROAD_SECONDARY] = {
            .fill = MAP_RGB(0xFF, 0xFF, 0xFF), .casing = MAP_RGB(0xCB, 0xC6, 0xBC),
            .base_width = 6, .casing_px = 2, .min_zoom = 12, .is_line = true },
        [SC_ROAD_TERTIARY] = {
            .fill = MAP_RGB(0xFF, 0xFF, 0xFF), .casing = MAP_RGB(0xCB, 0xC6, 0xBC),
            .base_width = 5, .casing_px = 1, .min_zoom = 13, .is_line = true },
        [SC_ROAD_MINOR] = {
            .fill = MAP_RGB(0xFF, 0xFF, 0xFF), .casing = MAP_RGB(0xD3, 0xCE, 0xC5),
            .base_width = 4, .casing_px = 1, .min_zoom = 14, .is_line = true },
        [SC_ROAD_SERVICE] = {
            .fill = MAP_RGB(0xFB, 0xFA, 0xF8), .casing = MAP_RGB(0xDA, 0xD5, 0xCC),
            .base_width = 3, .casing_px = 1, .min_zoom = 16, .is_line = true },
        [SC_ROAD_PATH] = {
            .fill = MAP_RGB(0xC0, 0xB4, 0xA4), .casing = 0,
            .base_width = 2, .casing_px = 0, .min_zoom = 16, .is_line = true },
        [SC_RAIL] = {
            .fill = MAP_RGB(0xB5, 0xAF, 0xA6), .casing = 0,
            .base_width = 2, .casing_px = 0, .min_zoom = 13, .is_line = true },
        [SC_BOUNDARY] = {
            .fill = MAP_RGB(0xC4, 0xAE, 0xC4), .casing = 0,
            .base_width = 1, .casing_px = 0, .min_zoom = 8, .is_line = true },
    },

    // ---- STYLE_NIGHT -----------------------------------------------------
    // AMOLED: true black background costs zero power on unlit pixels and is
    // far easier on night vision than a dimmed white map.
    [STYLE_NIGHT] = {
        [SC_BACKGROUND] = {
            .fill = MAP_RGB(0x0B, 0x0D, 0x12), .casing = 0,
            .base_width = 0, .casing_px = 0, .min_zoom = 0, .is_line = false },
        [SC_LANDUSE_PARK] = {
            .fill = MAP_RGB(0x12, 0x1E, 0x16), .casing = 0,
            .base_width = 0, .casing_px = 0, .min_zoom = 12, .is_line = false },
        [SC_LANDUSE_RESIDENTIAL] = {
            .fill = MAP_RGB(0x14, 0x16, 0x1C), .casing = 0,
            .base_width = 0, .casing_px = 0, .min_zoom = 13, .is_line = false },
        [SC_LANDUSE_INDUSTRIAL] = {
            .fill = MAP_RGB(0x17, 0x18, 0x1E), .casing = 0,
            .base_width = 0, .casing_px = 0, .min_zoom = 13, .is_line = false },
        [SC_WATER] = {
            .fill = MAP_RGB(0x0E, 0x1D, 0x2E), .casing = 0,
            .base_width = 0, .casing_px = 0, .min_zoom = 0, .is_line = false },
        [SC_WATERWAY] = {
            .fill = MAP_RGB(0x0E, 0x1D, 0x2E), .casing = 0,
            .base_width = 2, .casing_px = 0, .min_zoom = 13, .is_line = true },
        [SC_BUILDING] = {
            .fill = MAP_RGB(0x1A, 0x1C, 0x22), .casing = MAP_RGB(0x24, 0x26, 0x2D),
            .base_width = 0, .casing_px = 0, .min_zoom = 16, .is_line = false },

        [SC_ROAD_MOTORWAY] = {
            .fill = MAP_RGB(0xE8, 0x9A, 0x1C), .casing = MAP_RGB(0x5A, 0x3A, 0x0A),
            .base_width = 9, .casing_px = 2, .min_zoom = 6, .is_line = true },
        [SC_ROAD_TRUNK] = {
            .fill = MAP_RGB(0xC9, 0x92, 0x2E), .casing = MAP_RGB(0x4E, 0x39, 0x12),
            .base_width = 8, .casing_px = 2, .min_zoom = 8, .is_line = true },
        [SC_ROAD_PRIMARY] = {
            .fill = MAP_RGB(0x6E, 0x72, 0x80), .casing = MAP_RGB(0x2A, 0x2D, 0x36),
            .base_width = 7, .casing_px = 2, .min_zoom = 10, .is_line = true },
        [SC_ROAD_SECONDARY] = {
            .fill = MAP_RGB(0x5C, 0x60, 0x6C), .casing = MAP_RGB(0x25, 0x28, 0x30),
            .base_width = 6, .casing_px = 2, .min_zoom = 12, .is_line = true },
        [SC_ROAD_TERTIARY] = {
            .fill = MAP_RGB(0x50, 0x54, 0x5F), .casing = MAP_RGB(0x22, 0x24, 0x2B),
            .base_width = 5, .casing_px = 1, .min_zoom = 13, .is_line = true },
        [SC_ROAD_MINOR] = {
            .fill = MAP_RGB(0x44, 0x48, 0x52), .casing = MAP_RGB(0x1E, 0x20, 0x26),
            .base_width = 4, .casing_px = 1, .min_zoom = 14, .is_line = true },
        [SC_ROAD_SERVICE] = {
            .fill = MAP_RGB(0x36, 0x39, 0x42), .casing = 0,
            .base_width = 3, .casing_px = 0, .min_zoom = 16, .is_line = true },
        [SC_ROAD_PATH] = {
            .fill = MAP_RGB(0x3A, 0x36, 0x30), .casing = 0,
            .base_width = 2, .casing_px = 0, .min_zoom = 16, .is_line = true },
        [SC_RAIL] = {
            .fill = MAP_RGB(0x35, 0x38, 0x3E), .casing = 0,
            .base_width = 2, .casing_px = 0, .min_zoom = 13, .is_line = true },
        [SC_BOUNDARY] = {
            .fill = MAP_RGB(0x4A, 0x3A, 0x4A), .casing = 0,
            .base_width = 1, .casing_px = 0, .min_zoom = 8, .is_line = true },
    },
};

// Areas paint back to front: broad landuse, then water on top of it.
static const enum style_class g_area_order[] = {
    SC_LANDUSE_RESIDENTIAL,
    SC_LANDUSE_INDUSTRIAL,
    SC_LANDUSE_PARK,
    SC_WATER,
    SC_BUILDING,
};

// Roads paint least- to most-important, so motorways end up on top at
// junctions. Both the casing pass and the fill pass walk this same order.
static const enum style_class g_road_order[] = {
    SC_BOUNDARY,
    SC_RAIL,
    SC_WATERWAY,
    SC_ROAD_PATH,
    SC_ROAD_SERVICE,
    SC_ROAD_MINOR,
    SC_ROAD_TERTIARY,
    SC_ROAD_SECONDARY,
    SC_ROAD_PRIMARY,
    SC_ROAD_TRUNK,
    SC_ROAD_MOTORWAY,
};

void style_set_theme(style_theme_t t)
{
    if (t < STYLE__THEME_COUNT) g_theme = t;
}

style_theme_t style_get_theme(void) { return g_theme; }

const style_def_t *style_get(enum style_class c)
{
    if (c >= SC__COUNT) c = SC_BACKGROUND;
    return &g_style[g_theme][c];
}

rgb565_t style_background(void) { return g_style[g_theme][SC_BACKGROUND].fill; }

rgb565_t style_route_fill(void)
{
    return g_theme == STYLE_DAY ? MAP_RGB(0x1A, 0x73, 0xE8)
                                : MAP_RGB(0x4D, 0x9B, 0xFF);
}

rgb565_t style_route_casing(void)
{
    return g_theme == STYLE_DAY ? MAP_RGB(0x0D, 0x47, 0xA1)
                                : MAP_RGB(0x0A, 0x2A, 0x5C);
}

rgb565_t style_puck_fill(void)
{
    return g_theme == STYLE_DAY ? MAP_RGB(0x1A, 0x73, 0xE8)
                                : MAP_RGB(0x62, 0xB0, 0xFF);
}

rgb565_t style_puck_casing(void) { return MAP_RGB(0xFF, 0xFF, 0xFF); }

uint8_t style_width(enum style_class c, uint8_t display_zoom)
{
    const style_def_t *d = style_get(c);
    if (d->base_width == 0) return 0;
    if (display_zoom < d->min_zoom) return 0;

    int idx = (int)display_zoom - 10;
    if (idx < 0) idx = 0;
    if (idx > 9) idx = 9;

    uint32_t w = ((uint32_t)d->base_width * zoom_scale_q8[idx]) >> 8;
    if (w < 1) w = 1;
    if (w > 40) w = 40;
    return (uint8_t)w;
}

uint32_t style_area_order(const enum style_class **out)
{
    *out = g_area_order;
    return sizeof(g_area_order) / sizeof(g_area_order[0]);
}

uint32_t style_road_order(const enum style_class **out)
{
    *out = g_road_order;
    return sizeof(g_road_order) / sizeof(g_road_order[0]);
}
