/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * EarPort Daemon - AirPods integration for Linux
 */

#include <gio/gio.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "airpods_state.h"
#include "aap_link.h"
#include "aap_protocol.h"
#include "autoconnect.h"
#include "battery_provider.h"
#include "bluez_monitor.h"
#include "config.h"
#include "controls.h"
#include "dbus_service.h"
#include "device.h"
#include "media_control.h"
#include "research.h"
#include "sleep_pause.h"

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
    Controls controls;
    AutoConnect *autoconnect;
    BatteryProvider *battery_provider;
    char *adapter_path;          /* Adapter the AirPods are connected through */
    SleepPause *sleep_pause;
} AppContext;

static AppContext app = {0};

/* ============================================================================
 * Automatic connection
 * ========================================================================== */

static void on_proximity_keys(const AapProximityKeys *keys)
{
    if (keys->has_irk && app.state.device_address != NULL)
        autoconnect_set_irk(app.autoconnect, app.state.device_address, keys->irk);
    if (keys->has_irk && keys->has_enc && app.state.device_address != NULL)
        autoconnect_set_enc(app.autoconnect, app.state.device_address, keys->enc);
}

/* What the AirPods do with the device they are connected to */
static const char *nearby_host(uint8_t connection_state)
{
    switch (connection_state) {
    case PROXIMITY_CONN_DISCONNECTED:
        return "none";
    case PROXIMITY_CONN_MUSIC:
        return "music";
    case PROXIMITY_CONN_CALL:
    case PROXIMITY_CONN_RINGING:
    case PROXIMITY_CONN_HANGING_UP:
        return "call";
    default:
        return "idle";
    }
}

/* Bluetooth name of the AirPods, when they have no custom one */
static void on_alias_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, NULL);
    if (result == NULL)
        return;

    GVariant *value;
    g_variant_get(result, "(v)", &value);
    /* Still nearby, and still not renamed */
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) &&
        g_variant_get_string(value, NULL)[0] != '\0' && app.state.nearby_valid) {
        g_free(app.state.nearby_name);
        app.state.nearby_name = g_variant_dup_string(value, NULL);
        dbus_service_emit_properties_changed(app.dbus_service, "NearbyBattery");
    }
    g_variant_unref(value);
    g_variant_unref(result);
}

/* Name to show for the AirPods while they are not connected here: the
 * one given in the preferences, else their Bluetooth name (asked to BlueZ,
 * the model name meanwhile) */
static char *nearby_name(const char *address, uint16_t model)
{
    DeviceProfile profile;
    if (address == NULL)
        return g_strdup("AirPods");
    if (config_load_device_profile(address, &profile) && profile.display_name[0] != '\0')
        return g_strdup(profile.display_name);

    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (bus != NULL) {
        char *path = bluez_device_path(app.adapter_path ? app.adapter_path : "/org/bluez/hci0",
                                       address);
        g_dbus_connection_call(bus, "org.bluez", path, "org.freedesktop.DBus.Properties", "Get",
                               g_variant_new("(ss)", "org.bluez.Device1", "Alias"),
                               G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL,
                               on_alias_ready, NULL);
        g_free(path);
        g_object_unref(bus);
    }
    return g_strdup(model != 0 ? airpods_model_to_string((AirPodsModel)model) : "AirPods");
}

static void on_nearby_battery(const ProximityBattery *battery, const ProximityInfo *info,
                              void *user_data)
{
    (void)user_data;
    AirPodsState *s = &app.state;
    bool was_valid = s->nearby_valid;

    s->nearby_valid = battery != NULL;
    if (battery != NULL) {
        bool changed = !was_valid || s->nearby_left != battery->left ||
                       s->nearby_right != battery->right || s->nearby_case != battery->case_level ||
                       s->nearby_left_charging != battery->left_charging ||
                       s->nearby_right_charging != battery->right_charging ||
                       s->nearby_case_charging != battery->case_charging ||
                       s->nearby_headphones != airpods_model_is_headphones(info->model) ||
                       g_strcmp0(s->nearby_host, nearby_host(info->connection_state)) != 0;
        s->nearby_left = battery->left;
        s->nearby_right = battery->right;
        s->nearby_case = battery->case_level;
        s->nearby_left_charging = battery->left_charging;
        s->nearby_right_charging = battery->right_charging;
        s->nearby_case_charging = battery->case_charging;
        s->nearby_headphones = airpods_model_is_headphones(info->model);
        /* Once per appearance: the name may have changed in between */
        if (!was_valid) {
            g_free(s->nearby_name);
            s->nearby_name = nearby_name(autoconnect_get_address(app.autoconnect), info->model);
        }
        s->nearby_host = nearby_host(info->connection_state);
        if (!changed)
            return;
        g_message("Nearby AirPods: L=%d%% R=%d%% Case=%d%% (%s)",
                  battery->left, battery->right, battery->case_level, s->nearby_host);
    } else if (!was_valid) {
        return;
    }
    dbus_service_emit_properties_changed(app.dbus_service, "NearbyBattery");
}

