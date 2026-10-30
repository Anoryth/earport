/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Automatic connection from BLE adverts, against a fake org.bluez
 * (fake_bluez.c), with the delays shortened in meson.build (scan every
 * 300 ms for 100 ms, every 150 ms while in use, playback window 150 ms,
 * cooldown 250 ms, case refresh 500 ms). The keys are the Bluetooth spec's
 * sample IRK and a made-up encryption key (the real one is a secret).
 */

#include <gio/gio.h>
#include <string.h>

#include "aes128.h"
#include "airpods_state.h"
#include "autoconnect.h"
#include "ble_proximity.h"
#include "fake_bluez.h"
#include "test_wait.h"

#define AIRPODS "00:00:00:00:00:0A"
#define AIRPODS_PATH FAKE_ADAPTER_PATH "/dev_00_00_00_00_00_0A"
/* Resolves with the IRK below (Core Spec Vol 3 Part H D.7) */
#define AIRPODS_RPA "70:81:94:0D:FB:AA"
#define SCAN_INTERVAL 300
#define SCAN_BURST 100
#define PLAYBACK_WINDOW 150
#define COOLDOWN 250
#define CASE_REFRESH 500
/* Not seen for 2.2 refresh periods: gone (CASE_TIMEOUT_MS) */
#define CASE_TIMEOUT (2 * CASE_REFRESH + CASE_REFRESH * 6 / 5)
#define CASE_ADDRESS "00:00:00:00:00:0C"
#define OTHER_CASE_ADDRESS "00:00:00:00:00:0D"

static const uint8_t irk_msb[16] = {
    0xec, 0x02, 0x34, 0xa3, 0x57, 0xc8, 0xad, 0x05,
    0x34, 0x10, 0x10, 0xa6, 0x0a, 0x39, 0x7d, 0x9b,
};
static const uint8_t enc_key[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

/* Proximity advert of AirPods Pro 2 USB-C (layout of a captured one,
 * test_ble_proximity.c), with its status and connection state */
#define STATUS_IN_EAR 0x22
#define STATUS_IN_CASE 0x24

static GVariant *manufacturer_data(const uint8_t *data, size_t len)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{qv}"));
    g_variant_builder_add(&builder, "{qv}", (guint16)APPLE_COMPANY_ID,
                          g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, len, 1));
    return g_variant_builder_end(&builder);
}

/* The last 16 bytes: the battery, encrypted when 'battery' is given */
static GVariant *advert_with_battery(uint8_t status, uint8_t connection_state,
                                     const uint8_t *battery)
{
    uint8_t data[27] = {
        0x07, 0x19, 0x01, 0x24, 0x20, 0x00, 0x43, 0x8f, 0x11, 0x00, 0x00,
    };
    data[5] = status;
    data[10] = connection_state;
    if (battery != NULL)
        aes128_encrypt_block(enc_key, battery, data + sizeof(data) - 16);
    return manufacturer_data(data, sizeof(data));
}

static GVariant *advert(uint8_t status, uint8_t connection_state)
{
    return advert_with_battery(status, connection_state, NULL);
}

/* The case's own advert (layout of test_ble_proximity.c), at 'level' % */
static GVariant *case_advert(const uint8_t *key, uint8_t level, bool charging)
{
    const uint8_t plain[16] = {
        0x10, 0x20, 0x0E, (uint8_t)((charging ? 0x80 : 0) | level), 0xFF, 0xFF, 0x50, 0x01,
    };
    uint8_t data[23] = { 0x12, 0x02, 0x64, 0x00, 0x07, 0x11, 0x06 };
    aes128_encrypt_block(key, plain, data + 7);
    return manufacturer_data(data, sizeof(data));
}

/* What the callbacks reported */
typedef struct {
    int nearby_calls;
    bool nearby_gone;
    ProximityBattery battery;
    int case_calls;
    int case_level;
    bool case_charging;
} Seen;

static void on_nearby(const ProximityBattery *battery, const ProximityInfo *info, void *user_data)
{
    (void)info;
    Seen *seen = user_data;
    seen->nearby_calls++;
    seen->nearby_gone = battery == NULL;
    if (battery != NULL)
        seen->battery = *battery;
}

