#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "../render/raster.h"

#ifdef __cplusplus
extern "C" {
#endif


// Brings up the QSPI AMOLED panel and allocates the PSRAM framebuffer.
bool display_init(void);

// The framebuffer the renderer draws into. BSP_LCD_H_RES x BSP_LCD_V_RES,
// RGB565, PSRAM-resident.
rgb565_t *display_framebuffer(void);

// Push the whole framebuffer to the panel. Blocks until the DMA transfer has
// been handed off, not until it completes - call display_wait_flush() before
// touching the buffer again.
void display_flush(void);
void display_wait_flush(void);

// 0..255. AMOLED, so this is real power saving, not a backlight PWM.
void display_set_brightness(uint8_t level);

void display_sleep(bool on);

// Logs every responding I2C address. Use this to identify the board
// revision - see the banner in board_pins.h.
void bsp_i2c_scan(void);

// Cycles the panel through red/green/blue/white, bypassing the map renderer
// entirely. Tells you whether the panel itself is alive.
void display_selftest(void);

#ifdef __cplusplus
}
#endif
