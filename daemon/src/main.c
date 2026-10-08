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
#include "apple_identity.h"
#include "audio_route.h"
#include "aap_link.h"
#include "aap_protocol.h"
#include "autoconnect.h"
#include "battery_provider.h"
#include "bluez_monitor.h"
#include "config.h"
#include "controls.h"
#include "dbus_service.h"
#include "handoff.h"
#include "link_trace.h"
#include "device.h"
#include "media_control.h"
#include "relink.h"
#include "research.h"
#include "sleep_pause.h"
#include "tipi.h"

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
    AppleIdentity *apple_identity;
    Handoff *handoff;
    LinkTrace *link_trace;
    Tipi *tipi;
    Relink *relink;
    bool tipi_reconnecting;     /* Each announced as Reconnecting */
    bool relink_reconnecting;
    char *device_path;          /* AirPods' BlueZ object, kept after they left */
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
    tipi_playback_started(app.tipi);
    handoff_playback_started(app.handoff);
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

/* Changed from the preferences: the AirPods won't announce it */
static void on_setting_sent(uint8_t id, uint8_t value, void *user_data)
{
    (void)user_data;
    if (id == sleep_setting()->id)
        sleep_pause_setting_changed(app.sleep_pause, value == 0x01);
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

/* What a Mac tells the AirPods on connection, so that they treat this
 * computer as one of the user's Apple devices and keep it */
static void send_smart_routing_info(void)
{
    if (!app.config.apple_handoff || !aap_link_is_connected(app.link))
        return;

    uint8_t score[AAP_SMART_ROUTING_SCORE_SIZE];
    uint8_t state[AAP_SMART_ROUTING_STATE_SIZE];
    aap_build_smart_routing_score(0x07, score);
    aap_build_smart_routing_state(0x00, (uint32_t)(g_get_real_time() / G_USEC_PER_SEC), state);
    aap_link_send(app.link, score, sizeof(score));
    aap_link_send(app.link, state, sizeof(state));
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
    else if (pkt->type == AAP_PKT_TYPE_METADATA) {
        if (app.state.model != AIRPODS_MODEL_UNKNOWN)
            autoconnect_set_model(app.autoconnect, app.state.model);
        /* The AirPods answered: tell them about this computer, as a Mac */
        send_smart_routing_info();
    }
    else if (pkt->type == AAP_PKT_TYPE_HOSTS)
        tipi_hosts_changed(app.tipi, &pkt->data.hosts, app.state.host_address);
    else if (pkt->type == AAP_PKT_TYPE_EAR_DETECTION)
        tipi_ear_changed(app.tipi, app.state.ear_detection.primary_in_ear,
                         app.state.ear_detection.secondary_in_ear);
    else if (pkt->type == AAP_PKT_TYPE_AUDIO_SOURCE)
        handoff_source_changed(app.handoff, app.state.audio_source,
                               pkt->data.audio_source.type == AAP_AUDIO_SOURCE_CALL);
}

static void on_link_disconnected(void *user_data)
{
    (void)user_data;
    battery_provider_update(app.battery_provider, NULL, NULL, -1);
    sleep_pause_session_ended(app.sleep_pause);
    handoff_source_changed(app.handoff, NULL, false);
    device_session_ended(&app.device);
}

static void on_link_connected(const char *address, const char *name, void *user_data)
{
    (void)user_data;
    device_session_started(&app.device, address, name);
    /* Once announced: clients tell a reconnection by it */
    tipi_link_opened(app.tipi);
    relink_link_opened(app.relink);
    controls_send_saved_settings(&app.controls, address);
    /* Not the case of these AirPods */
    if (!connected_are_watched())
        on_case_battery(-1, false, NULL);
}

/* ============================================================================
 * BlueZ callbacks
 * ========================================================================== */

/* This computer's address, to tell its own audio source from another
 * device's */
static void on_adapter_address_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, NULL);
    if (result == NULL)
        return;

    GVariant *value;
    g_variant_get(result, "(v)", &value);
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
        g_strlcpy(app.state.host_address, g_variant_get_string(value, NULL),
                  sizeof(app.state.host_address));
    g_variant_unref(value);
    g_variant_unref(result);
}

