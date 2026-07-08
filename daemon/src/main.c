/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 LibrePods Contributors
 *
 * LibrePods Daemon - AirPods integration for Linux
 */

#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "airpods_state.h"
#include "aap_protocol.h"
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
    LibrePodsConfig config;

    /* Pending connect info */
    char *pending_address;
    char *pending_name;

    /* Reconnection */
    guint reconnect_timeout_id;
    int reconnect_attempts;
    bool bluez_connected;   /* Device still connected at BlueZ level */
} AppContext;

/* L2CAP reconnection: AirPods frequently refuse the first L2CAP connect
 * right after the BlueZ link comes up, so retry with exponential backoff. */
#define RECONNECT_MAX_ATTEMPTS 5
#define RECONNECT_BASE_DELAY_SEC 2

static AppContext app = {0};

/* Forward declarations */
static void connect_to_airpods(const char *address, const char *name);
static void disconnect_from_airpods(void);
static void apply_device_profile(const char *address);
static gboolean apply_saved_settings_idle(gpointer user_data);
static void schedule_reconnect(void);
static void cancel_reconnect(void);

/* ============================================================================
 * Bluetooth data handling
 * ========================================================================== */

static void on_bt_data_received(const uint8_t *data, size_t len, void *user_data)
{
    (void)user_data;

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

    case AAP_PKT_TYPE_CA_DETECTION:
        g_debug("CA detection event: volume_level=%d", packet.data.ca_volume_level);
        break;

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

        /* Update state */
        airpods_state_set_device(&app.state,
                                  app.pending_name,
                                  app.pending_address,
                                  AIRPODS_MODEL_UNKNOWN);  /* Model detected later via metadata */

        /* Load and apply saved device profile */
        apply_device_profile(app.pending_address);

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

        if (app.state.connected) {
            dbus_service_emit_device_disconnected(app.dbus_service,
                                                   app.state.device_address,
                                                   app.state.device_name);
        }

        airpods_state_reset(&app.state);
        dbus_service_emit_properties_changed(app.dbus_service, "Connected");

        /* L2CAP dropped but the device is still connected at BlueZ level
         * (e.g. AirPods went idle): try to re-establish the link. */
        schedule_reconnect();
        break;

    case BT_STATE_ERROR:
        g_warning("Bluetooth error: %s", error ? error : "unknown");
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

    g_message("Connecting to AirPods: %s (%s)", name, address);

    if (!bt_connection_connect(app.bt_conn, address)) {
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
    connect_to_airpods(device->address, device->name);
}

static void on_bluez_device_disconnected(const BluezDeviceInfo *device, void *user_data)
{
    (void)user_data;
    g_message("BlueZ: AirPods disconnected - %s (%s)", device->name, device->address);
    app.bluez_connected = false;
    cancel_reconnect();
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

    cancel_reconnect();

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
    (void)argc;
    (void)argv;

    g_message("LibrePods Daemon starting...");

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

    g_message("LibrePods Daemon running. Press Ctrl+C to quit.");

    /* Run main loop */
    g_main_loop_run(app.main_loop);

    /* Cleanup */
    cleanup();

    g_message("LibrePods Daemon stopped.");
    return 0;
}
