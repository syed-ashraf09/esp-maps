# esp-maps wire protocol v1

The contract between the Android companion app and the ESP32-S3 firmware.
Both sides implement this document. Changing it means changing both.

Design constraint that shapes everything below: **BLE gives us 20-60 KB/s.**
Every byte is budgeted against that.

---

## 1. BLE GATT layout

The ESP32 is the **peripheral** (GATT server). The phone is the **central**.

| Role | UUID |
|---|---|
| Service | `7a2b0001-5f3c-4d8e-9a1b-2c3d4e5f6071` |
| `RX` - phone to ESP32, *Write Without Response* | `7a2b0002-5f3c-4d8e-9a1b-2c3d4e5f6071` |
| `TX` - ESP32 to phone, *Notify* | `7a2b0003-5f3c-4d8e-9a1b-2c3d4e5f6071` |

Advertised local name: `espmaps`.

### Link setup (phone side, in order)

1. Connect, `autoConnect = false`.
2. `requestMtu(517)` - wait for callback. Effective payload is `mtu - 3`.
3. `requestConnectionPriority(CONNECTION_PRIORITY_HIGH)` - targets an 11.25-15 ms interval.
4. `setPreferredPhy(PHY_LE_2M, PHY_LE_2M, PHY_OPTION_NO_PREFERRED)`.
5. Enable notifications on `TX` (write `0x0001` to its CCCD).
6. Wait for `MSG_CREDITS` before sending any bulk data.

Skipping step 2 or 3 costs roughly 5x throughput. They are not optional.

### Flow control - credit based

The ESP32 has a bounded reassembly buffer. It grants credits; the phone spends them.

- One credit = permission to send one `RX` write of up to `mtu - 3` bytes.
- The phone MUST NOT write when its credit balance is zero.
- The ESP32 sends `MSG_CREDITS` whenever it frees buffer space.
- Credits are additive: the payload is the number of *additional* writes now
  permitted. The phone adds it to its balance.
- On disconnect the balance resets to zero.

Android's `writeCharacteristic` with `WRITE_TYPE_NO_RESPONSE` will happily
overrun the peer's buffer without this. It is the difference between a link
that sustains 60 KB/s and one that silently drops a third of its packets.

---

## 2. Packet framing

Every `RX` write and every `TX` notification is exactly one packet. Packets are
never split across BLE operations - chunking is explicit (section 4), not implicit.

```
offset  size  field
0       1     type       (see tables below)
1       1     flags      (reserved, must be 0)
2       2     seq        u16 LE, wraps; per-direction counter
4       2     len        u16 LE, payload length
6       len   payload
```

Header is 6 bytes. With MTU 517 the usable payload is **506** bytes, not 508.

The bound is **not** `mtu - 3`. Android's `BluetoothGatt.writeCharacteristic`
throws `IllegalArgumentException: value should not be longer than max length
of an attribute value` for anything over **512 bytes**, whatever MTU was
negotiated. So:

```
payload = min(mtu - 3, 512) - 6
        = min(514, 512) - 6
        = 506
```

Sizing against `mtu - 3` yields 508, and a full chunk then frames to 514 bytes
and throws — killing the process on the first full-size packet while small
packets (HELLO, TILE_START) sail through. That failure mode looks exactly like
a link that connects fine and then dies for no reason.

Multi-byte integers are **little-endian** throughout (both the ESP32-S3 and
ARM Android devices are LE; no byte swapping anywhere on the hot path).

Coordinates on the wire are `int32` in **1e-7 degrees** (`lat_e7`). This gives
~11 mm resolution and covers the full range in 32 bits.

---

## 3. Phone to ESP32 messages (`RX`)

