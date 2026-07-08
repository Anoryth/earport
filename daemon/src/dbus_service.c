/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 LibrePods Contributors
 */

#include "dbus_service.h"
#include <string.h>

/* D-Bus introspection XML */
static const gchar introspection_xml[] =
    "<node>"
    "  <interface name='" DBUS_INTERFACE_NAME "'>"
    "    <property name='Connected' type='b' access='read'/>"
    "    <property name='DeviceName' type='s' access='read'/>"
    "    <property name='DeviceAddress' type='s' access='read'/>"
    "    <property name='DeviceModel' type='s' access='read'/>"
    "    <property name='DisplayName' type='s' access='read'/>"
    "    <property name='IsHeadphones' type='b' access='read'/>"
    "    <property name='SupportsANC' type='b' access='read'/>"
    "    <property name='SupportsAdaptive' type='b' access='read'/>"
    "    <property name='BatteryLeft' type='i' access='read'/>"
    "    <property name='BatteryRight' type='i' access='read'/>"
    "    <property name='BatteryCase' type='i' access='read'/>"
    "    <property name='ChargingLeft' type='b' access='read'/>"
    "    <property name='ChargingRight' type='b' access='read'/>"
    "    <property name='ChargingCase' type='b' access='read'/>"
    "    <property name='NoiseControlMode' type='s' access='read'/>"
    "    <property name='ConversationalAwareness' type='b' access='read'/>"
    "    <property name='LeftInEar' type='b' access='read'/>"
    "    <property name='RightInEar' type='b' access='read'/>"
    "    <property name='AdaptiveNoiseLevel' type='i' access='read'/>"
    "    <property name='EarPauseMode' type='i' access='read'/>"
    "    <property name='ListeningModeOff' type='b' access='read'/>"
    "    <property name='ListeningModeTransparency' type='b' access='read'/>"
    "    <property name='ListeningModeANC' type='b' access='read'/>"
    "    <property name='ListeningModeAdaptive' type='b' access='read'/>"
    "    <method name='SetNoiseControlMode'>"
    "      <arg type='s' name='mode' direction='in'/>"
    "    </method>"
    "    <method name='SetConversationalAwareness'>"
    "      <arg type='b' name='enabled' direction='in'/>"
    "    </method>"
    "    <method name='SetAdaptiveNoiseLevel'>"
    "      <arg type='i' name='level' direction='in'/>"
    "    </method>"
    "    <method name='SetEarPauseMode'>"
    "      <arg type='i' name='mode' direction='in'/>"
    "    </method>"
    "    <method name='SetListeningModes'>"
    "      <arg type='b' name='off' direction='in'/>"
    "      <arg type='b' name='transparency' direction='in'/>"
    "      <arg type='b' name='anc' direction='in'/>"
    "      <arg type='b' name='adaptive' direction='in'/>"
    "    </method>"
    "    <method name='SetDisplayName'>"
    "      <arg type='s' name='name' direction='in'/>"
    "    </method>"
    "    <signal name='DeviceConnected'>"
    "      <arg type='s' name='address'/>"
    "      <arg type='s' name='name'/>"
    "    </signal>"
    "    <signal name='DeviceDisconnected'>"
    "      <arg type='s' name='address'/>"
    "      <arg type='s' name='name'/>"
    "    </signal>"
    "    <signal name='BatteryChanged'>"
    "      <arg type='i' name='left'/>"
    "      <arg type='i' name='right'/>"
    "      <arg type='i' name='case_battery'/>"
    "    </signal>"
    "    <signal name='NoiseControlModeChanged'>"
    "      <arg type='s' name='mode'/>"
    "    </signal>"
    "    <signal name='EarDetectionChanged'>"
    "      <arg type='b' name='leftInEar'/>"
    "      <arg type='b' name='rightInEar'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

struct DbusService {
    GDBusConnection *connection;
    GDBusNodeInfo *introspection_data;
    guint registration_id;
    guint bus_name_id;

    AirPodsState *state;

    DbusNoiseControlCallback noise_control_callback;
    void *noise_control_user_data;

    DbusConvAwarenessCallback conv_awareness_callback;
    void *conv_awareness_user_data;

    DbusAdaptiveLevelCallback adaptive_level_callback;
    void *adaptive_level_user_data;

    DbusEarPauseModeCallback ear_pause_mode_callback;
    void *ear_pause_mode_user_data;

    DbusListeningModesCallback listening_modes_callback;
    void *listening_modes_user_data;

    DbusDisplayNameCallback display_name_callback;
    void *display_name_user_data;
};

