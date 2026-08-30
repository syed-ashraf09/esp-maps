#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "../map/mercator.h"

#ifdef __cplusplus
extern "C" {
#endif


bool ble_link_init(void);

bool ble_link_connected(void);

// Current negotiated ATT MTU, or 23 before negotiation.
uint16_t ble_link_mtu(void);

// Ask the phone for these tiles. Batched into one notification.
void ble_link_request_tiles(const tile_id_t *ids, uint32_t n);

// Tell the phone where we are looking, so it can prefetch.
void ble_link_send_view(double lat, double lon, uint8_t zoom, float bearing_deg);

// User picked a destination on the touchscreen.
void ble_link_send_dest(double lat, double lon);

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_packets;
    uint32_t tiles_ok;
    uint32_t tiles_crc_fail;
    uint32_t credits_granted;
    uint16_t mtu;
    bool     connected;
} ble_link_stats_t;

void ble_link_get_stats(ble_link_stats_t *out);

#ifdef __cplusplus
}
#endif
