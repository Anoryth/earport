/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * EarPort Daemon - AirPods integration for Linux
 */

#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "airpods_state.h"
#include "aap_protocol.h"
#include "battery_estimator.h"
#include "bluetooth.h"
#include "bluez_monitor.h"
#include "config.h"
#include "dbus_service.h"
#include "media_control.h"

/* Global application state */
typedef struct {
    GMainLoop *main_loop;
    AirPodsState state;
    BluetoothConnection *bt_conn;
    BluezMonitor *bluez_monitor;
    DbusService *dbus_service;
    MediaControl *media_control;
    EarPortConfig config;

    /* Pending connect info */
    char *pending_address;
    char *pending_name;

    /* Reconnection */
    guint reconnect_timeout_id;
    int reconnect_attempts;
    bool bluez_connected;   /* Device still connected at BlueZ level */

    /* Notification request retries (until the first battery packet) */
    guint notif_retry_timeout_id;
    int notif_retry_attempts;
    bool battery_received;

    /* Silent L2CAP reconnect when battery info never arrives */
    bool silent_reconnect;       /* Reconnect in progress, hidden from clients */
    bool silent_reconnect_done;  /* Already tried for this BlueZ connection */

    bool ca_speaking;            /* Conversation awareness lowered the volume */

    /* Remaining listening time, learned per device */
    BatteryEstimator battery_estimator;
} AppContext;

/* L2CAP reconnection: AirPods frequently refuse the first L2CAP connect
 * right after the BlueZ link comes up, so retry with exponential backoff. */
#define RECONNECT_MAX_ATTEMPTS 5
#define RECONNECT_BASE_DELAY_SEC 2

/* When another Apple device (e.g. a nearby iPhone) is also connected to the
 * AirPods, they sometimes ignore our notification request: control commands
 * still flow, but battery and ear detection never arrive. Re-send the
 * request until the first battery packet shows up. */
#define NOTIF_RETRY_MAX_ATTEMPTS 5
#define NOTIF_RETRY_INTERVAL_SEC 2

/* If retries are not enough, re-open the L2CAP link once: a fresh
 * connection has been seen to restore battery updates. */
#define SILENT_RECONNECT_DELAY_MS 500

/* Opcode of the AirPods' acknowledgement of our SET_FEATURES packet */
#define AAP_OPCODE_FEATURES_ACK 0x2B

static AppContext app = {0};

/* Forward declarations */
static void connect_to_airpods(const char *address, const char *name);
static void disconnect_from_airpods(void);
static void apply_device_profile(const char *address);
static gboolean apply_saved_settings_idle(gpointer user_data);
static void schedule_reconnect(void);
static void cancel_reconnect(void);
static void cancel_notif_retry(void);
static void report_disconnected(void);

/* ============================================================================
 * Remaining listening time
 * ========================================================================== */

/* The learned discharge rate is kept per device */
static void save_learned_drain_rate(void)
{
    g_mutex_lock(&app.state.lock);
    char *address = g_strdup(app.state.device_address);
    g_mutex_unlock(&app.state.lock);

    g_message("Learned discharge rate: %.1f %%/h", app.battery_estimator.drain_rate);
    config_save_drain_rate(address, app.battery_estimator.drain_rate);
    g_free(address);
}

static void update_listening_time(const AapBatteryData *battery)
{
    int64_t now = g_get_monotonic_time() / G_USEC_PER_SEC;
    bool left_in_use = battery->left_status == BATTERY_STATUS_DISCHARGING;
    bool right_in_use = battery->right_status == BATTERY_STATUS_DISCHARGING;
    bool learned = false;

    learned |= battery_estimator_update(&app.battery_estimator, BATTERY_POD_LEFT, now,
                                        battery->left_level, left_in_use);
    learned |= battery_estimator_update(&app.battery_estimator, BATTERY_POD_RIGHT, now,
                                        battery->right_level, right_in_use);
    if (learned)
        save_learned_drain_rate();

    /* The pod in use with the lowest level runs out first */
    int level = -1;
    if (left_in_use)
        level = battery->left_level;
    if (right_in_use && (level < 0 || battery->right_level < level))
        level = battery->right_level;

    int minutes = battery_estimator_minutes_left(&app.battery_estimator, level);
    if (airpods_state_set_listening_minutes(&app.state, minutes))
        dbus_service_emit_properties_changed(app.dbus_service, "ListeningTimeRemaining");
}

/* ============================================================================
 * Bluetooth data handling
 * ========================================================================== */

