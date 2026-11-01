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
    guint saved_settings_id;     /* Pending send of the long-press modes */
    guint adaptive_check_id;     /* Pending check that Adaptive took effect */
    bool adaptive_resent;        /* Features and request sent again once */
    /* A setting was sent to the AirPods (they don't echo it), optional */
    void (*setting_sent)(uint8_t id, uint8_t value, void *user_data);
    /* The display name was changed, optional */
    void (*display_name_changed)(void *user_data);
    void *user_data;
} Controls;

/* Handle the D-Bus methods that change the AirPods */
void controls_init(Controls *c, Device *device, AapLink *link, DbusService *dbus);

/* Send the long-press modes set in EarPort to the AirPods shortly */
void controls_send_saved_settings(Controls *c, const char *address);

/* Conversation Awareness would lower the volume at each word said into the
 * HD microphone: turned off while it records, back on when it stops. The
 * AirPods keep it off through a disconnection or a daemon restart, so the
 * profile remembers to turn it back on (controls_restore_ca on connection). */
void controls_mic_capture_changed(Controls *c, bool capturing);
void controls_restore_ca(Controls *c);

/* Cancel what is still pending */
void controls_cleanup(Controls *c);

#endif /* CONTROLS_H */
