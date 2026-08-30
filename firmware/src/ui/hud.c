#include "hud.h"

#include <stdio.h>
#include <string.h>

#include "../render/font.h"
#include "../render/style.h"

#define PANEL_BG      MAP_RGB(0x18, 0x1B, 0x22)
#define PANEL_FG      MAP_RGB(0xFF, 0xFF, 0xFF)
#define PANEL_DIM     MAP_RGB(0x9A, 0xA2, 0xB0)
#define ACCENT        MAP_RGB(0x2E, 0xC4, 0x6B)
#define WARN          MAP_RGB(0xE5, 0x53, 0x4E)

static void fill_rect(surface_t *s, int x, int y, int w, int h, rgb565_t c)
{
    for (int i = 0; i < h; i++) raster_span(s, y + i, x, x + w, c);
}

// Rounded-ish panel: square body with the corner pixels shaved.
static void panel(surface_t *s, int x, int y, int w, int h, rgb565_t c)
{
    fill_rect(s, x + 2, y, w - 4, h, c);
    fill_rect(s, x, y + 2, w, h - 4, c);
    fill_rect(s, x + 1, y + 1, w - 2, h - 2, c);
}

static void fmt_distance(char *buf, size_t n, uint32_t m)
{
    if (m < 1000) snprintf(buf, n, "%lu m", (unsigned long)m);
    else if (m < 10000) snprintf(buf, n, "%lu.%lu km",
                                 (unsigned long)(m / 1000),
                                 (unsigned long)((m % 1000) / 100));
    else snprintf(buf, n, "%lu km", (unsigned long)(m / 1000));
}

static void fmt_duration(char *buf, size_t n, uint32_t s)
{
    uint32_t mins = s / 60;
    if (mins < 60) snprintf(buf, n, "%lu min", (unsigned long)mins);
    else snprintf(buf, n, "%luh %02lu", (unsigned long)(mins / 60),
                  (unsigned long)(mins % 60));
}

// ---- maneuver arrows -----------------------------------------------------

// Drawn from primitives rather than bitmaps: at 3 shapes x 2 mirrors plus a
// few specials, glyph data would cost more than the code.
static void draw_arrow(surface_t *s, int cx, int cy, int size, uint8_t kind,
                       rgb565_t col)
{
    int h = size / 2;
    int stem_y = cy + h;

    switch (kind) {
    case MANEUVER_LEFT:
    case MANEUVER_SHARP_LEFT:
    case MANEUVER_SLIGHT_LEFT:
    case MANEUVER_FORK_LEFT:
    case MANEUVER_RIGHT:
    case MANEUVER_SHARP_RIGHT:
    case MANEUVER_SLIGHT_RIGHT:
    case MANEUVER_FORK_RIGHT: {
        bool left = (kind == MANEUVER_LEFT || kind == MANEUVER_SHARP_LEFT ||
                     kind == MANEUVER_SLIGHT_LEFT || kind == MANEUVER_FORK_LEFT);
        int dir = left ? -1 : 1;

        // Stem up from the bottom, then a turn across, then a head.
        raster_thick_segment(s, cx, stem_y, cx, cy, size / 4, col);
        raster_thick_segment(s, cx, cy, cx + dir * h, cy, size / 4, col);

        int tipx = cx + dir * (h + size / 4);
        for (int i = 0; i < size / 3; i++) {
            raster_span(s, cy - size / 3 + i + 1,
                        left ? tipx + i : cx + h - i,
                        left ? cx + h + 1 : tipx - i + 1, col);
        }
        break;
    }

    case MANEUVER_UTURN:
        raster_thick_segment(s, cx + h / 2, stem_y, cx + h / 2, cy, size / 4, col);
        raster_ring(s, cx - h / 4, cy, h / 2 + size / 8, size / 4, col);
        raster_thick_segment(s, cx - h, cy, cx - h, cy + h / 2, size / 4, col);
        break;

    case MANEUVER_ROUNDABOUT:
        raster_ring(s, cx, cy, h, size / 5, col);
        raster_thick_segment(s, cx, stem_y, cx, cy + h, size / 4, col);
        break;

    case MANEUVER_ARRIVE:
        raster_disc(s, cx, cy, h - 2, col);
        raster_disc(s, cx, cy, h / 3, PANEL_BG);
        break;

    default:  // straight / depart / merge
        raster_thick_segment(s, cx, stem_y, cx, cy - h / 2, size / 4, col);
        for (int i = 0; i < size / 3; i++) {
            raster_span(s, cy - h / 2 + i, cx - size / 3 + i, cx + size / 3 - i + 1, col);
        }
        break;
    }
}

