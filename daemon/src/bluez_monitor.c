/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "bluez_monitor.h"
#include "bluetooth.h"

#include <string.h>

struct BluezMonitor {
    GDBusConnection *connection;
    guint properties_signal_id;
    guint interfaces_added_id;
    guint interfaces_removed_id;

    BluezDeviceCallback connected_callback;
    void *connected_user_data;

    BluezDeviceCallback disconnected_callback;
    void *disconnected_user_data;

    /* Track known devices */
    GHashTable *known_devices;  /* path -> BluezDeviceInfo */
};

void bluez_device_info_free(BluezDeviceInfo *info)
{
    if (info == NULL)
        return;
    g_free(info->address);
    g_free(info->name);
    g_free(info->object_path);
    g_free(info);
}

BluezDeviceInfo *bluez_device_info_copy(const BluezDeviceInfo *info)
{
    if (info == NULL)
        return NULL;

    BluezDeviceInfo *copy = g_new0(BluezDeviceInfo, 1);
    copy->address = g_strdup(info->address);
    copy->name = g_strdup(info->name);
    copy->object_path = g_strdup(info->object_path);
    copy->connected = info->connected;
    copy->paired = info->paired;
    return copy;
}

static bool device_is_airpods(GDBusConnection *connection, const char *object_path)
{
    GError *error = NULL;

    GVariant *result = g_dbus_connection_call_sync(
        connection,
        BLUEZ_SERVICE,
        object_path,
        DBUS_PROPERTIES_INTERFACE,
        "Get",
        g_variant_new("(ss)", BLUEZ_DEVICE_INTERFACE, "UUIDs"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error
    );

    if (error) {
        g_error_free(error);
        return false;
    }

    GVariant *variant = NULL;
    g_variant_get(result, "(v)", &variant);

    bool is_airpods = false;

    if (g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING_ARRAY)) {
        gsize n_uuids = 0;
        const gchar **uuids = g_variant_get_strv(variant, &n_uuids);

        for (gsize i = 0; i < n_uuids; i++) {
            if (g_ascii_strcasecmp(uuids[i], AIRPODS_UUID) == 0) {
                is_airpods = true;
                break;
            }
        }
        g_free(uuids);
    }

    g_variant_unref(variant);
    g_variant_unref(result);

    return is_airpods;
}