static void read_adapter_address(const char *adapter_path)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (bus == NULL || adapter_path == NULL) {
        g_clear_object(&bus);
        return;
    }
    g_dbus_connection_call(bus, "org.bluez", adapter_path, "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", "org.bluez.Adapter1", "Address"),
                           G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                           on_adapter_address_ready, NULL);
    g_object_unref(bus);
}

static void on_bluez_device_connected(const BluezDeviceInfo *device, void *user_data)
{
    (void)user_data;
    g_message("BlueZ: AirPods connected - %s (%s)", device->name, device->address);
    /* The adapter the AirPods use is the one to scan on */
    char *adapter_path = device->object_path ? g_path_get_dirname(device->object_path) : NULL;
    autoconnect_set_airpods_connected(app.autoconnect, true, adapter_path);
    g_free(app.adapter_path);
    app.adapter_path = adapter_path;
    read_adapter_address(adapter_path);
    link_trace_set_device(app.link_trace, device->object_path);
    g_free(app.device_path);
    app.device_path = g_strdup(device->object_path);

    aap_link_device_connected(app.link, device->address, device->name);
}

static void on_bluez_device_disconnected(const BluezDeviceInfo *device, void *user_data)
{
    (void)user_data;
    g_message("BlueZ: AirPods disconnected - %s (%s)", device->name, device->address);
    aap_link_device_disconnected(app.link);
    autoconnect_set_airpods_connected(app.autoconnect, false, NULL);
    relink_link_closed(app.relink);
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

/* Protocol experiments without stopping the service (research mode only) */
static bool research_send(const uint8_t *data, size_t len, void *user_data)
{
    (void)user_data;
    if (!app.config.research_log || len == 0 || !aap_link_is_connected(app.link)) {
        g_message("Research: not sending (research mode %d, %zu bytes, connected %d)",
                  app.config.research_log, len, aap_link_is_connected(app.link));
        return false;
    }

    GString *hex = g_string_new(NULL);
    for (size_t i = 0; i < len; i++)
        g_string_append_printf(hex, "%s%02X", i ? " " : "", data[i]);
    g_message("Research: sending %s", hex->str);
    g_string_free(hex, TRUE);
    return aap_link_send(app.link, data, len);
}

/* Like a Mac starting to play: have the AirPods switch to this computer */
static bool claim_audio(void *user_data)
{
    (void)user_data;
    if (!aap_link_is_connected(app.link))
        return false;

    const uint8_t claim = 0x01;
    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_control_cmd(AAP_CTRL_OWNS_CONNECTION, &claim, 1, packet);
    g_message("Asking the AirPods to play from this computer");
    if (!aap_link_send(app.link, packet, sizeof(packet)))
        return false;
    /* The request alone doesn't move them: playing here does */
    handoff_use_here(app.handoff);
    return true;
}

static bool tipi_send(const uint8_t *data, size_t len, void *user_data)
{
    (void)user_data;
    return aap_link_is_connected(app.link) && aap_link_send(app.link, data, len);
}

static void on_device_call_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    const char *method = user_data;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (result != NULL) {
        g_variant_unref(result);
    } else {
        g_message("%s the AirPods failed: %s", method, error->message);
        g_error_free(error);
    }
}

/* Device1.Connect or Disconnect on the AirPods used last */
static void call_airpods(const char *method)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (bus == NULL || app.device_path == NULL) {
        g_clear_object(&bus);
        return;
    }
    g_dbus_connection_call(bus, "org.bluez", app.device_path, "org.bluez.Device1", method,
                           NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL,
                           on_device_call_done, (gpointer)method);
    g_object_unref(bus);
}

static void connect_airpods(void *user_data)
{
    (void)user_data;
    call_airpods("Connect");
}

static void disconnect_airpods(void *user_data)
{
    (void)user_data;
    call_airpods("Disconnect");
}

static void update_reconnecting(void)
{
    dbus_service_set_reconnecting(app.dbus_service,
                                  app.tipi_reconnecting || app.relink_reconnecting);
}

static void tipi_reconnecting(bool reconnecting, void *user_data)
{
    (void)user_data;
    app.tipi_reconnecting = reconnecting;
    update_reconnecting();
}

