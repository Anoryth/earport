/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * What the AirPods tell us, applied to the state and announced on D-Bus:
 * battery and remaining listening time, ear detection (which drives media
 * pause/resume), noise control, conversation detection, settings, model.
 * Knows nothing about the Bluetooth link, so a packet sequence can be
 * replayed in tests.
 */

#ifndef DEVICE_H
#define DEVICE_H

#include <stdbool.h>

#include "aap_protocol.h"
#include "airpods_state.h"
#include "battery_estimator.h"
#include "dbus_service.h"
#include "media_control.h"

typedef struct {
    AirPodsState *state;
    DbusService *dbus;
    MediaControl *media;         /* May be NULL */

    bool ca_speaking;            /* Conversation awareness lowered the volume */

    /* Remaining listening time, learned per device */
    BatteryEstimator battery_estimator;
} Device;

void device_init(Device *dev, AirPodsState *state, DbusService *dbus, MediaControl *media);

/* A session with the AirPods started: apply their saved profile */
void device_session_started(Device *dev, const char *address, const char *name);

/* The session ended: tell clients and forget the state */
void device_session_ended(Device *dev);

void device_handle_packet(Device *dev, const AapParsedPacket *pkt);

/* Shutting down: keep what was learned during the current session */
void device_finish(Device *dev);

#endif /* DEVICE_H */
