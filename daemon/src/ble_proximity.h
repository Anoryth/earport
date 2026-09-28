/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Apple "proximity pairing" BLE advertisements (manufacturer 0x004C, type
 * 0x07), broadcast by AirPods even when they are not connected to us. Their
 * plaintext part tells whether the pods are in the ears and what the AirPods
 * are doing with their current host, which drives automatic connection.
 */

#ifndef BLE_PROXIMITY_H
#define BLE_PROXIMITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define APPLE_COMPANY_ID 0x004C

/* Connection state with the current host (byte 10), checked with an iPhone */
typedef enum {
    PROXIMITY_CONN_DISCONNECTED = 0x00,
    PROXIMITY_CONN_IDLE = 0x04,
    PROXIMITY_CONN_MUSIC = 0x05,
    PROXIMITY_CONN_CALL = 0x06,
    PROXIMITY_CONN_RINGING = 0x07,
    PROXIMITY_CONN_HANGING_UP = 0x09,
} ProximityConnectionState;

typedef struct {
    uint16_t model;             /* Same IDs as AirPodsModel (e.g. 0x2420) */
    bool in_ear;                /* At least one pod in an ear */
    bool both_in_case;
    uint8_t connection_state;   /* ProximityConnectionState */
} ProximityInfo;

/* Parse Apple manufacturer data; false if it is not a paired AirPods advert */
bool proximity_parse(const uint8_t *data, size_t len, ProximityInfo *info);

/* Whether a BLE resolvable private address ("70:7A:EC:AE:5F:F8") belongs to
 * the device owning this IRK (16 bytes, little-endian as sent by AirPods) */
bool proximity_address_matches(const uint8_t *irk, const char *address);

typedef enum {
    AUTOCONNECT_TRIGGER_EARS,       /* Pods just put in the ears */
    AUTOCONNECT_TRIGGER_PLAYBACK,   /* Playback started on this computer */
} AutoConnectTrigger;

/* Whether connecting now would not take the AirPods away from another
 * device that is using them. Connecting takes them from an idle iPhone,
 * so that is only done when playback starts here (an explicit choice). */
bool autoconnect_allowed(const ProximityInfo *info, AutoConnectTrigger trigger);

/* Whether the AirPods may soon be free for playback here: right after a
 * pause on the other device they still report music for 2-3 s */
bool autoconnect_worth_waiting(const ProximityInfo *info);

#endif /* BLE_PROXIMITY_H */
