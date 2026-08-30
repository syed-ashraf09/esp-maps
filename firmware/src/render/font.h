// Minimal 5x7 bitmap font for the HUD. LVGL is overkill for six numbers and
// costs a second full-screen buffer; this draws straight into the map surface.
#pragma once

#include <stdint.h>

#include "raster.h"

#ifdef __cplusplus
extern "C" {
#endif


#define FONT_W 5
#define FONT_H 7

// Width in pixels that `s` will occupy at the given integer scale.
int font_text_width(const char *s, int scale);

void font_draw_char(surface_t *sf, int x, int y, char c, int scale, rgb565_t col);

void font_draw_text(surface_t *sf, int x, int y, const char *s, int scale,
                    rgb565_t col);

// Same, but with a 1px (scaled) outline in `outline` so text stays legible
// over arbitrary map colours.
void font_draw_text_outlined(surface_t *sf, int x, int y, const char *s,
                             int scale, rgb565_t col, rgb565_t outline);

#ifdef __cplusplus
}
#endif