static void relink_reconnecting(bool reconnecting, void *user_data)
{
    (void)user_data;
    app.relink_reconnecting = reconnecting;
    update_reconnecting();
}

static void on_link_closed(bool left, void *user_data)
{
    (void)user_data;
    tipi_link_closed(app.tipi, left);
}

static void handoff_pause_players(void *user_data)
{
    (void)user_data;
    media_control_pause_for_handoff(app.media_control);
}

static void handoff_resume_players(void *user_data)
{
    (void)user_data;
    media_control_resume_handoff(app.media_control);
}

static void handoff_restart_audio(HandoffRestart how, void *user_data)
{
    (void)user_data;
    static const char *const modes[] = {
        [HANDOFF_RESTART_STREAMS] = "streams",
        [HANDOFF_RESTART_OUTPUT] = "output",
        [HANDOFF_RESTART_ANY] = "any",
    };
    audio_route_restart(app.state.device_address, modes[how]);
}

static void on_set_apple_handoff(bool enabled, void *user_data)
{
    (void)user_data;
    bool changed = enabled != app.config.apple_handoff;
    handoff_set_enabled(app.handoff, enabled);
    tipi_set_enabled(app.tipi, enabled);

    app.config.apple_handoff = enabled;
    config_save(&app.config);
    dbus_service_set_apple_handoff(app.dbus_service, enabled);
    apple_identity_set_enabled(app.apple_identity, enabled);
    send_smart_routing_info();

    /* The AirPods read the new identity at the second connection after
     * the change, and forget it at the next one */
    if (changed && aap_link_is_connected(app.link))
        relink_start(app.relink, enabled ? 2 : 1);
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
    apple_identity_free(app.apple_identity);
    app.apple_identity = NULL;
    handoff_free(app.handoff);
    app.handoff = NULL;
    link_trace_free(app.link_trace);
    app.link_trace = NULL;
    tipi_free(app.tipi);
    app.tipi = NULL;
    relink_free(app.relink);
    app.relink = NULL;
    g_free(app.device_path);
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
    dbus_service_set_apple_handoff_callback(app.dbus_service, on_set_apple_handoff, NULL);
    dbus_service_set_claim_audio_callback(app.dbus_service, claim_audio, NULL);
    dbus_service_set_research_send_callback(app.dbus_service, research_send, NULL);
    dbus_service_set_apple_handoff(app.dbus_service, app.config.apple_handoff);

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
    app.apple_identity = apple_identity_new();
    apple_identity_set_enabled(app.apple_identity, app.config.apple_handoff);
    static const HandoffCallbacks handoff_callbacks = {
        .pause_players = handoff_pause_players,
        .resume_players = handoff_resume_players,
        .restart_audio = handoff_restart_audio,
    };
    app.handoff = handoff_new(&handoff_callbacks, NULL);
    app.link_trace = link_trace_new();
    static const TipiCallbacks tipi_callbacks = {
        .send = tipi_send,
        .reconnect = connect_airpods,
        .reconnecting = tipi_reconnecting,
    };
    app.tipi = tipi_new(&tipi_callbacks, NULL);
    tipi_set_enabled(app.tipi, app.config.apple_handoff);
    link_trace_set_closed_callback(app.link_trace, on_link_closed, NULL);
    static const RelinkCallbacks relink_callbacks = {
        .disconnect = disconnect_airpods,
        .connect = connect_airpods,
        .reconnecting = relink_reconnecting,
    };
    app.relink = relink_new(&relink_callbacks, NULL);
    handoff_set_enabled(app.handoff, app.config.apple_handoff);
    /* Known before the first audio source arrives; refreshed on connection */
    read_adapter_address("/org/bluez/hci0");

    static const AapLinkCallbacks link_callbacks = {
        .connected = on_link_connected,
        .disconnected = on_link_disconnected,
        .packet = on_link_packet,
    };
    app.link = aap_link_new(&link_callbacks, NULL);
    controls_init(&app.controls, &app.device, app.link, app.dbus_service);
    app.controls.setting_sent = on_setting_sent;

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
