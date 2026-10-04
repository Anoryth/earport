/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Presents this computer as an Apple device to the AirPods: an Apple "PnP
 * Information" (Device ID) SDP record, published through BlueZ by the
 * service itself while enabled. The AirPods read it on each connection and
 * then switch on their own between this computer and the user's Apple
 * devices. No root and no change to the system Bluetooth configuration: the
 * record goes away when the option is turned off or the service stops.
 */

#ifndef APPLE_IDENTITY_H
#define APPLE_IDENTITY_H

#include <stdbool.h>

typedef struct AppleIdentity AppleIdentity;

AppleIdentity *apple_identity_new(void);
void apple_identity_free(AppleIdentity *ai);

/* Publish or withdraw the record; the AirPods notice on their next
 * connection */
void apple_identity_set_enabled(AppleIdentity *ai, bool enabled);

#endif /* APPLE_IDENTITY_H */
