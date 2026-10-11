/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Why the AirPods' link closed, from BlueZ's signals, against a fake
 * org.bluez (fake_bluez.c)
 */

#include <gio/gio.h>

#include "fake_bluez.h"
#include "link_trace.h"

#define AIRPODS_PATH FAKE_ADAPTER_PATH "/dev_00_00_00_00_00_0A"
#define OTHER_PATH FAKE_ADAPTER_PATH "/dev_00_00_00_00_00_0B"

typedef struct {
    int closed;
    bool left;
} Closed;

static void on_closed(bool left, void *user_data)
{
    Closed *closed = user_data;
    closed->closed++;
    closed->left = left;
}

static LinkTrace *trace_new(Closed *closed)
{
    *closed = (Closed){ 0 };
    LinkTrace *trace = link_trace_new();
    g_assert_nonnull(trace);
    link_trace_set_closed_callback(trace, on_closed, closed);
    fake_bluez_sync();
    return trace;
}

static void disconnected(const char *path, const char *reason)
{
    fake_bluez_emit(path, "org.bluez.Device1", "Disconnected",
                    g_variant_new("(ss)", reason, "Connection terminated (fake)"));
}

static void wait_closed(Closed *closed, int count)
{
    gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while (closed->closed < count && g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
    fake_bluez_run(30);
    g_assert_cmpint(closed->closed, ==, count);
}

/* They closed it or stopped answering (taken by another device), or this
 * computer closed it */
static void test_reasons(void)
{
    Closed closed;
    LinkTrace *trace = trace_new(&closed);
    link_trace_set_device(trace, AIRPODS_PATH);

    disconnected(AIRPODS_PATH, "org.bluez.Reason.Remote");
    wait_closed(&closed, 1);
    g_assert_true(closed.left);
    disconnected(AIRPODS_PATH, "org.bluez.Reason.Timeout");
    wait_closed(&closed, 2);
    g_assert_true(closed.left);
    disconnected(AIRPODS_PATH, "org.bluez.Reason.Local");
    wait_closed(&closed, 3);
    g_assert_false(closed.left);
    disconnected(AIRPODS_PATH, "org.bluez.Reason.Unknown");
    wait_closed(&closed, 4);
    g_assert_false(closed.left);
    link_trace_free(trace);
}

/* Other devices, or no AirPods followed: not ours */
static void test_other_devices(void)
{
    Closed closed;
    LinkTrace *trace = trace_new(&closed);

    disconnected(AIRPODS_PATH, "org.bluez.Reason.Remote");
    fake_bluez_run(50);
    link_trace_set_device(trace, AIRPODS_PATH);
    disconnected(OTHER_PATH, "org.bluez.Reason.Remote");
    fake_bluez_run(50);
    g_assert_cmpint(closed.closed, ==, 0);

    /* Followed elsewhere, then none */
    link_trace_set_device(trace, OTHER_PATH);
    disconnected(OTHER_PATH, "org.bluez.Reason.Remote");
    wait_closed(&closed, 1);
    link_trace_set_device(trace, NULL);
    disconnected(OTHER_PATH, "org.bluez.Reason.Remote");
    fake_bluez_run(50);
    g_assert_cmpint(closed.closed, ==, 1);
    link_trace_free(trace);
}

static void transport_state(const char *path, const char *interface, const char *state)
{
    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&changed, "{sv}", "State", g_variant_new_string(state));
    fake_bluez_emit(path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                    g_variant_new("(sa{sv}as)", interface, &changed, NULL));
}

/* The audio stream under the AirPods' path is logged, nothing else */
static void test_transport(void)
{
    Closed closed;
    LinkTrace *trace = trace_new(&closed);
    link_trace_set_device(trace, AIRPODS_PATH);

    /* Ignored ones first: logged, they would not match */
    g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "Audio stream to the AirPods: active");
    transport_state(OTHER_PATH "/sep2/fd0", "org.bluez.MediaTransport1", "idle");
    transport_state(AIRPODS_PATH, "org.bluez.Device1", "idle");
    transport_state(AIRPODS_PATH "/sep2/fd0", "org.bluez.MediaTransport1", "active");
    fake_bluez_run(50);
    g_test_assert_expected_messages();
    link_trace_free(trace);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    fake_bluez_start();

    g_test_add_func("/link-trace/reasons", test_reasons);
    g_test_add_func("/link-trace/other-devices", test_other_devices);
    g_test_add_func("/link-trace/transport", test_transport);
    int ret = g_test_run();

    fake_bluez_stop();
    return ret;
}
