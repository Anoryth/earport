/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "dbus_service.h"
#include <stdlib.h>
#include <string.h>

/* D-Bus introspection XML */
static const gchar introspection_xml[] =
    "<node>"
    "  <interface name='" DBUS_INTERFACE_NAME "'>"
    "    <property name='Version' type='s' access='read'/>"
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
    "    <property name='ListeningTimeRemaining' type='i' access='read'/>"
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
    "    <property name='Settings' type='a{sv}' access='read'/>"
    "    <property name='NearbyBattery' type='a{sv}' access='read'/>"
    "    <property name='AutoConnect' type='b' access='read'/>"
    "    <property name='StemTrackControls' type='b' access='read'/>"
    "    <property name='AppleHandoff' type='b' access='read'/>"
    "    <property name='HdMic' type='b' access='read'/>"
    "    <property name='HdMicAvailable' type='b' access='read'/>"
    "    <property name='AudioSource' type='s' access='read'/>"
    "    <property name='Reconnecting' type='b' access='read'/>"
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
    "    <method name='SetSetting'>"
    "      <arg type='s' name='key' direction='in'/>"
    "      <arg type='v' name='value' direction='in'/>"
    "    </method>"
    "    <method name='ClaimAudio'/>"
    "    <method name='ResearchSend'>"
    "      <arg type='ay' name='packet' direction='in'/>"
    "    </method>"
    "    <method name='SetAppleHandoff'>"
    "      <arg type='b' name='enabled' direction='in'/>"
    "    </method>"
    "    <method name='SetHdMic'>"
    "      <arg type='b' name='enabled' direction='in'/>"
    "    </method>"
    "    <method name='SetStemTrackControls'>"
    "      <arg type='b' name='enabled' direction='in'/>"
    "    </method>"
    "    <method name='SetAutoConnect'>"
    "      <arg type='b' name='enabled' direction='in'/>"
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
    "    <signal name='SpeakingChanged'>"
    "      <arg type='b' name='speaking'/>"
    "    </signal>"
    "    <signal name='PausedForSleep'/>"
    "  </interface>"
    "</node>";

struct DbusService {
    GDBusConnection *connection;
    GDBusNodeInfo *introspection_data;
    guint registration_id;
    guint bus_name_id;

    AirPodsState *state;

    DbusDeviceCallbacks device;
    void *device_user_data;
    DbusServiceCallbacks options;
    void *options_user_data;

    bool auto_connect;
    bool stem_track_controls;
    bool apple_handoff;
    bool hd_mic;
    bool hd_mic_available;
    bool reconnecting;
};

/* Settings announced by the AirPods, as {key: b|i} */
static GVariant *build_settings_variant(AirPodsState *state)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

    for (int i = 0; i < AIRPODS_SETTING_COUNT; i++) {
        const AirPodsSettingDef *def = airpods_setting_at(i);
        if (!state->setting_announced[i])
            continue;

        int value = airpods_setting_decode(def, state->setting_values[i]);
        if (value < 0)
            continue;  /* Unexpected byte: better hidden than wrong */

        if (def->kind == AIRPODS_SETTING_BOOL)
            g_variant_builder_add(&builder, "{sv}", def->key, g_variant_new_boolean(value));
        else
            g_variant_builder_add(&builder, "{sv}", def->key, g_variant_new_int32(value));
    }

    return g_variant_builder_end(&builder);
}

static void handle_set_setting(DbusService *service,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation)
{
    const gchar *key = NULL;
    GVariant *value = NULL;
    g_variant_get(parameters, "(&sv)", &key, &value);

    const AirPodsSettingDef *def = airpods_setting_by_key(key);
    int int_value = -1;

    if (def == NULL) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                              "Unknown setting: %s", key);
        goto out;
    }

    if (def->kind == AIRPODS_SETTING_BOOL && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
        int_value = g_variant_get_boolean(value) ? 1 : 0;
    } else if (def->kind == AIRPODS_SETTING_CHOICE && g_variant_is_of_type(value, G_VARIANT_TYPE_INT32)) {
        int_value = g_variant_get_int32(value);
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                              "Wrong value type for %s: expected %s, got %s", key,
                                              def->kind == AIRPODS_SETTING_BOOL ? "b" : "i",
                                              g_variant_get_type_string(value));
        goto out;
    }

    uint8_t byte;
    if (!airpods_setting_encode(def, int_value, &byte)) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                              "Value out of range for %s: %d", key, int_value);
        goto out;
    }

    uint8_t current;
    if (!airpods_state_get_setting(service->state, def->id, &current)) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                                              "Setting not available on these AirPods: %s", key);
        goto out;
    }

    g_message("D-Bus: SetSetting(%s, %d)", key, int_value);

    if (service->device.setting == NULL ||
        !service->device.setting(def, byte, service->device_user_data)) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                              "AirPods not connected");
        goto out;
    }

    g_dbus_method_invocation_return_value(invocation, NULL);

