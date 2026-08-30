#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "raster.h"
#include "../map/mercator.h"
#include "../map/nav_state.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct {
    surface_t        surf;
    raster_scratch_t scratch;

    int16_t         *xform;      // transformed screen coords, interleaved x,y
    uint32_t         xform_cap;  // capacity in points

    raster_ring_t   *rings;
    uint32_t         rings_cap;

    // Last frame's cost, for the on-screen debug overlay.
    uint32_t         last_us;
    uint32_t         last_points;
    uint32_t         last_tiles;
} map_renderer_t;

bool map_render_init(map_renderer_t *r, rgb565_t *fb, int16_t w, int16_t h);
void map_render_deinit(map_renderer_t *r);

// Draw one complete frame into the renderer's surface.
void map_render_frame(map_renderer_t *r, const map_view_t *v,
                      const nav_state_t *nav);

#ifdef __cplusplus
}
#endif
