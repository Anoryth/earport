/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "link_trace.h"

#include <gio/gio.h>

struct LinkTrace {
    GDBusConnection *bus;
    char *device_path;
    guint disconnected_id;
    guint transport_id;
};

/* org.bluez.Device1.Disconnected(name, message), BlueZ 5.73 and later */
static void on_disconnected(GDBusConnection *bus, const gchar *sender, const gchar *path,
                            const gchar *interface, const gchar *signal, GVariant *parameters,
                            gpointer user_data)
{
    (void)bus;
    (void)sender;
    (void)interface;
    (void)signal;
    LinkTrace *trace = user_data;
    if (g_strcmp0(path, trace->device_path) != 0)
        return;

    const gchar *name = NULL, *message = NULL;
    g_variant_get(parameters, "(&s&s)", &name, &message);
    g_message("BlueZ: AirPods link closed: %s (%s)", message, name);
}

/* The audio stream to the AirPods, under their path (".../sep2/fd0") */
static void on_transport_changed(GDBusConnection *bus, const gchar *sender, const gchar *path,
                                 const gchar *interface, const gchar *signal,
                                 GVariant *parameters, gpointer user_data)
{
    (void)bus;
    (void)sender;
    (void)interface;
    (void)signal;
    LinkTrace *trace = user_data;
    if (trace->device_path == NULL || !g_str_has_prefix(path, trace->device_path))
        return;

    const gchar *iface = NULL;
    GVariant *changed = NULL;
    g_variant_get(parameters, "(&s@a{sv}@as)", &iface, &changed, NULL);
    const gchar *state = NULL;
    if (g_strcmp0(iface, "org.bluez.MediaTransport1") == 0 &&
        g_variant_lookup(changed, "State", "&s", &state))
        g_message("Audio stream to the AirPods: %s", state);
    g_variant_unref(changed);
}

LinkTrace *link_trace_new(void)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (bus == NULL)
        return NULL;

    LinkTrace *trace = g_new0(LinkTrace, 1);
    trace->bus = bus;
    trace->disconnected_id = g_dbus_connection_signal_subscribe(
        bus, "org.bluez", "org.bluez.Device1", "Disconnected", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_disconnected, trace, NULL);
    trace->transport_id = g_dbus_connection_signal_subscribe(
        bus, "org.bluez", "org.freedesktop.DBus.Properties", "PropertiesChanged", NULL,
        "org.bluez.MediaTransport1", G_DBUS_SIGNAL_FLAGS_NONE, on_transport_changed, trace, NULL);
    return trace;
}

void link_trace_free(LinkTrace *trace)
{
    if (trace == NULL)
        return;
    g_dbus_connection_signal_unsubscribe(trace->bus, trace->disconnected_id);
    g_dbus_connection_signal_unsubscribe(trace->bus, trace->transport_id);
    g_object_unref(trace->bus);
    g_free(trace->device_path);
    g_free(trace);
}

void link_trace_set_device(LinkTrace *trace, const char *device_path)
{
    if (trace == NULL)
        return;
    g_free(trace->device_path);
    trace->device_path = g_strdup(device_path);
}
