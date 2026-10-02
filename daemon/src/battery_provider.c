/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "battery_provider.h"
#include "bluez_monitor.h"
#include "bluez-battery-provider.h"

#include <gio/gio.h>
#include <stdbool.h>

#define PROVIDER_ROOT "/io/github/anoryth/EarPort/BatteryProvider"

struct BatteryProvider {
    GDBusConnection *bus;
    GDBusObjectManagerServer *manager;
    GCancellable *cancellable;
    guint bluez_watch_id;
    char *adapter_path;         /* Adapter to register with */
    bool registering;           /* Registered, or the call is pending */
    char *object_path;          /* Exported battery, NULL if none */
    BluezBatteryProvider1 *battery;
};

static void on_register_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (reply != NULL) {
        g_message("Battery published to BlueZ");
        g_variant_unref(reply);
        return;
    }

    /* The provider may be gone: don't touch it */
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return;
    }

    /* Older BlueZ without the API: GNOME just won't show it. Tried again
     * with the next battery level. */
    BatteryProvider *bp = user_data;
    bp->registering = false;
    g_message("Could not publish the battery to BlueZ: %s", error->message);
    g_error_free(error);
}

static void register_with(BatteryProvider *bp, const char *adapter_path)
{
    if (bp->registering && g_strcmp0(bp->adapter_path, adapter_path) == 0)
        return;

    g_free(bp->adapter_path);
    bp->adapter_path = g_strdup(adapter_path);
    bp->registering = true;
    g_dbus_connection_call(bp->bus, "org.bluez", adapter_path,
                           "org.bluez.BatteryProviderManager1", "RegisterBatteryProvider",
                           g_variant_new("(o)", PROVIDER_ROOT), NULL, G_DBUS_CALL_FLAGS_NONE,
                           5000, bp->cancellable, on_register_done, bp);
}

/* bluetoothd restarted: it forgot the providers */
static void on_bluez_appeared(GDBusConnection *bus, const gchar *name, const gchar *owner,
                              gpointer user_data)
{
    (void)bus;
    (void)name;
    (void)owner;
    BatteryProvider *bp = user_data;
    bp->registering = false;
    if (bp->battery != NULL && bp->adapter_path != NULL)
        register_with(bp, bp->adapter_path);
}

static void remove_battery(BatteryProvider *bp)
{
    if (bp->object_path == NULL)
        return;

    g_dbus_object_manager_server_unexport(bp->manager, bp->object_path);
    g_clear_pointer(&bp->object_path, g_free);
    g_clear_object(&bp->battery);
}

BatteryProvider *battery_provider_new(void)
{
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus == NULL) {
        g_message("No system bus, the battery won't show in GNOME: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    BatteryProvider *bp = g_new0(BatteryProvider, 1);
    bp->bus = bus;
    bp->cancellable = g_cancellable_new();
    bp->manager = g_dbus_object_manager_server_new(PROVIDER_ROOT);
    g_dbus_object_manager_server_set_connection(bp->manager, bus);
    bp->bluez_watch_id = g_bus_watch_name_on_connection(bus, "org.bluez",
                                                        G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                        on_bluez_appeared, NULL, bp, NULL);
    return bp;
}

void battery_provider_free(BatteryProvider *bp)
{
    if (bp == NULL)
        return;

    g_cancellable_cancel(bp->cancellable);
    g_bus_unwatch_name(bp->bluez_watch_id);
    remove_battery(bp);
    g_object_unref(bp->manager);
    g_object_unref(bp->cancellable);
    g_object_unref(bp->bus);
    g_free(bp->adapter_path);
    g_free(bp);
}

void battery_provider_update(BatteryProvider *bp, const char *adapter_path,
                             const char *address, int percentage)
{
    if (bp == NULL)
        return;

    if (percentage < 0 || adapter_path == NULL || address == NULL) {
        remove_battery(bp);
        return;
    }

    g_autofree char *device = g_strdup(address);
    g_strdelimit(device, ":", '_');
    g_autofree char *object_path = g_strdup_printf("%s/dev_%s", PROVIDER_ROOT, device);
    g_autofree char *device_path = bluez_device_path(adapter_path, address);

    /* Other AirPods than last time */
    if (g_strcmp0(bp->object_path, object_path) != 0)
        remove_battery(bp);

    if (bp->battery == NULL) {
        bp->battery = bluez_battery_provider1_skeleton_new();
        bluez_battery_provider1_set_device(bp->battery, device_path);
        bluez_battery_provider1_set_source(bp->battery, "EarPort");
        bluez_battery_provider1_set_percentage(bp->battery, (guchar)percentage);

        GDBusObjectSkeleton *object = g_dbus_object_skeleton_new(object_path);
        g_dbus_object_skeleton_add_interface(object, G_DBUS_INTERFACE_SKELETON(bp->battery));
        g_dbus_object_manager_server_export(bp->manager, object);
        g_object_unref(object);
        bp->object_path = g_strdup(object_path);
    } else {
        bluez_battery_provider1_set_percentage(bp->battery, (guchar)percentage);
    }

    register_with(bp, adapter_path);
}
