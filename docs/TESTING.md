# Testing esp-maps

Test in stages. Each isolates a single layer, so a failure points at one cause
instead of five. Skipping ahead just means a blank screen and guesswork.

Stages 1 and 3 are **confirmed working on real hardware**. Everything from
stage 4 on is untested end-to-end.

---

## Stage 0 — what you need

- The board on USB-C (it enumerates as `COM6` here).
- A MapTiler key in `android/gradle.properties` — already set.
- An Android phone, Bluetooth on, location enabled, USB debugging on.
- Optional: **nRF Connect for Mobile** (Nordic, free), a generic BLE scanner.

Run PlatformIO from **PowerShell or cmd**, not Git Bash — the toolchain
refuses to run under MSys. Find the port with:

```bash
py -m platformio device list
```

---

## Stage 1 — flash and boot, no phone ✅ verified

Proves panel, pins, board revision, PSRAM and the rasteriser.

```bash
py -m platformio run -d "C:\Code\claude\esp-maps\firmware" -t upload -t monitor
```

If upload can't find the chip: hold **BOOT**, tap **RESET**, release BOOT,
retry. Native USB re-enumerates after flashing, so the monitor may need a
moment or a re-run.

### Expected serial

```
[esp-maps] starting, board rev B (CO5300/CST816)
[esp-maps] boot: internal 287 KB free, PSRAM 8189 KB free
[display] expander 0x20: all outputs driven high
[display] I2C scan on SDA=15 SCL=14:
[display]   0x15  CST816 touch (Rev B)
[display]   0x18  ES8311 audio codec
[display]   0x20  XCA9554/TCA9554 expander (Rev B)
[display]   0x34  AXP2101 PMU
[display]   0x51  PCF85063 RTC
[display]   0x6B  QMI8658 IMU
[display] QSPI CS=12 CLK=11 D0..3=4,5,6,7 @ 40 MHz
[display] panel up: 368x448, fb 322 KB at 0x3c0b0f80
[display] applied Waveshare vendor init sequence
[display] selftest: RED / GREEN / BLUE / WHITE
[ble] advertising as "espmaps"
[esp-maps] up. advertising as "espmaps" - connect the phone app.
```

### Expected screen

1. **Red, green, blue, white** for ~600 ms each — the panel self-test.
2. Then a **plain off-white screen**, a **dark bar along the bottom** reading
   `0 km/h`, a **red pip top-right**, and a small debug line top-left.

No roads is correct — there's no tile data until stage 5.

### What to check

1. **`PSRAM 8189 KB free`.** Near 0 or 2 MB means quad mode; `setup()` warns.
   Nothing downstream works properly until this is right.
2. **`0x15` + `0x20` present** → Rev B, the default. `0x38` with no `0x20` →
   Rev A, so set `-DESPMAPS_BOARD_REV=1` and reflash.
3. **The four self-test colours.** If those appear, the panel, QSPI bus,
   column offset and flush path are all proven — any later blankness is the
   renderer, not the hardware.

Once happy, set `-DESPMAPS_PANEL_SELFTEST=0` in `platformio.ini` to drop the
2.4 s colour cycle from boot.

---

## Stage 2 — BLE without the app (optional)

Worth five minutes: it splits "the app can't connect" into two very different
problems.

In **nRF Connect**, scan for `espmaps` and connect. You should see service
`7a2b0001-5f3c-4d8e-9a1b-2c3d4e5f6071` containing a write-without-response
characteristic (`...0002`) and a notify one (`...0003`).

Subscribe to `...0003`. A short notification should arrive immediately —
that's `MSG_CREDITS` granting the initial 32-write window, bytes
`80 00 xx xx 02 00 20 00`. If you get it, the whole server side of the
protocol works.

**Disconnect nRF Connect before stage 4** — the board accepts one connection.

---

## Stage 3 — install the app ✅ builds clean

Already built. To install:

```bash
C:\Users\admin\AppData\Local\Android\Sdk\platform-tools\adb.exe install -r "C:\Code\claude\esp-maps\android\app\build\outputs\apk\debug\app-debug.apk"
```

To rebuild after changes:

```bash
C:\Code\claude\esp-maps\android\gradlew.bat -p C:\Code\claude\esp-maps\android assembleDebug
```

