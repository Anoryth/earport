/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "idle.h"

#include <gio/gio.h>

int64_t idle_from_logind(bool idle_hint, uint64_t idle_since_us, uint64_t now_us)
{
    /* Zero until a desktop first sets the hint: no one reports input */
    if (idle_since_us == 0)
        return -1;
    if (!idle_hint || now_us < idle_since_us)
        return 0;
    return (int64_t)((now_us - idle_since_us) / 1000);
}

static GVariant *logind_property(GDBusConnection *bus, const char *path, const char *iface,
                                 const char *name)
{
    GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.login1", path,
                                                  "org.freedesktop.DBus.Properties", "Get",
                                                  g_variant_new("(ss)", iface, name),
                                                  G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE,
                                                  1000, NULL, NULL);
    if (reply == NULL)
        return NULL;
    GVariant *value;
    g_variant_get(reply, "(v)", &value);
    g_variant_unref(reply);
    return value;
}

static int64_t logind_idle_ms(void)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (bus == NULL)
        return -1;

    int64_t idle = -1;
    char *session = NULL;
    GVariant *display = logind_property(bus, "/org/freedesktop/login1/user/self",
                                        "org.freedesktop.login1.User", "Display");
    if (display != NULL) {
        if (g_variant_is_of_type(display, G_VARIANT_TYPE("(so)")))
            g_variant_get(display, "(so)", NULL, &session);
        g_variant_unref(display);
    }
    if (session != NULL && g_strcmp0(session, "/") != 0) {
        GVariant *hint = logind_property(bus, session, "org.freedesktop.login1.Session", "IdleHint");
        GVariant *since = logind_property(bus, session, "org.freedesktop.login1.Session",
                                          "IdleSinceHintMonotonic");
        if (hint != NULL && since != NULL && g_variant_is_of_type(hint, G_VARIANT_TYPE_BOOLEAN) &&
            g_variant_is_of_type(since, G_VARIANT_TYPE_UINT64))
            idle = idle_from_logind(g_variant_get_boolean(hint), g_variant_get_uint64(since),
                                    (uint64_t)g_get_monotonic_time());
        if (hint != NULL)
            g_variant_unref(hint);
        if (since != NULL)
            g_variant_unref(since);
    }
    g_free(session);
    g_object_unref(bus);
    return idle;
}

int64_t idle_ms(void)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus == NULL)
        return logind_idle_ms();

    GVariant *reply = g_dbus_connection_call_sync(bus, "org.gnome.Mutter.IdleMonitor",
                                                  "/org/gnome/Mutter/IdleMonitor/Core",
                                                  "org.gnome.Mutter.IdleMonitor", "GetIdletime",
                                                  NULL, G_VARIANT_TYPE("(t)"),
                                                  G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    g_object_unref(bus);
    if (reply == NULL)
        return logind_idle_ms();

    guint64 idle = 0;
    g_variant_get(reply, "(t)", &idle);
    g_variant_unref(reply);
    return (int64_t)idle;
}