static void on_bt_data_received(const uint8_t *data, size_t len, void *user_data)
{
    (void)user_data;

    /* Our notification request may have reached the AirPods before they
     * processed SET_FEATURES: request again once they acknowledge it. */
    if (aap_has_valid_header(data, len) &&
        aap_get_opcode(data, len) == AAP_OPCODE_FEATURES_ACK &&
        !app.battery_received) {
        g_debug("Features acknowledged, requesting notifications again");
        bt_connection_send_request_notifications(app.bt_conn);
    }

    AapParsedPacket packet;
    AapParseResult result = aap_parse_packet(data, len, &packet);

    if (result != AAP_PARSE_OK) {
        if (result != AAP_PARSE_UNKNOWN_OPCODE) {
            g_debug("Failed to parse packet: %d", result);
        }
        return;
    }

    switch (packet.type) {
    case AAP_PKT_TYPE_BATTERY:
        app.battery_received = true;
        cancel_notif_retry();

        g_message("Battery: L=%d%% (status=%d) R=%d%% (status=%d) Case=%d%% (status=%d)",
                  packet.data.battery.left_level,
                  packet.data.battery.left_status,
                  packet.data.battery.right_level,
                  packet.data.battery.right_status,
                  packet.data.battery.case_level,
                  packet.data.battery.case_status);

        airpods_state_set_battery(&app.state,
                                   packet.data.battery.left_level,
                                   packet.data.battery.left_status,
                                   packet.data.battery.right_level,
                                   packet.data.battery.right_status,
                                   packet.data.battery.case_level,
                                   packet.data.battery.case_status);

        dbus_service_emit_battery_changed(app.dbus_service,
                                           packet.data.battery.left_level,
                                           packet.data.battery.right_level,
                                           packet.data.battery.case_level);
        dbus_service_emit_properties_changed(app.dbus_service, "BatteryLeft");
        dbus_service_emit_properties_changed(app.dbus_service, "BatteryRight");
        dbus_service_emit_properties_changed(app.dbus_service, "BatteryCase");
        dbus_service_emit_properties_changed(app.dbus_service, "ChargingLeft");
        dbus_service_emit_properties_changed(app.dbus_service, "ChargingRight");
        dbus_service_emit_properties_changed(app.dbus_service, "ChargingCase");

        update_listening_time(&packet.data.battery);
        break;

    case AAP_PKT_TYPE_EAR_DETECTION: {
        bool primary_in_ear = packet.data.ear_detection.primary_in_ear;
        bool secondary_in_ear = packet.data.ear_detection.secondary_in_ear;

        g_message("Ear detection: primary=%s secondary=%s",
                  primary_in_ear ? "in" : "out",
                  secondary_in_ear ? "in" : "out");

        /* AirPods Max have a single sensor: the secondary slot always reads
         * "out", which would jam the one-out auto-pause logic. Mirror the
         * primary status instead. */
        g_mutex_lock(&app.state.lock);
        bool is_headphones = airpods_model_is_headphones(app.state.model);
        g_mutex_unlock(&app.state.lock);
        if (is_headphones)
            secondary_in_ear = primary_in_ear;

        airpods_state_set_ear_detection(&app.state,
                                         primary_in_ear,
                                         secondary_in_ear,
                                         packet.data.ear_detection.primary_left);

        dbus_service_emit_ear_detection_changed(app.dbus_service,
                                                 app.state.ear_detection.left_in_ear,
                                                 app.state.ear_detection.right_in_ear);
        dbus_service_emit_properties_changed(app.dbus_service, "LeftInEar");
        dbus_service_emit_properties_changed(app.dbus_service, "RightInEar");

        /* Trigger media pause/resume based on ear detection */
        if (app.media_control) {
            media_control_on_ear_detection_changed(app.media_control,
                                                    app.state.ear_detection.left_in_ear,
                                                    app.state.ear_detection.right_in_ear);
        }
        break;
    }

    case AAP_PKT_TYPE_NOISE_CONTROL:
        g_message("Noise control mode: %s",
                  noise_control_mode_to_string(packet.data.noise_control));

        airpods_state_set_noise_control(&app.state, packet.data.noise_control);

        dbus_service_emit_noise_control_changed(app.dbus_service,
                                                 packet.data.noise_control);
        dbus_service_emit_properties_changed(app.dbus_service, "NoiseControlMode");
        break;

    case AAP_PKT_TYPE_CONV_AWARENESS:
        g_message("Conversational awareness: %s",
                  packet.data.conversational_awareness ? "enabled" : "disabled");

        airpods_state_set_conversational_awareness(&app.state,
                                                    packet.data.conversational_awareness);

        dbus_service_emit_properties_changed(app.dbus_service, "ConversationalAwareness");
        break;

    case AAP_PKT_TYPE_CA_DETECTION: {
        int speaking = aap_ca_speaking_from_level(packet.data.ca_volume_level);
        g_debug("CA detection event: level=%d", packet.data.ca_volume_level);

        if (speaking >= 0 && (bool)speaking != app.ca_speaking) {
            app.ca_speaking = speaking;
            g_message("Conversation %s", speaking ? "started" : "ended");
            dbus_service_emit_speaking_changed(app.dbus_service, app.ca_speaking);
        }
        break;
    }

    case AAP_PKT_TYPE_LISTENING_MODES:
        g_message("Listening modes: off=%s transparency=%s anc=%s adaptive=%s (raw=0x%02X)",
                  packet.data.listening_modes.off_enabled ? "on" : "off",
                  packet.data.listening_modes.transparency_enabled ? "on" : "off",
                  packet.data.listening_modes.anc_enabled ? "on" : "off",
                  packet.data.listening_modes.adaptive_enabled ? "on" : "off",
                  packet.data.listening_modes.raw_value);

        airpods_state_set_listening_modes(&app.state,
                                           packet.data.listening_modes.off_enabled,
                                           packet.data.listening_modes.transparency_enabled,
                                           packet.data.listening_modes.anc_enabled,
                                           packet.data.listening_modes.adaptive_enabled);

        dbus_service_emit_properties_changed(app.dbus_service, "ListeningModeOff");
        dbus_service_emit_properties_changed(app.dbus_service, "ListeningModeTransparency");
        dbus_service_emit_properties_changed(app.dbus_service, "ListeningModeANC");
        dbus_service_emit_properties_changed(app.dbus_service, "ListeningModeAdaptive");
        break;

    case AAP_PKT_TYPE_CONTROL_SETTING: {
        const AirPodsSettingDef *def = airpods_setting_by_id(packet.data.control_setting.id);
        if (def == NULL)
            break;  /* Not exposed yet */

        uint8_t value = packet.data.control_setting.value[0];
        g_message("Setting %s: 0x%02X", def->key, value);

        if (airpods_state_set_setting(&app.state, def->id, value))
            dbus_service_emit_properties_changed(app.dbus_service, "Settings");
        break;
    }

    case AAP_PKT_TYPE_METADATA:
        g_message("Metadata received: device='%s' model='%s' manufacturer='%s'",
                  packet.data.metadata.device_name,
                  packet.data.metadata.model_number,
                  packet.data.metadata.manufacturer);

        /* Update model from model number */
        {
            AirPodsModel detected_model = airpods_model_from_number(packet.data.metadata.model_number);
            if (detected_model != AIRPODS_MODEL_UNKNOWN) {
                g_mutex_lock(&app.state.lock);
                app.state.model = detected_model;
                g_mutex_unlock(&app.state.lock);

                g_message("Detected AirPods model: %s", airpods_model_to_string(detected_model));
                dbus_service_emit_properties_changed(app.dbus_service, "DeviceModel");
                dbus_service_emit_properties_changed(app.dbus_service, "IsHeadphones");
                dbus_service_emit_properties_changed(app.dbus_service, "SupportsANC");
                dbus_service_emit_properties_changed(app.dbus_service, "SupportsAdaptive");
                /* DisplayName falls back to the model name, refresh it too */
                dbus_service_emit_properties_changed(app.dbus_service, "DisplayName");
            }
        }
        break;

    default:
        break;
    }
}

