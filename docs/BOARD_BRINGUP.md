# Board bring-up

Do this before your first flash. It takes about five minutes and saves you
debugging a black screen.

## 1. Which revision do you have?

Waveshare sells at least two different boards under the name
**ESP32-S3-Touch-AMOLED-1.8**, and they are not compatible:

| | Rev A | Rev B |
|---|---|---|
| Display driver | SH8601 | CO5300 |
| Touch controller | FT3168 | CST816 |
| I/O expander | none | TCA9554 @ 0x20 |
| Panel reset | direct GPIO | via expander |
| Column offset | 0 | 16 |

The Waveshare product page and wiki describe **Rev A**. The current demo
sources in their GitHub repo (`waveshareteam/ESP32-S3-Touch-AMOLED-1.8`,
`examples/arduino-v2`) instantiate `Arduino_CO5300`, `CST816` and a
`TCA9554` expander — i.e. **Rev B**. Which you have depends on when you bought
it, so check rather than assume.

The build defaults to **Rev B**, set in `firmware/platformio.ini`:

```ini
build_flags =
    -DESPMAPS_BOARD_REV=2   ; 2 = Rev B (CO5300), 1 = Rev A (SH8601)
```

### Identify it empirically

Flash and watch the serial monitor. `display_init()` runs an I2C scan before
touching the panel and prints every responding address:

```
[display] I2C scan on SDA=15 SCL=14:
[display]   0x15  CST816 touch (Rev B)
[display]   0x20  XCA9554/TCA9554 expander (Rev B)
[display]   0x34  AXP2101 PMU
[display]   0x51  PCF85063 RTC
[display]   0x6B  QMI8658 IMU
```

Read it as:

- `0x38` present, `0x20` absent → **Rev A**, set `-DESPMAPS_BOARD_REV=1`
- `0x15` and `0x20` present → **Rev B**, the default
- **nothing at all** → your I2C pins are wrong, fix those first (step 2)

## 2. The pin numbers

`firmware/src/bsp/board_pins.h` is the only file in the project with GPIO
numbers in it. Everything else is pin-agnostic.

