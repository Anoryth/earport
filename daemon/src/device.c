/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "device.h"
#include "config.h"

#include <glib.h>

/* ============================================================================
 * Remaining listening time
 * ========================================================================== */

/* The learned discharge rate is kept per device */
static void save_learned_drain_rate(Device *dev)
{
    g_message("Learned discharge rate: %.1f %%/h", dev->battery_estimator.drain_rate);
    config_save_drain_rate(dev->state->device_address, dev->battery_estimator.drain_rate);
}

static void update_listening_time(Device *dev, const AapBatteryData *battery)
{
    int64_t now = g_get_monotonic_time() / G_USEC_PER_SEC;
    bool left_in_use = battery->left_status == BATTERY_STATUS_DISCHARGING;
    bool right_in_use = battery->right_status == BATTERY_STATUS_DISCHARGING;
    bool learned = false;

    learned |= battery_estimator_update(&dev->battery_estimator, BATTERY_POD_LEFT, now,
                                        battery->left_level, left_in_use);
    learned |= battery_estimator_update(&dev->battery_estimator, BATTERY_POD_RIGHT, now,
                                        battery->right_level, right_in_use);
    if (learned)
        save_learned_drain_rate(dev);

    /* The pod in use with the lowest level runs out first */
    int level = -1;
    if (left_in_use)
        level = battery->left_level;
    if (right_in_use && (level < 0 || battery->right_level < level))
        level = battery->right_level;

    int minutes = battery_estimator_minutes_left(&dev->battery_estimator, level);
    if (airpods_state_set_listening_minutes(dev->state, minutes))
        dbus_service_emit_properties_changed(dev->dbus, "ListeningTimeRemaining");
}

/* ============================================================================
 * Session
 * ========================================================================== */

static void apply_device_profile(Device *dev, const char *address)
{
    if (address == NULL || address[0] == '\0') {
        return;
    }

    DeviceProfile profile;
    bool has_profile = config_load_device_profile(address, &profile);

    if (!has_profile) {
        g_message("No saved profile for device %s, using defaults", address);
        return;
    }

    g_message("Applying saved profile for device %s", address);

    /* Local only, never sent to the AirPods */
    airpods_state_set_display_name(dev->state, profile.display_name);

    /* Not announced by the AirPods: sent back by controls.c */
    if (profile.listening_modes_set) {
        airpods_state_set_listening_modes(dev->state,
                                           profile.listening_modes.off_enabled,
                                           profile.listening_modes.transparency_enabled,
                                           profile.listening_modes.anc_enabled,
                                           profile.listening_modes.adaptive_enabled);
    }

    /* Last values set in EarPort, until the AirPods announce theirs (they
     * keep what was set on another device) */
    if (profile.has_saved_settings) {
        dev->state->conversational_awareness = profile.conversational_awareness;
        dev->state->adaptive_noise_level = profile.adaptive_noise_level;
    }
}

void device_session_started(Device *dev, const char *address, const char *name)
{
    /* Model detected later via metadata */
    airpods_state_set_device(dev->state, name, address, AIRPODS_MODEL_UNKNOWN);

    /* Load and apply saved device profile */
    apply_device_profile(dev, address);
    battery_estimator_init(&dev->battery_estimator, config_load_drain_rate(address));

    /* Emit property changes BEFORE the DeviceConnected signal so the
     * extension's proxy cache is up-to-date when its handler reads
     * DisplayName for the connection notification. */
    dbus_service_emit_properties_changed(dev->dbus, "Connected");
    dbus_service_emit_properties_changed(dev->dbus, "DeviceName");
    dbus_service_emit_properties_changed(dev->dbus, "DeviceAddress");
    dbus_service_emit_properties_changed(dev->dbus, "DisplayName");

    dbus_service_emit_device_connected(dev->dbus, address, name);
}

void device_session_ended(Device *dev)
{
    if (dev->state->connected) {
        dbus_service_emit_device_disconnected(dev->dbus,
                                               dev->state->device_address,
                                               dev->state->device_name);
    }

    /* Let clients restore the volume lowered for a conversation */
    if (dev->ca_speaking) {
        dev->ca_speaking = false;
        dbus_service_emit_speaking_changed(dev->dbus, false);
    }

    /* Learn from the sessions cut short by the disconnection (needs the
     * device address, so before the state reset) */
    if (battery_estimator_finish(&dev->battery_estimator))
        save_learned_drain_rate(dev);

    airpods_state_reset(dev->state);
    dbus_service_emit_properties_changed(dev->dbus, "Connected");
    dbus_service_emit_properties_changed(dev->dbus, "AudioSource");
    dbus_service_emit_properties_changed(dev->dbus, "Settings");
}

/* ============================================================================
 * Packets from the AirPods
 * ========================================================================== */

