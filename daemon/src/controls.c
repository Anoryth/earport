/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "controls.h"
#include "config.h"

#include <glib.h>
#include <string.h>

/* ============================================================================
 * Saved profile
 * ========================================================================== */

typedef struct {
    Controls *controls;
    char *address;
} SavedSettingsJob;

static void saved_settings_job_free(gpointer data)
{
    SavedSettingsJob *job = data;
    g_free(job->address);
    g_free(job);
}

static gboolean send_saved_settings_cb(gpointer user_data)
{
    SavedSettingsJob *job = user_data;
    Controls *c = job->controls;
    c->saved_settings_id = 0;

    if (!aap_link_is_connected(c->link))
        return G_SOURCE_REMOVE;

    /* Only what the AirPods don't announce, and only if it was set here:
     * sending defaults or stale values would undo changes made on another
     * device (conversation awareness and the adaptive level are announced,
     * the AirPods keep them) */
    DeviceProfile profile;
    if (!config_load_device_profile(job->address, &profile) || !profile.listening_modes_set)
        return G_SOURCE_REMOVE;

    g_message("Sending the long-press modes set in EarPort to the AirPods");

    uint8_t modes = 0;
    if (profile.listening_modes.off_enabled) modes |= AAP_LISTENING_MODE_OFF;
    if (profile.listening_modes.transparency_enabled) modes |= AAP_LISTENING_MODE_TRANSPARENCY;
    if (profile.listening_modes.anc_enabled) modes |= AAP_LISTENING_MODE_ANC;
    if (profile.listening_modes.adaptive_enabled) modes |= AAP_LISTENING_MODE_ADAPTIVE;

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_listening_modes_cmd(modes, packet);
    aap_link_send(c->link, packet, AAP_CONTROL_CMD_SIZE);

    return G_SOURCE_REMOVE;
}

void controls_send_saved_settings(Controls *c, const char *address)
{
    SavedSettingsJob *job = g_new0(SavedSettingsJob, 1);
    job->controls = c;
    job->address = g_strdup(address);

    /* After the connection stabilizes; a new session replaces the send
     * still pending for the previous one */
    if (c->saved_settings_id > 0)
        g_source_remove(c->saved_settings_id);
    c->saved_settings_id = g_timeout_add_full(G_PRIORITY_DEFAULT, 500, send_saved_settings_cb,
                                              job, saved_settings_job_free);
}

void controls_cleanup(Controls *c)
{
    if (c->saved_settings_id > 0) {
        g_source_remove(c->saved_settings_id);
        c->saved_settings_id = 0;
    }
    if (c->adaptive_check_id > 0) {
        g_source_remove(c->adaptive_check_id);
        c->adaptive_check_id = 0;
    }
}

/* ============================================================================
 * D-Bus methods
 * ========================================================================== */

/* Adaptive needs SET_FEATURES: AirPods that missed it switch but report
 * another mode or nothing (seen on AirPods Pro 2, reply 0x01). The replies
 * came within 1 s. */
#define ADAPTIVE_CHECK_MS 1500

static void send_noise_control(Controls *c, NoiseControlMode mode)
{
    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_noise_control_cmd(mode, packet);
    aap_link_send(c->link, packet, AAP_CONTROL_CMD_SIZE);
}

static gboolean adaptive_check_cb(gpointer user_data)
{
    Controls *c = user_data;
    c->adaptive_check_id = 0;

    NoiseControlMode reported = c->device->state->noise_control_mode;
    if (reported == NOISE_CONTROL_ADAPTIVE || c->adaptive_resent || !aap_link_is_connected(c->link))
        return G_SOURCE_REMOVE;

    g_message("Asked for Adaptive, the AirPods report '%s': sending the features and the request again",
              noise_control_mode_to_string(reported));
    c->adaptive_resent = true;
    aap_link_send(c->link, AAP_PKT_SET_FEATURES, AAP_SET_FEATURES_SIZE);
    send_noise_control(c, NOISE_CONTROL_ADAPTIVE);
    c->adaptive_check_id = g_timeout_add(ADAPTIVE_CHECK_MS, adaptive_check_cb, c);
    return G_SOURCE_REMOVE;
}

static bool on_set_noise_control(NoiseControlMode mode, void *user_data)
{
    Controls *c = user_data;

    if (!aap_link_is_connected(c->link)) {
        return false;
    }

    send_noise_control(c, mode);

    if (c->adaptive_check_id > 0) {
        g_source_remove(c->adaptive_check_id);
        c->adaptive_check_id = 0;
    }
    if (mode == NOISE_CONTROL_ADAPTIVE) {
        c->adaptive_resent = false;
        c->adaptive_check_id = g_timeout_add(ADAPTIVE_CHECK_MS, adaptive_check_cb, c);
    }

    return true;
}

