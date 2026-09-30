/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * The AAP link to the AirPods connected through BlueZ: L2CAP connection,
 * handshake, and keeping the link useful. The AirPods often refuse the
 * first L2CAP connect, and sometimes ignore our notification request
 * while another Apple device is connected: retry both, and re-open the
 * link once without telling clients if that is not enough.
 */

#ifndef AAP_LINK_H
#define AAP_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "aap_protocol.h"

typedef struct {
    /* A new session started (not called when the link is silently re-opened) */
    void (*connected)(const char *address, const char *name, void *user_data);
    /* The session ended: clients must be told and the state forgotten */
    void (*disconnected)(void *user_data);
    /* A packet the parser understood */
    void (*packet)(const AapParsedPacket *packet, void *user_data);
} AapLinkCallbacks;

typedef struct AapLink AapLink;

AapLink *aap_link_new(const AapLinkCallbacks *callbacks, void *user_data);
void aap_link_free(AapLink *link);

/* BlueZ reports the AirPods connected: open the link */
void aap_link_device_connected(AapLink *link, const char *address, const char *name);

/* BlueZ reports the AirPods gone: close the link, stop retrying */
void aap_link_device_disconnected(AapLink *link);

bool aap_link_is_connected(AapLink *link);

/* Send a packet; false when the link is down */
bool aap_link_send(AapLink *link, const uint8_t *data, size_t len);

#endif /* AAP_LINK_H */
