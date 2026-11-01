/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Idle time: GNOME's idle monitor first, logind's session hint when Mutter
 * isn't there, unknown when neither answers. One private bus stands in
 * for both the session and the system bus; the fakes answer from their own
 * thread, as idle_ms() calls synchronously.
 */

#include <gio/gio.h>

#include "idle.h"

#define SESSION_PATH "/org/freedesktop/login1/session/_31"

static const char fake_xml[] =
    "<node>"
    "  <interface name='org.gnome.Mutter.IdleMonitor'>"
    "    <method name='GetIdletime'><arg type='t' direction='out'/></method>"
    "  </interface>"
    "  <interface name='org.freedesktop.login1.User'>"
    "    <property name='Display' type='(so)' access='read'/>"
    "  </interface>"
    "  <interface name='org.freedesktop.login1.Session'>"
    "    <property name='IdleHint' type='b' access='read'/>"
    "    <property name='IdleSinceHintMonotonic' type='t' access='read'/>"
    "  </interface>"
    "</node>";

static GTestDBus *test_bus;
static GThread *fake_thread;
static GMainContext *fake_context;
static GMainLoop *fake_loop;
static GMutex lock;
static GCond ready_cond;
static int names_owned;

/* What the fakes answer, under 'lock' */
static gint64 mutter_idle;       /* -1: Mutter absent */
static const char *display;      /* NULL: logind absent */
static gboolean idle_hint;
static guint64 idle_since_us;

static void on_method(GDBusConnection *connection, const char *sender, const char *path,
                      const char *interface, const char *method, GVariant *parameters,
                      GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)connection, (void)sender, (void)path, (void)interface, (void)method;
    (void)parameters, (void)user_data;
    g_mutex_lock(&lock);
    gint64 idle = mutter_idle;
    g_mutex_unlock(&lock);
    if (idle < 0)
        g_dbus_method_invocation_return_dbus_error(invocation,
                                                   "org.freedesktop.DBus.Error.ServiceUnknown",
                                                   "No Mutter");
    else
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(t)", (guint64)idle));
}

static GVariant *on_get_property(GDBusConnection *connection, const char *sender,
                                 const char *path, const char *interface, const char *property,
                                 GError **error, gpointer user_data)
{
    (void)connection, (void)sender, (void)path, (void)interface, (void)user_data;
    GVariant *value = NULL;
    g_mutex_lock(&lock);
    if (display == NULL)
        g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN, "No logind");
    else if (g_strcmp0(property, "Display") == 0)
        value = g_variant_new("(so)", "1", display);
    else if (g_strcmp0(property, "IdleHint") == 0)
        value = g_variant_new_boolean(idle_hint);
    else
        value = g_variant_new_uint64(idle_since_us);
    g_mutex_unlock(&lock);
    return value;
}

static const GDBusInterfaceVTable vtable = {
    .method_call = on_method,
    .get_property = on_get_property,
};

static void on_name_acquired(GDBusConnection *connection, const char *name, gpointer user_data)
{
    (void)connection, (void)name, (void)user_data;
    g_mutex_lock(&lock);
    names_owned++;
    g_cond_signal(&ready_cond);
    g_mutex_unlock(&lock);
}

static gpointer fake_main(gpointer user_data)
{
    (void)user_data;
    g_main_context_push_thread_default(fake_context);
    GDBusConnection *connection = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(test_bus),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    g_assert_nonnull(connection);

    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(fake_xml, NULL);
    g_dbus_connection_register_object(connection, "/org/gnome/Mutter/IdleMonitor/Core",
                                      info->interfaces[0], &vtable, NULL, NULL, NULL);
    g_dbus_connection_register_object(connection, "/org/freedesktop/login1/user/self",
                                      info->interfaces[1], &vtable, NULL, NULL, NULL);
    g_dbus_connection_register_object(connection, SESSION_PATH, info->interfaces[2], &vtable,
                                      NULL, NULL, NULL);
    g_dbus_node_info_unref(info);
    guint mutter = g_bus_own_name_on_connection(connection, "org.gnome.Mutter.IdleMonitor",
                                                G_BUS_NAME_OWNER_FLAGS_NONE, on_name_acquired,
                                                NULL, NULL, NULL);
    guint logind = g_bus_own_name_on_connection(connection, "org.freedesktop.login1",
                                                G_BUS_NAME_OWNER_FLAGS_NONE, on_name_acquired,
                                                NULL, NULL, NULL);
    g_main_loop_run(fake_loop);

    g_bus_unown_name(mutter);
    g_bus_unown_name(logind);
    g_object_unref(connection);
    g_main_context_pop_thread_default(fake_context);
    return NULL;
}

static void fakes_answer(gint64 mutter, const char *session, gboolean hint, guint64 since)
{
    g_mutex_lock(&lock);
    mutter_idle = mutter;
    display = session;
    idle_hint = hint;
    idle_since_us = since;
    g_mutex_unlock(&lock);
}

static void test_logind_hint(void)
{
    g_assert_cmpint(idle_from_logind(false, 0, 5000000), ==, -1);
    g_assert_cmpint(idle_from_logind(false, 1000000, 5000000), ==, 0);
    g_assert_cmpint(idle_from_logind(true, 1000000, 5000000), ==, 4000);
    /* The clock read before the hint was set */
    g_assert_cmpint(idle_from_logind(true, 5000000, 1000000), ==, 0);
}

static void test_mutter_first(void)
{
    fakes_answer(1234, SESSION_PATH, TRUE, 1);
    g_assert_cmpint(idle_ms(), ==, 1234);
}

static void test_logind_without_mutter(void)
{
    /* Idle for 3 s, by logind's monotonic clock (the same as GLib's) */
    guint64 now = (guint64)g_get_monotonic_time();
    fakes_answer(-1, SESSION_PATH, TRUE, now - 3 * G_USEC_PER_SEC);
    int64_t idle = idle_ms();
    g_assert_cmpint(idle, >=, 3000);
    g_assert_cmpint(idle, <, 13000);

    fakes_answer(-1, SESSION_PATH, FALSE, now);
    g_assert_cmpint(idle_ms(), ==, 0);
}

static void test_no_graphical_session(void)
{
    fakes_answer(-1, "/", TRUE, 1);
    g_assert_cmpint(idle_ms(), ==, -1);
}

static void test_nobody_answers(void)
{
    fakes_answer(-1, NULL, FALSE, 0);
    g_assert_cmpint(idle_ms(), ==, -1);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);
    g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(test_bus), TRUE);

    fake_context = g_main_context_new();
    fake_loop = g_main_loop_new(fake_context, FALSE);
    fake_thread = g_thread_new("fakes", fake_main, NULL);
    g_mutex_lock(&lock);
    while (names_owned < 2)
        g_cond_wait(&ready_cond, &lock);
    g_mutex_unlock(&lock);

    g_test_add_func("/idle/logind-hint", test_logind_hint);
    g_test_add_func("/idle/mutter-first", test_mutter_first);
    g_test_add_func("/idle/logind-without-mutter", test_logind_without_mutter);
    g_test_add_func("/idle/no-graphical-session", test_no_graphical_session);
    g_test_add_func("/idle/nobody-answers", test_nobody_answers);

    int result = g_test_run();

    g_main_loop_quit(fake_loop);
    g_thread_join(fake_thread);
    g_main_loop_unref(fake_loop);
    g_main_context_unref(fake_context);
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
    return result;
}
