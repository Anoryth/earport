/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Battery published to BlueZ, against a fake org.bluez (fake_bluez.c)
 */

#include <gio/gio.h>

#include "battery_provider.h"
#include "fake_bluez.h"

#define PROVIDER_ROOT "/io/github/anoryth/EarPort/BatteryProvider"
#define ADDRESS_A "00:00:00:00:00:0A"
#define ADDRESS_B "00:00:00:00:00:0B"

static void on_objects_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    *(GVariant **)user_data = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    g_assert_no_error(error);
}

/* What BlueZ would read from the provider: the battery of 'address', NULL
 * if none. Asked like BlueZ does, over the bus, while the main context
 * runs to answer. */
static GVariant *exported_battery(const char *address)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    GVariant *reply = NULL;
    g_dbus_connection_call(bus, g_dbus_connection_get_unique_name(bus), PROVIDER_ROOT,
                           "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                           G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                           on_objects_ready, &reply);
    while (reply == NULL)
        g_main_context_iteration(NULL, TRUE);
    g_object_unref(bus);

    g_autofree char *device = g_strdup(address);
    g_strdelimit(device, ":", '_');
    g_autofree char *path = g_strdup_printf("%s/dev_%s", PROVIDER_ROOT, device);
    g_autoptr(GVariant) objects = g_variant_get_child_value(reply, 0);
    g_variant_unref(reply);
    g_autoptr(GVariant) interfaces = g_variant_lookup_value(objects, path, NULL);
    if (interfaces == NULL)
        return NULL;
    return g_variant_lookup_value(interfaces, "org.bluez.BatteryProvider1", NULL);
}

static int exported_percentage(const char *address)
{
    g_autoptr(GVariant) battery = exported_battery(address);
    guchar percentage;
    if (battery == NULL || !g_variant_lookup(battery, "Percentage", "y", &percentage))
        return -1;
    return percentage;
}

static BatteryProvider *provider_new(void)
{
    /* Calls of the previous test still in flight come from the same
     * connection: let them in before forgetting them */
    fake_bluez_run(50);
    fake_bluez_reset();
    BatteryProvider *bp = battery_provider_new();
    g_assert_nonnull(bp);
    return bp;
}

/* Exported for the AirPods' device, registered once */
static void test_publish(void)
{
    BatteryProvider *bp = provider_new();

    battery_provider_update(bp, FAKE_ADAPTER_PATH, ADDRESS_A, 80);
    g_assert_cmpint(fake_bluez_wait_registered("BatteryProvider", 1), ==, 1);
    g_autoptr(GVariant) params = fake_bluez_last_call("RegisterBatteryProvider");
    const char *root = NULL;
    g_variant_get(params, "(&o)", &root);
    g_assert_cmpstr(root, ==, PROVIDER_ROOT);

    g_autoptr(GVariant) battery = exported_battery(ADDRESS_A);
    g_assert_nonnull(battery);
    const char *device = NULL, *source = NULL;
    g_assert_true(g_variant_lookup(battery, "Device", "&o", &device));
    g_assert_cmpstr(device, ==, FAKE_ADAPTER_PATH "/dev_00_00_00_00_00_0A");
    g_assert_true(g_variant_lookup(battery, "Source", "&s", &source));
    g_assert_cmpstr(source, ==, "EarPort");
    g_assert_cmpint(exported_percentage(ADDRESS_A), ==, 80);

    /* Not registered again with each level */
    g_assert_cmpint(fake_bluez_wait_registered("BatteryProvider", 1), ==, 1);
    fake_bluez_run(30);
    int registrations = fake_bluez_calls("RegisterBatteryProvider");
    battery_provider_update(bp, FAKE_ADAPTER_PATH, ADDRESS_A, 75);
    g_assert_cmpint(exported_percentage(ADDRESS_A), ==, 75);
    fake_bluez_run(30);
    g_assert_cmpint(fake_bluez_calls("RegisterBatteryProvider"), ==, registrations);
    battery_provider_free(bp);
}

/* No level (link closed): removed, until the next one */
static void test_remove(void)
{
    BatteryProvider *bp = provider_new();

    battery_provider_update(bp, FAKE_ADAPTER_PATH, ADDRESS_A, 80);
    battery_provider_update(bp, FAKE_ADAPTER_PATH, ADDRESS_A, -1);
    g_assert_cmpint(exported_percentage(ADDRESS_A), ==, -1);
    battery_provider_update(bp, FAKE_ADAPTER_PATH, ADDRESS_A, 60);
    g_assert_cmpint(exported_percentage(ADDRESS_A), ==, 60);
    battery_provider_free(bp);
}

/* Other AirPods: the previous battery goes */
static void test_other_airpods(void)
{
    BatteryProvider *bp = provider_new();

    battery_provider_update(bp, FAKE_ADAPTER_PATH, ADDRESS_A, 80);
    battery_provider_update(bp, FAKE_ADAPTER_PATH, ADDRESS_B, 50);
    g_assert_cmpint(exported_percentage(ADDRESS_A), ==, -1);
    g_assert_cmpint(exported_percentage(ADDRESS_B), ==, 50);
    battery_provider_free(bp);
}

/* Refused (older BlueZ): tried again with the next level */
static void test_register_fails(void)
{
    BatteryProvider *bp = provider_new();
    fake_bluez_fail("RegisterBatteryProvider", true);

    battery_provider_update(bp, FAKE_ADAPTER_PATH, ADDRESS_A, 80);
    g_assert_cmpint(fake_bluez_wait_calls("RegisterBatteryProvider", 1), >=, 1);
    fake_bluez_run(30);
    g_assert_cmpint(fake_bluez_registered("BatteryProvider"), ==, 0);
    fake_bluez_fail("RegisterBatteryProvider", false);
    battery_provider_update(bp, FAKE_ADAPTER_PATH, ADDRESS_A, 79);
    g_assert_cmpint(fake_bluez_wait_registered("BatteryProvider", 1), ==, 1);
    battery_provider_free(bp);
}

/* bluetoothd restarted: it forgot the provider, registered again */
static void test_bluez_restart(void)
{
    BatteryProvider *bp = provider_new();

    battery_provider_update(bp, FAKE_ADAPTER_PATH, ADDRESS_A, 80);
    g_assert_cmpint(fake_bluez_wait_registered("BatteryProvider", 1), ==, 1);
    /* bluetoothd's first appearance may come late, with a second
     * registration (AlreadyExists): let it arrive first */
    fake_bluez_run(100);
    fake_bluez_restart();
    g_assert_cmpint(fake_bluez_registered("BatteryProvider"), ==, 0);
    g_assert_cmpint(fake_bluez_wait_registered("BatteryProvider", 1), ==, 1);
    g_assert_cmpint(exported_percentage(ADDRESS_A), ==, 80);

    /* Not after the battery went away (once the calls in flight are in) */
    fake_bluez_run(100);
    battery_provider_update(bp, FAKE_ADAPTER_PATH, ADDRESS_A, -1);
    fake_bluez_restart();
    fake_bluez_run(100);
    g_assert_cmpint(fake_bluez_registered("BatteryProvider"), ==, 0);
    battery_provider_free(bp);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    fake_bluez_start();

    g_test_add_func("/battery-provider/publish", test_publish);
    g_test_add_func("/battery-provider/remove", test_remove);
    g_test_add_func("/battery-provider/other-airpods", test_other_airpods);
    g_test_add_func("/battery-provider/register-fails", test_register_fails);
    g_test_add_func("/battery-provider/bluez-restart", test_bluez_restart);
    int ret = g_test_run();

    fake_bluez_stop();
    return ret;
}