The values come from Waveshare's own BSP component
(`waveshare/esp32_s3_touch_amoled_1_8` v2.0.3), specifically
`bsp/esp32_s3_touch_amoled_1_8/include/bsp/esp32_s3_touch_amoled_1_8.h` in
[Waveshare-ESP32-components](https://github.com/waveshareteam/Waveshare-ESP32-components).
These are the vendor's numbers:

| Signal | GPIO |
|---|---|
| LCD CS / PCLK | 12 / 11 |
| LCD D0–D3 | 4, 5, 6, 7 |
| LCD RST | not connected (via expander on Rev B) |
| I2C SDA / SCL | **15 / 14** |
| Touch INT / RST | 21 / not connected |
| SD CLK / CMD / D0 | 2 / 1 / 3 |

Note the I2C pins — 15 and 14, not the 47/48 that most ESP32-S3 boards use.

### The CO5300 column offset

On Rev B the visible area starts 16 columns into the controller's address
space. That is passed as the `col_offset1` argument to the `Arduino_CO5300`
constructor in `src/bsp/display.cpp`, from `BSP_LCD_X_GAP`. Without it the
whole image sits 16 px off with a garbage strip down one edge. Waveshare's
own demo passes the same 16.

## 3. Expected first boot

```
[esp-maps] starting, board rev B (CO5300/CST816)
[esp-maps] boot: internal 340 KB free, PSRAM 8192 KB free
[display] I2C scan on SDA=15 SCL=14:
...
[display] QSPI CS=12 CLK=11 D0..3=4,5,6,7 @ 80 MHz
[display] panel up: 368x448, fb 322 KB at 0x3c...
[tile_cache] cache: 96 slots, 5120 KB budget
[ble] advertising as "espmaps"
[esp-maps] after init: internal 210 KB free, PSRAM 2600 KB free
[esp-maps] up. advertising as "espmaps" - connect the phone app.
```

You should see a blank map (background colour, no roads) with the HUD bar
along the bottom and a red link pip top-right. That is correct with no phone
connected — there is no tile data yet.

## 4. Troubleshooting

**`fatal error: esp32-hal-periman.h: No such file or directory`.** You are
building against Arduino-ESP32 core 2.x. That header arrived in core 3.0, and
Arduino_GFX 1.6.x needs it. PlatformIO's *official* `espressif32` platform
still ships core 2.0.17, which is why `platformio.ini` points at the
**pioarduino** fork instead:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
```

If you replaced that line with a bare `platform = espressif32`, this is the
error you get.

**`no matching function for call to Arduino_CO5300::Arduino_CO5300(...)`.**
The constructor in Arduino_GFX 1.6.x is
`(bus, rst, rotation, w, h, col_offset1, row_offset1, col_offset2, row_offset2)`
— note there is **no `ips` argument**, unlike the TFT classes. Passing one
silently binds to `w` and sets the panel width to 0.

**PSRAM shows ~0 or 2 MB instead of 8 MB.** `board_build.arduino.memory_type`
is not `qio_opi`. This is the most consequential thing in the whole build:
the R8 part is *octal* PSRAM, and in quad mode the framebuffer and tile cache
run at roughly a quarter of the bandwidth. `setup()` warns about this
explicitly. Run `py -m platformio run -t clean` after changing it.

**`canvas begin() failed`.** Either the QSPI pins are wrong, or PSRAM is not
available so the 322 KB framebuffer could not be allocated. Check the PSRAM
line printed just above it.

**Black screen, no errors in the log.** Brightness starts at 0 in the init
sequence and is raised by `display_set_brightness(200)`. If the panel is alive
but dark, the `0x51` command isn't reaching it — check CS.

**Panel completely dark, but every log line reports success.** Confirmed on
real hardware — this happened during bring-up and had two causes, both fixed
in the current code. If you are adapting this for another board, they are the
first two things to check:

1. **QSPI clock too high.** `BSP_LCD_PCLK_HZ` must be **40 MHz**
   (`ESP32QSPI_FREQUENCY`, Arduino_GFX's own default). An earlier version
   forced 80 MHz on the assumption it was free headroom; at double the rated
   clock the panel latches nothing, while every write still "succeeds" on the
   ESP32 side. Perfectly healthy log, dead screen.
2. **The generic CO5300 init is not sufficient.** Waveshare's BSP ships a
   custom `lcd_init_cmds[]`, and `panel_apply_vendor_init()` in
   `src/bsp/display.cpp` replays it over the top of Arduino_GFX's. Theirs
   differs in ordering (sleep-out *after* the config writes, not before),
   brightness (`0xFF` not `0xD0`), and sets `0x63` and both address windows
   explicitly. Disable with `-DESPMAPS_VENDOR_INIT=0` to isolate.

A third cause was ruled out but is worth knowing: driving the TCA9554
expander's pins as outputs and pulling a subset low holds the panel dead. The
vendor BSP never touches the expander at all — it powers up as
high-impedance inputs held high, which already means "all resets released".
`ESPMAPS_EXPANDER_TOUCH=0` reproduces that exactly.

**Garbled or torn rows.** Drop `BSP_LCD_PCLK_HZ` further, to 20 MHz.

**Image shifted sideways, garbage strip down one edge.** The column offset is
wrong for your revision. Rev B needs `BSP_LCD_X_GAP` of `0x10`; Rev A needs
`0`. Check `-DESPMAPS_BOARD_REV` matches what the I2C scan told you.

**Compile error on the `Arduino_CO5300` constructor.** Arduino_GFX has changed
this signature between versions — some take an `ips` bool, some don't. Match
`src/bsp/display.cpp` to the constructor in your installed
`Arduino_CO5300.h`; the offsets are the trailing four arguments.

**Frame times above 60 ms.** Almost always PSRAM in quad mode. Same fix as
the first entry.