| Type | Name | Payload |
|---|---|---|
| `0x01` | `MSG_HELLO` | `u8 proto_ver, u8 caps, u16 tile_extent, u32 epoch_s` |
| `0x02` | `MSG_TIME_SYNC` | `u32 epoch_s, i16 tz_offset_min` |
| `0x10` | `MSG_GPS_FIX` | see below |
| `0x20` | `MSG_TILE_START` | `u8 z, u32 x, u32 y, u32 total_len, u16 chunks` |
| `0x21` | `MSG_TILE_CHUNK` | `u16 index, u8 data[]` |
| `0x22` | `MSG_TILE_END` | `u32 crc32` |
| `0x23` | `MSG_TILE_EMPTY` | `u8 z, u32 x, u32 y` - tile has no drawable content |
| `0x30` | `MSG_ROUTE_START` | `u32 total_pts, u32 dist_m, u32 dur_s, u16 chunks` |
| `0x31` | `MSG_ROUTE_CHUNK` | `u16 index, u8 data[]` - delta-varint points (section 5.3) |
| `0x32` | `MSG_ROUTE_END` | `u32 crc32` |
| `0x33` | `MSG_ROUTE_CLEAR` | *(empty)* |
| `0x40` | `MSG_MANEUVER` | `u8 kind, u8 exit_no, u32 dist_m, u16 pt_index, u8 text_len, char text[]` |
| `0x50` | `MSG_NAV_STATE` | `u8 state, u32 eta_epoch_s, u32 remain_m, u32 remain_s` |

### `MSG_GPS_FIX` (0x10) - 20 bytes, sent at 1 Hz

```
offset size  field
0      4     i32  lat_e7
4      4     i32  lon_e7
8      2     u16  speed_cm_s      cm/s, 0xFFFF = unknown
10     2     u16  bearing_cdeg    centidegrees 0..35999, 0xFFFF = unknown
12     2     u16  accuracy_dm     decimetres, 0xFFFF = unknown
14     2     i16  altitude_m
16     4     u32  epoch_s
```

`bearing` is course-over-ground from the phone's fused provider. The board's
QMI8658 is accel+gyro only - **there is no magnetometer**, so heading-up
rotation depends entirely on this field. When it reads `0xFFFF` (stationary),
the firmware holds the last known bearing rather than snapping to north.

### `MSG_MANEUVER` (0x40) - `kind` enum

```
0  NONE           8  MERGE
1  STRAIGHT       9  FORK_LEFT
2  SLIGHT_LEFT    10 FORK_RIGHT
3  LEFT           11 ARRIVE
4  SHARP_LEFT     12 SLIGHT_RIGHT
5  ROUNDABOUT     13 RIGHT
6  UTURN          14 SHARP_RIGHT
7  DEPART
```

`pt_index` indexes into the route polyline, so the firmware can compute
distance-to-maneuver locally at 10 Hz without further radio traffic.

---

## 4. ESP32 to phone messages (`TX`, notify)

| Type | Name | Payload |
|---|---|---|
| `0x80` | `MSG_CREDITS` | `u16 additional_writes` |
| `0x81` | `MSG_TILE_REQUEST` | `u8 count, { u8 z, u32 x, u32 y, u8 prio }[count]` |
| `0x82` | `MSG_TILE_CANCEL` | `u8 count, { u8 z, u32 x, u32 y }[count]` |
| `0x83` | `MSG_STATUS` | `u8 batt_pct, u8 state, u16 cache_tiles, u16 free_kb` |
| `0x84` | `MSG_VIEW` | `i32 lat_e7, i32 lon_e7, u8 zoom, u16 bearing_cdeg` |
| `0x85` | `MSG_DEST` | `i32 lat_e7, i32 lon_e7` - user picked a destination on the touchscreen |
| `0x86` | `MSG_LOG` | `char[]` - dev builds only |

`MSG_TILE_REQUEST` batches up to 16 tiles per notification, ordered by
priority (0 = highest). The firmware requests the 3x3 neighbourhood around the
view centre, biased forward along the current bearing.

`MSG_VIEW` lets the phone prefetch ahead of demand rather than waiting to be
asked. Sent on any pan/zoom and at most 1 Hz otherwise.

---

## 5. Payload encodings

### 5.1 The `ETIL` vector tile format

This is a **re-encoding** of a Mapbox Vector Tile, done on the phone. The
firmware never sees protobuf, never sees gzip, and never allocates during
parse beyond one buffer per tile.

Why re-encode instead of forwarding the `.pbf`: a raw OpenMapTiles `.pbf` at
z14 is 40-120 KB and is mostly labels, POIs, house numbers and attributes that a
368x448 navigation display will never draw. Stripped to road/water/landuse
geometry with a one-byte class, the same tile is **4-15 KB**. That is the
difference between 2 seconds per tile and 12 seconds per tile over BLE.

