// Wire protocol constants. Mirrors docs/PROTOCOL.md.
// Any change here must be mirrored in android/.../ble/Proto.kt.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#define ESPMAPS_PROTO_VER 1
#define ESPMAPS_ADV_NAME  "espmaps"

// NimBLE-Arduino takes UUIDs as strings and handles endianness itself, so
// these stay in the human-readable form that matches Proto.kt and
// docs/PROTOCOL.md.
#define ESPMAPS_SVC_UUID "7a2b0001-5f3c-4d8e-9a1b-2c3d4e5f6071"
#define ESPMAPS_RX_UUID  "7a2b0002-5f3c-4d8e-9a1b-2c3d4e5f6071"  // phone -> esp
#define ESPMAPS_TX_UUID  "7a2b0003-5f3c-4d8e-9a1b-2c3d4e5f6071"  // esp -> phone

// C11 spells it _Static_assert; C++ has had static_assert since C++11. This
// header is included from both.
#ifdef __cplusplus
#define ESPMAPS_ASSERT_SIZE(c, m) static_assert(c, m)
#else
#define ESPMAPS_ASSERT_SIZE(c, m) _Static_assert(c, m)
#endif

// ---- framing -------------------------------------------------------------

#define PKT_HDR_LEN 6

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  flags;
    uint16_t seq;
    uint16_t len;
} pkt_hdr_t;

ESPMAPS_ASSERT_SIZE(sizeof(pkt_hdr_t) == PKT_HDR_LEN, "packet header must be 6 bytes");

// ---- phone -> esp32 ------------------------------------------------------

enum {
    MSG_HELLO        = 0x01,
    MSG_TIME_SYNC    = 0x02,
    MSG_GPS_FIX      = 0x10,
    MSG_TILE_START   = 0x20,
    MSG_TILE_CHUNK   = 0x21,
    MSG_TILE_END     = 0x22,
    MSG_TILE_EMPTY   = 0x23,
    MSG_ROUTE_START  = 0x30,
    MSG_ROUTE_CHUNK  = 0x31,
    MSG_ROUTE_END    = 0x32,
    MSG_ROUTE_CLEAR  = 0x33,
    MSG_MANEUVER     = 0x40,
    MSG_NAV_STATE    = 0x50,
};

// ---- esp32 -> phone ------------------------------------------------------

enum {
    MSG_CREDITS      = 0x80,
    MSG_TILE_REQUEST = 0x81,
    MSG_TILE_CANCEL  = 0x82,
    MSG_STATUS       = 0x83,
    MSG_VIEW         = 0x84,
    MSG_DEST         = 0x85,
    MSG_LOG          = 0x86,
};

// ---- payload structs -----------------------------------------------------

#define GPS_UNKNOWN_U16 0xFFFFu

typedef struct __attribute__((packed)) {
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint16_t speed_cm_s;
    uint16_t bearing_cdeg;
    uint16_t accuracy_dm;
    int16_t  altitude_m;
    uint32_t epoch_s;
} msg_gps_fix_t;

ESPMAPS_ASSERT_SIZE(sizeof(msg_gps_fix_t) == 20, "gps fix must be 20 bytes");

typedef struct __attribute__((packed)) {
    uint8_t  z;
    uint32_t x;
    uint32_t y;
    uint32_t total_len;
    uint16_t chunks;
} msg_tile_start_t;

typedef struct __attribute__((packed)) {
    uint8_t  z;
    uint32_t x;
    uint32_t y;
    uint8_t  prio;
} tile_req_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t  kind;
    uint8_t  exit_no;
    uint32_t dist_m;
    uint16_t pt_index;
    uint8_t  text_len;
    // char text[text_len] follows
} msg_maneuver_hdr_t;

enum maneuver_kind {
    MANEUVER_NONE = 0,
    MANEUVER_STRAIGHT,
    MANEUVER_SLIGHT_LEFT,
    MANEUVER_LEFT,
    MANEUVER_SHARP_LEFT,
    MANEUVER_ROUNDABOUT,
    MANEUVER_UTURN,
    MANEUVER_DEPART,
    MANEUVER_MERGE,
    MANEUVER_FORK_LEFT,
    MANEUVER_FORK_RIGHT,
    MANEUVER_ARRIVE,
    MANEUVER_SLIGHT_RIGHT,
    MANEUVER_RIGHT,
    MANEUVER_SHARP_RIGHT,
    MANEUVER__COUNT,
};

// ---- ETIL tile format ----------------------------------------------------

#define ETIL_MAGIC0   0x45  // 'E'
#define ETIL_MAGIC1   0x54  // 'T'
#define ETIL_VERSION  1

typedef struct __attribute__((packed)) {
    uint8_t  magic[2];
    uint8_t  version;
    uint8_t  flags;
    uint8_t  zoom;
    uint8_t  layer_count;
    uint16_t extent;
    uint32_t payload_len;
} etil_hdr_t;

ESPMAPS_ASSERT_SIZE(sizeof(etil_hdr_t) == 12, "etil header must be 12 bytes");

typedef struct __attribute__((packed)) {
    uint8_t  kind;
    uint8_t  style_class;
    uint16_t feature_count;
} etil_layer_hdr_t;

enum etil_kind {
    ETIL_POLYGON = 0,
    ETIL_LINE    = 1,
    ETIL_POINT   = 2,
};

// Feature point-count field packs a flag in bit 0.
#define ETIL_RING_CONTINUES 0x1u
#define ETIL_PTCOUNT(v)     ((v) >> 1)

#ifdef __cplusplus
}
#endif
