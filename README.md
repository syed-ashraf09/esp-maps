# esp-maps

A real map navigator on a Waveshare ESP32-S3-Touch-AMOLED-1.8 — roads, water,
landuse, and a route line, rendered on-device from vector data streamed over
BLE from an Android phone.

Not turn arrows on a blank screen. An actual map.

---

## Why it works this way

The whole design falls out of one measurement: **BLE moves 20–60 KB/s in
practice.**

| Approach | Bytes per screen | Time over BLE |
|---|---|---|
| Raster map tiles (what a map server actually serves) | 150–200 KB | 3–10 s |
| Vector geometry, drawn on the ESP32 | 10–20 KB | ~0.3 s |

Streaming pictures of a map means the map freezes the moment you start moving.
So the ESP32 draws the map itself, and the phone sends geometry.

Two further multipliers, both on the phone side:

- **Re-encode, don't forward.** A raw OpenMapTiles `.pbf` at z14 is 40–120 KB,
  mostly labels, POIs and attributes a 368×448 nav display will never draw.
  Stripped to road/water/landuse geometry with a one-byte class, the same tile
  is 4–15 KB.
- **Fetch a lower zoom than you display.** One z14 tile covers many screens at
  display zoom 17, so panning is free. At 50 km/h you cross a z14 tile every
  ~2.3 minutes — about 45 KB every couple of minutes, with the ESP32 panning,
  zooming and rotating locally at 20 fps with the radio idle.

## Layout

```
docs/PROTOCOL.md      the wire contract — read this first
firmware/             PlatformIO + Arduino app for the ESP32-S3
  platformio.ini      board, PSRAM mode, flash layout, library pins
  src/bsp/            panel (Arduino_GFX), I2C, pin map
  src/ble/            NimBLE GATT server, credit flow control
  src/map/            projection, tile decode, LRU cache, nav state
  src/render/         rasteriser, cartographic style, bitmap font
  src/ui/             turn banner, speed/ETA chrome
android/              companion app
  ble/                GATT client, credit flow control
  map/                MVT parser, ETIL encoder, tile pump
  nav/                GraphHopper + OSRM routing
```

## Build

```bash
cd firmware && py -m platformio run -t upload -t monitor
```

PlatformIO handles the toolchain.

`py -m platformio` is used throughout rather than the shorter `pio`, because a
`pip install platformio` puts `pio.exe` in Python's `Scripts` directory, which
is **not on PATH by default** — pip warns about this during install and it is
easy to miss. `py -m platformio` always works. To get the short form, add that
directory to your user PATH once:

```powershell
[Environment]::SetEnvironmentVariable('PATH',
  [Environment]::GetEnvironmentVariable('PATH','User') + ';' +
  "$env:LOCALAPPDATA\Programs\Python\Python313\Scripts", 'User')
```

Then open a new terminal — PATH changes do not apply to already-running shells.

> **On Windows, build from PowerShell or cmd — not Git Bash.** The pioarduino
> platform fetches its compiler through `idf_tools.py`, which hard-refuses to
> run under MSys/Mingw (`ERROR: MSys/Mingw is not supported`). It fails
> *quietly enough to look like success*: the packages register, but their
> `bin/` directories stay empty and the build then dies with
> `'xtensa-esp32s3-elf-g++' is not recognized`.

Everything that would be a board-menu setting in the Arduino IDE lives in
`platformio.ini` instead — PSRAM mode, flash size, partition table — so
getting one wrong shows up in a diff rather than silently costing you 4×
memory bandwidth.

**Before your first flash, read [docs/BOARD_BRINGUP.md](docs/BOARD_BRINGUP.md).**
Waveshare ships two incompatible revisions under this product name. Bring-up
is a five-minute check, and everything else in the project is pin-agnostic.

For the phone side — API keys and expected throughput —
see [docs/ANDROID_SETUP.md](docs/ANDROID_SETUP.md).

To bring the whole thing up, work through
[docs/TESTING.md](docs/TESTING.md) in order. Each stage isolates one layer,
so a failure points at a single cause instead of five.

## Status

| Piece | State |
|---|---|
| Wire protocol spec | done |
| Mercator projection + fixed-point transforms | done |
| ETIL tile decoder | done |
| PSRAM LRU tile cache with pinning | done |
| Rasteriser (thick polylines, polygon fill, discs) | done |
| Cartographic style, day + night | done |
| Route + position puck rendering | done |
| BLE GATT server, credit flow control, reassembly | done |
| HUD (turn banner, speed, ETA) | done |
| Firmware compiles + links | **yes** — 725 KB flash, 42 KB internal RAM |
| Panel bring-up | **working on hardware** — 40 MHz QSPI + vendor init sequence |
| HUD renders on device | **working** — background, speed bar, debug overlay |
| **End-to-end map on the display** | **working** — tiles stream over BLE and render |
| Android: BLE client, MVT parse, ETIL encode, GPS, routing | done |
| Android app compiles + packages | **yes** — 7.0 MB debug APK |
| Google Maps guidance via notification listener | done, built against captured output |
| Turn banner from Google Maps | **working on hardware** |
| Route line, from a place shared out of Maps | **working on hardware** |
| Unit tests | 30 passing, JVM — no device needed |
| Simulated drive (test without a car) | done |
| Touch input (pan/zoom/destination on device) | not started |
| Off-route detection / reroute | not started |

## Design notes worth knowing

**No magnetometer.** The QMI8658 is accel + gyro only. Heading-up rotation
comes from the phone's GPS course-over-ground, and is held rather than reset
when you stop moving.

**Casings are a separate pass.** All road outlines draw before any road fill,
across every class. Doing it per-class instead leaves dark seams at every
junction — this is the detail that separates "looks like a map" from "looks
like a wireframe".

**The cache pins, it doesn't lock.** The render task pins the tiles it needs
under a brief mutex, draws without holding it, then unpins. Eviction skips
pinned slots, so a tile can't be freed mid-draw and a frame never blocks on
the radio.

**Routes are projected once.** 2000 route points through `log`/`atan` every
frame would cost more than drawing them, so the route is converted to world
pixels at a fixed zoom when it arrives and only affine-transformed after that.
