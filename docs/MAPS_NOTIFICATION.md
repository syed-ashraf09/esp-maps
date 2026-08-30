# Google Maps navigation notification

Reference for `nav/GuidanceParser.kt`, captured from a real trip on
2026-08-30. Google documents none of this, and it changes between versions —
if guidance stops working, re-capture and compare against what's here.

```bash
adb logcat -s MapsNav:V
```

Set `MapsNavListener.dumpNotifications = true` first.

---

## The template

Current Maps uses **Android 16's `ProgressStyle`** ("Live Updates"):

```
extra[android.template] = android.app.Notification$ProgressStyle
extra[androidx.core.app.extra.COMPAT_TEMPLATE]
    = androidx.core.app.NotificationCompat$ProgressStyle
```

This matters a lot: the older text-only navigation notification carried
distances only as prose. ProgressStyle exposes the journey numerically.

## Fields that carry navigation state

| Key | Example | Meaning |
|---|---|---|
| `android.title` | `Head toward St 2` | the maneuver instruction |
| `android.subText` | `Arrive 11:43 am` | arrival **wall-clock time** |
| `android.progress` | `33` | distance travelled, **metres** |
| `android.progressMax` | `12571` | total route distance, **metres** |
| `...primaryInfo` | `0 m` *or* `towards St 2` | **alternates** — see below |
| `...secondaryInfo` | `towards St 2` or empty | instruction |
| `...chipExpandedText` | `0 m` *or* `Head west` | same alternation |
| `...nowbarPrimaryInfo` | mirrors `primaryInfo` | |

Prefix `...` is `android.ongoingActivityNoti.`.

`android.text` and `android.bigText` are **always null** — the old parser
looked there first and would have found nothing.

### Remaining distance

```
progressMax - progress
```

Verified against a real trip: `progressMax` jumps (12571 → 13734 → 12488) at
each reroute, `progress` counts up from 0 within the current route. Over a
7 s window progress went 0 → 33, i.e. ~4.7 m/s ≈ 17 km/h — consistent with
driving.

### The primaryInfo alternation

`primaryInfo`, `chipExpandedText` and `nowbarPrimaryInfo` flip between the
distance-to-next-turn and the instruction on successive updates:

```
11:19:47.021  primaryInfo = 0 m
11:19:47.024  primaryInfo = towards Bhagavatguda Rd
11:19:47.026  primaryInfo = 0 m
```

So distance is **sticky** in the parser: only overwritten when a field
actually parses as a distance, otherwise the last value is kept.

## Transient titles

These appear as ongoing notifications but carry no guidance:

```
Starting navigation…
Rerouting...
```

During `Rerouting...`, `subText` degrades to a bare `Arrive` with no time,
and `progress` resets to 0 while `progressMax` still holds the *old* total.
The parser returns null for these so the previous banner stays up — blanking
the display every time Maps recalculates would be much worse.

## Update rate

Bursty: 3–4 notifications can arrive within the same millisecond, and several
per second while moving. `MapsNavListener` de-duplicates on a rounded key
(maneuver, distance/10, instruction, remaining/25) so the BLE link only sees
real changes.

## Wording observed

```
Head west
Head toward St 2
Head toward Bhagavatguda Rd
```

`extractStreet` strips `toward`/`towards`/`onto`; where there is no road name
(`Head west`) the title is kept whole. All of these map to `STRAIGHT`.

Turn wording (`turn left`, `keep right`, `roundabout`, …) has not been
captured yet — the trip above never got past the first maneuver. Worth
re-capturing on a longer drive to confirm `parseManeuver`.

**English only.** Every keyword in `parseManeuver` and `extractStreet` is
English; Maps localises these, so another display language needs new keywords.

## What is NOT here

No coordinates, no polyline, no destination address. The route line cannot be
reconstructed from this notification — it has to be computed independently.
