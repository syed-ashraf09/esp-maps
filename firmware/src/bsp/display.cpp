// Panel bring-up via Arduino_GFX.
//
// This is deliberately the same driver stack Waveshare's own demos for this
// board use (Arduino_ESP32QSPI + Arduino_CO5300), so the init sequence is
// proven rather than reconstructed.
//
// Arduino_Canvas owns a full-frame PSRAM buffer and hands us the raw pointer,
// which is exactly what the rasteriser wants - we draw into it directly and
// flush once per frame. No LVGL, no partial-update bookkeeping.

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

#include "display.h"
#include "board_pins.h"

static Arduino_ESP32QSPI *g_qspi;
static Arduino_DataBus   *g_bus;
static Arduino_GFX       *g_panel;
static Arduino_Canvas    *g_canvas;
static rgb565_t          *g_fb;

extern "C" {

void bsp_i2c_scan(void)
{
    Serial.printf("[display] I2C scan on SDA=%d SCL=%d:\n",
                  (int)BSP_PIN_I2C_SDA, (int)BSP_PIN_I2C_SCL);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() != 0) continue;

        const char *who = "?";
        switch (addr) {
        case BSP_I2C_ADDR_FT3168:   who = "FT3168 touch (Rev A)"; break;
        case BSP_I2C_ADDR_CST816:   who = "CST816 touch (Rev B)"; break;
        case BSP_I2C_ADDR_XCA9554:  who = "XCA9554/TCA9554 expander (Rev B)"; break;
        case BSP_I2C_ADDR_QMI8658:  who = "QMI8658 IMU"; break;
        case BSP_I2C_ADDR_PCF85063: who = "PCF85063 RTC"; break;
        case BSP_I2C_ADDR_AXP2101:  who = "AXP2101 PMU"; break;
        case BSP_I2C_ADDR_ES8311:   who = "ES8311 audio codec"; break;
        }
        Serial.printf("[display]   0x%02X  %s\n", addr, who);
        found++;
    }
    if (!found)
        Serial.println("[display]   nothing responded - check SDA/SCL in board_pins.h");
}

#if BSP_HAS_EXPANDER && ESPMAPS_EXPANDER_TOUCH
// Rev B may gate panel power and both reset lines through the TCA9554. The panel
// will not come up at all until this runs.
static bool expander_power_up(void)
{
    Wire.beginTransmission(BSP_I2C_ADDR_XCA9554);
    if (Wire.endTransmission() != 0) {
        Serial.printf("[display] expander absent at 0x%02X - is this a Rev A board?\n",
                      BSP_I2C_ADDR_XCA9554);
        return false;
    }

    // Drive EVERY pin high, not a hand-picked subset.
    //
    // Waveshare's own BSP never touches this expander when bringing up the
    // panel: a TCA9554 powers up with all pins as high-impedance inputs held
    // high by external pull-ups, which is already "all resets released". So
    // the only way to get this wrong is to claim the pins as outputs and then
    // drive some of them low - which is exactly what an earlier version of
    // this function did, holding the panel dead while every command still
    // reported success.
    //
    // Set ESPMAPS_EXPANDER_TOUCH=0 to skip this entirely and rely on the
    // power-on state, matching the vendor BSP.

    // Reset pulse: all low briefly...
    Wire.beginTransmission(BSP_I2C_ADDR_XCA9554);
    Wire.write(0x01);          // output port
    Wire.write(0x00);
    Wire.endTransmission();

    // Config register 0x03: 0 = output. Claim the pins only after the output
    // latch holds a known value.
    Wire.beginTransmission(BSP_I2C_ADDR_XCA9554);
    Wire.write(0x03);          // configuration
    Wire.write(0x00);          // all outputs
    Wire.endTransmission();
    delay(20);

    // ...then release everything.
    Wire.beginTransmission(BSP_I2C_ADDR_XCA9554);
    Wire.write(0x01);
    Wire.write(0xFF);          // every rail enabled, every reset released
    Wire.endTransmission();
    delay(150);

    Serial.println("[display] expander 0x20: all outputs driven high");
    return true;
}
#endif

