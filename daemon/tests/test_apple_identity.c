/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Apple Device ID record published to BlueZ, against a fake org.bluez
 * (fake_bluez.c)
 */

#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <string.h>
#include <unistd.h>

#include "apple_identity.h"
#include "fake_bluez.h"

#define PROFILE_PATH "/io/github/anoryth/EarPort/AppleIdentity"
#define PNP_INFORMATION_UUID "00001200-0000-1000-8000-00805f9b34fb"

static AppleIdentity *identity_new(void)
{
    /* Calls of the previous test still in flight come from the same
     * connection: let them in before forgetting them */
    fake_bluez_run(50);
    fake_bluez_reset();
    AppleIdentity *ai = apple_identity_new();
    g_assert_nonnull(ai);
    return ai;
}

/* Published as an Apple device (vendor 0x004C), once, then withdrawn */
static void test_enable(void)
{
    AppleIdentity *ai = identity_new();
    fake_bluez_run(50);
    g_assert_cmpint(fake_bluez_calls("RegisterProfile"), ==, 0);

    apple_identity_set_enabled(ai, true);
    g_assert_cmpint(fake_bluez_wait_registered("Profile", 1), ==, 1);
    g_autoptr(GVariant) params = fake_bluez_last_call("RegisterProfile");
    const char *path, *uuid, *record;
    g_autoptr(GVariant) options = NULL;
    g_variant_get(params, "(&o&s@a{sv})", &path, &uuid, &options);
    g_assert_cmpstr(path, ==, PROFILE_PATH);
    g_assert_cmpstr(uuid, ==, PNP_INFORMATION_UUID);
    g_assert_true(g_variant_lookup(options, "ServiceRecord", "&s", &record));
    g_assert_nonnull(strstr(record, "<attribute id=\"0x0201\"><uint16 value=\"0x004c\"/>"));
    gboolean auto_connect = TRUE;
    g_assert_true(g_variant_lookup(options, "AutoConnect", "b", &auto_connect));
    g_assert_false(auto_connect);

    apple_identity_set_enabled(ai, true);
    fake_bluez_run(50);
    g_assert_cmpint(fake_bluez_calls("RegisterProfile"), ==, 1);

    apple_identity_set_enabled(ai, false);
    g_assert_cmpint(fake_bluez_wait_registered("Profile", 0), ==, 0);
    apple_identity_free(ai);
}

/* Enabled right away, as at startup: BlueZ's first appearance doesn't
 * register it a second time */
static void test_enabled_at_start(void)
{
    AppleIdentity *ai = identity_new();
    apple_identity_set_enabled(ai, true);
    g_assert_cmpint(fake_bluez_wait_registered("Profile", 1), ==, 1);
    fake_bluez_run(100);
    g_assert_cmpint(fake_bluez_calls("RegisterProfile"), ==, 1);
    apple_identity_free(ai);
}

/* bluetoothd restarted: published again, unless turned off */
static void test_bluez_restart(void)
{
    AppleIdentity *ai = identity_new();
    apple_identity_set_enabled(ai, true);
    g_assert_cmpint(fake_bluez_wait_registered("Profile", 1), ==, 1);
    fake_bluez_run(50);

    fake_bluez_restart();
    g_assert_cmpint(fake_bluez_registered("Profile"), ==, 0);
    g_assert_cmpint(fake_bluez_wait_registered("Profile", 1), ==, 1);

    apple_identity_set_enabled(ai, false);
    g_assert_cmpint(fake_bluez_wait_registered("Profile", 0), ==, 0);
    fake_bluez_restart();
    fake_bluez_run(100);
    g_assert_cmpint(fake_bluez_registered("Profile"), ==, 0);
    apple_identity_free(ai);
}

/* Refused: tried again when turned on again */
static void test_register_fails(void)
{
    AppleIdentity *ai = identity_new();
    fake_bluez_run(50);
    fake_bluez_fail("RegisterProfile", true);
    g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "Could not publish the Apple identity*");
    apple_identity_set_enabled(ai, true);
    g_assert_cmpint(fake_bluez_wait_calls("RegisterProfile", 1), ==, 1);
    fake_bluez_run(50);
    g_test_assert_expected_messages();

    fake_bluez_fail("RegisterProfile", false);
    apple_identity_set_enabled(ai, true);
    g_assert_cmpint(fake_bluez_wait_registered("Profile", 1), ==, 1);
    apple_identity_free(ai);
}

/* The service stops: the record goes with it */
static void test_free(void)
{
    AppleIdentity *ai = identity_new();
    apple_identity_set_enabled(ai, true);
    g_assert_cmpint(fake_bluez_wait_registered("Profile", 1), ==, 1);
    apple_identity_free(ai);
    g_assert_cmpint(fake_bluez_wait_registered("Profile", 0), ==, 0);
}

static void on_new_connection_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(source),
                                                                      NULL, res, &error);
    g_assert_no_error(error);
    g_variant_unref(reply);
    *(bool *)user_data = true;
}

/* BlueZ hands over a connection to the record: closed at once */
static void test_new_connection(void)
{
    AppleIdentity *ai = identity_new();
    apple_identity_set_enabled(ai, true);
    g_assert_cmpint(fake_bluez_wait_registered("Profile", 1), ==, 1);

    int pipe_fds[2];
    g_assert_cmpint(pipe(pipe_fds), ==, 0);
    GUnixFDList *fds = g_unix_fd_list_new_from_array(&pipe_fds[1], 1);   /* Takes it */
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    bool done = false;
    g_dbus_connection_call_with_unix_fd_list(
        bus, g_dbus_connection_get_unique_name(bus), PROFILE_PATH, "org.bluez.Profile1",
        "NewConnection",
        g_variant_new("(oh@a{sv})", FAKE_ADAPTER_PATH "/dev_00_00_00_00_00_0A", 0,
                      g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, fds, NULL, on_new_connection_done, &done);
    g_object_unref(fds);
    while (!done)
        g_main_context_iteration(NULL, TRUE);
    g_object_unref(bus);

    /* Every write end closed: end of file */
    char byte;
    g_assert_cmpint(fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK), ==, 0);
    g_assert_cmpint(read(pipe_fds[0], &byte, 1), ==, 0);
    close(pipe_fds[0]);
    apple_identity_free(ai);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    fake_bluez_start();

    g_test_add_func("/apple-identity/enable", test_enable);
    g_test_add_func("/apple-identity/enabled-at-start", test_enabled_at_start);
    g_test_add_func("/apple-identity/bluez-restart", test_bluez_restart);
    g_test_add_func("/apple-identity/register-fails", test_register_fails);
    g_test_add_func("/apple-identity/free", test_free);
    g_test_add_func("/apple-identity/new-connection", test_new_connection);
    int ret = g_test_run();

    fake_bluez_stop();
    return ret;
}
