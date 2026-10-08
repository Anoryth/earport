/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Reconnects the AirPods on purpose, a few times in a row: they only take
 * this computer for an Apple device (or stop doing so) once they read its
 * new identity, at the second connection after the change. Disconnecting
 * and connecting are left to the callbacks.
 */

#ifndef RELINK_H
#define RELINK_H

#include <stdbool.h>

typedef struct {
    /* Disconnect the AirPods from this computer */
    void (*disconnect)(void *user_data);
    /* Connect them */
    void (*connect)(void *user_data);
    /* Reconnecting them, until done or given up */
    void (*reconnecting)(bool reconnecting, void *user_data);
} RelinkCallbacks;

typedef struct Relink Relink;

Relink *relink_new(const RelinkCallbacks *callbacks, void *user_data);
void relink_free(Relink *relink);

/* Reconnect them this many times, starting shortly (call while connected) */
void relink_start(Relink *relink, int times);

/* BlueZ answered the disconnect callback's request */
void relink_disconnect_done(Relink *relink);

/* BlueZ answered the connect callback's request */
void relink_connect_done(Relink *relink, bool connected);

/* The link to the AirPods opened (once announced) or closed */
void relink_link_opened(Relink *relink);
void relink_link_closed(Relink *relink);

#endif /* RELINK_H */
