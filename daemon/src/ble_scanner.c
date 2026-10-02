/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "ble_scanner.h"
#include "ble_proximity.h"
#include <gio/gio.h>
#include <string.h>

#define BLUEZ_SERVICE "org.bluez"
#define ADAPTER_INTERFACE "org.bluez.Adapter1"
#define DEVICE_INTERFACE "org.bluez.Device1"

/* Far away devices are not ours to connect to */
#define DISCOVERY_RSSI_THRESHOLD -80

struct BleScanner {
    GDBusConnection *bus;
    char *adapter_path;
    guint added_id;
    guint changed_id;
    bool running;

    BleAddressFilter filter;
    BleAdvertCallback callback;
    void *user_data;

    GHashTable *matches;    /* address -> GINT_TO_POINTER(matches filter) */
    GCancellable *cancellable;
};

/* "/org/bluez/hci0/dev_70_7A_EC_AE_5F_F8" -> "70:7A:EC:AE:5F:F8" */
static char *address_from_path(const char *path)
{
    const char *dev = strrchr(path, '/');
    if (dev == NULL || !g_str_has_prefix(dev, "/dev_") || strlen(dev + 5) != 17)
        return NULL;

    char *address = g_strdup(dev + 5);
    g_strdelimit(address, "_", ':');
    return address;
}

/* Cached decision for an address: -1 when it was not seen yet */
static int cached_match(BleScanner *scanner, const char *address)
{
    gpointer cached;
    if (g_hash_table_lookup_extended(scanner->matches, address, NULL, &cached))
        return GPOINTER_TO_INT(cached);
    return -1;
}

/* Report the Apple entry of a ManufacturerData (a{qv}) value, if the
 * address is one to report */
static void report_manufacturer_data(BleScanner *scanner, const char *address, GVariant *data)
{
    GVariantIter iter;
    guint16 company;
    GVariant *value;
    g_variant_iter_init(&iter, data);
    while (g_variant_iter_next(&iter, "{qv}", &company, &value)) {
        if (company == APPLE_COMPANY_ID && g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING)) {
            gsize len;
            const guint8 *bytes = g_variant_get_fixed_array(value, &len, 1);
            int match = cached_match(scanner, address);
            if (match < 0) {
                match = scanner->filter(address, bytes, len, scanner->user_data);
                g_hash_table_insert(scanner->matches, g_strdup(address), GINT_TO_POINTER(match));
            }
            if (match)
                scanner->callback(address, bytes, len, scanner->user_data);
        }
        g_variant_unref(value);
    }
}

/* Pending ManufacturerData read; the scanner may be freed before it ends */
typedef struct {
    BleScanner *scanner;
    GCancellable *cancellable;  /* Cancelled when the scanner is freed */
    char *address;
} DataRequest;

static void on_manufacturer_data_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    DataRequest *request = user_data;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, NULL);

    /* A reply may still arrive after cancellation: check before use */
    if (result != NULL && !g_cancellable_is_cancelled(request->cancellable) &&
        request->scanner->running) {
        GVariant *value;
        g_variant_get(result, "(v)", &value);
        report_manufacturer_data(request->scanner, request->address, value);
        g_variant_unref(value);
    }

    g_clear_pointer(&result, g_variant_unref);
    g_object_unref(request->cancellable);
    g_free(request->address);
    g_free(request);
}

static void on_interfaces_added(GDBusConnection *bus G_GNUC_UNUSED,
                                const gchar *sender G_GNUC_UNUSED,
                                const gchar *object_path G_GNUC_UNUSED,
                                const gchar *interface G_GNUC_UNUSED,
                                const gchar *signal G_GNUC_UNUSED,
                                GVariant *parameters,
                                gpointer user_data)
{
    BleScanner *scanner = user_data;
    const gchar *path;
    GVariant *interfaces;

    if (!scanner->running)
        return;

    g_variant_get(parameters, "(&o@a{sa{sv}})", &path, &interfaces);
    GVariant *device = g_variant_lookup_value(interfaces, DEVICE_INTERFACE, NULL);
    char *address = address_from_path(path);

    if (device != NULL && address != NULL) {
        GVariant *data = g_variant_lookup_value(device, "ManufacturerData", NULL);
        if (data != NULL) {
            report_manufacturer_data(scanner, address, data);
            g_variant_unref(data);
        }
    }

    g_free(address);
    g_clear_pointer(&device, g_variant_unref);
    g_variant_unref(interfaces);
}