// ---- panels --------------------------------------------------------------

static void draw_turn_banner(surface_t *s, const nav_state_t *nav, uint32_t dist_m)
{
    const int H = 74;
    panel(s, 0, 0, s->w, H, PANEL_BG);

    const nav_maneuver_t *m = &nav->route.next_maneuver;
    draw_arrow(s, 40, H / 2, 44, m->kind, PANEL_FG);

    char buf[32];
    fmt_distance(buf, sizeof(buf), dist_m ? dist_m : m->dist_m);
    font_draw_text(s, 80, 12, buf, 3, PANEL_FG);

    if (m->text[0]) {
        // Truncate rather than wrap; a second line would crowd the map.
        char name[26];
        strncpy(name, m->text, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        while (font_text_width(name, 2) > s->w - 88 && strlen(name) > 3)
            name[strlen(name) - 1] = '\0';
        font_draw_text(s, 80, 46, name, 2, PANEL_DIM);
    }
}

static void draw_bottom_bar(surface_t *s, const nav_state_t *nav)
{
    const int H = 46;
    int y = s->h - H;
    panel(s, 0, y, s->w, H, PANEL_BG);

    char buf[32];

    // Speed, left.
    int kph = (int)(nav->fix.speed_mps * 3.6f + 0.5f);
    snprintf(buf, sizeof(buf), "%d", nav->fix.valid ? kph : 0);
    font_draw_text(s, 10, y + 10, buf, 4, PANEL_FG);
    font_draw_text(s, 10 + font_text_width(buf, 4) + 6, y + 22, "km/h", 1, PANEL_DIM);

    // Remaining distance and ETA, right.
    if (nav->route.has_route) {
        fmt_distance(buf, sizeof(buf), nav->route.remain_m);
        int w = font_text_width(buf, 2);
        font_draw_text(s, s->w - w - 10, y + 8, buf, 2, PANEL_FG);

        fmt_duration(buf, sizeof(buf), nav->route.remain_s);
        w = font_text_width(buf, 2);
        font_draw_text(s, s->w - w - 10, y + 26, buf, 2, ACCENT);
    }
}

static void draw_link_pip(surface_t *s, const nav_state_t *nav, int y)
{
    rgb565_t c = nav->link_up ? (nav->fix.valid ? ACCENT : MAP_RGB(0xE0, 0xA8, 0x30))
                              : WARN;
    raster_disc(s, s->w - 14, y, 5, c);
}

static void draw_debug(surface_t *s, const hud_debug_t *d, int y)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%lu.%lums %lut/%lup mtu%u %lukB",
             (unsigned long)(d->frame_us / 1000),
             (unsigned long)((d->frame_us % 1000) / 100),
             (unsigned long)d->tiles_ready,
             (unsigned long)d->tiles_pending,
             d->mtu,
             (unsigned long)d->rx_kb);
    font_draw_text_outlined(s, 6, y, buf, 1, PANEL_FG, MAP_RGB(0, 0, 0));
}

void hud_draw(surface_t *s, const map_view_t *v, const nav_state_t *nav,
              uint32_t dist_to_maneuver_m, const hud_debug_t *dbg)
{
    (void)v;
    surface_reset_clip(s);

    int pip_y = 14;

    if (nav->route.has_route && nav->route.has_maneuver) {
        draw_turn_banner(s, nav, dist_to_maneuver_m);
        pip_y = 88;
    }

    draw_bottom_bar(s, nav);
    draw_link_pip(s, nav, pip_y);

    if (dbg && dbg->show_debug) draw_debug(s, dbg, pip_y + 14);
}
