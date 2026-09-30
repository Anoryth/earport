/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "autoconnect.h"
#include "aap_protocol.h"
#include "ble_proximity.h"
#include "ble_scanner.h"
#include "config.h"

#include <gio/gio.h>
#include <string.h>

/* Scanning keeps bluetoothd, the system bus and the shell's Bluetooth code
 * busy (measured: ~4% CPU for bluetoothd when continuous), but AirPods
 * nearby answer within 0.5 s. So scan in short bursts that end at their
 * first advert, and scan right away when playback starts. Don't insist
 * when a connection attempt fails. */
#define SCAN_INTERVAL_SEC 30
#define SCAN_BURST_SEC 3
/* Long enough for AirPods paused on another device to report it (2-3 s) */
#define PLAYBACK_WINDOW_SEC 8
#define AUTOCONNECT_COOLDOWN_SEC 30
#define DEFAULT_ADAPTER_PATH "/org/bluez/hci0"

struct AutoConnect {
    bool enabled;
    bool airpods_connected;      /* Connected to this computer */

    BleScanner *ble_scanner;
    char *adapter_path;
    char *irk_address;           /* AirPods the IRK belongs to */
    uint8_t irk[AAP_PROXIMITY_KEY_SIZE];
    bool has_irk;

    guint scan_timer_id;         /* Periodic short scans */
    guint burst_timer_id;        /* End of the current short scan */
    guint playback_window_id;    /* Waiting for an advert after playback started */
    bool playback_pending;
    bool playback_seen_busy;     /* Saw them in use while waiting */
    bool prev_in_ear_known;
    bool prev_in_ear;
    gint64 last_attempt_us;
};

static void on_device_connect_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (result != NULL) {
        g_variant_unref(result);
    } else {
        g_message("Automatic connection failed: %s", error->message);
        g_error_free(error);
    }
}

static void try_connect(AutoConnect *ac, const char *reason)
{
    gint64 now = g_get_monotonic_time();
    if (ac->last_attempt_us > 0 &&
        now - ac->last_attempt_us < AUTOCONNECT_COOLDOWN_SEC * G_USEC_PER_SEC)
        return;
    ac->last_attempt_us = now;

    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (bus == NULL)
        return;

    char *device = g_strdup(ac->irk_address);
    g_strdelimit(device, ":", '_');
    char *path = g_strdup_printf("%s/dev_%s", ac->adapter_path, device);

    g_message("Connecting to the AirPods automatically (%s)", reason);
    g_dbus_connection_call(bus, "org.bluez", path, "org.bluez.Device1", "Connect",
                           NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL,
                           on_device_connect_done, NULL);

    g_free(path);
    g_free(device);
    g_object_unref(bus);
}

static bool ble_address_is_ours(const char *address, void *user_data)
{
    AutoConnect *ac = user_data;
    return ac->has_irk && proximity_address_matches(ac->irk, address);
}

static void end_playback_wait(AutoConnect *ac)
{
    ac->playback_pending = false;
    if (ac->playback_window_id > 0) {
        g_source_remove(ac->playback_window_id);
        ac->playback_window_id = 0;
    }
    ble_scanner_stop(ac->ble_scanner);
}

/* Decide on playback started here, now that we know what the AirPods do */
static void evaluate_playback_trigger(AutoConnect *ac, const ProximityInfo *info)
{
    if (autoconnect_allowed(info, AUTOCONNECT_TRIGGER_PLAYBACK)) {
        end_playback_wait(ac);
        try_connect(ac, "playback started");
    } else if (autoconnect_worth_waiting(info)) {
        /* Maybe just paused on the other device: keep watching until the
         * end of the window */
        ac->playback_seen_busy = true;
    } else {
        end_playback_wait(ac);
        g_message(info->in_ear ? "Not connecting: the AirPods are in use by another device"
                               : "Not connecting: the AirPods are not in the ears");
    }
}

static void on_ble_advert(const char *address, const uint8_t *data, size_t len, void *user_data)
{
    (void)address;
    AutoConnect *ac = user_data;
    ProximityInfo info;

    if (!proximity_parse(data, len, &info) || ac->airpods_connected)
        return;

    /* React to the pods being put in, not to them being in: after a manual
     * disconnection, AirPods still in the ears must stay disconnected */
    if (ac->prev_in_ear_known && !ac->prev_in_ear && info.in_ear &&
        autoconnect_allowed(&info, AUTOCONNECT_TRIGGER_EARS))
        try_connect(ac, "AirPods put in");
    ac->prev_in_ear = info.in_ear;
    ac->prev_in_ear_known = true;

    if (ac->playback_pending) {
        evaluate_playback_trigger(ac, &info);
    } else {
        /* One advert is all a periodic scan needs */
        ble_scanner_stop(ac->ble_scanner);
    }
}