/* ============================================================================
 * L2CAP reconnection with exponential backoff
 * ========================================================================== */

static gboolean reconnect_timeout_cb(gpointer user_data)
{
    (void)user_data;
    app.reconnect_timeout_id = 0;

    if (!app.bluez_connected || app.pending_address == NULL)
        return G_SOURCE_REMOVE;

    if (app.bt_conn && bt_connection_is_connected(app.bt_conn))
        return G_SOURCE_REMOVE;

    g_message("L2CAP reconnect attempt %d/%d to %s",
              app.reconnect_attempts, RECONNECT_MAX_ATTEMPTS, app.pending_address);

    /* On failure, the BT_STATE_ERROR callback schedules the next attempt */
    connect_to_airpods(app.pending_address, app.pending_name);

    return G_SOURCE_REMOVE;
}

static void schedule_reconnect(void)
{
    if (app.reconnect_timeout_id > 0)
        return;

    if (!app.bluez_connected || app.pending_address == NULL)
        return;

    if (app.reconnect_attempts >= RECONNECT_MAX_ATTEMPTS) {
        g_warning("Giving up L2CAP reconnection after %d attempts", app.reconnect_attempts);
        return;
    }

    guint delay = RECONNECT_BASE_DELAY_SEC << app.reconnect_attempts;  /* 2,4,8,16,32s */
    app.reconnect_attempts++;

    g_message("Scheduling L2CAP reconnect attempt %d/%d in %us",
              app.reconnect_attempts, RECONNECT_MAX_ATTEMPTS, delay);

    app.reconnect_timeout_id = g_timeout_add_seconds(delay, reconnect_timeout_cb, NULL);
}