/* Whether the AirPods connected here are the ones watched over BLE (the
 * last that gave their keys) */
static bool connected_are_watched(void)
{
    const char *watched = autoconnect_get_address(app.autoconnect);
    return watched != NULL && app.state.device_address != NULL &&
           g_ascii_strcasecmp(watched, app.state.device_address) == 0;
}

static void on_case_battery(int level, bool charging, void *user_data)
{
    (void)user_data;
    AirPodsState *s = &app.state;

    /* Other AirPods connected, until they give their keys */
    if (s->connected && !connected_are_watched()) {
        level = -1;
        charging = false;
    }

    if (s->case_advert_level == level && s->case_advert_charging == charging)
        return;
    s->case_advert_level = level;
    s->case_advert_charging = charging;
    g_message("Case: %d%%%s", level, charging ? " (charging)" : "");

    dbus_service_emit_properties_changed(app.dbus_service, "BatteryCase");
    dbus_service_emit_properties_changed(app.dbus_service, "ChargingCase");
    if (s->nearby_valid)
        dbus_service_emit_properties_changed(app.dbus_service, "NearbyBattery");
}

static void on_playback_started(void *user_data)
{
    (void)user_data;
    autoconnect_on_playback_started(app.autoconnect);
}

/* ============================================================================
 * AirPods link
 * ========================================================================== */

/* ============================================================================
 * Battery in GNOME (through BlueZ)
 * ========================================================================== */

/* The lowest pod, as the panel shows; the only battery for headphones */
static void publish_battery(void)
{
    int left = app.state.battery.left.level;
    int right = app.state.battery.right.level;
    int level = left;

    if (!airpods_model_is_headphones(app.state.model) && (left < 0 || (right >= 0 && right < left)))
        level = right;
    battery_provider_update(app.battery_provider, app.adapter_path,
                            app.state.device_address, app.state.connected ? level : -1);
}

/* ============================================================================
 * Pause when falling asleep
 * ========================================================================== */

static const AirPodsSettingDef *sleep_setting(void)
{
    return airpods_setting_by_key("SleepDetection");
}

static void sleep_send(const uint8_t *data, size_t len, void *user_data)
{
    (void)user_data;
    aap_link_send(app.link, data, len);
}

static bool sleep_is_enabled(void *user_data)
{
    (void)user_data;
    uint8_t value;
    return airpods_state_get_setting(&app.state, sleep_setting()->id, &value) && value == 0x01;
}

/* GNOME's idle time: input on this computer means the user is awake */
static int64_t sleep_idle_ms(void *user_data)
{
    (void)user_data;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus == NULL)
        return -1;

    GVariant *reply = g_dbus_connection_call_sync(bus, "org.gnome.Mutter.IdleMonitor",
                                                  "/org/gnome/Mutter/IdleMonitor/Core",
                                                  "org.gnome.Mutter.IdleMonitor", "GetIdletime",
                                                  NULL, G_VARIANT_TYPE("(t)"),
                                                  G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    g_object_unref(bus);
    if (reply == NULL)
        return -1;

    guint64 idle = 0;
    g_variant_get(reply, "(t)", &idle);
    g_variant_unref(reply);
    return (int64_t)idle;
}

static bool sleep_pause_announced;

static void sleep_pause_media(int rewind_seconds, void *user_data)
{
    (void)user_data;
    sleep_pause_announced = false;
    media_control_pause_for_sleep(app.media_control, rewind_seconds);
}