static gboolean playback_window_cb(gpointer user_data)
{
    AutoConnect *ac = user_data;
    ac->playback_window_id = 0;
    ac->playback_pending = false;
    ble_scanner_stop(ac->ble_scanner);
    g_message(ac->playback_seen_busy ? "Not connecting: the AirPods are in use by another device"
                                     : "Not connecting: the AirPods were not seen nearby");
    return G_SOURCE_REMOVE;
}

void autoconnect_on_playback_started(AutoConnect *ac)
{
    if (!ac->enabled || !ac->has_irk || ac->airpods_connected)
        return;

    /* The AirPods' state may have changed since the last periodic scan:
     * look at them now, it takes about half a second */
    ac->playback_pending = true;
    ac->playback_seen_busy = false;
    if (ac->playback_window_id > 0)
        g_source_remove(ac->playback_window_id);
    ac->playback_window_id = g_timeout_add_seconds(PLAYBACK_WINDOW_SEC, playback_window_cb, ac);
    ble_scanner_start(ac->ble_scanner);
}

static gboolean burst_end_cb(gpointer user_data)
{
    AutoConnect *ac = user_data;
    ac->burst_timer_id = 0;
    /* AirPods away: no advert came, stop anyway */
    if (!ac->playback_pending)
        ble_scanner_stop(ac->ble_scanner);
    return G_SOURCE_REMOVE;
}

/* Periodic short scan, to notice the pods being put in */
static gboolean scan_cycle_cb(gpointer user_data)
{
    AutoConnect *ac = user_data;

    ble_scanner_start(ac->ble_scanner);
    if (ac->burst_timer_id > 0)
        g_source_remove(ac->burst_timer_id);
    ac->burst_timer_id = g_timeout_add_seconds(SCAN_BURST_SEC, burst_end_cb, ac);
    return G_SOURCE_CONTINUE;
}

/* Scan only when it can lead somewhere: option on, keys known, AirPods
 * not connected to this computer */
static void update(AutoConnect *ac)
{
    bool wanted = ac->enabled && ac->has_irk && !ac->airpods_connected;

    if (wanted && ac->ble_scanner == NULL)
        ac->ble_scanner = ble_scanner_new(ac->adapter_path, ble_address_is_ours, on_ble_advert, ac);

    if (wanted && ac->scan_timer_id == 0 && ac->ble_scanner != NULL) {
        g_message("Watching for the AirPods to connect automatically");
        scan_cycle_cb(ac);
        ac->scan_timer_id = g_timeout_add_seconds(SCAN_INTERVAL_SEC, scan_cycle_cb, ac);
    } else if (!wanted && ac->scan_timer_id > 0) {
        g_source_remove(ac->scan_timer_id);
        ac->scan_timer_id = 0;
        if (ac->burst_timer_id > 0) {
            g_source_remove(ac->burst_timer_id);
            ac->burst_timer_id = 0;
        }
        ble_scanner_stop(ac->ble_scanner);
    }

    if (!wanted) {
        ac->playback_pending = false;
        ac->prev_in_ear_known = false;
    }
}

AutoConnect *autoconnect_new(bool enabled)
{
    AutoConnect *ac = g_new0(AutoConnect, 1);
    ac->enabled = enabled;
    ac->adapter_path = g_strdup(DEFAULT_ADAPTER_PATH);
    ac->has_irk = config_load_proximity_irk(&ac->irk_address, ac->irk);
    return ac;
}

void autoconnect_start(AutoConnect *ac)
{
    update(ac);
}

void autoconnect_free(AutoConnect *ac)
{
    if (ac == NULL)
        return;

    if (ac->scan_timer_id > 0)
        g_source_remove(ac->scan_timer_id);
    if (ac->burst_timer_id > 0)
        g_source_remove(ac->burst_timer_id);
    if (ac->playback_window_id > 0)
        g_source_remove(ac->playback_window_id);
    ble_scanner_free(ac->ble_scanner);
    g_free(ac->adapter_path);
    g_free(ac->irk_address);
    g_free(ac);
}

void autoconnect_set_enabled(AutoConnect *ac, bool enabled)
{
    ac->enabled = enabled;
    update(ac);
}

void autoconnect_set_airpods_connected(AutoConnect *ac, bool connected,
                                       const char *adapter_path)
{
    ac->airpods_connected = connected;
    if (connected && adapter_path != NULL) {
        g_free(ac->adapter_path);
        ac->adapter_path = g_strdup(adapter_path);
    }
    update(ac);
}

void autoconnect_set_irk(AutoConnect *ac, const char *address, const uint8_t *irk)
{
    bool changed = !ac->has_irk || g_strcmp0(address, ac->irk_address) != 0 ||
                   memcmp(ac->irk, irk, sizeof(ac->irk)) != 0;
    if (!changed)
        return;

    memcpy(ac->irk, irk, sizeof(ac->irk));
    ac->has_irk = true;
    g_free(ac->irk_address);
    ac->irk_address = g_strdup(address);
    config_save_proximity_irk(address, irk);
    ble_scanner_reset_filter(ac->ble_scanner);
    g_message("Stored the keys to recognize the AirPods nearby");
}