static GVariant *get_property(GDBusConnection *connection G_GNUC_UNUSED,
                               const gchar *sender G_GNUC_UNUSED,
                               const gchar *object_path G_GNUC_UNUSED,
                               const gchar *interface_name G_GNUC_UNUSED,
                               const gchar *property_name,
                               GError **error G_GNUC_UNUSED,
                               gpointer user_data)
{
    DbusService *service = user_data;
    AirPodsState *state = service->state;

    g_mutex_lock(&state->lock);

    GVariant *result = NULL;

    if (g_strcmp0(property_name, "Connected") == 0) {
        result = g_variant_new_boolean(state->connected);
    } else if (g_strcmp0(property_name, "DeviceName") == 0) {
        result = g_variant_new_string(state->device_name ? state->device_name : "");
    } else if (g_strcmp0(property_name, "DeviceAddress") == 0) {
        result = g_variant_new_string(state->device_address ? state->device_address : "");
    } else if (g_strcmp0(property_name, "DeviceModel") == 0) {
        result = g_variant_new_string(airpods_model_to_string(state->model));
    } else if (g_strcmp0(property_name, "DisplayName") == 0) {
        result = g_variant_new_string(airpods_state_get_display_name(state));
    } else if (g_strcmp0(property_name, "IsHeadphones") == 0) {
        result = g_variant_new_boolean(airpods_model_is_headphones(state->model));
    } else if (g_strcmp0(property_name, "SupportsANC") == 0) {
        result = g_variant_new_boolean(airpods_model_supports_anc(state->model));
    } else if (g_strcmp0(property_name, "SupportsAdaptive") == 0) {
        result = g_variant_new_boolean(airpods_model_supports_adaptive(state->model));
    } else if (g_strcmp0(property_name, "BatteryLeft") == 0) {
        result = g_variant_new_int32(state->battery.left.level);
    } else if (g_strcmp0(property_name, "BatteryRight") == 0) {
        result = g_variant_new_int32(state->battery.right.level);
    } else if (g_strcmp0(property_name, "BatteryCase") == 0) {
        result = g_variant_new_int32(state->battery.case_battery.level);
    } else if (g_strcmp0(property_name, "ChargingLeft") == 0) {
        result = g_variant_new_boolean(state->battery.left.status == BATTERY_STATUS_CHARGING);
    } else if (g_strcmp0(property_name, "ChargingRight") == 0) {
        result = g_variant_new_boolean(state->battery.right.status == BATTERY_STATUS_CHARGING);
    } else if (g_strcmp0(property_name, "ChargingCase") == 0) {
        result = g_variant_new_boolean(state->battery.case_battery.status == BATTERY_STATUS_CHARGING);
    } else if (g_strcmp0(property_name, "NoiseControlMode") == 0) {
        result = g_variant_new_string(noise_control_mode_to_string(state->noise_control_mode));
    } else if (g_strcmp0(property_name, "ConversationalAwareness") == 0) {
        result = g_variant_new_boolean(state->conversational_awareness);
    } else if (g_strcmp0(property_name, "LeftInEar") == 0) {
        result = g_variant_new_boolean(state->ear_detection.left_in_ear);
    } else if (g_strcmp0(property_name, "RightInEar") == 0) {
        result = g_variant_new_boolean(state->ear_detection.right_in_ear);
    } else if (g_strcmp0(property_name, "AdaptiveNoiseLevel") == 0) {
        result = g_variant_new_int32(state->adaptive_noise_level);
    } else if (g_strcmp0(property_name, "EarPauseMode") == 0) {
        result = g_variant_new_int32(state->ear_pause_mode);
    } else if (g_strcmp0(property_name, "ListeningModeOff") == 0) {
        result = g_variant_new_boolean(state->listening_modes.off_enabled);
    } else if (g_strcmp0(property_name, "ListeningModeTransparency") == 0) {
        result = g_variant_new_boolean(state->listening_modes.transparency_enabled);
    } else if (g_strcmp0(property_name, "ListeningModeANC") == 0) {
        result = g_variant_new_boolean(state->listening_modes.anc_enabled);
    } else if (g_strcmp0(property_name, "ListeningModeAdaptive") == 0) {
        result = g_variant_new_boolean(state->listening_modes.adaptive_enabled);
    }

    g_mutex_unlock(&state->lock);

    return result;
}