out:
    g_variant_unref(value);
}

/* A command for the AirPods was sent, or could not be */
static void return_command_result(GDBusMethodInvocation *invocation, bool sent)
{
    if (sent)
        g_dbus_method_invocation_return_value(invocation, NULL);
    else
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                              "AirPods not connected");
}

static void return_invalid(GDBusMethodInvocation *invocation, const char *message)
{
    g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR,
                                                  G_DBUS_ERROR_INVALID_ARGS, message);
}

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


    GVariant *result = NULL;

    if (g_strcmp0(property_name, "Version") == 0) {
        /* Lets the extension, updated separately, spot an old service */
        result = g_variant_new_string(EARPORT_VERSION);
    } else if (g_strcmp0(property_name, "Connected") == 0) {
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
        bool charging;
        result = g_variant_new_int32(airpods_state_case_level(state, &charging));
    } else if (g_strcmp0(property_name, "ChargingLeft") == 0) {
        result = g_variant_new_boolean(state->battery.left.status == BATTERY_STATUS_CHARGING);
    } else if (g_strcmp0(property_name, "ChargingRight") == 0) {
        result = g_variant_new_boolean(state->battery.right.status == BATTERY_STATUS_CHARGING);
    } else if (g_strcmp0(property_name, "ChargingCase") == 0) {
        bool charging;
        airpods_state_case_level(state, &charging);
        result = g_variant_new_boolean(charging);
    } else if (g_strcmp0(property_name, "ListeningTimeRemaining") == 0) {
        result = g_variant_new_int32(state->listening_minutes_left);
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
    } else if (g_strcmp0(property_name, "AutoConnect") == 0) {
        result = g_variant_new_boolean(service->auto_connect);
    } else if (g_strcmp0(property_name, "StemTrackControls") == 0) {
        result = g_variant_new_boolean(service->stem_track_controls);
    } else if (g_strcmp0(property_name, "AppleHandoff") == 0) {
        result = g_variant_new_boolean(service->apple_handoff);
    } else if (g_strcmp0(property_name, "HdMic") == 0) {
        result = g_variant_new_boolean(service->hd_mic);
    } else if (g_strcmp0(property_name, "HdMicAvailable") == 0) {
        result = g_variant_new_boolean(service->hd_mic_available);
    } else if (g_strcmp0(property_name, "Reconnecting") == 0) {
        result = g_variant_new_boolean(service->reconnecting);
    } else if (g_strcmp0(property_name, "AudioSource") == 0) {
        result = g_variant_new_string(state->audio_source ? state->audio_source : "");
    } else if (g_strcmp0(property_name, "NearbyBattery") == 0) {
        /* Empty unless the AirPods are seen nearby but not connected here */
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        if (state->nearby_valid) {
            g_variant_builder_add(&builder, "{sv}", "Left", g_variant_new_int32(state->nearby_left));
            g_variant_builder_add(&builder, "{sv}", "Right", g_variant_new_int32(state->nearby_right));
            bool case_charging;
            int case_level = airpods_state_nearby_case_level(state, &case_charging);
            g_variant_builder_add(&builder, "{sv}", "Case", g_variant_new_int32(case_level));
            g_variant_builder_add(&builder, "{sv}", "LeftCharging",
                                  g_variant_new_boolean(state->nearby_left_charging));
            g_variant_builder_add(&builder, "{sv}", "RightCharging",
                                  g_variant_new_boolean(state->nearby_right_charging));
            g_variant_builder_add(&builder, "{sv}", "CaseCharging",
                                  g_variant_new_boolean(case_charging));
            g_variant_builder_add(&builder, "{sv}", "Host", g_variant_new_string(state->nearby_host));
            g_variant_builder_add(&builder, "{sv}", "Name",
                                  g_variant_new_string(state->nearby_name ? state->nearby_name : "AirPods"));
            g_variant_builder_add(&builder, "{sv}", "Headphones",
                                  g_variant_new_boolean(state->nearby_headphones));
        }
        result = g_variant_builder_end(&builder);
    } else if (g_strcmp0(property_name, "Settings") == 0) {
        result = build_settings_variant(state);
    }


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

        NoiseControlMode mode;
        if (!noise_control_mode_from_string(mode_str, &mode)) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                  "Unknown noise control mode: %s", mode_str);
            return;
        }
        g_message("D-Bus: SetNoiseControlMode(%s) -> %d", mode_str, mode);

        return_command_result(invocation, service->device.noise_control &&
                              service->device.noise_control(mode, service->device_user_data));

    } else if (g_strcmp0(method_name, "SetConversationalAwareness") == 0) {
        gboolean enabled = FALSE;
        g_variant_get(parameters, "(b)", &enabled);

        g_message("D-Bus: SetConversationalAwareness(%s)", enabled ? "true" : "false");

        return_command_result(invocation, service->device.conv_awareness &&
                              service->device.conv_awareness(enabled, service->device_user_data));

    } else if (g_strcmp0(method_name, "SetAdaptiveNoiseLevel") == 0) {
        gint32 level = 0;
        g_variant_get(parameters, "(i)", &level);

        g_message("D-Bus: SetAdaptiveNoiseLevel(%d)", level);

        if (level < 0 || level > 100) {
            return_invalid(invocation, "The adaptive noise level goes from 0 to 100");
            return;
        }
        return_command_result(invocation, service->device.adaptive_level &&
                              service->device.adaptive_level(level, service->device_user_data));

    } else if (g_strcmp0(method_name, "SetEarPauseMode") == 0) {
        gint32 mode = 0;
        g_variant_get(parameters, "(i)", &mode);

        g_message("D-Bus: SetEarPauseMode(%d)", mode);

        if (mode < 0 || mode > 2) {
            return_invalid(invocation, "Ear pause mode: 0 (off), 1 (one out) or 2 (both out)");
            return;
        }
        if (service->options.ear_pause_mode)
            service->options.ear_pause_mode(mode, service->options_user_data);

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetListeningModes") == 0) {
        gboolean off = FALSE, transparency = FALSE, anc = FALSE, adaptive = FALSE;
        g_variant_get(parameters, "(bbbb)", &off, &transparency, &anc, &adaptive);

        g_message("D-Bus: SetListeningModes(off=%s, transparency=%s, anc=%s, adaptive=%s)",
                  off ? "true" : "false",
                  transparency ? "true" : "false",
                  anc ? "true" : "false",
                  adaptive ? "true" : "false");

        /* The long press cycles through them: it needs two */
        if (off + transparency + anc + adaptive < 2) {
            return_invalid(invocation, "At least two listening modes must be enabled");
            return;
        }
        return_command_result(invocation, service->device.listening_modes &&
                              service->device.listening_modes(off, transparency, anc, adaptive,
                                                              service->device_user_data));

    } else if (g_strcmp0(method_name, "SetAutoConnect") == 0) {
        gboolean enabled = FALSE;
        g_variant_get(parameters, "(b)", &enabled);
        g_message("D-Bus: SetAutoConnect(%s)", enabled ? "true" : "false");

        if (service->options.auto_connect)
            service->options.auto_connect(enabled, service->options_user_data);
        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetStemTrackControls") == 0) {
        gboolean enabled;
        g_variant_get(parameters, "(b)", &enabled);
        g_message("D-Bus: SetStemTrackControls(%s)", enabled ? "true" : "false");

        if (service->options.stem_track_controls)
            service->options.stem_track_controls(enabled, service->options_user_data);
        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "ResearchSend") == 0) {
        gsize len = 0;
        GVariant *bytes = g_variant_get_child_value(parameters, 0);
        const guint8 *data = g_variant_get_fixed_array(bytes, &len, 1);
        if (service->options.research_send &&
            service->options.research_send(data, len, service->options_user_data))
            g_dbus_method_invocation_return_value(invocation, NULL);
        else
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "Research mode off or AirPods not connected");
        g_variant_unref(bytes);

    } else if (g_strcmp0(method_name, "ClaimAudio") == 0) {
        g_message("D-Bus: ClaimAudio()");
        if (service->options.claim_audio &&
            service->options.claim_audio(service->options_user_data))
            g_dbus_method_invocation_return_value(invocation, NULL);
        else
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "AirPods not connected");

    } else if (g_strcmp0(method_name, "SetAppleHandoff") == 0) {
        gboolean enabled = FALSE;
        g_variant_get(parameters, "(b)", &enabled);
        g_message("D-Bus: SetAppleHandoff(%s)", enabled ? "true" : "false");

        if (service->options.apple_handoff)
            service->options.apple_handoff(enabled, service->options_user_data);
        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetHdMic") == 0) {
        gboolean enabled = FALSE;
        g_variant_get(parameters, "(b)", &enabled);
        g_message("D-Bus: SetHdMic(%s)", enabled ? "true" : "false");

        if (!service->hd_mic_available)
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                                                  "Built without the HD microphone");
        else {
            if (service->options.hd_mic)
                service->options.hd_mic(enabled, service->options_user_data);
            g_dbus_method_invocation_return_value(invocation, NULL);
        }

    } else if (g_strcmp0(method_name, "SetSetting") == 0) {
        handle_set_setting(service, parameters, invocation);

    } else if (g_strcmp0(method_name, "SetDisplayName") == 0) {
        const gchar *name = NULL;
        g_variant_get(parameters, "(&s)", &name);

        g_message("D-Bus: SetDisplayName('%s')", name ? name : "");

        if (service->device.display_name)
            service->device.display_name(name, service->device_user_data);

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
    /* Another instance runs: two AAP links would steal the AirPods from
     * each other (they only talk to the last one opened) */
    g_printerr("D-Bus name %s not available (EarPort already running?), exiting\n", name);
    exit(1);
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

void dbus_service_set_device_callbacks(DbusService *service, const DbusDeviceCallbacks *callbacks,
                                       void *user_data)
{
    service->device = *callbacks;
    service->device_user_data = user_data;
}

void dbus_service_set_service_callbacks(DbusService *service,
                                        const DbusServiceCallbacks *callbacks, void *user_data)
{
    service->options = *callbacks;
    service->options_user_data = user_data;
}

void dbus_service_set_stem_track_controls(DbusService *service, bool enabled)
{
    service->stem_track_controls = enabled;
    dbus_service_emit_properties_changed(service, "StemTrackControls");
}

void dbus_service_set_auto_connect(DbusService *service, bool enabled)
{
    service->auto_connect = enabled;
    dbus_service_emit_properties_changed(service, "AutoConnect");
}

void dbus_service_set_reconnecting(DbusService *service, bool reconnecting)
{
    service->reconnecting = reconnecting;
    dbus_service_emit_properties_changed(service, "Reconnecting");
}

void dbus_service_set_apple_handoff(DbusService *service, bool enabled)
{
    service->apple_handoff = enabled;
    dbus_service_emit_properties_changed(service, "AppleHandoff");
}

void dbus_service_set_hd_mic(DbusService *service, bool available, bool enabled)
{
    service->hd_mic_available = available;
    service->hd_mic = enabled;
    dbus_service_emit_properties_changed(service, "HdMicAvailable");
    dbus_service_emit_properties_changed(service, "HdMic");
}

static void emit_signal(DbusService *service,
                         const char *signal_name,
                         GVariant *parameters)
{
    if (service->connection == NULL) {
        /* Nobody to send it to: drop the floating reference */
        if (parameters != NULL)
            g_variant_unref(g_variant_ref_sink(parameters));
        return;
    }

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

void dbus_service_emit_speaking_changed(DbusService *service, bool speaking)
{
    emit_signal(service, "SpeakingChanged", g_variant_new("(b)", speaking));
}

void dbus_service_emit_paused_for_sleep(DbusService *service)
{
    emit_signal(service, "PausedForSleep", NULL);
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