#if ESPMAPS_VENDOR_INIT
// Waveshare's own init sequence for this panel, from lcd_init_cmds[] in
// Waveshare-ESP32-components/bsp/esp32_s3_touch_amoled_1_8.
//
// Their BSP passes this as a custom co5300_vendor_config rather than relying
// on the driver default, which is a strong hint that the generic sequence in
// Arduino_GFX is not sufficient for this particular panel. Notable
// differences: sleep-out comes AFTER the configuration writes rather than
// before, brightness is 0xFF rather than 0xD0, and 0x63 (HBM brightness) and
// the address windows are set explicitly.
//
// Applied after Arduino_GFX's own begin(), so it overrides rather than
// replaces - the panel ends up in the state the vendor ships.
static void panel_apply_vendor_init(void)
{
    if (!g_qspi) return;

    g_qspi->beginWrite();
    g_qspi->writeC8D8(0xFE, 0x00);   // page select 0
    g_qspi->writeC8D8(0xC4, 0x80);   // SPI/QSPI mode control
    g_qspi->writeC8D8(0x3A, 0x55);   // pixel format, 16 bpp
    g_qspi->writeC8D8(0x35, 0x00);   // tearing effect on
    g_qspi->writeC8D8(0x53, 0x20);   // write CTRL display
    g_qspi->writeC8D8(0x51, 0xFF);   // brightness, full
    g_qspi->writeC8D8(0x63, 0xFF);   // HBM brightness, full

    uint8_t caset[4] = { 0x00, 0x00, 0x01, 0x6F };  // columns 0..367
    uint8_t raset[4] = { 0x00, 0x00, 0x01, 0xBF };  // rows    0..447
    g_qspi->writeC8Bytes(0x2A, caset, 4);
    g_qspi->writeC8Bytes(0x2B, raset, 4);
    g_qspi->endWrite();

    g_qspi->beginWrite();
    g_qspi->writeCommand(0x11);      // sleep out
    g_qspi->endWrite();
    delay(100);

    g_qspi->beginWrite();
    g_qspi->writeCommand(0x29);      // display on
    g_qspi->endWrite();
    delay(20);

    Serial.println("[display] applied Waveshare vendor init sequence");
}
#endif

