/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Automatic connection: watch the AirPods' BLE adverts while they are not
 * connected to this computer, and connect them when they are put in or when
 * playback starts here, unless another device is using them.
 */

#ifndef AUTOCONNECT_H
#define AUTOCONNECT_H

#include <stdint.h>
#include <stdbool.h>

#include "ble_proximity.h"

typedef struct AutoConnect AutoConnect;

/* Battery of the AirPods while they are not connected to this computer
 * (on another device, or idle), with what they do with their current
 * host (ProximityConnectionState); NULL when not seen for a while */
typedef void (*AutoConnectNearbyCallback)(const ProximityBattery *battery,
                                          uint8_t connection_state, void *user_data);

/* Loads the keys of the AirPods used last. Watching also reports their
 * battery when they are nearby but not connected here. */
AutoConnect *autoconnect_new(bool enabled, AutoConnectNearbyCallback nearby_callback,
                             void *user_data);
void autoconnect_free(AutoConnect *ac);

/* Start watching, once we know whether the AirPods are already connected */
void autoconnect_start(AutoConnect *ac);

void autoconnect_set_enabled(AutoConnect *ac, bool enabled);

/* The AirPods connected to this computer (through the adapter at
 * adapter_path, the one to scan on) or left it (adapter_path unused) */
void autoconnect_set_airpods_connected(AutoConnect *ac, bool connected,
                                       const char *adapter_path);

/* Keys the connected AirPods gave to recognize their adverts; stored when
 * they changed */
void autoconnect_set_irk(AutoConnect *ac, const char *address, const uint8_t *irk);

/* A player started on this computer */
void autoconnect_on_playback_started(AutoConnect *ac);

/* Key decrypting the battery in their adverts; stored when it changed */
void autoconnect_set_enc(AutoConnect *ac, const char *address, const uint8_t *enc);

#endif /* AUTOCONNECT_H */