/* Several players may be paused: one notification is enough */
static void on_paused_for_sleep(const char *player, void *user_data)
{
    (void)player;
    (void)user_data;
    if (!sleep_pause_announced) {
        sleep_pause_announced = true;
        dbus_service_emit_paused_for_sleep(app.dbus_service);
    }
}

static void on_link_packet(const AapParsedPacket *pkt, void *user_data)
{
    (void)user_data;

    if (pkt->type == AAP_PKT_TYPE_PROXIMITY_KEYS) {
        on_proximity_keys(&pkt->data.proximity_keys);
        return;
    }
    if (pkt->type == AAP_PKT_TYPE_SLEEP_DETECTION) {
        sleep_pause_handle(app.sleep_pause, &pkt->data.sleep_detection);
        return;
    }

    /* The AirPods announce the setting on connection */
    if (pkt->type == AAP_PKT_TYPE_CONTROL_SETTING &&
        pkt->data.control_setting.id == sleep_setting()->id)
        sleep_pause_setting_changed(app.sleep_pause, pkt->data.control_setting.value[0] == 0x01);

    device_handle_packet(&app.device, pkt);

    if (pkt->type == AAP_PKT_TYPE_BATTERY)
        publish_battery();
    else if (pkt->type == AAP_PKT_TYPE_METADATA && app.state.model != AIRPODS_MODEL_UNKNOWN)
        autoconnect_set_model(app.autoconnect, app.state.model);
}

static void on_link_disconnected(void *user_data)
{
    (void)user_data;
    battery_provider_update(app.battery_provider, NULL, NULL, -1);
    sleep_pause_session_ended(app.sleep_pause);
    device_session_ended(&app.device);
}

static void on_link_connected(const char *address, const char *name, void *user_data)
{
    (void)user_data;
    device_session_started(&app.device, address, name);
    controls_send_saved_settings(&app.controls, address);
    /* Not the case of these AirPods */
    if (!connected_are_watched())
        on_case_battery(-1, false, NULL);
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
    g_free(app.adapter_path);
    app.adapter_path = adapter_path;

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
 * D-Bus methods about the service itself
 * ========================================================================== */

static void on_set_ear_pause_mode(int mode, void *user_data)
{
    (void)user_data;

    g_message("Setting ear pause mode to %d", mode);

    /* Update state */
    app.state.ear_pause_mode = mode;

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

static void on_set_auto_connect(bool enabled, void *user_data)
{
    (void)user_data;

    app.config.auto_connect = enabled;
    config_save(&app.config);
    dbus_service_set_auto_connect(app.dbus_service, enabled);
    autoconnect_set_enabled(app.autoconnect, enabled);
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

    battery_provider_free(app.battery_provider);
    app.battery_provider = NULL;
    g_free(app.adapter_path);
    sleep_pause_free(app.sleep_pause);
    app.sleep_pause = NULL;
    autoconnect_free(app.autoconnect);
    app.autoconnect = NULL;

    device_finish(&app.device);

    controls_cleanup(&app.controls);
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
    research_set_enabled(app.config.research_log);

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

    dbus_service_set_ear_pause_mode_callback(app.dbus_service, on_set_ear_pause_mode, NULL);
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
        media_control_set_paused_for_sleep_callback(app.media_control, on_paused_for_sleep, NULL);
    }

    device_init(&app.device, &app.state, app.dbus_service, app.media_control);
    app.autoconnect = autoconnect_new(app.config.auto_connect, on_nearby_battery,
                                     on_case_battery, NULL);
    app.battery_provider = battery_provider_new();

    static const AapLinkCallbacks link_callbacks = {
        .connected = on_link_connected,
        .disconnected = on_link_disconnected,
        .packet = on_link_packet,
    };
    app.link = aap_link_new(&link_callbacks, NULL);
    controls_init(&app.controls, &app.device, app.link, app.dbus_service);

    static const SleepPauseCallbacks sleep_callbacks = {
        .send = sleep_send,
        .is_enabled = sleep_is_enabled,
        .idle_ms = sleep_idle_ms,
        .pause_media = sleep_pause_media,
    };
    app.sleep_pause = sleep_pause_new(&sleep_callbacks, NULL);

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
