// Cartographic style: which class draws in what colour, at what width, and in
// what order. The style_class values here are the wire enum from
// docs/PROTOCOL.md section 5.2 and must stay in sync with TileEncoder.kt.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "raster.h"

#ifdef __cplusplus
extern "C" {
#endif


enum style_class {
    SC_BACKGROUND = 0,
    SC_LANDUSE_PARK,
    SC_LANDUSE_RESIDENTIAL,
    SC_LANDUSE_INDUSTRIAL,
    SC_WATER,
    SC_WATERWAY,
    SC_BUILDING,
    SC_ROAD_MOTORWAY,
    SC_ROAD_TRUNK,
    SC_ROAD_PRIMARY,
    SC_ROAD_SECONDARY,
    SC_ROAD_TERTIARY,
    SC_ROAD_MINOR,
    SC_ROAD_SERVICE,
    SC_ROAD_PATH,
    SC_RAIL,
    SC_BOUNDARY,
    SC__COUNT,
};

typedef enum {
    STYLE_DAY = 0,
    STYLE_NIGHT,
    STYLE__THEME_COUNT,
} style_theme_t;

typedef struct {
    rgb565_t fill;
    rgb565_t casing;
    uint8_t  base_width;  // px at display zoom 17
    uint8_t  casing_px;   // extra width each side for the outline pass
    uint8_t  min_zoom;    // hidden below this display zoom
    bool     is_line;     // false = area fill
} style_def_t;

void style_set_theme(style_theme_t t);
style_theme_t style_get_theme(void);

const style_def_t *style_get(enum style_class c);

rgb565_t style_background(void);
rgb565_t style_route_fill(void);
rgb565_t style_route_casing(void);
rgb565_t style_puck_fill(void);
rgb565_t style_puck_casing(void);

// Stroke width in px for a class at a given display zoom, 0 = do not draw.
uint8_t style_width(enum style_class c, uint8_t display_zoom);

// Paint order, back to front. Areas first, then road casings, then road fills.
// Returns the count; `out` receives the class list.
uint32_t style_area_order(const enum style_class **out);
uint32_t style_road_order(const enum style_class **out);

#ifdef __cplusplus
}
#endif