bool display_init(void)
{
    Wire.begin((int)BSP_PIN_I2C_SDA, (int)BSP_PIN_I2C_SCL, BSP_I2C_HZ);

    // Release the expander BEFORE scanning. The touch controller's reset line
    // runs through it, so a scan done first reports no touch chip at all -
    // which looks like a hardware fault and is really just ordering.
#if BSP_HAS_EXPANDER && ESPMAPS_EXPANDER_TOUCH
    expander_power_up();
#elif BSP_HAS_EXPANDER
    Serial.println("[display] expander left at power-on state (untouched)");
#endif

    bsp_i2c_scan();

    Serial.printf("[display] QSPI CS=%d CLK=%d D0..3=%d,%d,%d,%d @ %d MHz\n",
                  (int)BSP_PIN_LCD_CS, (int)BSP_PIN_LCD_PCLK,
                  (int)BSP_PIN_LCD_D0, (int)BSP_PIN_LCD_D1,
                  (int)BSP_PIN_LCD_D2, (int)BSP_PIN_LCD_D3,
                  BSP_LCD_PCLK_HZ / 1000000);

    g_qspi = new Arduino_ESP32QSPI(
        (int8_t)BSP_PIN_LCD_CS, (int8_t)BSP_PIN_LCD_PCLK,
        (int8_t)BSP_PIN_LCD_D0, (int8_t)BSP_PIN_LCD_D1,
        (int8_t)BSP_PIN_LCD_D2, (int8_t)BSP_PIN_LCD_D3);
    g_bus = g_qspi;

    // Signature (both panels, Arduino_GFX 1.6.x):
    //   (bus, rst, rotation, w, h, col_offset1, row_offset1,
    //    col_offset2, row_offset2)
    // Note there is no `ips` argument here, unlike the TFT classes.
    //
    // col_offset1 is why the image would otherwise sit shifted with a garbage
    // strip down one edge: the visible area does not start at the
    // controller's column 0 on Rev B.
#if ESPMAPS_BOARD_REV == ESPMAPS_REV_A
    g_panel = new Arduino_SH8601(g_bus, GFX_NOT_DEFINED /* RST */, 0 /* rot */,
                                 BSP_LCD_H_RES, BSP_LCD_V_RES,
                                 BSP_LCD_X_GAP, 0, 0, 0);
#else
    g_panel = new Arduino_CO5300(g_bus, GFX_NOT_DEFINED /* RST */, 0 /* rot */,
                                 BSP_LCD_H_RES, BSP_LCD_V_RES,
                                 BSP_LCD_X_GAP, 0, 0, 0);
#endif

    // Canvas allocates the framebuffer in PSRAM and gives us the pointer.
    g_canvas = new Arduino_Canvas(BSP_LCD_H_RES, BSP_LCD_V_RES, g_panel);
    if (!g_canvas->begin(BSP_LCD_PCLK_HZ)) {
        Serial.println("[display] canvas begin() failed - check pins and PSRAM");
        return false;
    }

    g_fb = (rgb565_t *)g_canvas->getFramebuffer();
    if (!g_fb) {
        Serial.println("[display] no framebuffer - PSRAM not enabled?");
        return false;
    }

    Serial.printf("[display] panel up: %dx%d, fb %u KB at %p\n",
                  BSP_LCD_H_RES, BSP_LCD_V_RES,
                  (unsigned)(BSP_LCD_H_RES * BSP_LCD_V_RES * 2 / 1024), g_fb);

#if ESPMAPS_VENDOR_INIT
    panel_apply_vendor_init();
#endif

    display_set_brightness(255);
    return true;
}

void display_selftest(void)
{
    if (!g_canvas || !g_fb) return;

    // Solid fills straight into the framebuffer, bypassing the whole map
    // renderer. If these show up the panel, QSPI bus, offsets and flush path
    // are all good and any remaining blackness is the renderer's fault.
    struct { rgb565_t c; const char *name; } steps[] = {
        { 0xF800, "RED"   },
        { 0x07E0, "GREEN" },
        { 0x001F, "BLUE"  },
        { 0xFFFF, "WHITE" },
    };

    const size_t px = (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES;
    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
        for (size_t i = 0; i < px; i++) g_fb[i] = steps[s].c;
        g_canvas->flush();
        Serial.printf("[display] selftest: %s\n", steps[s].name);
        delay(600);
    }
    Serial.println("[display] selftest done - saw 4 colours? panel is fine.");
}

rgb565_t *display_framebuffer(void) { return g_fb; }

void display_flush(void)
{
    if (g_canvas) g_canvas->flush();
}

void display_wait_flush(void)
{
    // Arduino_Canvas::flush() blocks until the transfer is handed off, so
    // there is nothing to wait for. Kept so the call sites read the same as
    // they would with an async driver.
}

void display_set_brightness(uint8_t level)
{
    if (!g_bus) return;
    // MIPI DCS 0x51, honoured by both SH8601 and CO5300. On an AMOLED this is
    // genuine power saving, not a backlight PWM.
    g_bus->beginWrite();
    g_bus->writeC8D8(BSP_LCD_CMD_BRIGHTNESS, level);
    g_bus->endWrite();
}

void display_sleep(bool on)
{
    if (!g_panel) return;
    if (on) g_panel->displayOff();
    else    g_panel->displayOn();
}

}  // extern "C"