```
Tile header - 12 bytes
offset size  field
0      2     magic       'E','T'  (0x45 0x54)
2      1     version     = 1
3      1     flags       bit0: reserved
4      1     zoom
5      1     layer_count
6      2     extent      coordinate space, typically 2048
8      4     payload_len bytes following this header

Layer header - 4 bytes, repeated layer_count times
offset size  field
0      1     kind        0 = POLYGON, 1 = LINE, 2 = POINT
1      1     style_class see section 5.2
2      2     feature_count

Feature - variable, repeated feature_count times
  varint         point_count_and_flags
                   bit0 = ring continues (next feature is a hole of this polygon)
                   bits 1.. = actual point count
  zigzag varint  dx0, dy0     delta from (0,0)
  zigzag varint  dx, dy       delta from previous point
  ... x (point_count - 1)
```

Polygon features use the MVT convention: exterior ring first, holes follow,
each ring encoded as its own feature with the ring-continues bit set on all
but the last ring of a polygon.

**Varint**: unsigned LEB128, 7 bits per byte, high bit = continue.
**Zigzag**: `(n << 1) ^ (n >> 31)` - maps small negatives to small positives.

The phone MUST run Douglas-Peucker simplification before encoding, with
tolerance = 4 units at extent 2048 (~0.7 px on a 368 px display at matched
zoom). This is where most of the size reduction actually comes from -
typically 3-5x on top of the attribute stripping.

### 5.2 `style_class` enum

Shared verbatim between `TileEncoder.kt` and `style.c`.

```
 0 BACKGROUND             9  ROAD_PRIMARY
 1 LANDUSE_PARK           10 ROAD_SECONDARY
 2 LANDUSE_RESIDENTIAL    11 ROAD_TERTIARY
 3 LANDUSE_INDUSTRIAL     12 ROAD_MINOR
 4 WATER                  13 ROAD_SERVICE
 5 WATERWAY               14 ROAD_PATH
 6 BUILDING               15 RAIL
 7 ROAD_MOTORWAY          16 BOUNDARY
 8 ROAD_TRUNK
```

Draw order is defined by the firmware (`map_render.c`), not by layer order in
the tile - the encoder may emit layers in any order.

### 5.3 Route polyline encoding

Same varint/zigzag scheme as tile geometry, but in **absolute 1e-6 degrees**
rather than tile-local coordinates, because a route spans many tiles:

```
  zigzag varint  d_lat_e6     delta from previous point (first from 0)
  zigzag varint  d_lon_e6
```

At 1e-6 degrees (~11 cm) consecutive route points typically encode in 2-3
bytes each. A 40 km urban route of ~2000 points lands around 5 KB - one
transfer at connection time, then nothing.

---

## 6. Tile request lifecycle

```
ESP32                                   Phone
  |  MSG_VIEW (centre moved) --------->  |
  |                                      | compute needed z/x/y set
  |  MSG_TILE_REQUEST [9 tiles] ------>  |
  |                                      | fetch .pbf from tile source
  |                                      | parse MVT, filter, simplify, re-encode
  | <----------------- MSG_TILE_START    |
  | <----------------- MSG_TILE_CHUNK xN |   (spends credits)
  | <----------------- MSG_TILE_END      |
  |  MSG_CREDITS --------------------->  |
  |                                      |
  |  (tile decoded into PSRAM cache, rendered)
```

Rules:

- The firmware never blocks rendering on a tile. Missing tiles draw as
  background; they pop in when they arrive.
- A tile already in cache is never re-requested. The cache is keyed on
  `(z, x, y)` and is an LRU over PSRAM.
- `MSG_TILE_EMPTY` is a positive answer, not an error - it caches as "nothing
  here" so ocean and countryside are not requested repeatedly.
- If `MSG_TILE_END`'s CRC32 fails, the firmware discards the tile and
  re-requests it once. Two failures and it caches as empty for 60 s to avoid a
  hot loop.

---

## 7. Zoom policy

The firmware renders at a *display* zoom that can differ from the *data* zoom.

| Display zoom | Data zoom fetched | Rationale |
|---|---|---|
| 17-18 | 14 | one z14 tile covers many screens; overzoom is free |
| 15-16 | 13 | |
| 13-14 | 12 | |
| 12 and below | 10 | wide-area overview, roads filtered to trunk and above |

Fetching a lower data zoom than display zoom is the core bandwidth trick: at
50 km/h a z14 tile (~1.9 km at mid-latitudes) lasts ~2.3 minutes, so the link
sees roughly 3 tiles per boundary crossing - about 45 KB every couple of
minutes. Well inside budget, with headroom for prefetch.
