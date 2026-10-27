/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "bluez_monitor.h"
#include "bluetooth.h"

#include <string.h>

/* These calls are synchronous: if bluetoothd stops answering, give up
 * after a few seconds instead of the default 25 s */
#define BLUEZ_CALL_TIMEOUT_MS 5000

/* A connection under way is done by then */
#ifndef BLUEZ_BUSY_SETTLE_MS
#define BLUEZ_BUSY_SETTLE_MS 3000
#endif
#define BLUEZ_CONNECT_TIMEOUT_MS 30000

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

    /* The AirPods reported to the callbacks, kept after they left: with
     * several pairs paired, the others are ignored while these are here */
    char *followed_path;
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

static void report_connected(BluezMonitor *monitor, const BluezDeviceInfo *info)
{
    if (monitor->followed_path != NULL && g_strcmp0(info->object_path, monitor->followed_path) != 0 &&
        g_hash_table_contains(monitor->known_devices, monitor->followed_path)) {
        g_message("Other AirPods connected, ignored: %s", info->name ? info->name : "Unknown");
        return;
    }
    g_free(monitor->followed_path);
    monitor->followed_path = g_strdup(info->object_path);
    if (monitor->connected_callback)
        monitor->connected_callback(info, monitor->connected_user_data);
}

static void report_disconnected(BluezMonitor *monitor, const BluezDeviceInfo *info)
{
    if (g_strcmp0(info->object_path, monitor->followed_path) != 0)
        return;
    if (monitor->disconnected_callback)
        monitor->disconnected_callback(info, monitor->disconnected_user_data);
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
        BLUEZ_CALL_TIMEOUT_MS,
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
        BLUEZ_CALL_TIMEOUT_MS,
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

    GVariant *connected_var = g_variant_lookup_value(changed_props, "Connected", G_VARIANT_TYPE_BOOLEAN);
    /* On a first pairing, BlueZ reports the connection before the services:
     * the AirPods are only recognised when their UUIDs come */
    bool uuids_changed = g_variant_lookup(changed_props, "UUIDs", "@as", NULL);
    g_variant_unref(changed_props);
    if (connected_var == NULL &&
        (!uuids_changed || g_hash_table_contains(monitor->known_devices, object_path)))
        return;

    bool connected = connected_var == NULL || g_variant_get_boolean(connected_var);
    if (connected_var != NULL)
        g_variant_unref(connected_var);

    /* Check if this is an AirPods device */
    if (!device_is_airpods(connection, object_path)) {
        return;
    }

    BluezDeviceInfo *info = get_device_info(connection, object_path);
    if (info == NULL) {
        return;
    }
    if (connected_var == NULL && !info->connected) {
        bluez_device_info_free(info);
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

        report_connected(monitor, info);
    } else {
        /* Remove from known devices */
        g_hash_table_remove(monitor->known_devices, object_path);

        report_disconnected(monitor, info);
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

            report_connected(monitor, info);
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
    g_autofree const gchar **interfaces = NULL;
    g_variant_get(parameters, "(&o^a&s)", &obj_path, &interfaces);

    /* Battery1 goes away when our battery provider drops it: not a departure */
    if (!g_strv_contains(interfaces, "org.bluez.Device1"))
        return;

    /* Check if we were tracking this device */
    BluezDeviceInfo *info = g_hash_table_lookup(monitor->known_devices, obj_path);
    if (info) {
        g_message("AirPods device removed: %s", info->name);

        report_disconnected(monitor, info);

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
    g_free(monitor->followed_path);
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
        BLUEZ_CALL_TIMEOUT_MS,
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

                    report_connected(monitor, info);
                }
                bluez_device_info_free(info);
            }
        }
        g_variant_unref(interfaces);
    }

    g_variant_unref(objects);
    g_variant_unref(result);
}

char *bluez_device_path(const char *adapter_path, const char *address)
{
    char *device = g_strdup(address);
    g_strdelimit(device, ":", '_');
    char *path = g_strdup_printf("%s/dev_%s", adapter_path, device);
    g_free(device);
    return path;
}

typedef struct {
    GDBusConnection *bus;
    char *path;
    BluezConnectCallback callback;
    void *user_data;
    bool retried;
} ConnectRequest;

static void connect_finish(ConnectRequest *request, bool connected, const char *error)
{
    if (request->callback != NULL)
        request->callback(connected, error, request->user_data);
    g_object_unref(request->bus);
    g_free(request->path);
    g_free(request);
}

static void connect_call(ConnectRequest *request);

static void on_stuck_disconnected(GObject *source, GAsyncResult *res, gpointer user_data)
{
    /* Whatever it answers: try again */
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, NULL);
    if (reply != NULL)
        g_variant_unref(reply);
    connect_call(user_data);
}

static void on_connected_read(GObject *source, GAsyncResult *res, gpointer user_data)
{
    ConnectRequest *request = user_data;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, NULL);
    gboolean connected = FALSE;
    if (reply != NULL) {
        GVariant *value;
        g_variant_get(reply, "(v)", &value);
        connected = g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) &&
                    g_variant_get_boolean(value);
        g_variant_unref(value);
        g_variant_unref(reply);
    }
    if (connected) {
        connect_finish(request, true, NULL);
        return;
    }

    g_message("BlueZ is stuck connecting the AirPods: cancelling, then trying again");
    request->retried = true;
    g_dbus_connection_call(request->bus, BLUEZ_SERVICE, request->path, BLUEZ_DEVICE_INTERFACE,
                           "Disconnect", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                           BLUEZ_CALL_TIMEOUT_MS, NULL, on_stuck_disconnected, request);
}

static gboolean on_busy_settled(gpointer user_data)
{
    ConnectRequest *request = user_data;
    g_dbus_connection_call(request->bus, BLUEZ_SERVICE, request->path, DBUS_PROPERTIES_INTERFACE,
                           "Get", g_variant_new("(ss)", BLUEZ_DEVICE_INTERFACE, "Connected"),
                           G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, BLUEZ_CALL_TIMEOUT_MS,
                           NULL, on_connected_read, request);
    return G_SOURCE_REMOVE;
}

static void on_connect_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    ConnectRequest *request = user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (reply != NULL) {
        g_variant_unref(reply);
        connect_finish(request, true, NULL);
        return;
    }

    g_autofree char *remote = g_dbus_error_get_remote_error(error);
    if (!request->retried && g_strcmp0(remote, "org.bluez.Error.InProgress") == 0) {
        g_error_free(error);
        g_timeout_add(BLUEZ_BUSY_SETTLE_MS, on_busy_settled, request);
        return;
    }
    connect_finish(request, false, error->message);
    g_error_free(error);
}

static void connect_call(ConnectRequest *request)
{
    g_dbus_connection_call(request->bus, BLUEZ_SERVICE, request->path, BLUEZ_DEVICE_INTERFACE,
                           "Connect", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                           BLUEZ_CONNECT_TIMEOUT_MS, NULL, on_connect_done, request);
}

void bluez_device_connect(const char *device_path, BluezConnectCallback callback,
                          void *user_data)
{
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus == NULL) {
        if (callback != NULL)
            callback(false, error->message, user_data);
        g_error_free(error);
        return;
    }

    ConnectRequest *request = g_new0(ConnectRequest, 1);
    request->bus = bus;
    request->path = g_strdup(device_path);
    request->callback = callback;
    request->user_data = user_data;
    connect_call(request);
}