static void on_properties_changed(GDBusConnection *bus,
                                  const gchar *sender G_GNUC_UNUSED,
                                  const gchar *object_path,
                                  const gchar *interface G_GNUC_UNUSED,
                                  const gchar *signal G_GNUC_UNUSED,
                                  GVariant *parameters,
                                  gpointer user_data)
{
    BleScanner *scanner = user_data;
    GVariant *changed;

    if (!scanner->running || !g_str_has_prefix(object_path, scanner->adapter_path))
        return;

    char *address = address_from_path(object_path);
    if (address == NULL || cached_match(scanner, address) == 0) {
        g_free(address);
        return;
    }

    g_variant_get(parameters, "(&s@a{sv}@as)", NULL, &changed, NULL);
    GVariant *data = g_variant_lookup_value(changed, "ManufacturerData", NULL);

    if (data != NULL) {
        report_manufacturer_data(scanner, address, data);
        g_variant_unref(data);
        g_free(address);
    } else if (cached_match(scanner, address) > 0 &&
               g_variant_lookup(changed, "RSSI", "n", NULL)) {
        /* Same advert again (only the RSSI changed): its data is still
         * current, fetch it so that each advert gives a fresh state */
        DataRequest *request = g_new0(DataRequest, 1);
        request->scanner = scanner;
        request->cancellable = g_object_ref(scanner->cancellable);
        request->address = address;
        g_dbus_connection_call(bus, BLUEZ_SERVICE, object_path,
                               "org.freedesktop.DBus.Properties", "Get",
                               g_variant_new("(ss)", DEVICE_INTERFACE, "ManufacturerData"),
                               G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1,
                               scanner->cancellable, on_manufacturer_data_ready, request);
    } else {
        g_free(address);
    }

    g_variant_unref(changed);
}

BleScanner *ble_scanner_new(const char *adapter_path,
                            BleAddressFilter filter,
                            BleAdvertCallback callback,
                            void *user_data)
{
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus == NULL) {
        g_warning("BLE scanner: no system bus: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    BleScanner *scanner = g_new0(BleScanner, 1);
    scanner->bus = bus;
    scanner->adapter_path = g_strdup(adapter_path);
    scanner->filter = filter;
    scanner->callback = callback;
    scanner->user_data = user_data;
    scanner->matches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    scanner->cancellable = g_cancellable_new();

    scanner->added_id = g_dbus_connection_signal_subscribe(
        bus, BLUEZ_SERVICE, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
        "/", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_interfaces_added, scanner, NULL);
    scanner->changed_id = g_dbus_connection_signal_subscribe(
        bus, BLUEZ_SERVICE, "org.freedesktop.DBus.Properties", "PropertiesChanged",
        NULL, DEVICE_INTERFACE, G_DBUS_SIGNAL_FLAGS_NONE, on_properties_changed, scanner, NULL);

    return scanner;
}

void ble_scanner_free(BleScanner *scanner)
{
    if (scanner == NULL)
        return;

    ble_scanner_stop(scanner);
    g_cancellable_cancel(scanner->cancellable);
    g_object_unref(scanner->cancellable);
    g_dbus_connection_signal_unsubscribe(scanner->bus, scanner->added_id);
    g_dbus_connection_signal_unsubscribe(scanner->bus, scanner->changed_id);
    g_object_unref(scanner->bus);
    g_hash_table_destroy(scanner->matches);
    g_free(scanner->adapter_path);
    g_free(scanner);
}

static void on_discovery_call_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    const char *method = user_data;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (result != NULL) {
        g_variant_unref(result);
    } else {
        /* StopDiscovery fails harmlessly if BlueZ already stopped it */
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_debug("BLE scanner: %s failed: %s", method, error->message);
        g_error_free(error);
    }
}

void ble_scanner_start(BleScanner *scanner)
{
    if (scanner == NULL || scanner->running)
        return;

    GVariantBuilder filter;
    g_variant_builder_init(&filter, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&filter, "{sv}", "Transport", g_variant_new_string("le"));
    /* Report every advert: a state that did not change must still be seen
     * as fresh when playback starts */
    g_variant_builder_add(&filter, "{sv}", "DuplicateData", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&filter, "{sv}", "RSSI", g_variant_new_int16(DISCOVERY_RSSI_THRESHOLD));

    /* Both calls go through the same connection in order */
    g_dbus_connection_call(scanner->bus, BLUEZ_SERVICE, scanner->adapter_path,
                           ADAPTER_INTERFACE, "SetDiscoveryFilter",
                           g_variant_new("(a{sv})", &filter), NULL, G_DBUS_CALL_FLAGS_NONE,
                           -1, scanner->cancellable, on_discovery_call_done,
                           (gpointer)"SetDiscoveryFilter");
    g_dbus_connection_call(scanner->bus, BLUEZ_SERVICE, scanner->adapter_path,
                           ADAPTER_INTERFACE, "StartDiscovery", NULL, NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, scanner->cancellable,
                           on_discovery_call_done, (gpointer)"StartDiscovery");
    scanner->running = true;
}

void ble_scanner_stop(BleScanner *scanner)
{
    if (scanner == NULL || !scanner->running)
        return;

    scanner->running = false;
    g_dbus_connection_call(scanner->bus, BLUEZ_SERVICE, scanner->adapter_path,
                           ADAPTER_INTERFACE, "StopDiscovery", NULL, NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                           on_discovery_call_done, (gpointer)"StopDiscovery");
}

bool ble_scanner_is_running(const BleScanner *scanner)
{
    return scanner != NULL && scanner->running;
}

void ble_scanner_reset_filter(BleScanner *scanner)
{
    if (scanner != NULL)
        g_hash_table_remove_all(scanner->matches);
}
