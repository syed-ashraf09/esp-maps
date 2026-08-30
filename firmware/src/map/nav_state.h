// Navigation state shared between the BLE task (writer) and the render task
// (reader). Guarded by a mutex; readers take a snapshot rather than holding
// the lock across a frame.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "mercator.h"
#include "../ble/proto.h"

#ifdef __cplusplus
extern "C" {
#endif


// Route points are stored as world pixels at a fixed high zoom, precomputed
// once when the route arrives. Re-projecting 2000 lat/lons through log/atan
// every frame would cost more than drawing them. At z21 the world is
// 536,870,912 px across, which fits int32 with ~7 cm resolution.
#define ROUTE_REF_ZOOM 21

#define NAV_MAX_ROUTE_PTS 8192
#define NAV_MANEUVER_TEXT 48

typedef struct {
    bool     valid;
    double   lat;
    double   lon;
    float    bearing_deg;    // course over ground; held over when stationary
    float    speed_mps;
    float    accuracy_m;
    uint32_t epoch_s;
    uint32_t rx_ms;          // local monotonic time this fix landed
} nav_fix_t;

typedef struct {
    uint8_t  kind;           // enum maneuver_kind
    uint8_t  exit_no;
    uint32_t dist_m;         // as sent by the phone
    uint16_t pt_index;       // index into the route polyline
    char     text[NAV_MANEUVER_TEXT];
} nav_maneuver_t;

typedef struct {
    bool      has_route;
    int32_t  *world_pts;     // interleaved x,y at ROUTE_REF_ZOOM
    uint32_t  n_pts;
    uint32_t  cap_pts;
    uint32_t  total_dist_m;
    uint32_t  total_dur_s;

    nav_maneuver_t next_maneuver;
    bool           has_maneuver;

    uint32_t eta_epoch_s;
    uint32_t remain_m;
    uint32_t remain_s;
} nav_route_t;

typedef struct {
    nav_fix_t   fix;
    nav_route_t route;
    bool        link_up;      // BLE connected
    uint8_t     batt_pct;
} nav_state_t;

bool nav_init(void);

// Writers (BLE task).
void nav_set_fix(const msg_gps_fix_t *m, uint32_t now_ms);
void nav_set_link(bool up);
bool nav_route_begin(uint32_t n_pts, uint32_t dist_m, uint32_t dur_s);
bool nav_route_append(const uint8_t *varint_data, uint32_t len);
void nav_route_commit(void);
void nav_route_clear(void);
void nav_set_maneuver(const nav_maneuver_t *m);
void nav_set_progress(uint32_t eta_epoch_s, uint32_t remain_m, uint32_t remain_s);

// Reader (render task). Copies scalars; `route.world_pts` stays owned by nav
// and is safe to read because the route buffer is only replaced under lock
// between frames.
void nav_snapshot(nav_state_t *out);

// Distance in metres from the current fix to the next maneuver, computed
// locally from the route polyline so it updates smoothly between GPS fixes.
uint32_t nav_dist_to_maneuver(void);

#ifdef __cplusplus
}
#endif
