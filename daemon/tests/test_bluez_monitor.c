/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * BlueZ monitoring against a fake org.bluez (fake_bluez.c)
 */

#include <gio/gio.h>
#include <string.h>

#include "bluetooth.h"
#include "bluez_monitor.h"
#include "fake_bluez.h"

#define ADDRESS_A "00:00:00:00:00:0A"
#define ADDRESS_B "00:00:00:00:00:0B"

static const char *path_a, *path_b, *path_headset;

/* Two pairs of AirPods and a headset, none connected */
static void add_devices(void)
{
    fake_bluez_reset();
    path_a = fake_bluez_add_device(ADDRESS_A, "AirPods A", true, false, false);
    path_b = fake_bluez_add_device(ADDRESS_B, "AirPods B", true, false, false);
    path_headset = fake_bluez_add_device("00:00:00:00:00:0C", "Headset", false, false, false);
}

static void set_connected(const char *path, bool connected)
{
    fake_bluez_set(path, "Connected", g_variant_new_boolean(connected), true);
}

/* ============================================================================
 * The monitor under test
 * ========================================================================== */

typedef struct {
    BluezMonitor *monitor;
    int connected;
    int disconnected;
    char *last_path;
} Events;

static void on_connected(const BluezDeviceInfo *device, void *user_data)
{
    Events *ev = user_data;
    ev->connected++;
    g_free(ev->last_path);
    ev->last_path = g_strdup(device->object_path);
}

static void on_disconnected(const BluezDeviceInfo *device, void *user_data)
{
    Events *ev = user_data;
    ev->disconnected++;
    g_free(ev->last_path);
    ev->last_path = g_strdup(device->object_path);
}

static void events_start(Events *ev)
{
    add_devices();
    *ev = (Events){ 0 };
    ev->monitor = bluez_monitor_new();
    g_assert_nonnull(ev->monitor);
    bluez_monitor_set_connected_callback(ev->monitor, on_connected, ev);
    bluez_monitor_set_disconnected_callback(ev->monitor, on_disconnected, ev);
    g_assert_true(bluez_monitor_start(ev->monitor));
    fake_bluez_sync();
}

static void events_stop(Events *ev)
{
    bluez_monitor_free(ev->monitor);
    g_free(ev->last_path);
}