void device_handle_packet(Device *dev, const AapParsedPacket *pkt)
{
    switch (pkt->type) {
    case AAP_PKT_TYPE_BATTERY:
        g_message("Battery: L=%d%% (status=%d) R=%d%% (status=%d) Case=%d%% (status=%d)",
                  pkt->data.battery.left_level,
                  pkt->data.battery.left_status,
                  pkt->data.battery.right_level,
                  pkt->data.battery.right_status,
                  pkt->data.battery.case_level,
                  pkt->data.battery.case_status);

        airpods_state_set_battery(dev->state,
                                   pkt->data.battery.left_level,
                                   pkt->data.battery.left_status,
                                   pkt->data.battery.right_level,
                                   pkt->data.battery.right_status,
                                   pkt->data.battery.case_level,
                                   pkt->data.battery.case_status);

        bool case_charging;
        dbus_service_emit_battery_changed(dev->dbus,
                                           pkt->data.battery.left_level,
                                           pkt->data.battery.right_level,
                                           airpods_state_case_level(dev->state, &case_charging));
        dbus_service_emit_properties_changed(dev->dbus, "BatteryLeft");
        dbus_service_emit_properties_changed(dev->dbus, "BatteryRight");
        dbus_service_emit_properties_changed(dev->dbus, "BatteryCase");
        dbus_service_emit_properties_changed(dev->dbus, "ChargingLeft");
        dbus_service_emit_properties_changed(dev->dbus, "ChargingRight");
        dbus_service_emit_properties_changed(dev->dbus, "ChargingCase");

        update_listening_time(dev, &pkt->data.battery);

        /* Left and right of the ear detection follow the primary pod */
        if (pkt->data.battery.primary_known &&
            airpods_state_set_primary_left(dev->state, pkt->data.battery.primary_left)) {
            dbus_service_emit_properties_changed(dev->dbus, "LeftInEar");
            dbus_service_emit_properties_changed(dev->dbus, "RightInEar");
        }
        break;

    case AAP_PKT_TYPE_EAR_DETECTION: {
        bool primary_in_ear = pkt->data.ear_detection.primary_in_ear;
        bool secondary_in_ear = pkt->data.ear_detection.secondary_in_ear;

        g_message("Ear detection: primary=%s secondary=%s",
                  primary_in_ear ? "in" : "out",
                  secondary_in_ear ? "in" : "out");

        /* AirPods Max have a single sensor: the secondary slot always reads
         * "out", which would jam the one-out auto-pause logic. Mirror the
         * primary status instead. */
        if (airpods_model_is_headphones(dev->state->model))
            secondary_in_ear = primary_in_ear;

        airpods_state_set_ear_detection(dev->state, primary_in_ear, secondary_in_ear);

        dbus_service_emit_ear_detection_changed(dev->dbus,
                                                 dev->state->ear_detection.left_in_ear,
                                                 dev->state->ear_detection.right_in_ear);
        dbus_service_emit_properties_changed(dev->dbus, "LeftInEar");
        dbus_service_emit_properties_changed(dev->dbus, "RightInEar");

        /* Trigger media pause/resume based on ear detection */
        if (dev->media) {
            media_control_on_ear_detection_changed(dev->media,
                                                    dev->state->ear_detection.left_in_ear,
                                                    dev->state->ear_detection.right_in_ear);
        }
        break;
    }

    case AAP_PKT_TYPE_NOISE_CONTROL:
        g_message("Noise control mode: %s",
                  noise_control_mode_to_string(pkt->data.noise_control));

        airpods_state_set_noise_control(dev->state, pkt->data.noise_control);

        dbus_service_emit_noise_control_changed(dev->dbus,
                                                 pkt->data.noise_control);
        dbus_service_emit_properties_changed(dev->dbus, "NoiseControlMode");
        break;

    case AAP_PKT_TYPE_CONV_AWARENESS:
        g_message("Conversational awareness: %s",
                  pkt->data.conversational_awareness ? "enabled" : "disabled");

        airpods_state_set_conversational_awareness(dev->state,
                                                    pkt->data.conversational_awareness);

        dbus_service_emit_properties_changed(dev->dbus, "ConversationalAwareness");
        break;

    case AAP_PKT_TYPE_CA_DETECTION: {
        int speaking = aap_ca_speaking_from_level(pkt->data.ca_volume_level);
        g_debug("CA detection event: level=%d", pkt->data.ca_volume_level);

        if (speaking >= 0 && (bool)speaking != dev->ca_speaking) {
            dev->ca_speaking = speaking;
            g_message("Conversation %s", speaking ? "started" : "ended");
            dbus_service_emit_speaking_changed(dev->dbus, dev->ca_speaking);
        }
        break;
    }

    case AAP_PKT_TYPE_LISTENING_MODES:
        g_message("Listening modes: off=%s transparency=%s anc=%s adaptive=%s (raw=0x%02X)",
                  pkt->data.listening_modes.off_enabled ? "on" : "off",
                  pkt->data.listening_modes.transparency_enabled ? "on" : "off",
                  pkt->data.listening_modes.anc_enabled ? "on" : "off",
                  pkt->data.listening_modes.adaptive_enabled ? "on" : "off",
                  pkt->data.listening_modes.raw_value);

        airpods_state_set_listening_modes(dev->state,
                                           pkt->data.listening_modes.off_enabled,
                                           pkt->data.listening_modes.transparency_enabled,
                                           pkt->data.listening_modes.anc_enabled,
                                           pkt->data.listening_modes.adaptive_enabled);

        dbus_service_emit_properties_changed(dev->dbus, "ListeningModeOff");
        dbus_service_emit_properties_changed(dev->dbus, "ListeningModeTransparency");
        dbus_service_emit_properties_changed(dev->dbus, "ListeningModeANC");
        dbus_service_emit_properties_changed(dev->dbus, "ListeningModeAdaptive");
        break;

    case AAP_PKT_TYPE_ADAPTIVE_LEVEL:
        g_message("Adaptive noise level: %d", pkt->data.adaptive_level);
        airpods_state_set_adaptive_noise_level(dev->state, pkt->data.adaptive_level);
        dbus_service_emit_properties_changed(dev->dbus, "AdaptiveNoiseLevel");
        break;

    case AAP_PKT_TYPE_CONTROL_SETTING: {
        const AirPodsSettingDef *def = airpods_setting_by_id(pkt->data.control_setting.id);
        if (def == NULL)
            break;  /* Not exposed yet */

        uint8_t value = pkt->data.control_setting.value[0];
        g_message("Setting %s: 0x%02X", def->key, value);

        if (airpods_state_set_setting(dev->state, def->id, value))
            dbus_service_emit_properties_changed(dev->dbus, "Settings");
        break;
    }

    case AAP_PKT_TYPE_HOSTS: {
        /* Logged to understand how the AirPods see each host */
        GString *line = g_string_new(NULL);
        for (uint8_t i = 0; i < pkt->data.hosts.count; i++) {
            const AapHost *host = &pkt->data.hosts.hosts[i];
            bool me = g_ascii_strcasecmp(host->address, dev->state->host_address) == 0;
            g_string_append_printf(line, "%s%s status %u flags 0x%02x", i ? ", " : "",
                                   me ? "this computer" : "other device", host->status, host->flags);
        }
        g_message("AirPods hosts (state %u): %s", pkt->data.hosts.state, line->str);
        g_string_free(line, TRUE);
        break;
    }

    case AAP_PKT_TYPE_AUDIO_SOURCE: {
        const AapAudioSource *source = &pkt->data.audio_source;
        const char *value = "none";
        if (source->type != AAP_AUDIO_SOURCE_NONE)
            value = g_ascii_strcasecmp(source->address, dev->state->host_address) == 0
                        ? "computer" : "other";
        if (source->type != dev->audio_source_type)
            g_message("Audio source type: %s", source->type == AAP_AUDIO_SOURCE_CALL ? "call"
                      : source->type == AAP_AUDIO_SOURCE_MEDIA ? "media" : "none");
        dev->audio_source_type = source->type;
        if (g_strcmp0(value, dev->state->audio_source) != 0) {
            dev->state->audio_source = value;
            g_message("Audio source: %s", value);
            dbus_service_emit_properties_changed(dev->dbus, "AudioSource");
        }
        break;
    }

    case AAP_PKT_TYPE_METADATA:
        g_message("Metadata received: device='%s' model='%s' manufacturer='%s'",
                  pkt->data.metadata.device_name,
                  pkt->data.metadata.model_number,
                  pkt->data.metadata.manufacturer);

        /* Update model from model number */
        {
            AirPodsModel detected_model = airpods_model_from_number(pkt->data.metadata.model_number);
            if (detected_model != AIRPODS_MODEL_UNKNOWN) {
                dev->state->model = detected_model;

                g_message("Detected AirPods model: %s", airpods_model_to_string(detected_model));
                dbus_service_emit_properties_changed(dev->dbus, "DeviceModel");
                dbus_service_emit_properties_changed(dev->dbus, "IsHeadphones");
                dbus_service_emit_properties_changed(dev->dbus, "SupportsANC");
                dbus_service_emit_properties_changed(dev->dbus, "SupportsAdaptive");
                /* DisplayName falls back to the model name, refresh it too */
                dbus_service_emit_properties_changed(dev->dbus, "DisplayName");
            }
        }
        break;

    default:
        break;
    }
}

void device_init(Device *dev, AirPodsState *state, DbusService *dbus, MediaControl *media)
{
    *dev = (Device) {
        .state = state,
        .dbus = dbus,
        .media = media,
    };
}

void device_finish(Device *dev)
{
    if (battery_estimator_finish(&dev->battery_estimator))
        save_learned_drain_rate(dev);
}
