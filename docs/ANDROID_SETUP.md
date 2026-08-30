# Android app setup

## 1. Keys

Both go in `android/gradle.properties`, which is already gitignored-shaped —
keep them out of source.

**MapTiler** (required — this is where map data comes from):
sign up at [cloud.maptiler.com](https://cloud.maptiler.com/account/keys/).
Free tier, no card. Copy the key into:

```properties
MAPTILER_KEY=your_key_here
```

**GraphHopper** (recommended, for routing):
sign up at [graphhopper.com/dashboard](https://graphhopper.com/dashboard/).
One-time key, free tier far beyond occasional use.

```properties
GRAPHHOPPER_KEY=your_key_here
```

Leaving `GRAPHHOPPER_KEY` blank is fine — `FallbackRouteService` drops to the
keyless OSRM demo server. That works, but it is explicitly demo-only and
rate-limited, so it may be unavailable exactly when you need it.

## 2. SDK / AGP versions

These are already set correctly — this section explains *why*, so you can fix
it if you change one of them.

| Setting | Value | Why |
|---|---|---|
| `compileSdk` / `targetSdk` | 35 | highest AGP 8.9 is tested against |
| `minSdk` | 26 | floor for LE 2M PHY, which the tile stream depends on |
| Android Gradle Plugin | 8.9.0 | |
| Gradle | 8.11.1 | AGP 8.9 refuses anything older |
| Kotlin plugin | 2.0.21 | Kotlin 1.9.x doesn't support Gradle 8.11 |
| JDK | 17+ | AGP 8.9 minimum; JDK 21 is what this was built with |

These move together. Change one and the others usually have to follow —
Gradle's messages name the exact minimum, e.g. *"Minimum supported Gradle
version is 8.11.1. Current version is 8.7."*

### Why not compileSdk 37?

Worth recording, because a freshly installed Android Studio hands you an
API 37 SDK and it is tempting to just target it. AGP 8.9 pushes back three
separate ways:

```
WARNING: This Android Gradle plugin (8.9.0) was tested up to compileSdk = 35
This version only understands SDK XML versions up to 3 but an SDK XML file
  of version 4 was encountered
Failed to find Platform SDK with path: platforms;android-37
```

That last one is real: the installed directory is `platforms/android-37.0`,
not `android-37`, and its `source.properties` reports
`AndroidVersion.ApiLevel=37.0` rather than a plain integer.

Nothing in this app uses an API above 33, so 35 costs nothing and AGP
downloads that platform itself on first build. Targeting 37 properly means
**AGP 9.3 + Gradle 9.5**, which also brings breaking DSL changes
(`kotlinOptions`, `packagingOptions`) — a worthwhile upgrade eventually, but
not one to do while bringing the rest of the system up.

Android Studio's *Tools → AGP Upgrade Assistant* handles that matrix for you
when you want to make the jump.

### The Gradle wrapper

`gradlew`, `gradlew.bat` and `gradle/wrapper/gradle-wrapper.jar` are committed,
which is standard for Android projects — it means the build is reproducible
without a system Gradle install. If you ever need to regenerate them, note
that `gradle wrapper` cannot run inside this project when the wrapper is
missing (Gradle configures the project first, and AGP rejects the old Gradle
it would be running under). Generate it in an empty directory with just a
`settings.gradle.kts`, then copy the files across.

## 3. Build

The Gradle wrapper JAR isn't checked in. Open `android/` in Android Studio —
it generates the wrapper and downloads AGP on first sync, which is the least
painful route given the version dance above. Then *Run*, or:

```bash
cd android && ./gradlew installDebug
```

## 3b. Google Maps guidance

Start a trip in Google Maps and the display follows it automatically — no
destination typing.

Grant it once: open esp-maps, tap **Grant notification access**, find
*esp-maps* in the list, enable it. This cannot be a normal permission dialog;
notification access only exists as a Settings screen.

### What comes from where

| On the display | Source | Needs Google Maps? |
|---|---|---|
| Roads, water, landuse | MapTiler → `TileEncoder` | no |
| Position, heading, speed | `FusedLocationProvider` | no |
| Turn banner, distance, ETA | Maps' navigation notification | yes |
| Highlighted route line | GraphHopper, from a destination you set | no |

**Google Maps does not expose its route polyline to any third-party app** —
no API, no broadcast, no content provider. The ongoing notification is the
only machine-readable surface it offers, and it carries text and a maneuver,
not geometry. So the route *line* has to be computed independently.

This is also why Wear OS Maps can draw the line and we can't: it's Google's
own app talking to Google's servers with your account.

### Getting the route line: share the destination

Maps won't give us its route, but it will happily share *where you are going*.

1. In Google Maps, tap the place → **Share** → **esp-maps**.
2. Start navigation in Maps as normal.

The display then shows the map, Maps' own turn guidance, and a route line to
the same destination. One extra tap per trip, and it is the closest to
automatic that is technically possible.

`MapsLinkParser` handles the formats Maps produces:

| Form | Example |
|---|---|
| Place URL | `.../place/X/@17.36,78.47,17z/...!3d17.3616!4d78.4746` |
| Short link | `https://maps.app.goo.gl/AbCdEf` (redirect resolved) |
| Query / directions | `?q=`, `&destination=`, `&daddr=`, `&ll=` |
| `geo:` URI | `geo:17.385,78.4867` |
| Typed by hand | `17.385, 78.4867` |

Place URLs contain coordinates twice: `@lat,lon` is the map *viewport centre*
and `!3d/!4d` is the actual place, which can differ by hundreds of metres.
The parser prefers `!3d/!4d`.

### Short links usually have no coordinates at all

This is the case that matters, because it's what the Maps share sheet
produces. A real captured example resolved to:

```
/maps/place/Crispy+Things,+Whisper+Vly+Rd,+Ambedkar+Nagar,+Shaikpet,
    +Hyderabad,+Telangana+500104/
    data=!4m2!3m1!1s0x3bcb97b47afaf027:0xc4a6a0a0e86d78d0!18m1!1e1
```

No `@lat,lon`. No `!3d/!4d`. Just an address and a Google feature id
(`!1s0x...:0x...`), which is an internal identifier with no public lookup.
The page body is a JavaScript shell, so there is nothing to scrape either.

So `MapsLinkResolver` works in two stages:

1. Follow the `maps.app.goo.gl` redirects, checking each hop for coordinates.
2. If none appear, extract the address from the `/maps/place/…` path and hand
   it to Android's built-in `Geocoder` — Google-backed on most devices, and
   needs no API key of ours.

That makes the destination accurate to whatever the geocoder returns for the
address, typically within tens of metres, which is fine for drawing a route
line. It needs a network connection at share time.

Covered by 17 JVM tests in `MapsLinkParserTest`, including the captured URL
above.

Note that the route line is *our* route to that destination, not Google's. It
will usually match road-for-road, but can diverge when Google picks
differently for traffic. When Maps is guiding, its maneuvers drive the banner
regardless — it knows about reroutes and we don't.

### Tuning the parser

`GuidanceParser` in `nav/MapsNavListener.kt` is keyword-based, because the
drawables Maps ships change far more often than its wording. The extras it
populates are undocumented and shift between versions, so
`MapsNavListener.dumpNotifications` defaults to **true** and logs every field:

```bash
adb logcat -s MapsNav:V
```

Drive one trip, read the dump, and adjust `parseManeuver` / `extractStreet`
against what your version of Maps actually emits. Set `dumpNotifications` to
false afterwards — it is noisy.

Only English wording is handled today.

## 4. First run

1. Power the ESP32. It advertises as `espmaps` with no pairing needed.
2. Open the app, tap **Start**, grant location + nearby-devices + notifications.
3. The app scans, connects, and negotiates MTU 517 + 2M PHY. Watch logcat:

```
BleLink: found AA:BB:CC:DD:EE:FF, connecting
BleLink: MTU 517 (payload 508)
BleLink: notifications enabled, link ready
TilePump: tile 14/8188/5448: 71204 B pbf -> 9871 B etil (20 chunks, 13%)
```

That last line is the thing to watch — **13%** is the compression doing its
job. If you see ratios above ~40%, the encoder is keeping layers it should be
dropping; check that your tile source really is OpenMapTiles schema, since
`TileEncoder.classify()` keys off `class` attribute names that differ between
schemas.

4. The device should start filling in roads within a few seconds.
5. To test routing, type a destination as `lat,lon` and hit **Navigate**.

## 5. Throughput expectations

| Phase | Expected |
|---|---|
| MTU negotiated | 517 (if it stays 23, the connection dropped and reconnected) |
| Sustained rate | 30–70 KB/s on a modern Android phone |
| One z14 tile | 4–15 KB, so well under a second |
| Cold start, 9 tiles | 3–8 seconds to a full screen |
| Steady driving | ~3 tiles every couple of minutes |

If throughput is far below this, check in order:

1. **MTU stuck at 23** — `requestMtu` failed or was called too late.
2. **Connection priority** — some OEM ROMs silently ignore
   `CONNECTION_PRIORITY_HIGH` when battery saver is on.
3. **Credits exhausted** — if the log goes quiet after exactly 32 packets, the
   ESP32 stopped granting credits; that is a firmware-side bug, not the phone.

## 6. Known gaps

- No destination search — coordinates only, until device-side touch input
  lands and a geocoder is wired in.
- Off-route detection isn't implemented; the route is sent once and not
  recalculated if you deviate.
- `onViewChanged` is received but unused. Predictive prefetch along the
  bearing would cut the pop-in when you turn onto a new road.