Must be a real phone — BLE doesn't work on the emulator.

---

## Stage 4 — link up

Open the app, tap **Start**, grant location + nearby-devices + notifications.

```bash
C:\Users\admin\AppData\Local\Android\Sdk\platform-tools\adb.exe logcat -s BleLink:V TilePump:V NavService:V MapsNav:V
```

Expected:

```
BleLink: scanning for "espmaps"
BleLink: found AA:BB:CC:DD:EE:FF, connecting
BleLink: MTU 517 (payload 508)
BleLink: notifications enabled, link ready
```

**`MTU 517` is the number that matters.** Stuck at 23 means ~5× less
throughput and tiles will crawl.

On the display: the pip goes **red → amber**, and the debug line shows a
non-zero MTU.

---

## Stage 5 — tiles, the payoff

Within a few seconds of linking, roads should fill in.

```
TilePump: tile 14/8188/5448: 71204 B pbf -> 9871 B etil (13%)
```

**That percentage is the headline metric.** 5–20% means the encoder is working
as designed. 40%+ means it's keeping layers it should drop — check the tile
source really is OpenMapTiles schema, since `TileEncoder.classify()` keys off
`class` attribute names.

The debug line reads `frame_us · tiles_ready/pending · MTU · bytes`:

```
28.4ms 9t/0p mtu517 143kB
```

Frame time should settle around 20–50 ms. Above 60 ms is almost always quad
-mode PSRAM — back to stage 1. Nine tiles ready with zero pending is a full
screen of map.

---

## Stage 6 — position

Near a window or outdoors, for a GPS fix.

- Pip turns **green**.
- A blue puck appears with a white outline and a direction cone.
- The map centres on you.

### Testing movement at a desk

You never need to drive anywhere to test this. Two options, in order of
usefulness:

**Built-in simulation (no setup).** Draw a route, then tap **Start / stop
simulation**. `DemoDriver` plays a synthetic position along that route at
50 km/h and publishes matching turn guidance through the same `MapsNav` path
Google Maps uses. Because the path follows a really-routed road, this
exercises tile prefetch across boundaries, heading-up rotation, the puck, the
route line and the turn banner countdown — everything a real trip does.

It feeds `NavService` directly rather than acting as a mock provider, so it
needs no Developer Options setup and can't be mistaken for a real fix. Real
GPS is ignored while it runs, so the two can't fight over the map centre.

**Mock location provider.** Install a mock-location app (Lockito or similar)
and set it under **Developer Options → Select mock location app**. Slower to
set up, but it also drives *Google Maps itself*, which is the only way to see
real Maps notification wording without going outside.

The firmware **holds the last heading** below ~1 m/s rather than snapping to
north — there's no magnetometer (QMI8658 is accel + gyro only), so a
stationary bearing is meaningless. Not rotating while stopped is correct.

---

## Stage 7 — Google Maps guidance

Grant it once: in esp-maps tap **Grant notification access**, find *esp-maps*
in the list, enable it. It can't be a normal permission dialog — notification
access only exists as a Settings screen. The app's status line should then
read *"Connected — start a trip in Google Maps"*.

Now start any trip in Google Maps. The display should show a **turn banner**
with an arrow, distance and street name, updating as you go.

### Tuning the parser

`GuidanceParser` is a best guess at Maps' notification format, which is
undocumented and version-dependent. `dumpNotifications` is **on by default**:

```bash
C:\Users\admin\AppData\Local\Android\Sdk\platform-tools\adb.exe logcat -s MapsNav:V
```

Start a trip and you'll get a dump of every field per notification:

```
--- Maps notification (id=1, ongoing=true) ---
  title   = In 200 m, turn left onto High Street
  text    = 12 min · 3.4 km · 14:32
  extra[...] = ...
```

If the banner shows the wrong maneuver or no distance, that dump is what to
adjust `parseManeuver` / `extractStreet` / `parseDistance` against. English
wording only.

### Testing the parser without Maps

`GuidanceParser` is deliberately free of Android types, so it runs as a plain
JVM test — no device, no emulator, no driving:

```bash
C:\Code\claude\esp-maps\android\gradlew.bat -p C:\Code\claude\esp-maps\android testDebugUnitTest
```