static void on_case(int level, bool charging, void *user_data)
{
    Seen *seen = user_data;
    seen->case_calls++;
    seen->case_level = level;
    seen->case_charging = charging;
}

static const char *rpa_path;

static void send_advert(uint8_t status, uint8_t connection_state)
{
    fake_bluez_set(rpa_path, "ManufacturerData", advert(status, connection_state), true);
    fake_bluez_run(20);
}

static AutoConnect *autoconnect_ready_seen(bool enabled, Seen *seen)
{
    fake_bluez_reset();
    fake_bluez_add_device(AIRPODS, "AirPods", true, false, false);
    rpa_path = fake_bluez_add_device(AIRPODS_RPA, "", false, false, false);

    uint8_t irk[16];
    for (int i = 0; i < 16; i++)
        irk[i] = irk_msb[15 - i];
    AutoConnect *ac = autoconnect_new(enabled, seen != NULL ? on_nearby : NULL,
                                      seen != NULL ? on_case : NULL, seen);
    autoconnect_set_irk(ac, AIRPODS, irk);
    autoconnect_start(ac);
    fake_bluez_sync();
    g_assert_cmpint(fake_bluez_wait_calls("StartDiscovery", 1), ==, 1);
    return ac;
}

static AutoConnect *autoconnect_ready(bool enabled)
{
    return autoconnect_ready_seen(enabled, NULL);
}

/* No advert: short scans, at intervals */
static void test_scan_cycle(void)
{
    AutoConnect *ac = autoconnect_ready(true);

    fake_bluez_run(SCAN_BURST / 2);
    g_assert_cmpint(fake_bluez_calls("StopDiscovery"), ==, 0);
    g_assert_cmpint(fake_bluez_wait_calls("StopDiscovery", 1), ==, 1);
    fake_bluez_run(SCAN_INTERVAL - SCAN_BURST - 50);
    g_assert_cmpint(fake_bluez_calls("StartDiscovery"), ==, 1);
    g_assert_cmpint(fake_bluez_wait_calls("StartDiscovery", 2), ==, 2);
    g_assert_cmpstr(autoconnect_get_address(ac), ==, AIRPODS);
    autoconnect_free(ac);
}

/* Seen: the scan ends at once */
static void test_scan_ends_early(void)
{
    AutoConnect *ac = autoconnect_ready(true);

    send_advert(STATUS_IN_CASE, PROXIMITY_CONN_DISCONNECTED);
    g_assert_cmpint(fake_bluez_wait_calls("StopDiscovery", 1), ==, 1);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 0);
    autoconnect_free(ac);
}

/* Each scan ends at the first advert, and adverts in between are not
 * looked at: one advert per scan */
static void wait_next_scan(void)
{
    int starts = fake_bluez_calls("StartDiscovery");
    g_assert_cmpint(fake_bluez_wait_calls("StartDiscovery", starts + 1), ==, starts + 1);
}

/* Put in the ears while free: connected, then not again too soon */
static void test_ears(void)
{
    AutoConnect *ac = autoconnect_ready(true);

    send_advert(STATUS_IN_CASE, PROXIMITY_CONN_DISCONNECTED);
    wait_next_scan();
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_DISCONNECTED);
    g_assert_cmpint(fake_bluez_wait_calls("Connect", 1), ==, 1);

    /* Failed (still free): playback starting here does not insist yet */
    /* Scanning from now on, or already */
    autoconnect_on_playback_started(ac);
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_DISCONNECTED);
    fake_bluez_run(30);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 1);

    /* After the cooldown it does */
    fake_bluez_run(COOLDOWN);
    /* Scanning from now on, or already */
    autoconnect_on_playback_started(ac);
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_DISCONNECTED);
    g_assert_cmpint(fake_bluez_wait_calls("Connect", 2), ==, 2);
    autoconnect_free(ac);
}

/* Already in the ears when first seen (disconnected by hand), or put in
 * while in use elsewhere, or with the option off: left alone */