static void handle_method_call(GDBusConnection *connection G_GNUC_UNUSED,
                                const gchar *sender G_GNUC_UNUSED,
                                const gchar *object_path G_GNUC_UNUSED,
                                const gchar *interface_name G_GNUC_UNUSED,
                                const gchar *method_name,
                                GVariant *parameters,
                                GDBusMethodInvocation *invocation,
                                gpointer user_data)
{
    DbusService *service = user_data;

    if (g_strcmp0(method_name, "SetNoiseControlMode") == 0) {
        const gchar *mode_str = NULL;
        g_variant_get(parameters, "(&s)", &mode_str);

        NoiseControlMode mode = noise_control_mode_from_string(mode_str);
        g_message("D-Bus: SetNoiseControlMode(%s) -> %d", mode_str, mode);

        if (service->noise_control_callback) {
            service->noise_control_callback(mode, service->noise_control_user_data);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetConversationalAwareness") == 0) {
        gboolean enabled = FALSE;
        g_variant_get(parameters, "(b)", &enabled);

        g_message("D-Bus: SetConversationalAwareness(%s)", enabled ? "true" : "false");

        if (service->conv_awareness_callback) {
            service->conv_awareness_callback(enabled, service->conv_awareness_user_data);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetAdaptiveNoiseLevel") == 0) {
        gint32 level = 0;
        g_variant_get(parameters, "(i)", &level);

        g_message("D-Bus: SetAdaptiveNoiseLevel(%d)", level);

        if (service->adaptive_level_callback) {
            service->adaptive_level_callback(level, service->adaptive_level_user_data);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetEarPauseMode") == 0) {
        gint32 mode = 0;
        g_variant_get(parameters, "(i)", &mode);

        g_message("D-Bus: SetEarPauseMode(%d)", mode);

        if (service->ear_pause_mode_callback) {
            service->ear_pause_mode_callback(mode, service->ear_pause_mode_user_data);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetListeningModes") == 0) {
        gboolean off = FALSE, transparency = FALSE, anc = FALSE, adaptive = FALSE;
        g_variant_get(parameters, "(bbbb)", &off, &transparency, &anc, &adaptive);

        g_message("D-Bus: SetListeningModes(off=%s, transparency=%s, anc=%s, adaptive=%s)",
                  off ? "true" : "false",
                  transparency ? "true" : "false",
                  anc ? "true" : "false",
                  adaptive ? "true" : "false");

        if (service->listening_modes_callback) {
            service->listening_modes_callback(off, transparency, anc, adaptive,
                                               service->listening_modes_user_data);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetDisplayName") == 0) {
        const gchar *name = NULL;
        g_variant_get(parameters, "(&s)", &name);

        g_message("D-Bus: SetDisplayName('%s')", name ? name : "");

        if (service->display_name_callback) {
            service->display_name_callback(name, service->display_name_user_data);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else {
        g_dbus_method_invocation_return_error(invocation,
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_UNKNOWN_METHOD,
                                               "Unknown method: %s",
                                               method_name);
    }
}

static const GDBusInterfaceVTable interface_vtable = {
    .method_call = handle_method_call,
    .get_property = get_property,
    .set_property = NULL,  /* No writable properties */
};

static void on_bus_acquired(GDBusConnection *connection,
                             const gchar *name G_GNUC_UNUSED,
                             gpointer user_data)
{
    DbusService *service = user_data;
    GError *error = NULL;

    service->connection = g_object_ref(connection);

    service->registration_id = g_dbus_connection_register_object(
        connection,
        DBUS_OBJECT_PATH,
        service->introspection_data->interfaces[0],
        &interface_vtable,
        service,
        NULL,
        &error
    );

    if (error) {
        g_warning("Failed to register D-Bus object: %s", error->message);
        g_error_free(error);
    } else {
        g_message("D-Bus object registered at %s", DBUS_OBJECT_PATH);
    }
}

static void on_name_acquired(GDBusConnection *connection G_GNUC_UNUSED,
                              const gchar *name,
                              gpointer user_data G_GNUC_UNUSED)
{
    g_message("D-Bus name acquired: %s", name);
}

static void on_name_lost(GDBusConnection *connection G_GNUC_UNUSED,
                          const gchar *name,
                          gpointer user_data G_GNUC_UNUSED)
{
    g_warning("D-Bus name lost: %s", name);
}

DbusService *dbus_service_new(AirPodsState *state)
{
    DbusService *service = g_new0(DbusService, 1);
    service->state = state;

    GError *error = NULL;
    service->introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (error) {
        g_warning("Failed to parse introspection XML: %s", error->message);
        g_error_free(error);
        g_free(service);
        return NULL;
    }

    return service;
}

void dbus_service_free(DbusService *service)
{
    if (service == NULL)
        return;

    dbus_service_stop(service);

    if (service->introspection_data)
        g_dbus_node_info_unref(service->introspection_data);

    g_free(service);
}

bool dbus_service_start(DbusService *service)
{
    service->bus_name_id = g_bus_own_name(
        G_BUS_TYPE_SESSION,
        DBUS_SERVICE_NAME,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired,
        on_name_acquired,
        on_name_lost,
        service,
        NULL
    );

    return service->bus_name_id > 0;
}

void dbus_service_stop(DbusService *service)
{
    if (service->registration_id > 0 && service->connection) {
        g_dbus_connection_unregister_object(service->connection, service->registration_id);
        service->registration_id = 0;
    }

    if (service->bus_name_id > 0) {
        g_bus_unown_name(service->bus_name_id);
        service->bus_name_id = 0;
    }

    if (service->connection) {
        g_object_unref(service->connection);
        service->connection = NULL;
    }
}

void dbus_service_set_noise_control_callback(DbusService *service,
                                              DbusNoiseControlCallback callback,
                                              void *user_data)
{
    service->noise_control_callback = callback;
    service->noise_control_user_data = user_data;
}

void dbus_service_set_conv_awareness_callback(DbusService *service,
                                               DbusConvAwarenessCallback callback,
                                               void *user_data)
{
    service->conv_awareness_callback = callback;
    service->conv_awareness_user_data = user_data;
}

void dbus_service_set_adaptive_level_callback(DbusService *service,
                                               DbusAdaptiveLevelCallback callback,
                                               void *user_data)
{
    service->adaptive_level_callback = callback;
    service->adaptive_level_user_data = user_data;
}

void dbus_service_set_ear_pause_mode_callback(DbusService *service,
                                               DbusEarPauseModeCallback callback,
                                               void *user_data)
{
    service->ear_pause_mode_callback = callback;
    service->ear_pause_mode_user_data = user_data;
}

void dbus_service_set_listening_modes_callback(DbusService *service,
                                                DbusListeningModesCallback callback,
                                                void *user_data)
{
    service->listening_modes_callback = callback;
    service->listening_modes_user_data = user_data;
}

void dbus_service_set_display_name_callback(DbusService *service,
                                             DbusDisplayNameCallback callback,
                                             void *user_data)
{
    service->display_name_callback = callback;
    service->display_name_user_data = user_data;
}

static void emit_signal(DbusService *service,
                         const char *signal_name,
                         GVariant *parameters)
{
    if (service->connection == NULL)
        return;

    GError *error = NULL;
    g_dbus_connection_emit_signal(
        service->connection,
        NULL,  /* Broadcast to all listeners */
        DBUS_OBJECT_PATH,
        DBUS_INTERFACE_NAME,
        signal_name,
        parameters,
        &error
    );

    if (error) {
        g_warning("Failed to emit signal %s: %s", signal_name, error->message);
        g_error_free(error);
    }
}

void dbus_service_emit_device_connected(DbusService *service,
                                         const char *address,
                                         const char *name)
{
    emit_signal(service, "DeviceConnected",
                g_variant_new("(ss)", address ? address : "", name ? name : ""));
}

void dbus_service_emit_device_disconnected(DbusService *service,
                                            const char *address,
                                            const char *name)
{
    emit_signal(service, "DeviceDisconnected",
                g_variant_new("(ss)", address ? address : "", name ? name : ""));
}

void dbus_service_emit_battery_changed(DbusService *service,
                                        int left, int right, int case_battery)
{
    emit_signal(service, "BatteryChanged",
                g_variant_new("(iii)", left, right, case_battery));
}

void dbus_service_emit_noise_control_changed(DbusService *service,
                                              NoiseControlMode mode)
{
    emit_signal(service, "NoiseControlModeChanged",
                g_variant_new("(s)", noise_control_mode_to_string(mode)));
}

void dbus_service_emit_ear_detection_changed(DbusService *service,
                                              bool left_in_ear,
                                              bool right_in_ear)
{
    emit_signal(service, "EarDetectionChanged",
                g_variant_new("(bb)", left_in_ear, right_in_ear));
}

void dbus_service_emit_properties_changed(DbusService *service,
                                           const char *property_name)
{
    if (service->connection == NULL)
        return;

    GVariant *prop_value = get_property(service->connection, NULL,
                                         DBUS_OBJECT_PATH, DBUS_INTERFACE_NAME,
                                         property_name, NULL, service);

    if (prop_value == NULL)
        return;

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", property_name, prop_value);

    GError *error = NULL;
    g_dbus_connection_emit_signal(
        service->connection,
        NULL,
        DBUS_OBJECT_PATH,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        g_variant_new("(sa{sv}as)",
                      DBUS_INTERFACE_NAME,
                      &builder,
                      NULL),
        &error
    );

    if (error) {
        g_warning("Failed to emit PropertiesChanged: %s", error->message);
        g_error_free(error);
    }
}