static void cancel_reconnect(void)
{
    if (app.reconnect_timeout_id > 0) {
        g_source_remove(app.reconnect_timeout_id);
        app.reconnect_timeout_id = 0;
    }
    app.reconnect_attempts = 0;
}

/* ============================================================================
 * Notification request retries
 * ========================================================================== */

static gboolean notif_retry_timeout_cb(gpointer user_data)
{
    (void)user_data;

    if (app.battery_received || !app.bt_conn || !bt_connection_is_connected(app.bt_conn)) {
        app.notif_retry_timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    if (app.notif_retry_attempts >= NOTIF_RETRY_MAX_ATTEMPTS) {
        app.notif_retry_timeout_id = 0;

        if (!app.silent_reconnect_done) {
            g_message("No battery info after %d notification requests, "
                      "re-opening the AirPods link", app.notif_retry_attempts);
            app.silent_reconnect = true;
            app.silent_reconnect_done = true;
            bt_connection_disconnect(app.bt_conn);
        } else {
            g_warning("No battery info after %d notification requests "
                      "(another Apple device may own the AirPods connection)",
                      app.notif_retry_attempts);
        }
        return G_SOURCE_REMOVE;
    }

    app.notif_retry_attempts++;
    g_message("No battery info yet, re-requesting notifications (%d/%d)",
              app.notif_retry_attempts, NOTIF_RETRY_MAX_ATTEMPTS);

    bt_connection_send_set_features(app.bt_conn);
    g_usleep(50000);
    bt_connection_send_request_notifications(app.bt_conn);

    return G_SOURCE_CONTINUE;
}

static void cancel_notif_retry(void)
{
    if (app.notif_retry_timeout_id > 0) {
        g_source_remove(app.notif_retry_timeout_id);
        app.notif_retry_timeout_id = 0;
    }
}

static void start_notif_retry(void)
{
    cancel_notif_retry();
    app.battery_received = false;
    app.notif_retry_attempts = 0;
    app.notif_retry_timeout_id = g_timeout_add_seconds(NOTIF_RETRY_INTERVAL_SEC,
                                                       notif_retry_timeout_cb, NULL);
}

static gboolean silent_reconnect_cb(gpointer user_data)
{
    (void)user_data;

    if (!app.silent_reconnect)
        return G_SOURCE_REMOVE;

    if (app.bluez_connected && app.pending_address != NULL)
        connect_to_airpods(app.pending_address, app.pending_name);
    else
        report_disconnected();

    return G_SOURCE_REMOVE;
}

/* Tell clients the AirPods are gone and forget their state */
static void report_disconnected(void)
{
    app.silent_reconnect = false;

    if (app.state.connected) {
        dbus_service_emit_device_disconnected(app.dbus_service,
                                               app.state.device_address,
                                               app.state.device_name);
    }

    /* Let clients restore the volume lowered for a conversation */
    if (app.ca_speaking) {
        app.ca_speaking = false;
        dbus_service_emit_speaking_changed(app.dbus_service, false);
    }

    /* Learn from the sessions cut short by the disconnection (needs the
     * device address, so before the state reset) */
    if (battery_estimator_finish(&app.battery_estimator))
        save_learned_drain_rate();

    cancel_notif_retry();
    airpods_state_reset(&app.state);
    dbus_service_emit_properties_changed(app.dbus_service, "Connected");
    dbus_service_emit_properties_changed(app.dbus_service, "Settings");
}

static void on_bt_state_changed(BluetoothState state, const char *error, void *user_data)
{
    (void)user_data;

    switch (state) {
    case BT_STATE_CONNECTED:
        g_message("Bluetooth connected, sending handshake...");
        cancel_reconnect();

        /* Attach to main loop for data reception */
        bt_connection_attach_to_mainloop(app.bt_conn, NULL);

        /* Send initialization sequence */
        g_usleep(100000);  /* 100ms delay */
        bt_connection_send_handshake(app.bt_conn);

        g_usleep(50000);  /* 50ms delay */
        bt_connection_send_set_features(app.bt_conn);

        g_usleep(50000);
        bt_connection_send_request_notifications(app.bt_conn);

        start_notif_retry();

        if (app.silent_reconnect) {
            /* Clients never saw the link go down: keep the current state
             * and don't announce a new connection. */
            app.silent_reconnect = false;
            g_message("AirPods link re-opened");
            break;
        }

        /* Update state */
        airpods_state_set_device(&app.state,
                                  app.pending_name,
                                  app.pending_address,
                                  AIRPODS_MODEL_UNKNOWN);  /* Model detected later via metadata */

        /* Load and apply saved device profile */
        apply_device_profile(app.pending_address);
        battery_estimator_init(&app.battery_estimator,
                               config_load_drain_rate(app.pending_address));

        /* Emit property changes BEFORE the DeviceConnected signal so the
         * extension's proxy cache is up-to-date when its handler reads
         * DisplayName for the connection notification. */
        dbus_service_emit_properties_changed(app.dbus_service, "Connected");
        dbus_service_emit_properties_changed(app.dbus_service, "DeviceName");
        dbus_service_emit_properties_changed(app.dbus_service, "DeviceAddress");
        dbus_service_emit_properties_changed(app.dbus_service, "DisplayName");

        dbus_service_emit_device_connected(app.dbus_service,
                                            app.pending_address,
                                            app.pending_name);

        /* Schedule sending saved settings after connection stabilizes (500ms delay) */
        g_timeout_add(500, apply_saved_settings_idle, g_strdup(app.pending_address));
        break;

    case BT_STATE_DISCONNECTED:
        g_message("Bluetooth disconnected");

        if (app.silent_reconnect) {
            g_timeout_add(SILENT_RECONNECT_DELAY_MS, silent_reconnect_cb, NULL);
            break;
        }

        report_disconnected();

        /* L2CAP dropped but the device is still connected at BlueZ level
         * (e.g. AirPods went idle): try to re-establish the link. */
        schedule_reconnect();
        break;

    case BT_STATE_ERROR:
        g_warning("Bluetooth error: %s", error ? error : "unknown");

        /* The silent reconnect failed: the link is really down now */
        if (app.silent_reconnect)
            report_disconnected();

        schedule_reconnect();
        break;

    default:
        break;
    }
}

/* ============================================================================
 * Device profile management
 * ========================================================================== */

static void apply_device_profile(const char *address)
{
    if (address == NULL || address[0] == '\0') {
        return;
    }

    DeviceProfile profile;
    bool has_profile = config_load_device_profile(address, &profile);

    if (!has_profile || !profile.has_saved_settings) {
        g_message("No saved profile for device %s, using defaults", address);
        return;
    }

    g_message("Applying saved profile for device %s", address);

    /* Apply display name */
    airpods_state_set_display_name(&app.state, profile.display_name);

    /* Apply listening modes */
    airpods_state_set_listening_modes(&app.state,
                                       profile.listening_modes.off_enabled,
                                       profile.listening_modes.transparency_enabled,
                                       profile.listening_modes.anc_enabled,
                                       profile.listening_modes.adaptive_enabled);

    /* Apply conversational awareness (will be sent after connection stabilizes) */
    g_mutex_lock(&app.state.lock);
    app.state.conversational_awareness = profile.conversational_awareness;
    app.state.adaptive_noise_level = profile.adaptive_noise_level;
    g_mutex_unlock(&app.state.lock);
}

static gboolean apply_saved_settings_idle(gpointer user_data)
{
    const char *address = (const char *)user_data;

    if (!app.bt_conn || !bt_connection_is_connected(app.bt_conn)) {
        g_free((gchar *)address);
        return G_SOURCE_REMOVE;
    }

    DeviceProfile profile;
    if (!config_load_device_profile(address, &profile) || !profile.has_saved_settings) {
        g_free((gchar *)address);
        return G_SOURCE_REMOVE;
    }

    g_message("Sending saved settings to AirPods...");

    /* Send listening modes configuration */
    uint8_t modes = 0;
    if (profile.listening_modes.off_enabled) modes |= AAP_LISTENING_MODE_OFF;
    if (profile.listening_modes.transparency_enabled) modes |= AAP_LISTENING_MODE_TRANSPARENCY;
    if (profile.listening_modes.anc_enabled) modes |= AAP_LISTENING_MODE_ANC;
    if (profile.listening_modes.adaptive_enabled) modes |= AAP_LISTENING_MODE_ADAPTIVE;

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_listening_modes_cmd(modes, packet);
    bt_connection_send(app.bt_conn, packet, AAP_CONTROL_CMD_SIZE);

    /* Send conversational awareness setting */
    g_usleep(50000);  /* 50ms delay between commands */
    aap_build_conv_awareness_cmd(profile.conversational_awareness, packet);
    bt_connection_send(app.bt_conn, packet, AAP_CONTROL_CMD_SIZE);

    /* Send adaptive noise level */
    g_usleep(50000);
    aap_build_adaptive_level_cmd(profile.adaptive_noise_level, packet);
    bt_connection_send(app.bt_conn, packet, AAP_CONTROL_CMD_SIZE);

    /* The values the AirPods announced on connection are now outdated */
    airpods_state_set_conversational_awareness(&app.state, profile.conversational_awareness);
    airpods_state_set_adaptive_noise_level(&app.state, profile.adaptive_noise_level);
    dbus_service_emit_properties_changed(app.dbus_service, "ConversationalAwareness");
    dbus_service_emit_properties_changed(app.dbus_service, "AdaptiveNoiseLevel");

    g_free((gchar *)address);
    return G_SOURCE_REMOVE;
}

/* ============================================================================
 * Connection management
 * ========================================================================== */

static void connect_to_airpods(const char *address, const char *name)
{
    if (app.bt_conn && bt_connection_is_connected(app.bt_conn)) {
        g_message("Already connected, ignoring connect request");
        return;
    }

    /* Store pending info (dup first: on reconnect, address/name may alias
     * app.pending_address/app.pending_name) */
    char *addr_copy = g_strdup(address);
    char *name_copy = g_strdup(name);
    g_free(app.pending_address);
    g_free(app.pending_name);
    app.pending_address = addr_copy;
    app.pending_name = name_copy;

    /* Create new connection if needed */
    if (app.bt_conn == NULL) {
        app.bt_conn = bt_connection_new();
        bt_connection_set_data_callback(app.bt_conn, on_bt_data_received, NULL);
        bt_connection_set_state_callback(app.bt_conn, on_bt_state_changed, NULL);
    }

    /* address/name may have just been freed: only use the copies from here */
    g_message("Connecting to AirPods: %s (%s)", app.pending_name, app.pending_address);

    if (!bt_connection_connect(app.bt_conn, app.pending_address)) {
        g_warning("Failed to initiate connection");
    }
}

static void disconnect_from_airpods(void)
{
    if (app.bt_conn) {
        bt_connection_disconnect(app.bt_conn);
    }
}

/* ============================================================================
 * BlueZ callbacks
 * ========================================================================== */

static void on_bluez_device_connected(const BluezDeviceInfo *device, void *user_data)
{
    (void)user_data;
    g_message("BlueZ: AirPods connected - %s (%s)", device->name, device->address);
    app.bluez_connected = true;
    app.reconnect_attempts = 0;
    app.silent_reconnect_done = false;
    connect_to_airpods(device->address, device->name);
}

static void on_bluez_device_disconnected(const BluezDeviceInfo *device, void *user_data)
{
    (void)user_data;
    g_message("BlueZ: AirPods disconnected - %s (%s)", device->name, device->address);
    app.bluez_connected = false;
    cancel_reconnect();

    /* The AirPods left during a silent reconnect: clients still think
     * they are connected, so report it now. */
    if (app.silent_reconnect)
        report_disconnected();

    disconnect_from_airpods();
}

/* ============================================================================
 * D-Bus method callbacks
 * ========================================================================== */

static void on_set_noise_control(NoiseControlMode mode, void *user_data)
{
    (void)user_data;

    if (!app.bt_conn || !bt_connection_is_connected(app.bt_conn)) {
        g_warning("Cannot set noise control: not connected");
        return;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_noise_control_cmd(mode, packet);
    bt_connection_send(app.bt_conn, packet, AAP_CONTROL_CMD_SIZE);
}

static void on_set_conv_awareness(bool enabled, void *user_data)
{
    (void)user_data;

    if (!app.bt_conn || !bt_connection_is_connected(app.bt_conn)) {
        g_warning("Cannot set conversational awareness: not connected");
        return;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_conv_awareness_cmd(enabled, packet);
    bt_connection_send(app.bt_conn, packet, AAP_CONTROL_CMD_SIZE);

    /* The AirPods don't echo the change back */
    airpods_state_set_conversational_awareness(&app.state, enabled);
    dbus_service_emit_properties_changed(app.dbus_service, "ConversationalAwareness");

    /* Save to device profile */
    if (app.state.device_address && app.state.device_address[0] != '\0') {
        DeviceProfile profile;
        config_load_device_profile(app.state.device_address, &profile);
        profile.conversational_awareness = enabled;
        config_save_device_profile(app.state.device_address, &profile);
    }
}

static void on_set_adaptive_level(int level, void *user_data)
{
    (void)user_data;

    if (!app.bt_conn || !bt_connection_is_connected(app.bt_conn)) {
        g_warning("Cannot set adaptive level: not connected");
        return;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_adaptive_level_cmd(level, packet);
    bt_connection_send(app.bt_conn, packet, AAP_CONTROL_CMD_SIZE);

    airpods_state_set_adaptive_noise_level(&app.state, level);
    dbus_service_emit_properties_changed(app.dbus_service, "AdaptiveNoiseLevel");

    /* Save to device profile */
    if (app.state.device_address && app.state.device_address[0] != '\0') {
        DeviceProfile profile;
        config_load_device_profile(app.state.device_address, &profile);
        profile.adaptive_noise_level = level;
        config_save_device_profile(app.state.device_address, &profile);
    }
}

static void on_set_ear_pause_mode(int mode, void *user_data)
{
    (void)user_data;

    g_message("Setting ear pause mode to %d", mode);

    /* Update state */
    g_mutex_lock(&app.state.lock);
    app.state.ear_pause_mode = mode;
    g_mutex_unlock(&app.state.lock);

    /* Update media control */
    if (app.media_control) {
        media_control_set_ear_pause_mode(app.media_control, (EarPauseMode)mode);
    }

    /* Save to config file */
    app.config.ear_pause_mode = mode;
    config_save(&app.config);

    /* Notify property change */
    dbus_service_emit_properties_changed(app.dbus_service, "EarPauseMode");
}

static void on_set_listening_modes(bool off, bool transparency, bool anc, bool adaptive, void *user_data)
{
    (void)user_data;

    if (!app.bt_conn || !bt_connection_is_connected(app.bt_conn)) {
        g_warning("Cannot set listening modes: not connected");
        return;
    }

    /* Build the bitmask */
    uint8_t modes = 0;
    if (off) modes |= AAP_LISTENING_MODE_OFF;
    if (transparency) modes |= AAP_LISTENING_MODE_TRANSPARENCY;
    if (anc) modes |= AAP_LISTENING_MODE_ANC;
    if (adaptive) modes |= AAP_LISTENING_MODE_ADAPTIVE;

    /* Ensure at least 2 modes are enabled */
    int count = (off ? 1 : 0) + (transparency ? 1 : 0) + (anc ? 1 : 0) + (adaptive ? 1 : 0);
    if (count < 2) {
        g_warning("At least 2 listening modes must be enabled");
        return;
    }

    g_message("Setting listening modes: 0x%02X", modes);

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_listening_modes_cmd(modes, packet);
    bt_connection_send(app.bt_conn, packet, AAP_CONTROL_CMD_SIZE);

    /* Update local state immediately */
    airpods_state_set_listening_modes(&app.state, off, transparency, anc, adaptive);

    /* Save to device profile */
    if (app.state.device_address && app.state.device_address[0] != '\0') {
        DeviceProfile profile;
        config_load_device_profile(app.state.device_address, &profile);
        profile.listening_modes.off_enabled = off;
        profile.listening_modes.transparency_enabled = transparency;
        profile.listening_modes.anc_enabled = anc;
        profile.listening_modes.adaptive_enabled = adaptive;
        config_save_device_profile(app.state.device_address, &profile);
    }

    dbus_service_emit_properties_changed(app.dbus_service, "ListeningModeOff");
    dbus_service_emit_properties_changed(app.dbus_service, "ListeningModeTransparency");
    dbus_service_emit_properties_changed(app.dbus_service, "ListeningModeANC");
    dbus_service_emit_properties_changed(app.dbus_service, "ListeningModeAdaptive");
}

static bool on_set_setting(const AirPodsSettingDef *def, uint8_t byte, void *user_data)
{
    (void)user_data;

    if (!app.bt_conn || !bt_connection_is_connected(app.bt_conn)) {
        g_warning("Cannot set %s: not connected", def->key);
        return false;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_control_cmd(def->id, &byte, 1, packet);
    bt_connection_send(app.bt_conn, packet, AAP_CONTROL_CMD_SIZE);

    /* The AirPods don't echo settings back: update the state right away.
     * No need to save it in the profile, the AirPods remember it. */
    if (airpods_state_set_setting(&app.state, def->id, byte))
        dbus_service_emit_properties_changed(app.dbus_service, "Settings");

    return true;
}

static void on_set_display_name(const char *name, void *user_data)
{
    (void)user_data;

    g_message("Setting display name to '%s'", name ? name : "");

    /* Update state */
    airpods_state_set_display_name(&app.state, name);

    /* Save to device profile */
    if (app.state.device_address && app.state.device_address[0] != '\0') {
        DeviceProfile profile;
        config_load_device_profile(app.state.device_address, &profile);
        if (name && name[0] != '\0') {
            strncpy(profile.display_name, name, sizeof(profile.display_name) - 1);
            profile.display_name[sizeof(profile.display_name) - 1] = '\0';
        } else {
            profile.display_name[0] = '\0';
        }
        config_save_device_profile(app.state.device_address, &profile);
    }

    /* Notify property change */
    dbus_service_emit_properties_changed(app.dbus_service, "DisplayName");
}

/* ============================================================================
 * Signal handlers
 * ========================================================================== */

static gboolean on_sigint(gpointer user_data)
{
    (void)user_data;
    g_message("Received SIGINT, shutting down...");
    g_main_loop_quit(app.main_loop);
    return G_SOURCE_REMOVE;
}

static gboolean on_sigterm(gpointer user_data)
{
    (void)user_data;
    g_message("Received SIGTERM, shutting down...");
    g_main_loop_quit(app.main_loop);
    return G_SOURCE_REMOVE;
}

/* ============================================================================
 * Main
 * ========================================================================== */

static void cleanup(void)
{
    g_message("Cleaning up...");

    if (battery_estimator_finish(&app.battery_estimator))
        save_learned_drain_rate();

    cancel_reconnect();
    cancel_notif_retry();

    if (app.bt_conn) {
        bt_connection_free(app.bt_conn);
        app.bt_conn = NULL;
    }

    if (app.bluez_monitor) {
        bluez_monitor_free(app.bluez_monitor);
        app.bluez_monitor = NULL;
    }

    if (app.dbus_service) {
        dbus_service_free(app.dbus_service);
        app.dbus_service = NULL;
    }

    if (app.media_control) {
        media_control_free(app.media_control);
        app.media_control = NULL;
    }

    g_free(app.pending_address);
    g_free(app.pending_name);

    airpods_state_cleanup(&app.state);

    if (app.main_loop) {
        g_main_loop_unref(app.main_loop);
        app.main_loop = NULL;
    }
}

int main(int argc, char *argv[])
{
    if (argc > 1) {
        if (strcmp(argv[1], "--version") == 0) {
            printf("earport-daemon %s\n", EARPORT_VERSION);
            return 0;
        }
        printf("Usage: %s [--version]\n"
               "AirPods integration service for GNOME, started by systemd or D-Bus.\n",
               argv[0]);
        return strcmp(argv[1], "--help") == 0 ? 0 : 1;
    }

    g_message("EarPort Daemon %s starting...", EARPORT_VERSION);

    /* Load configuration */
    config_load(&app.config);

    /* Initialize state */
    airpods_state_init(&app.state);

    /* Create main loop */
    app.main_loop = g_main_loop_new(NULL, FALSE);

    /* Set up signal handlers */
    g_unix_signal_add(SIGINT, on_sigint, NULL);
    g_unix_signal_add(SIGTERM, on_sigterm, NULL);

    /* Create D-Bus service */
    app.dbus_service = dbus_service_new(&app.state);
    if (app.dbus_service == NULL) {
        g_error("Failed to create D-Bus service");
        cleanup();
        return 1;
    }

    dbus_service_set_noise_control_callback(app.dbus_service, on_set_noise_control, NULL);
    dbus_service_set_conv_awareness_callback(app.dbus_service, on_set_conv_awareness, NULL);
    dbus_service_set_adaptive_level_callback(app.dbus_service, on_set_adaptive_level, NULL);
    dbus_service_set_ear_pause_mode_callback(app.dbus_service, on_set_ear_pause_mode, NULL);
    dbus_service_set_listening_modes_callback(app.dbus_service, on_set_listening_modes, NULL);
    dbus_service_set_display_name_callback(app.dbus_service, on_set_display_name, NULL);
    dbus_service_set_setting_callback(app.dbus_service, on_set_setting, NULL);

    if (!dbus_service_start(app.dbus_service)) {
        g_error("Failed to start D-Bus service");
        cleanup();
        return 1;
    }

    /* Create media control for MPRIS integration */
    app.media_control = media_control_new();
    if (app.media_control == NULL) {
        g_warning("Failed to create media control (MPRIS pause/resume disabled)");
    } else {
        /* Load ear pause mode from config */
        app.state.ear_pause_mode = app.config.ear_pause_mode;
        media_control_set_ear_pause_mode(app.media_control, (EarPauseMode)app.config.ear_pause_mode);
        g_message("Media control enabled (ear_pause_mode=%d)", app.config.ear_pause_mode);
    }

    /* Create BlueZ monitor */
    app.bluez_monitor = bluez_monitor_new();
    if (app.bluez_monitor == NULL) {
        g_error("Failed to create BlueZ monitor");
        cleanup();
        return 1;
    }

    bluez_monitor_set_connected_callback(app.bluez_monitor, on_bluez_device_connected, NULL);
    bluez_monitor_set_disconnected_callback(app.bluez_monitor, on_bluez_device_disconnected, NULL);

    if (!bluez_monitor_start(app.bluez_monitor)) {
        g_error("Failed to start BlueZ monitor");
        cleanup();
        return 1;
    }

    /* Check for already connected devices */
    bluez_monitor_check_existing_devices(app.bluez_monitor);

    g_message("EarPort Daemon running. Press Ctrl+C to quit.");

    /* Run main loop */
    g_main_loop_run(app.main_loop);

    /* Cleanup */
    cleanup();

    g_message("EarPort Daemon stopped.");
    return 0;
}