static bool on_set_conv_awareness(bool enabled, void *user_data)
{
    Controls *c = user_data;

    if (!aap_link_is_connected(c->link)) {
        return false;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_conv_awareness_cmd(enabled, packet);
    aap_link_send(c->link, packet, AAP_CONTROL_CMD_SIZE);

    /* The AirPods don't echo the change back */
    airpods_state_set_conversational_awareness(c->device->state, enabled);
    dbus_service_emit_properties_changed(c->device->dbus, "ConversationalAwareness");

    /* Save to device profile */
    if (c->device->state->device_address && c->device->state->device_address[0] != '\0') {
        DeviceProfile profile;
        config_load_device_profile(c->device->state->device_address, &profile);
        profile.conversational_awareness = enabled;
        config_save_device_profile(c->device->state->device_address, &profile);
    }

    return true;
}

static bool on_set_adaptive_level(int level, void *user_data)
{
    Controls *c = user_data;

    if (!aap_link_is_connected(c->link)) {
        return false;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_adaptive_level_cmd(level, packet);
    aap_link_send(c->link, packet, AAP_CONTROL_CMD_SIZE);

    airpods_state_set_adaptive_noise_level(c->device->state, level);
    dbus_service_emit_properties_changed(c->device->dbus, "AdaptiveNoiseLevel");

    /* Save to device profile */
    if (c->device->state->device_address && c->device->state->device_address[0] != '\0') {
        DeviceProfile profile;
        config_load_device_profile(c->device->state->device_address, &profile);
        profile.adaptive_noise_level = level;
        config_save_device_profile(c->device->state->device_address, &profile);
    }

    return true;
}

static bool on_set_listening_modes(bool off, bool transparency, bool anc, bool adaptive, void *user_data)
{
    Controls *c = user_data;

    if (!aap_link_is_connected(c->link)) {
        return false;
    }

    /* Build the bitmask */
    uint8_t modes = 0;
    if (off) modes |= AAP_LISTENING_MODE_OFF;
    if (transparency) modes |= AAP_LISTENING_MODE_TRANSPARENCY;
    if (anc) modes |= AAP_LISTENING_MODE_ANC;
    if (adaptive) modes |= AAP_LISTENING_MODE_ADAPTIVE;

    g_message("Setting listening modes: 0x%02X", modes);

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_listening_modes_cmd(modes, packet);
    aap_link_send(c->link, packet, AAP_CONTROL_CMD_SIZE);

    /* Update local state immediately */
    airpods_state_set_listening_modes(c->device->state, off, transparency, anc, adaptive);

    /* Save to device profile */
    if (c->device->state->device_address && c->device->state->device_address[0] != '\0') {
        DeviceProfile profile;
        config_load_device_profile(c->device->state->device_address, &profile);
        profile.listening_modes.off_enabled = off;
        profile.listening_modes.transparency_enabled = transparency;
        profile.listening_modes.anc_enabled = anc;
        profile.listening_modes.adaptive_enabled = adaptive;
        profile.listening_modes_set = true;
        config_save_device_profile(c->device->state->device_address, &profile);
    }

    dbus_service_emit_properties_changed(c->device->dbus, "ListeningModeOff");
    dbus_service_emit_properties_changed(c->device->dbus, "ListeningModeTransparency");
    dbus_service_emit_properties_changed(c->device->dbus, "ListeningModeANC");
    dbus_service_emit_properties_changed(c->device->dbus, "ListeningModeAdaptive");

    return true;
}

static bool on_set_setting(const AirPodsSettingDef *def, uint8_t byte, void *user_data)
{
    Controls *c = user_data;

    if (!aap_link_is_connected(c->link)) {
        g_warning("Cannot set %s: not connected", def->key);
        return false;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_control_cmd(def->id, &byte, 1, packet);
    aap_link_send(c->link, packet, AAP_CONTROL_CMD_SIZE);

    /* The AirPods don't echo settings back: update the state right away.
     * No need to save it in the profile, the AirPods remember it. */
    if (airpods_state_set_setting(c->device->state, def->id, byte))
        dbus_service_emit_properties_changed(c->device->dbus, "Settings");
    if (c->setting_sent != NULL)
        c->setting_sent(def->id, byte, c->user_data);

    return true;
}

static void on_set_display_name(const char *name, void *user_data)
{
    Controls *c = user_data;

    /* As long as the profile keeps it, cut between two characters: an
     * invalid UTF-8 name would not load back */
    char display_name[sizeof(((DeviceProfile *)NULL)->display_name)];
    const char *end;
    g_strlcpy(display_name, name ? name : "", sizeof(display_name));
    g_utf8_validate(display_name, -1, &end);
    *(char *)end = '\0';

    g_message("Setting display name to '%s'", display_name);

    /* Update state */
    airpods_state_set_display_name(c->device->state, display_name);

    /* Save to device profile */
    if (c->device->state->device_address && c->device->state->device_address[0] != '\0') {
        DeviceProfile profile;
        config_load_device_profile(c->device->state->device_address, &profile);
        g_strlcpy(profile.display_name, display_name, sizeof(profile.display_name));
        config_save_device_profile(c->device->state->device_address, &profile);
    }

    /* Notify property change */
    dbus_service_emit_properties_changed(c->device->dbus, "DisplayName");
}

void controls_init(Controls *c, Device *device, AapLink *link, DbusService *dbus)
{
    c->device = device;
    c->link = link;
    c->saved_settings_id = 0;
    c->adaptive_check_id = 0;
    c->adaptive_resent = false;

    dbus_service_set_noise_control_callback(dbus, on_set_noise_control, c);
    dbus_service_set_conv_awareness_callback(dbus, on_set_conv_awareness, c);
    dbus_service_set_adaptive_level_callback(dbus, on_set_adaptive_level, c);
    dbus_service_set_listening_modes_callback(dbus, on_set_listening_modes, c);
    dbus_service_set_display_name_callback(dbus, on_set_display_name, c);
    dbus_service_set_setting_callback(dbus, on_set_setting, c);
}
