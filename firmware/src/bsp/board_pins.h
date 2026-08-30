// Board pin map for Waveshare ESP32-S3-Touch-AMOLED-1.8.
//
// Pin values below are taken from Waveshare's own BSP component,
// waveshare/esp32_s3_touch_amoled_1_8 v2.0.3:
//   Waveshare-ESP32-components/bsp/esp32_s3_touch_amoled_1_8/
//     include/bsp/esp32_s3_touch_amoled_1_8.h
// so they are the vendor's numbers, not guesses. Still worth a sanity check
// against your board with the boot-time I2C scan - see docs/BOARD_BRINGUP.md.
//
// Waveshare ships (at least) two silicon revisions under this same product
// name, and they are NOT driver-compatible:
//
//   REV A  - SH8601 display driver, FT3168 touch, no I/O expander
//   REV B  - CO5300 display driver, CST816 touch, XCA9554 I/O expander @ 0x20
//
// The product page advertises Rev A; the current GitHub demo sources
// (waveshareteam/ESP32-S3-Touch-AMOLED-1.8, examples/arduino-v2) instantiate
// Arduino_CO5300 + CST816 + XCA9554, i.e. Rev B. Which one you have depends on
// when you bought it.
//
// Boot the firmware once with CONFIG_ESPMAPS_I2C_SCAN=y and read the console.
// The address list tells you unambiguously which revision is on your desk:
//
//   0x38 present, 0x20 absent  -> REV A (FT3168 touch)
//   0x15 present, 0x20 present -> REV B (CST816 touch + XCA9554 expander)
//
// Then set ESPMAPS_BOARD_REV below to match.
//
// Everything else in this project is pin-agnostic; this file is the only
// place that needs correcting.

#pragma once

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif


#define ESPMAPS_REV_A 1
#define ESPMAPS_REV_B 2

// Whether to drive the Rev B I/O expander at all. Waveshare's own BSP does
// not - the TCA9554 powers up as high-impedance inputs held high by
// pull-ups, which already means "all resets released". Set to 0 to match
// that exactly if the panel misbehaves.
#ifndef ESPMAPS_EXPANDER_TOUCH
#define ESPMAPS_EXPANDER_TOUCH 1
#endif

// Cycle the panel through solid colours at boot, before any map rendering.
// Answers "is the panel alive?" independently of the renderer.
#ifndef ESPMAPS_PANEL_SELFTEST
#define ESPMAPS_PANEL_SELFTEST 1
#endif

// Replay Waveshare's own lcd_init_cmds[] over the top of Arduino_GFX's
// generic CO5300 init. Their BSP ships a custom sequence rather than using
// the driver default, so the generic one may not be enough for this panel.
#ifndef ESPMAPS_VENDOR_INIT
#define ESPMAPS_VENDOR_INIT (ESPMAPS_BOARD_REV == ESPMAPS_REV_B)
#endif

#ifndef ESPMAPS_BOARD_REV
#define ESPMAPS_BOARD_REV ESPMAPS_REV_B
#endif

// ---- panel geometry (identical on both revisions) ------------------------

#define BSP_LCD_H_RES 368
#define BSP_LCD_V_RES 448
#define BSP_LCD_BPP   16  // RGB565

// SH8601 and CO5300 both expose brightness over MIPI command 0x51.
#define BSP_LCD_CMD_BRIGHTNESS 0x51

// ---- QSPI display bus ----------------------------------------------------

#define BSP_LCD_QSPI_HOST SPI2_HOST

#define BSP_PIN_LCD_CS    GPIO_NUM_12
#define BSP_PIN_LCD_PCLK  GPIO_NUM_11
#define BSP_PIN_LCD_D0    GPIO_NUM_4
#define BSP_PIN_LCD_D1    GPIO_NUM_5
#define BSP_PIN_LCD_D2    GPIO_NUM_6
#define BSP_PIN_LCD_D3    GPIO_NUM_7
#define BSP_PIN_LCD_RST   GPIO_NUM_NC  // Rev B drives reset via the expander
#define BSP_PIN_LCD_TE    GPIO_NUM_NC  // tearing-effect line, optional

// 40 MHz is Arduino_GFX's own default for this bus (ESP32QSPI_FREQUENCY) and
// what the vendor drivers use. An earlier version of this file forced 80 MHz
// on the assumption it was headroom; if the panel will not light, that is one
// of the first things to suspect. Raise it only once the display works.
#define BSP_LCD_PCLK_HZ (40 * 1000 * 1000)

// ---- shared I2C bus (touch, IMU, RTC, PMU, expander) ---------------------

#define BSP_I2C_PORT    I2C_NUM_0
#define BSP_PIN_I2C_SDA GPIO_NUM_15
#define BSP_PIN_I2C_SCL GPIO_NUM_14
#define BSP_I2C_HZ      (400 * 1000)

#define BSP_I2C_ADDR_FT3168  0x38
#define BSP_I2C_ADDR_CST816  0x15
#define BSP_I2C_ADDR_XCA9554 0x20
#define BSP_I2C_ADDR_QMI8658 0x6B
#define BSP_I2C_ADDR_PCF85063 0x51
#define BSP_I2C_ADDR_AXP2101 0x34
#define BSP_I2C_ADDR_ES8311  0x18

#define BSP_PIN_TP_INT GPIO_NUM_21
#define BSP_PIN_TP_RST GPIO_NUM_NC  // via expander on Rev B

#if ESPMAPS_BOARD_REV == ESPMAPS_REV_A
#define BSP_TOUCH_ADDR   BSP_I2C_ADDR_FT3168
#define BSP_HAS_EXPANDER 0
#define BSP_LCD_X_GAP    0
#else
#define BSP_TOUCH_ADDR   BSP_I2C_ADDR_CST816
#define BSP_HAS_EXPANDER 1
// The CO5300 panel on Rev B starts 16 columns into the controller's address
// space. Miss this and the whole image sits 16 px off with a garbage strip
// down one edge. Waveshare's BSP applies the same 0x10 offset.
#define BSP_LCD_X_GAP    0x10
// TCA9554 output bits: pin 7 gates panel power, pins 0..2 are display reset /
// touch reset / power-enable.
#define BSP_EXP_BIT_LCD_RST  0
#define BSP_EXP_BIT_TP_RST   1
#define BSP_EXP_BIT_PWR_EN   2
#define BSP_EXP_BIT_PANEL_ON 7
#endif

// ---- microSD (SDMMC 1-bit; the demos use SD_MMC, not SPI) ----------------

#define BSP_PIN_SD_CLK  GPIO_NUM_2
#define BSP_PIN_SD_CMD  GPIO_NUM_1
#define BSP_PIN_SD_D0   GPIO_NUM_3
#define BSP_SD_MOUNT    "/sdcard"

// ---- audio (unused by esp-maps, listed for completeness) ------------------
// Kept here so a future turn-by-turn voice prompt has somewhere to start.

#define BSP_PIN_I2S_MCLK  GPIO_NUM_16
#define BSP_PIN_I2S_BCLK  GPIO_NUM_9
#define BSP_PIN_I2S_WS    GPIO_NUM_45
#define BSP_PIN_I2S_DOUT  GPIO_NUM_8
#define BSP_PIN_I2S_DIN   GPIO_NUM_10
#define BSP_PIN_AUDIO_PA  GPIO_NUM_46

#ifdef __cplusplus
}
#endif
