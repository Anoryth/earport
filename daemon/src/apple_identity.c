/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "apple_identity.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

#define PROFILE_PATH "/io/github/anoryth/EarPort/AppleIdentity"
#define PNP_INFORMATION_UUID "00001200-0000-1000-8000-00805f9b34fb"

/* Device ID record of an Apple device: vendor 0x004C (Bluetooth SIG
 * assigned), product and version 0. The AirPods read the vendor; BlueZ's own
 * record stays, the AirPods accept the Apple one next to it. */
static const char SERVICE_RECORD[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
    "<record>"
    "<attribute id=\"0x0001\"><sequence><uuid value=\"0x1200\"/></sequence></attribute>"
    "<attribute id=\"0x0200\"><uint16 value=\"0x0103\"/></attribute>"
    "<attribute id=\"0x0201\"><uint16 value=\"0x004c\"/></attribute>"
    "<attribute id=\"0x0202\"><uint16 value=\"0x0000\"/></attribute>"
    "<attribute id=\"0x0203\"><uint16 value=\"0x0000\"/></attribute>"
    "<attribute id=\"0x0204\"><boolean value=\"true\"/></attribute>"
    "<attribute id=\"0x0205\"><uint16 value=\"0x0001\"/></attribute>"
    "</record>";

static const char PROFILE_XML[] =
    "<node>"
    "  <interface name='org.bluez.Profile1'>"
    "    <method name='Release'/>"
    "    <method name='NewConnection'>"
    "      <arg type='o' direction='in'/>"
    "      <arg type='h' direction='in'/>"
    "      <arg type='a{sv}' direction='in'/>"
    "    </method>"
    "    <method name='RequestDisconnection'>"
    "      <arg type='o' direction='in'/>"
    "    </method>"
    "  </interface>"
    "</node>";

struct AppleIdentity {
    GDBusConnection *bus;
    GDBusNodeInfo *node_info;
    guint object_id;
    guint bluez_watch_id;
    GCancellable *cancellable;
    bool enabled;
    bool registered;            /* Registered, or the call is pending */
};

/* The record carries no service: nothing ever connects to it, but BlueZ
 * hands over any connection it gets */
static void on_profile_method(GDBusConnection *bus, const gchar *sender,
                              const gchar *object_path, const gchar *interface,
                              const gchar *method, GVariant *parameters,
                              GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)bus;
    (void)sender;
    (void)object_path;
    (void)interface;
    (void)parameters;
    (void)user_data;

    if (g_strcmp0(method, "NewConnection") == 0) {
        GUnixFDList *fds = g_dbus_message_get_unix_fd_list(
            g_dbus_method_invocation_get_message(invocation));
        if (fds != NULL) {
            gint n;
            gint *list = g_unix_fd_list_steal_fds(fds, &n);
            for (gint i = 0; i < n; i++)
                close(list[i]);
            g_free(list);
        }
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static const GDBusInterfaceVTable profile_vtable = {
    .method_call = on_profile_method,
};

static void on_register_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (reply != NULL) {
        g_message("Presenting this computer as an Apple device to the AirPods");
        g_variant_unref(reply);
        return;
    }

    /* The object may be gone: don't touch it */
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return;
    }

    /* Already there: fine */
    gchar *remote = g_dbus_error_get_remote_error(error);
    bool exists = g_strcmp0(remote, "org.bluez.Error.AlreadyExists") == 0;
    g_free(remote);
    if (exists) {
        g_error_free(error);
        return;
    }

    AppleIdentity *ai = user_data;
    ai->registered = false;
    g_warning("Could not publish the Apple identity to BlueZ: %s", error->message);
    g_error_free(error);
}

static void register_record(AppleIdentity *ai)
{
    if (ai->registered)
        return;

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "Name", g_variant_new_string("EarPort"));
    g_variant_builder_add(&options, "{sv}", "ServiceRecord", g_variant_new_string(SERVICE_RECORD));
    g_variant_builder_add(&options, "{sv}", "AutoConnect", g_variant_new_boolean(FALSE));

    ai->registered = true;
    g_dbus_connection_call(ai->bus, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1",
                           "RegisterProfile",
                           g_variant_new("(osa{sv})", PROFILE_PATH, PNP_INFORMATION_UUID, &options),
                           NULL, G_DBUS_CALL_FLAGS_NONE, 5000, ai->cancellable,
                           on_register_done, ai);
}

static void unregister_record(AppleIdentity *ai)
{
    if (!ai->registered)
        return;

    ai->registered = false;
    g_dbus_connection_call(ai->bus, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1",
                           "UnregisterProfile", g_variant_new("(o)", PROFILE_PATH),
                           NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
    g_message("No longer presenting this computer as an Apple device");
}

/* bluetoothd started (or restarted, and forgot the record) */
static void on_bluez_appeared(GDBusConnection *bus, const gchar *name, const gchar *owner,
                              gpointer user_data)
{
    (void)bus;
    (void)name;
    (void)owner;
    AppleIdentity *ai = user_data;
    if (ai->enabled)
        register_record(ai);
}

static void on_bluez_vanished(GDBusConnection *bus, const gchar *name, gpointer user_data)
{
    (void)bus;
    (void)name;
    AppleIdentity *ai = user_data;
    ai->registered = false;
}

AppleIdentity *apple_identity_new(void)
{
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus == NULL) {
        g_message("No system bus, Apple device switching unavailable: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    AppleIdentity *ai = g_new0(AppleIdentity, 1);
    ai->bus = bus;
    ai->cancellable = g_cancellable_new();
    ai->node_info = g_dbus_node_info_new_for_xml(PROFILE_XML, NULL);
    ai->object_id = g_dbus_connection_register_object(bus, PROFILE_PATH,
                                                      ai->node_info->interfaces[0],
                                                      &profile_vtable, ai, NULL, NULL);
    ai->bluez_watch_id = g_bus_watch_name_on_connection(bus, "org.bluez",
                                                        G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                        on_bluez_appeared, on_bluez_vanished,
                                                        ai, NULL);
    return ai;
}

void apple_identity_free(AppleIdentity *ai)
{
    if (ai == NULL)
        return;

    g_cancellable_cancel(ai->cancellable);
    g_bus_unwatch_name(ai->bluez_watch_id);
    unregister_record(ai);
    /* Sent before the connection may close */
    g_dbus_connection_flush_sync(ai->bus, NULL, NULL);
    if (ai->object_id > 0)
        g_dbus_connection_unregister_object(ai->bus, ai->object_id);
    g_dbus_node_info_unref(ai->node_info);
    g_object_unref(ai->cancellable);
    g_object_unref(ai->bus);
    g_free(ai);
}

void apple_identity_set_enabled(AppleIdentity *ai, bool enabled)
{
    if (ai == NULL)
        return;

    ai->enabled = enabled;
    if (enabled)
        register_record(ai);
    else
        unregister_record(ai);
}
