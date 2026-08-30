#pragma once

#include "../render/raster.h"
#include "../map/nav_state.h"
#include "../map/mercator.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct {
    bool show_debug;
    uint32_t frame_us;
    uint32_t tiles_ready;
    uint32_t tiles_pending;
    uint16_t mtu;
    uint32_t rx_kb;
} hud_debug_t;

// Draws the navigation chrome directly over the rendered map.
void hud_draw(surface_t *s, const map_view_t *v, const nav_state_t *nav,
              uint32_t dist_to_maneuver_m, const hud_debug_t *dbg);

#ifdef __cplusplus
}
#endif
