/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Commands sent to the AirPods: D-Bus methods (noise control, conversation
 * awareness, adaptive level, long-press modes, settings, display name) and
 * the long-press modes set here, sent back on connection (the AirPods
 * don't announce them). The AirPods don't echo most
 * changes, so the state is updated right away.
 */

#ifndef CONTROLS_H
#define CONTROLS_H

#include "aap_link.h"
#include "dbus_service.h"
#include "device.h"

typedef struct {
    Device *device;
    AapLink *link;
} Controls;

/* Handle the D-Bus methods that change the AirPods */
void controls_init(Controls *c, Device *device, AapLink *link, DbusService *dbus);

/* Send the long-press modes set in EarPort to the AirPods shortly */
void controls_send_saved_settings(Controls *c, const char *address);

#endif /* CONTROLS_H */
