// Software rasteriser targeting an RGB565 surface.
//
// Everything funnels through raster_span() (a clipped horizontal run), which
// is the only function that touches pixels. That keeps the clipping logic in
// one place and makes the surface easy to retarget later - e.g. to SRAM
// strips instead of a PSRAM full-frame buffer - without touching callers.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef uint16_t rgb565_t;

#define MAP_RGB(r, g, b) \
    ((rgb565_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef struct {
    rgb565_t *px;
    int16_t   w, h;
    // Clip rect in surface coords; x1/y1 exclusive.
    int16_t   clip_x0, clip_y0, clip_x1, clip_y1;
} surface_t;

// Scratch memory for polygon scan conversion. Sized once at startup and
// reused, so the render loop never allocates.
typedef struct {
    void    *mem;
    size_t   size;
    uint32_t max_edges;
} raster_scratch_t;

void surface_init(surface_t *s, rgb565_t *px, int16_t w, int16_t h);
void surface_set_clip(surface_t *s, int16_t x0, int16_t y0, int16_t x1, int16_t y1);
void surface_reset_clip(surface_t *s);
void surface_fill(surface_t *s, rgb565_t c);

bool raster_scratch_init(raster_scratch_t *sc, uint32_t max_edges);
void raster_scratch_free(raster_scratch_t *sc);

// The one primitive: fill [x0, x1) on row y, clipped.
void raster_span(surface_t *s, int32_t y, int32_t x0, int32_t x1, rgb565_t c);

// 1px line, Bresenham, clipped.
void raster_line(surface_t *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                 rgb565_t c);

// Single segment as a filled quad of the given width (>= 1).
void raster_thick_segment(surface_t *s, int32_t x0, int32_t y0,
                          int32_t x1, int32_t y1, int32_t width, rgb565_t c);

// Filled circle - used for round joins and caps, and the position marker.
void raster_disc(surface_t *s, int32_t cx, int32_t cy, int32_t r, rgb565_t c);

void raster_ring(surface_t *s, int32_t cx, int32_t cy, int32_t r,
                 int32_t thickness, rgb565_t c);

// Polyline of screen-space points (interleaved x,y) with round joins.
// This is how every road is drawn.
void raster_polyline(surface_t *s, const int16_t *pts, uint32_t n_pts,
                     int32_t width, rgb565_t c);

// Even-odd scanline fill. `rings` describes one or more closed rings that
// share a fill - exterior first, holes after - matching the MVT convention.
typedef struct {
    const int16_t *pts;  // interleaved x,y in screen space
    uint32_t       n_pts;
} raster_ring_t;

void raster_fill_rings(surface_t *s, raster_scratch_t *sc,
                       const raster_ring_t *rings, uint32_t n_rings, rgb565_t c);

#ifdef __cplusplus
}
#endif