static BluezDeviceInfo *get_device_info(GDBusConnection *connection, const char *object_path)
{
    GError *error = NULL;

    GVariant *result = g_dbus_connection_call_sync(
        connection,
        BLUEZ_SERVICE,
        object_path,
        DBUS_PROPERTIES_INTERFACE,
        "GetAll",
        g_variant_new("(s)", BLUEZ_DEVICE_INTERFACE),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error
    );

    if (error) {
        g_warning("Failed to get device properties: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    BluezDeviceInfo *info = g_new0(BluezDeviceInfo, 1);
    info->object_path = g_strdup(object_path);

    GVariant *props = NULL;
    g_variant_get(result, "(@a{sv})", &props);

    GVariant *value;

    value = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
    if (value) {
        info->address = g_variant_dup_string(value, NULL);
        g_variant_unref(value);
    }

    value = g_variant_lookup_value(props, "Name", G_VARIANT_TYPE_STRING);
    if (value) {
        info->name = g_variant_dup_string(value, NULL);
        g_variant_unref(value);
    }

    value = g_variant_lookup_value(props, "Connected", G_VARIANT_TYPE_BOOLEAN);
    if (value) {
        info->connected = g_variant_get_boolean(value);
        g_variant_unref(value);
    }

    value = g_variant_lookup_value(props, "Paired", G_VARIANT_TYPE_BOOLEAN);
    if (value) {
        info->paired = g_variant_get_boolean(value);
        g_variant_unref(value);
    }

    g_variant_unref(props);
    g_variant_unref(result);

    return info;
}

static void on_properties_changed(GDBusConnection *connection,
                                   const gchar *sender_name G_GNUC_UNUSED,
                                   const gchar *object_path,
                                   const gchar *interface_name G_GNUC_UNUSED,
                                   const gchar *signal_name G_GNUC_UNUSED,
                                   GVariant *parameters,
                                   gpointer user_data)
{
    BluezMonitor *monitor = user_data;

    const gchar *iface = NULL;
    GVariant *changed_props = NULL;

    g_variant_get(parameters, "(&s@a{sv}as)", &iface, &changed_props, NULL);

    /* Only care about Device1 interface */
    if (g_strcmp0(iface, BLUEZ_DEVICE_INTERFACE) != 0) {
        g_variant_unref(changed_props);
        return;
    }

    /* Check if Connected property changed */
    GVariant *connected_var = g_variant_lookup_value(changed_props, "Connected", G_VARIANT_TYPE_BOOLEAN);
    if (connected_var == NULL) {
        g_variant_unref(changed_props);
        return;
    }

    bool connected = g_variant_get_boolean(connected_var);
    g_variant_unref(connected_var);
    g_variant_unref(changed_props);

    /* Check if this is an AirPods device */
    if (!device_is_airpods(connection, object_path)) {
        return;
    }

    BluezDeviceInfo *info = get_device_info(connection, object_path);
    if (info == NULL) {
        return;
    }

    g_message("AirPods %s: %s (%s)",
              connected ? "connected" : "disconnected",
              info->name ? info->name : "Unknown",
              info->address ? info->address : "Unknown");

    if (connected) {
        /* Store in known devices */
        g_hash_table_insert(monitor->known_devices,
                            g_strdup(object_path),
                            bluez_device_info_copy(info));

        if (monitor->connected_callback) {
            monitor->connected_callback(info, monitor->connected_user_data);
        }
    } else {
        /* Remove from known devices */
        g_hash_table_remove(monitor->known_devices, object_path);

        if (monitor->disconnected_callback) {
            monitor->disconnected_callback(info, monitor->disconnected_user_data);
        }
    }

    bluez_device_info_free(info);
}

static void on_interfaces_added(GDBusConnection *connection,
                                 const gchar *sender_name G_GNUC_UNUSED,
                                 const gchar *object_path G_GNUC_UNUSED,
                                 const gchar *interface_name G_GNUC_UNUSED,
                                 const gchar *signal_name G_GNUC_UNUSED,
                                 GVariant *parameters,
                                 gpointer user_data)
{
    /* New device appeared - check if it's connected AirPods */
    BluezMonitor *monitor = user_data;

    const gchar *obj_path = NULL;
    GVariant *interfaces = NULL;

    g_variant_get(parameters, "(&o@a{sa{sv}})", &obj_path, &interfaces);

    /* Check if Device1 interface is present */
    if (!g_variant_lookup(interfaces, BLUEZ_DEVICE_INTERFACE, "@a{sv}", NULL)) {
        g_variant_unref(interfaces);
        return;
    }

    g_variant_unref(interfaces);

    /* Check if AirPods and connected */
    if (device_is_airpods(connection, obj_path)) {
        BluezDeviceInfo *info = get_device_info(connection, obj_path);
        if (info && info->connected) {
            g_message("New connected AirPods discovered: %s", info->name);

            g_hash_table_insert(monitor->known_devices,
                                g_strdup(obj_path),
                                bluez_device_info_copy(info));

            if (monitor->connected_callback) {
                monitor->connected_callback(info, monitor->connected_user_data);
            }
        }
        bluez_device_info_free(info);
    }
}

static void on_interfaces_removed(GDBusConnection *connection G_GNUC_UNUSED,
                                   const gchar *sender_name G_GNUC_UNUSED,
                                   const gchar *object_path G_GNUC_UNUSED,
                                   const gchar *interface_name G_GNUC_UNUSED,
                                   const gchar *signal_name G_GNUC_UNUSED,
                                   GVariant *parameters,
                                   gpointer user_data)
{
    BluezMonitor *monitor = user_data;

    const gchar *obj_path = NULL;
    g_variant_get(parameters, "(&oas)", &obj_path, NULL);

    /* Check if we were tracking this device */
    BluezDeviceInfo *info = g_hash_table_lookup(monitor->known_devices, obj_path);
    if (info) {
        g_message("AirPods device removed: %s", info->name);

        if (monitor->disconnected_callback) {
            monitor->disconnected_callback(info, monitor->disconnected_user_data);
        }

        g_hash_table_remove(monitor->known_devices, obj_path);
    }
}

BluezMonitor *bluez_monitor_new(void)
{
    GError *error = NULL;

    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (error) {
        g_warning("Failed to connect to system bus: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    BluezMonitor *monitor = g_new0(BluezMonitor, 1);
    monitor->connection = connection;
    monitor->known_devices = g_hash_table_new_full(
        g_str_hash, g_str_equal,
        g_free, (GDestroyNotify)bluez_device_info_free
    );

    return monitor;
}

void bluez_monitor_free(BluezMonitor *monitor)
{
    if (monitor == NULL)
        return;

    bluez_monitor_stop(monitor);
    g_hash_table_destroy(monitor->known_devices);
    g_object_unref(monitor->connection);
    g_free(monitor);
}

bool bluez_monitor_start(BluezMonitor *monitor)
{
    /* Subscribe to PropertiesChanged signal */
    monitor->properties_signal_id = g_dbus_connection_signal_subscribe(
        monitor->connection,
        BLUEZ_SERVICE,
        DBUS_PROPERTIES_INTERFACE,
        "PropertiesChanged",
        NULL,  /* Match all object paths */
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_properties_changed,
        monitor,
        NULL
    );

    /* Subscribe to InterfacesAdded signal */
    monitor->interfaces_added_id = g_dbus_connection_signal_subscribe(
        monitor->connection,
        BLUEZ_SERVICE,
        DBUS_OBJECT_MANAGER_INTERFACE,
        "InterfacesAdded",
        "/",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_interfaces_added,
        monitor,
        NULL
    );

    /* Subscribe to InterfacesRemoved signal */
    monitor->interfaces_removed_id = g_dbus_connection_signal_subscribe(
        monitor->connection,
        BLUEZ_SERVICE,
        DBUS_OBJECT_MANAGER_INTERFACE,
        "InterfacesRemoved",
        "/",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_interfaces_removed,
        monitor,
        NULL
    );

    g_message("BlueZ monitor started");
    return true;
}

void bluez_monitor_stop(BluezMonitor *monitor)
{
    if (monitor->properties_signal_id > 0) {
        g_dbus_connection_signal_unsubscribe(monitor->connection, monitor->properties_signal_id);
        monitor->properties_signal_id = 0;
    }

    if (monitor->interfaces_added_id > 0) {
        g_dbus_connection_signal_unsubscribe(monitor->connection, monitor->interfaces_added_id);
        monitor->interfaces_added_id = 0;
    }

    if (monitor->interfaces_removed_id > 0) {
        g_dbus_connection_signal_unsubscribe(monitor->connection, monitor->interfaces_removed_id);
        monitor->interfaces_removed_id = 0;
    }
}

void bluez_monitor_set_connected_callback(BluezMonitor *monitor,
                                           BluezDeviceCallback callback,
                                           void *user_data)
{
    monitor->connected_callback = callback;
    monitor->connected_user_data = user_data;
}

void bluez_monitor_set_disconnected_callback(BluezMonitor *monitor,
                                              BluezDeviceCallback callback,
                                              void *user_data)
{
    monitor->disconnected_callback = callback;
    monitor->disconnected_user_data = user_data;
}

void bluez_monitor_check_existing_devices(BluezMonitor *monitor)
{
    GError *error = NULL;

    /* Call GetManagedObjects to enumerate all devices */
    GVariant *result = g_dbus_connection_call_sync(
        monitor->connection,
        BLUEZ_SERVICE,
        "/",
        DBUS_OBJECT_MANAGER_INTERFACE,
        "GetManagedObjects",
        NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error
    );

    if (error) {
        g_warning("Failed to get managed objects: %s", error->message);
        g_error_free(error);
        return;
    }

    GVariant *objects = NULL;
    g_variant_get(result, "(@a{oa{sa{sv}}})", &objects);

    GVariantIter iter;
    const gchar *object_path;
    GVariant *interfaces;

    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &object_path, &interfaces)) {
        /* Check if this object has Device1 interface */
        if (g_variant_lookup(interfaces, BLUEZ_DEVICE_INTERFACE, "@a{sv}", NULL)) {
            /* Check if it's AirPods */
            if (device_is_airpods(monitor->connection, object_path)) {
                BluezDeviceInfo *info = get_device_info(monitor->connection, object_path);
                if (info && info->connected) {
                    g_message("Found already connected AirPods: %s (%s)",
                              info->name ? info->name : "Unknown",
                              info->address ? info->address : "Unknown");

                    g_hash_table_insert(monitor->known_devices,
                                        g_strdup(object_path),
                                        bluez_device_info_copy(info));

                    if (monitor->connected_callback) {
                        monitor->connected_callback(info, monitor->connected_user_data);
                    }
                }
                bluez_device_info_free(info);
            }
        }
        g_variant_unref(interfaces);
    }

    g_variant_unref(objects);
    g_variant_unref(result);
}
