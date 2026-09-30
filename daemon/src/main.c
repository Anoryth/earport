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
#include "aap_link.h"
#include "aap_protocol.h"
#include "autoconnect.h"
#include "bluez_monitor.h"
#include "config.h"
#include "dbus_service.h"
#include "device.h"
#include "media_control.h"

/* Global application state */
typedef struct {
    GMainLoop *main_loop;
    AirPodsState state;
    AapLink *link;
    BluezMonitor *bluez_monitor;
    DbusService *dbus_service;
    MediaControl *media_control;
    EarPortConfig config;
    Device device;
    AutoConnect *autoconnect;
} AppContext;

static AppContext app = {0};

/* Forward declarations */
static gboolean apply_saved_settings_idle(gpointer user_data);

/* ============================================================================
 * Automatic connection
 * ========================================================================== */

static void on_proximity_keys(const AapProximityKeys *keys)
{
    g_mutex_lock(&app.state.lock);
    char *address = g_strdup(app.state.device_address);
    g_mutex_unlock(&app.state.lock);

    if (keys->has_irk && address != NULL)
        autoconnect_set_irk(app.autoconnect, address, keys->irk);
    g_free(address);
}

static void on_playback_started(void *user_data)
{
    (void)user_data;
    autoconnect_on_playback_started(app.autoconnect);
}

/* ============================================================================
 * AirPods link
 * ========================================================================== */

static void on_link_packet(const AapParsedPacket *pkt, void *user_data)
{
    (void)user_data;

    if (pkt->type == AAP_PKT_TYPE_PROXIMITY_KEYS)
        on_proximity_keys(&pkt->data.proximity_keys);
    else
        device_handle_packet(&app.device, pkt);
}

static void on_link_disconnected(void *user_data)
{
    (void)user_data;
    device_session_ended(&app.device);
}

static void on_link_connected(const char *address, const char *name, void *user_data)
{
    (void)user_data;
    device_session_started(&app.device, address, name);

    /* Schedule sending saved settings after connection stabilizes (500ms delay) */
    g_timeout_add(500, apply_saved_settings_idle, g_strdup(address));
}

/* ============================================================================
 * Device profile management
 * ========================================================================== */

static gboolean apply_saved_settings_idle(gpointer user_data)
{
    const char *address = (const char *)user_data;

    if (!aap_link_is_connected(app.link)) {
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
    aap_link_send(app.link, packet, AAP_CONTROL_CMD_SIZE);

    /* Send conversational awareness setting */
    g_usleep(50000);  /* 50ms delay between commands */
    aap_build_conv_awareness_cmd(profile.conversational_awareness, packet);
    aap_link_send(app.link, packet, AAP_CONTROL_CMD_SIZE);

    /* Send adaptive noise level */
    g_usleep(50000);
    aap_build_adaptive_level_cmd(profile.adaptive_noise_level, packet);
    aap_link_send(app.link, packet, AAP_CONTROL_CMD_SIZE);

    /* The values the AirPods announced on connection are now outdated */
    airpods_state_set_conversational_awareness(&app.state, profile.conversational_awareness);
    airpods_state_set_adaptive_noise_level(&app.state, profile.adaptive_noise_level);
    dbus_service_emit_properties_changed(app.dbus_service, "ConversationalAwareness");
    dbus_service_emit_properties_changed(app.dbus_service, "AdaptiveNoiseLevel");

    g_free((gchar *)address);
    return G_SOURCE_REMOVE;
}

/* ============================================================================
 * BlueZ callbacks
 * ========================================================================== */

static void on_bluez_device_connected(const BluezDeviceInfo *device, void *user_data)
{
    (void)user_data;
    g_message("BlueZ: AirPods connected - %s (%s)", device->name, device->address);
    /* The adapter the AirPods use is the one to scan on */
    char *adapter_path = device->object_path ? g_path_get_dirname(device->object_path) : NULL;
    autoconnect_set_airpods_connected(app.autoconnect, true, adapter_path);
    g_free(adapter_path);

    aap_link_device_connected(app.link, device->address, device->name);
}

static void on_bluez_device_disconnected(const BluezDeviceInfo *device, void *user_data)
{
    (void)user_data;
    g_message("BlueZ: AirPods disconnected - %s (%s)", device->name, device->address);
    aap_link_device_disconnected(app.link);
    autoconnect_set_airpods_connected(app.autoconnect, false, NULL);
}

/* ============================================================================
 * D-Bus method callbacks
 * ========================================================================== */

static void on_set_noise_control(NoiseControlMode mode, void *user_data)
{
    (void)user_data;

    if (!aap_link_is_connected(app.link)) {
        g_warning("Cannot set noise control: not connected");
        return;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_noise_control_cmd(mode, packet);
    aap_link_send(app.link, packet, AAP_CONTROL_CMD_SIZE);
}

static void on_set_conv_awareness(bool enabled, void *user_data)
{
    (void)user_data;

    if (!aap_link_is_connected(app.link)) {
        g_warning("Cannot set conversational awareness: not connected");
        return;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_conv_awareness_cmd(enabled, packet);
    aap_link_send(app.link, packet, AAP_CONTROL_CMD_SIZE);

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

    if (!aap_link_is_connected(app.link)) {
        g_warning("Cannot set adaptive level: not connected");
        return;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_adaptive_level_cmd(level, packet);
    aap_link_send(app.link, packet, AAP_CONTROL_CMD_SIZE);

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

    if (!aap_link_is_connected(app.link)) {
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
    aap_link_send(app.link, packet, AAP_CONTROL_CMD_SIZE);

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

    if (!aap_link_is_connected(app.link)) {
        g_warning("Cannot set %s: not connected", def->key);
        return false;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_control_cmd(def->id, &byte, 1, packet);
    aap_link_send(app.link, packet, AAP_CONTROL_CMD_SIZE);

    /* The AirPods don't echo settings back: update the state right away.
     * No need to save it in the profile, the AirPods remember it. */
    if (airpods_state_set_setting(&app.state, def->id, byte))
        dbus_service_emit_properties_changed(app.dbus_service, "Settings");

    return true;
}

static void on_set_auto_connect(bool enabled, void *user_data)
{
    (void)user_data;

    app.config.auto_connect = enabled;
    config_save(&app.config);
    dbus_service_set_auto_connect(app.dbus_service, enabled);
    autoconnect_set_enabled(app.autoconnect, enabled);
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

    autoconnect_free(app.autoconnect);
    app.autoconnect = NULL;

    device_finish(&app.device);

    aap_link_free(app.link);
    app.link = NULL;

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
    dbus_service_set_auto_connect_callback(app.dbus_service, on_set_auto_connect, NULL);
    dbus_service_set_auto_connect(app.dbus_service, app.config.auto_connect);

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
        media_control_set_playback_started_callback(app.media_control, on_playback_started, NULL);
    }

    device_init(&app.device, &app.state, app.dbus_service, app.media_control);
    app.autoconnect = autoconnect_new(app.config.auto_connect);

    static const AapLinkCallbacks link_callbacks = {
        .connected = on_link_connected,
        .disconnected = on_link_disconnected,
        .packet = on_link_packet,
    };
    app.link = aap_link_new(&link_callbacks, NULL);

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
    autoconnect_start(app.autoconnect);

    g_message("EarPort Daemon running. Press Ctrl+C to quit.");

    /* Run main loop */
    g_main_loop_run(app.main_loop);

    /* Cleanup */
    cleanup();

    g_message("EarPort Daemon stopped.");
    return 0;
}
