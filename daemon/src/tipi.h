/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Sharing the AirPods with an Apple device, like a Mac (once they take this
 * computer for one, see apple_identity.h). The AirPods only keep two hosts
 * together when one of them listed both; when the primary pod changes they
 * keep the host playing and drop the other, which has to come back by
 * itself. So:
 * - another device connected: list this computer and it, the one playing
 *   first, again when that changes or when the AirPods forget it;
 * - playback starts here: tell the AirPods this computer plays;
 * - dropped when the primary pod came out, the other one still worn,
 *   while another device played: connect again shortly, as an iPhone does.
 *   Not when another device takes them otherwise: the user chose it.
 * Sending and connecting are left to the callbacks.
 */

#ifndef TIPI_H
#define TIPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aap_protocol.h"

typedef struct {
    /* Send a packet to the AirPods; false when not connected */
    bool (*send)(const uint8_t *data, size_t len, void *user_data);
    /* Connect the AirPods again */
    void (*reconnect)(void *user_data);
    /* Connecting them again after a drop, until the link opens or the
     * attempts give up (optional) */
    void (*reconnecting)(bool reconnecting, void *user_data);
} TipiCallbacks;

typedef struct Tipi Tipi;

Tipi *tipi_new(const TipiCallbacks *callbacks, void *user_data);
void tipi_free(Tipi *tipi);

void tipi_set_enabled(Tipi *tipi, bool enabled);

/* The link to the AirPods opened: a new session; call once it is
 * announced, the end of a reconnection follows */
void tipi_link_opened(Tipi *tipi);

/* The hosts table, with this computer's address (empty when unknown) */
void tipi_hosts_changed(Tipi *tipi, const AapHosts *hosts, const char *my_address);

/* Ear detection, as the AirPods report it (primary and secondary pod) */
void tipi_ear_changed(Tipi *tipi, bool primary_in, bool secondary_in);

/* A player started playing here */
void tipi_playback_started(Tipi *tipi);

/* The link closed; left: by the AirPods (closed, or lost: they stop
 * answering when a third device comes in), not by this computer */
void tipi_link_closed(Tipi *tipi, bool left);

#endif /* TIPI_H */