18 tests, split between values captured verbatim from a real trip and the
turn wording we haven't observed yet. Report lands in
`app/build/reports/tests/testDebugUnitTest/index.html`.

Add a case there whenever a new dump shows wording the parser gets wrong —
that's much faster than reflashing and driving.

---

## Stage 8 — route line (optional)

Only needed if you want the highlighted route drawn. Google does **not**
expose its route polyline to any app, so this comes from GraphHopper instead.

Type `lat,lon` into **Route line (optional)** and tap **Draw route**.

- Logcat: `NavService: route: 1843 pts, 12400 m, 1080 s`
- Display: a thick blue line with a darker casing along the roads.

With `GRAPHHOPPER_KEY` blank the app falls back to the keyless OSRM demo
server — works, but rate-limited and demo-only, the usual cause of
intermittent routing failures.

Note that when Google Maps is guiding, **its** maneuvers drive the banner —
the firmware ignores our route's maneuvers, since Maps knows about traffic and
reroutes and we don't.

---

## Failure quick-reference

| Symptom | Most likely cause |
|---|---|
| Nothing on serial | USB CDC not up; BOOT + RESET, reflash |
| `PSRAM ... 0 KB` warning | `memory_type` not `qio_opi`; `run -t clean` |
| I2C scan finds nothing | I2C pins wrong (should be SDA 15 / SCL 14) |
| Reaches `panel up`, screen black | clock too high, or vendor init skipped |
| Image shifted, garbage edge strip | wrong `ESPMAPS_BOARD_REV` / column offset |
| `espmaps` not in nRF Connect | BLE init failed; check serial for `[ble]` |
| App scans forever | nRF Connect still connected — one link only |
| MTU stays 23 | connection dropped and re-established |
| Link up, no roads ever | tile fetch failing — check MapTiler key in logcat |
| Roads appear then vanish | CRC failures; look for `chunk out of order` |
| Frame time > 60 ms | PSRAM in quad mode |
| Compression ratio > 40% | tile source is not OpenMapTiles schema |
| No turn banner in Maps trip | notification access not granted, or parser miss |

## Bugs found during first bring-up

All four were real, all had the same misleading signature — a link that
connects cleanly and then does nothing. Worth knowing if you port this.

| Symptom | Cause |
|---|---|
| `rx 0 B in 0 pkts`, link healthy | Credits granted from `onConnect`/`onMTUChange`, but the phone subscribes to notifications *after* both. Grant from **`onSubscribe`** or the phone is never permitted to write. |
| One packet sent, then silence | `writeInFlight` was non-volatile, cleared by the GATT binder thread outside the lock that `pump()` reads it under. The producer thread saw a stale `true` forever. |
| App dies mid-transfer, `reason 0x213` | `startForeground` with `FOREGROUND_SERVICE_TYPE_LOCATION` throws `SecurityException` when location was granted **"Only this time"** and has lapsed. Use `connectedDevice` as the base type. |
| Small packets fine, first full chunk crashes | Android caps a GATT write at **512 B** regardless of MTU. Sizing against `mtu - 3` (514) throws on the first full-size chunk. |

The pattern: none of these produced an error at the point of failure. Two ends
that each believe they are working is the hardest shape of bug here, which is
why the `[link] cache ready=... | rx ... B in ... pkts` counters exist — they
turn "nothing happens" into a number that says which side is silent.

---

## Commands

```bash
py -m platformio device list
```

```bash
py -m platformio run -d "C:\Code\claude\esp-maps\firmware" -t upload -t monitor
```

```bash
C:\Code\claude\esp-maps\android\gradlew.bat -p C:\Code\claude\esp-maps\android assembleDebug
```

```bash
C:\Users\admin\AppData\Local\Android\Sdk\platform-tools\adb.exe install -r "C:\Code\claude\esp-maps\android\app\build\outputs\apk\debug\app-debug.apk"
```

```bash
C:\Users\admin\AppData\Local\Android\Sdk\platform-tools\adb.exe logcat -s BleLink:V TilePump:V NavService:V MapsNav:V
```

The on-device debug overlay is `g_show_debug` in `firmware/src/main.cpp`.