static void wait_for(Events *ev, int connected, int disconnected)
{
    gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while ((ev->connected < connected || ev->disconnected < disconnected) &&
           g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
    /* Then nothing more than expected */
    fake_bluez_run(50);
    g_assert_cmpint(ev->connected, ==, connected);
    g_assert_cmpint(ev->disconnected, ==, disconnected);
}

/* ============================================================================
 * Tests
 * ========================================================================== */

/* AirPods already connected at start are found, other devices are not */
static void test_existing(void)
{
    Events ev;
    events_start(&ev);
    fake_bluez_set(path_a, "Connected", g_variant_new_boolean(TRUE), false);
    fake_bluez_set(path_headset, "Connected", g_variant_new_boolean(TRUE), false);

    bluez_monitor_check_existing_devices(ev.monitor);
    g_assert_cmpint(ev.connected, ==, 1);
    g_assert_cmpstr(ev.last_path, ==, path_a);
    events_stop(&ev);
}

static void test_connect_disconnect(void)
{
    Events ev;
    events_start(&ev);

    set_connected(path_headset, true);
    set_connected(path_a, true);
    wait_for(&ev, 1, 0);
    g_assert_cmpstr(ev.last_path, ==, path_a);

    set_connected(path_headset, false);
    set_connected(path_a, false);
    wait_for(&ev, 1, 1);
    g_assert_cmpstr(ev.last_path, ==, path_a);
    events_stop(&ev);
}

/* First pairing: connected before BlueZ knows the services, the AirPods
 * are recognised when the UUIDs come (issue #12), once */
static void test_uuids_late(void)
{
    Events ev;
    events_start(&ev);
    const char *uuids[] = { "0000110b-0000-1000-8000-00805f9b34fb", AIRPODS_UUID, NULL };
    fake_bluez_set(path_a, "UUIDs", g_variant_new_strv((const char *[]){ NULL }, -1), false);

    set_connected(path_a, true);
    wait_for(&ev, 0, 0);
    fake_bluez_set(path_a, "UUIDs", g_variant_new_strv(uuids, -1), true);
    wait_for(&ev, 1, 0);
    g_assert_cmpstr(ev.last_path, ==, path_a);
    fake_bluez_set(path_a, "UUIDs", g_variant_new_strv(uuids, -1), true);
    wait_for(&ev, 1, 0);

    /* Services resolved while not connected: nothing */
    fake_bluez_set(path_b, "UUIDs", g_variant_new_strv(uuids, -1), true);
    wait_for(&ev, 1, 0);
    events_stop(&ev);
}

/* Our battery provider dropping Battery1 when the link closes is no
 * departure: the AirPods are still connected to BlueZ */
static void test_battery_removed(void)
{
    Events ev;
    events_start(&ev);
    set_connected(path_a, true);
    wait_for(&ev, 1, 0);

    fake_bluez_remove_interfaces(path_a, (const char *[]){ "org.bluez.Battery1", NULL });
    wait_for(&ev, 1, 0);

    /* Unpaired: the device object goes away */
    fake_bluez_remove_interfaces(path_a, (const char *[]){ "org.bluez.Battery1", BLUEZ_DEVICE_INTERFACE,
                                                "org.freedesktop.DBus.Properties", NULL });
    wait_for(&ev, 1, 1);
    events_stop(&ev);
}

/* Two pairs paired: the one followed is kept while it is connected */
static void test_other_pair(void)
{
    Events ev;
    events_start(&ev);
    set_connected(path_a, true);
    wait_for(&ev, 1, 0);

    set_connected(path_b, true);
    wait_for(&ev, 1, 0);
    set_connected(path_b, false);
    wait_for(&ev, 1, 0);

    /* A leaves: B is followed from its next connection */
    set_connected(path_a, false);
    wait_for(&ev, 1, 1);
    set_connected(path_b, true);
    wait_for(&ev, 2, 1);
    g_assert_cmpstr(ev.last_path, ==, path_b);

    /* Now A is the other one, even removed */
    set_connected(path_a, true);
    wait_for(&ev, 2, 1);
    fake_bluez_remove_interfaces(path_a, (const char *[]){ BLUEZ_DEVICE_INTERFACE, NULL });
    wait_for(&ev, 2, 1);
    set_connected(path_b, false);
    wait_for(&ev, 2, 2);
    g_assert_cmpstr(ev.last_path, ==, path_b);
    events_stop(&ev);
}

/* Both connected at start: one of them, only */
static void test_existing_two_pairs(void)
{
    Events ev;
    events_start(&ev);
    fake_bluez_set(path_a, "Connected", g_variant_new_boolean(TRUE), false);
    fake_bluez_set(path_b, "Connected", g_variant_new_boolean(TRUE), false);

    bluez_monitor_check_existing_devices(ev.monitor);
    g_assert_cmpint(ev.connected, ==, 1);
    events_stop(&ev);
}

/* ============================================================================
 * Connecting, with BlueZ stuck "busy" (wait shortened to 100 ms in
 * meson.build)
 * ========================================================================== */

typedef struct {
    bool done;
    bool connected;
    char *error;
} Connected;

static void on_connect_done(bool connected, const char *error, void *user_data)
{
    Connected *result = user_data;
    result->done = true;
    result->connected = connected;
    result->error = g_strdup(error);
}

static void connect_and_wait(const char *path, Connected *result)
{
    *result = (Connected){ 0 };
    bluez_device_connect(path, on_connect_done, result);
    gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while (!result->done && g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
    g_assert_true(result->done);
}

#define BUSY "org.bluez.Error.InProgress"

static void test_connect(void)
{
    add_devices();
    Connected result;

    connect_and_wait(path_a, &result);
    g_assert_true(result.connected);
    g_assert_null(result.error);

    /* Refused: said so, nothing more */
    fake_bluez_fail("Connect", true);
    connect_and_wait(path_a, &result);
    g_assert_false(result.connected);
    g_assert_nonnull(strstr(result.error, "Failed (fake)"));
    g_free(result.error);
    fake_bluez_settle();
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 2);
    g_assert_cmpint(fake_bluez_calls("Disconnect"), ==, 0);
}

/* Busy because the AirPods were connecting by themselves: left alone */
static void test_connect_busy(void)
{
    add_devices();
    fake_bluez_fail_with("Connect", BUSY, 1);
    Connected result = { 0 };
    bluez_device_connect(path_a, on_connect_done, &result);
    fake_bluez_wait_calls("Connect", 1);
    fake_bluez_set(path_a, "Connected", g_variant_new_boolean(TRUE), false);

    gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while (!result.done && g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
    g_assert_true(result.connected);
    g_assert_cmpint(fake_bluez_calls("Disconnect"), ==, 0);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 1);
}

/* Stuck: cancelled, then once more; stuck again, given up */
static void test_connect_stuck(void)
{
    add_devices();
    Connected result;
    fake_bluez_fail_with("Connect", BUSY, 1);
    connect_and_wait(path_a, &result);
    g_assert_true(result.connected);
    g_assert_cmpint(fake_bluez_calls("Disconnect"), ==, 1);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 2);

    add_devices();
    fake_bluez_fail_with("Connect", BUSY, -1);
    connect_and_wait(path_a, &result);
    g_assert_false(result.connected);
    g_free(result.error);
    fake_bluez_settle();
    g_assert_cmpint(fake_bluez_calls("Disconnect"), ==, 1);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 2);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    fake_bluez_start();

    g_test_add_func("/bluez-monitor/existing", test_existing);
    g_test_add_func("/bluez-monitor/existing-two-pairs", test_existing_two_pairs);
    g_test_add_func("/bluez-monitor/connect-disconnect", test_connect_disconnect);
    g_test_add_func("/bluez-monitor/uuids-late", test_uuids_late);
    g_test_add_func("/bluez-monitor/battery-removed", test_battery_removed);
    g_test_add_func("/bluez-monitor/other-pair", test_other_pair);
    g_test_add_func("/bluez-connect/connect", test_connect);
    g_test_add_func("/bluez-connect/busy", test_connect_busy);
    g_test_add_func("/bluez-connect/stuck", test_connect_stuck);
    int ret = g_test_run();

    fake_bluez_stop();
    return ret;
}
