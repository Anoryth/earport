/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Fake org.bluez on a private bus standing in for the system bus. It
 * answers from its own thread, as the daemon calls BlueZ synchronously in
 * places. Its behaviour follows BlueZ's sources, not a capture.
 */

#ifndef FAKE_BLUEZ_H
#define FAKE_BLUEZ_H

#include <gio/gio.h>
#include <stdbool.h>

#define FAKE_ADAPTER_PATH "/org/bluez/hci0"

/* Starts the bus, points DBUS_SYSTEM_BUS_ADDRESS to it and owns org.bluez */
void fake_bluez_start(void);
void fake_bluez_stop(void);

/* Forgets the devices, the calls and the failures, once the calls in
 * flight have arrived */
void fake_bluez_reset(void);

/* A device under hci0, path "/org/bluez/hci0/dev_XX_XX_...". Announced with
 * InterfacesAdded when 'announce'. Returns its path (owned by the fake). */
const char *fake_bluez_add_device(const char *address, const char *name, bool airpods,
                                  bool connected, bool announce);

/* InterfacesAdded for a device, with its current properties */
void fake_bluez_announce(const char *path);

/* Sets a Device1 property (the value is sunk), with PropertiesChanged when
 * 'emit' */
void fake_bluez_set(const char *path, const char *property, GVariant *value, bool emit);

/* InterfacesRemoved; the device goes away when Device1 is in the list */
void fake_bluez_remove_interfaces(const char *path, const char *const *interfaces);

/* Method calls received: Connect, StartDiscovery, RegisterBatteryProvider...
 * and "Get" for Properties.Get, with (object path, property) */
int fake_bluez_calls(const char *method);
/* Parameters of the last one, NULL if none (free with g_variant_unref) */
GVariant *fake_bluez_last_call(const char *method);
/* Waits up to 2 s for 'count' calls; returns the number received */
int fake_bluez_wait_calls(const char *method, int count);

/* The next calls of this method fail with org.bluez.Error.Failed */
void fake_bluez_fail(const char *method, bool fail);
/* The next 'times' calls (-1: all, 0: none any more) fail with this error,
 * e.g. "org.bluez.Error.InProgress" */
void fake_bluez_fail_with(const char *method, const char *error, int times);

/* Registrations of a kind, "BatteryProvider" or "Profile": Register<kind>
 * answers AlreadyExists for the same sender and path, as BlueZ does */
int fake_bluez_registered(const char *kind);
/* Waits up to 2 s for exactly 'count' of them; returns the number */
int fake_bluez_wait_registered(const char *kind, int count);

/* Any other signal from BlueZ (the parameters are sunk) */
void fake_bluez_emit(const char *path, const char *interface, const char *signal,
                     GVariant *parameters);

/* bluetoothd restarts: the name goes away and comes back, the battery
 * providers are forgotten */
void fake_bluez_restart(void);

/* Round trip to the fake: the signal subscriptions sent before apply, and
 * the calls sent before are counted */
void fake_bluez_sync(void);

/* fake_bluez_sync, then the answers it overtook are dispatched: what was
 * sent before has had its effect */
void fake_bluez_settle(void);

/* Properties.Get answered this late (0: at once), to catch an answer in
 * flight */
void fake_bluez_delay_get(guint ms);

/* Runs the default main context for a while */
void fake_bluez_run(guint ms);

#endif /* FAKE_BLUEZ_H */