static void test_ears_left_alone(void)
{
    AutoConnect *ac = autoconnect_ready(true);
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_DISCONNECTED);
    wait_next_scan();
    send_advert(STATUS_IN_CASE, PROXIMITY_CONN_DISCONNECTED);
    wait_next_scan();
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_MUSIC);
    fake_bluez_run(30);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 0);
    autoconnect_free(ac);

    ac = autoconnect_ready(false);
    send_advert(STATUS_IN_CASE, PROXIMITY_CONN_DISCONNECTED);
    wait_next_scan();
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_DISCONNECTED);
    fake_bluez_run(30);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 0);
    autoconnect_free(ac);
}

static bool in_use_answer;

static bool computer_in_use(void *user_data)
{
    (void)user_data;
    return in_use_answer;
}

/* Put in while connected to an idle iPhone: taken only if this computer
 * is in use (playing, or used just now), like on a Mac */
static void test_ears_in_use(void)
{
    AutoConnect *ac = autoconnect_ready(true);
    autoconnect_set_in_use_callback(ac, computer_in_use, NULL);

    in_use_answer = false;
    send_advert(STATUS_IN_CASE, PROXIMITY_CONN_IDLE);
    wait_next_scan();
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_IDLE);
    fake_bluez_run(30);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 0);

    in_use_answer = true;
    wait_next_scan();
    send_advert(STATUS_IN_CASE, PROXIMITY_CONN_IDLE);
    wait_next_scan();
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_IDLE);
    g_assert_cmpint(fake_bluez_wait_calls("Connect", 1), ==, 1);

    /* Never from a device playing or in a call */
    fake_bluez_run(COOLDOWN);
    wait_next_scan();
    send_advert(STATUS_IN_CASE, PROXIMITY_CONN_MUSIC);
    wait_next_scan();
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_MUSIC);
    fake_bluez_run(30);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 1);

    in_use_answer = false;
    autoconnect_free(ac);
}

/* Not seen while in their closed case, then first seen in the ears: put
 * in. Seen in the ears right after leaving here: disconnected by hand. */
static void test_back_after_away(void)
{
    AutoConnect *ac = autoconnect_ready(true);
    autoconnect_set_in_use_callback(ac, computer_in_use, NULL);
    in_use_answer = true;

    autoconnect_set_airpods_connected(ac, true, NULL);
    autoconnect_set_airpods_connected(ac, false, NULL);
    wait_next_scan();
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_IDLE);
    fake_bluez_run(30);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 0);

    autoconnect_set_airpods_connected(ac, true, NULL);
    autoconnect_set_airpods_connected(ac, false, NULL);
    fake_bluez_run(2 * SCAN_INTERVAL + SCAN_INTERVAL / 2 + 100);
    wait_next_scan();
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_IDLE);
    g_assert_cmpint(fake_bluez_wait_calls("Connect", 1), ==, 1);

    in_use_answer = false;
    autoconnect_free(ac);
}

/* In use: looked for at every tick (IN_USE_SCAN_INTERVAL) instead of
 * every SCAN_INTERVAL */
static void test_scan_in_use(void)
{
    AutoConnect *ac = autoconnect_ready(true);
    autoconnect_set_in_use_callback(ac, computer_in_use, NULL);

    in_use_answer = true;
    int starts = fake_bluez_calls("StartDiscovery");
    fake_bluez_run(SCAN_INTERVAL - 50);
    g_assert_cmpint(fake_bluez_calls("StartDiscovery"), >=, starts + 1);

    in_use_answer = false;
    autoconnect_free(ac);
}

/* Playback starts here, AirPods in the ears and idle: connected */
static void test_playback(void)
{
    AutoConnect *ac = autoconnect_ready(true);
    fake_bluez_wait_calls("StopDiscovery", 1);

    autoconnect_on_playback_started(ac);
    g_assert_cmpint(fake_bluez_wait_calls("StartDiscovery", 2), ==, 2);
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_IDLE);
    g_assert_cmpint(fake_bluez_wait_calls("Connect", 1), ==, 1);
    g_assert_cmpint(fake_bluez_wait_calls("StopDiscovery", 2), ==, 2);
    autoconnect_free(ac);
}

