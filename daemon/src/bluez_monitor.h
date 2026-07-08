/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * BlueZ D-Bus monitoring for AirPods detection
 */

#ifndef BLUEZ_MONITOR_H
#define BLUEZ_MONITOR_H

#include <glib.h>
#include <gio/gio.h>
#include <stdbool.h>

/* BlueZ D-Bus constants */
#define BLUEZ_SERVICE           "org.bluez"
#define BLUEZ_ADAPTER_INTERFACE "org.bluez.Adapter1"
#define BLUEZ_DEVICE_INTERFACE  "org.bluez.Device1"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define DBUS_OBJECT_MANAGER_INTERFACE "org.freedesktop.DBus.ObjectManager"

/* Device info from BlueZ */
typedef struct {
    char *address;
    char *name;
    char *object_path;
    bool connected;
    bool paired;
} BluezDeviceInfo;

/* Callback types */
typedef void (*BluezDeviceCallback)(const BluezDeviceInfo *device, void *user_data);

/* BlueZ monitor context */
typedef struct BluezMonitor BluezMonitor;

/**
 * Create a new BlueZ monitor
 *
 * @return New monitor or NULL on error
 */
BluezMonitor *bluez_monitor_new(void);

/**
 * Free BlueZ monitor
 */
void bluez_monitor_free(BluezMonitor *monitor);

/**
 * Start monitoring for AirPods devices
 *
 * @param monitor Monitor context
 * @return true on success
 */
bool bluez_monitor_start(BluezMonitor *monitor);

/**
 * Stop monitoring
 */
void bluez_monitor_stop(BluezMonitor *monitor);

/**
 * Set callback for device connected events
 */
void bluez_monitor_set_connected_callback(BluezMonitor *monitor,
                                           BluezDeviceCallback callback,
                                           void *user_data);

/**
 * Set callback for device disconnected events
 */
void bluez_monitor_set_disconnected_callback(BluezMonitor *monitor,
                                              BluezDeviceCallback callback,
                                              void *user_data);

/**
 * Check for already connected AirPods devices
 * Will trigger connected callback for each found device
 */
void bluez_monitor_check_existing_devices(BluezMonitor *monitor);

/**
 * Free device info structure
 */
void bluez_device_info_free(BluezDeviceInfo *info);

/**
 * Copy device info structure
 */
BluezDeviceInfo *bluez_device_info_copy(const BluezDeviceInfo *info);

#endif /* BLUEZ_MONITOR_H */