/* Playing on another device: waits for a pause until the window ends */
static void test_playback_busy(void)
{
    AutoConnect *ac = autoconnect_ready(true);
    fake_bluez_wait_calls("StopDiscovery", 1);

    autoconnect_on_playback_started(ac);
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_MUSIC);
    fake_bluez_run(PLAYBACK_WINDOW / 2);
    g_assert_cmpint(fake_bluez_calls("StopDiscovery"), ==, 1);
    g_assert_cmpint(fake_bluez_wait_calls("StopDiscovery", 2), ==, 2);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 0);

    /* Paused within the window: connected */
    autoconnect_on_playback_started(ac);
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_MUSIC);
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_IDLE);
    g_assert_cmpint(fake_bluez_wait_calls("Connect", 1), ==, 1);
    autoconnect_free(ac);
}

/* The option turned off while waiting: nothing happens */
static void test_playback_disabled(void)
{
    AutoConnect *ac = autoconnect_ready(true);

    autoconnect_on_playback_started(ac);
    autoconnect_set_enabled(ac, false);
    send_advert(STATUS_IN_EAR, PROXIMITY_CONN_IDLE);
    fake_bluez_run(30);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 0);
    autoconnect_free(ac);
}

/* Connected here: no scan, nothing to connect */
static void test_connected_here(void)
{
    AutoConnect *ac = autoconnect_ready(true);

    autoconnect_set_airpods_connected(ac, true, FAKE_ADAPTER_PATH);
    g_assert_cmpint(fake_bluez_wait_calls("StopDiscovery", 1), ==, 1);
    autoconnect_on_playback_started(ac);
    fake_bluez_run(SCAN_INTERVAL + 50);
    g_assert_cmpint(fake_bluez_calls("StartDiscovery"), ==, 1);
    g_assert_cmpint(fake_bluez_calls("Connect"), ==, 0);
    autoconnect_free(ac);
}

/* Battery of the AirPods on another device, decrypted with their key; gone
 * once they stop advertising */
static void test_nearby_battery(void)
{
    Seen seen = { 0 };
    AutoConnect *ac = autoconnect_ready_seen(false, &seen);
    autoconnect_set_enc(ac, AIRPODS, enc_key);

    /* Left pod primary: [?] [left 80 %] [right 70 %, charging] [case unknown] */
    const uint8_t battery[16] = { 0x00, 80, 0x80 | 70, 0x7F };
    fake_bluez_set(rpa_path, "ManufacturerData",
                   advert_with_battery(STATUS_IN_EAR, PROXIMITY_CONN_MUSIC, battery), true);
    WAIT_FOR(seen.nearby_calls >= 1);
    g_assert_cmpint(seen.nearby_calls, ==, 1);
    g_assert_false(seen.nearby_gone);
    g_assert_cmpint(seen.battery.left, ==, 80);
    g_assert_false(seen.battery.left_charging);
    g_assert_cmpint(seen.battery.right, ==, 70);
    g_assert_true(seen.battery.right_charging);
    g_assert_cmpint(seen.battery.case_level, ==, -1);

    WAIT_FOR(seen.nearby_gone);
    g_assert_true(seen.nearby_gone);
    g_assert_cmpint(seen.nearby_calls, ==, 2);
    autoconnect_free(ac);
}

/* The case's battery from its own adverts, also while the AirPods are
 * connected here; another case is ignored, and silence means gone */
static void test_case(void)
{
    Seen seen = { 0 };
    const uint8_t other_key[16] = { 0 };
    AutoConnect *ac = autoconnect_ready_seen(false, &seen);
    const char *case_path = fake_bluez_add_device(CASE_ADDRESS, "", false, false, false);
    const char *other_path = fake_bluez_add_device(OTHER_CASE_ADDRESS, "", false, false, false);

    autoconnect_set_airpods_connected(ac, true, FAKE_ADAPTER_PATH);
    autoconnect_set_model(ac, AIRPODS_MODEL_PRO_2_USBC);
    /* The key makes the case worth a scan */
    int starts = fake_bluez_calls("StartDiscovery");
    autoconnect_set_enc(ac, AIRPODS, enc_key);
    g_assert_cmpint(fake_bluez_wait_calls("StartDiscovery", starts + 1), ==, starts + 1);

    fake_bluez_set(other_path, "ManufacturerData", case_advert(other_key, 30, false), true);
    fake_bluez_run(30);
    g_assert_cmpint(seen.case_calls, ==, 0);

    fake_bluez_set(case_path, "ManufacturerData", case_advert(enc_key, 49, true), true);
    WAIT_FOR(seen.case_calls >= 1);
    g_assert_cmpint(seen.case_calls, ==, 1);
    g_assert_cmpint(seen.case_level, ==, 49);
    g_assert_true(seen.case_charging);

    fake_bluez_run(CASE_TIMEOUT);
    WAIT_FOR(seen.case_level == -1);
    g_assert_cmpint(seen.case_level, ==, -1);
    g_assert_cmpint(seen.case_calls, ==, 2);
    autoconnect_free(ac);
}

/* Events in any order: never a connection while connected here or with
 * the option off, never two within the cooldown, and no scan left running
 * once connected here. Replay with --seed. */
static void test_storm(void)
{
    AutoConnect *ac = autoconnect_ready(true);
    bool enabled = true, connected = false;
    gint64 last_connect = 0;
    int connects = 0;
    static const uint8_t states[] = {
        PROXIMITY_CONN_DISCONNECTED, PROXIMITY_CONN_IDLE, PROXIMITY_CONN_MUSIC, PROXIMITY_CONN_CALL,
    };

    for (int i = 1; i <= 300; i++) {
        /* Weighted so that adverts often come while a scan is running */
        switch (g_test_rand_int_range(0, 10)) {
        case 0:
        case 9:
            autoconnect_on_playback_started(ac);
            break;
        case 1:
        case 8:
            enabled = g_test_rand_bit();
            autoconnect_set_enabled(ac, enabled);
            break;
        case 2:
            connected = g_test_rand_int_range(0, 4) == 0;
            autoconnect_set_airpods_connected(ac, connected, FAKE_ADAPTER_PATH);
            break;
        default:
            send_advert(g_test_rand_bit() ? STATUS_IN_EAR : STATUS_IN_CASE,
                        states[g_test_rand_int_range(0, G_N_ELEMENTS(states))]);
            break;
        }
        fake_bluez_run(g_test_rand_int_range(0, 20));
        /* A Connect decided at this step has arrived */
        fake_bluez_sync();

        int now_connects = fake_bluez_calls("Connect");
        if (now_connects > connects) {
            g_assert_cmpint(now_connects, ==, connects + 1);
            g_assert_true(enabled);
            g_assert_false(connected);
            gint64 now = g_get_monotonic_time();
            /* The call arrived after it was decided: a little slack */
            g_assert_true(last_connect == 0 || now - last_connect >= (COOLDOWN - 50) * 1000);
            last_connect = now;
            connects = now_connects;
        }

        if (i % 50 == 0) {
            connected = true;
            autoconnect_set_airpods_connected(ac, true, FAKE_ADAPTER_PATH);
            fake_bluez_run(50);
            g_assert_cmpint(fake_bluez_calls("StartDiscovery"), ==, fake_bluez_calls("StopDiscovery"));
        }
    }
    autoconnect_free(ac);
}

int main(int argc, char **argv)
{
    /* The keys are saved in the configuration */
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
    fake_bluez_start();

    g_test_add_func("/autoconnect/scan-cycle", test_scan_cycle);
    g_test_add_func("/autoconnect/scan-ends-early", test_scan_ends_early);
    g_test_add_func("/autoconnect/ears", test_ears);
    g_test_add_func("/autoconnect/ears-left-alone", test_ears_left_alone);
    g_test_add_func("/autoconnect/ears-in-use", test_ears_in_use);
    g_test_add_func("/autoconnect/scan-in-use", test_scan_in_use);
    g_test_add_func("/autoconnect/back-after-away", test_back_after_away);
    g_test_add_func("/autoconnect/playback", test_playback);
    g_test_add_func("/autoconnect/playback-busy", test_playback_busy);
    g_test_add_func("/autoconnect/playback-disabled", test_playback_disabled);
    g_test_add_func("/autoconnect/connected-here", test_connected_here);
    g_test_add_func("/autoconnect/nearby-battery", test_nearby_battery);
    g_test_add_func("/autoconnect/case", test_case);
    g_test_add_func("/autoconnect/storm", test_storm);
    int ret = g_test_run();

    fake_bluez_stop();
    return ret;
}
