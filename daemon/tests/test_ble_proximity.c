/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * BLE proximity advertisement and automatic connection tests
 */

#include <glib.h>
#include <string.h>

#include "aes128.h"
#include "airpods_state.h"
#include "ble_proximity.h"

/* Advert captured from AirPods Pro 2 USB-C in both ears, playing music on an
 * iPhone (2026-09-28); the last 16 bytes are the encrypted payload */
static const uint8_t advert_music[] = {
    0x07, 0x19, 0x01, 0x24, 0x20, 0x0b, 0x43, 0x8f, 0x11, 0x00, 0x05, 0xad, 0x53,
    0xf7, 0xc3, 0x70, 0x5d, 0x6d, 0xba, 0xe4, 0xc4, 0x86, 0x5c, 0xb8, 0x48, 0x4f, 0x6d,
};

static void test_parse(void)
{
    ProximityInfo info;

    g_assert_true(proximity_parse(advert_music, sizeof(advert_music), &info));
    g_assert_cmpuint(info.model, ==, 0x2420);
    g_assert_true(info.in_ear);
    g_assert_false(info.both_in_case);
    g_assert_cmpuint(info.connection_state, ==, PROXIMITY_CONN_MUSIC);

    /* One pod out (status 0x09, also captured): still in an ear */
    uint8_t one_out[sizeof(advert_music)];
    memcpy(one_out, advert_music, sizeof(one_out));
    one_out[5] = 0x09;
    g_assert_true(proximity_parse(one_out, sizeof(one_out), &info));
    g_assert_true(info.in_ear);

    /* Not a proximity advert, pairing mode, too short */
    uint8_t other_type[sizeof(advert_music)];
    memcpy(other_type, advert_music, sizeof(other_type));
    other_type[0] = 0x10;
    g_assert_false(proximity_parse(other_type, sizeof(other_type), &info));
    uint8_t pairing[sizeof(advert_music)];
    memcpy(pairing, advert_music, sizeof(pairing));
    pairing[2] = 0x00;
    g_assert_false(proximity_parse(pairing, sizeof(pairing), &info));
    g_assert_false(proximity_parse(advert_music, 10, &info));
    g_assert_false(proximity_parse(NULL, 0, &info));
}

static void test_address_resolution(void)
{
    /* Bluetooth Core Spec Vol 3 Part H D.7: IRK ec0234a357c8ad05341010a60a397d9b,
     * prand 708194 -> hash 0dfbaa. The AirPods send the IRK reversed. */
    const uint8_t irk_msb[16] = {
        0xec, 0x02, 0x34, 0xa3, 0x57, 0xc8, 0xad, 0x05,
        0x34, 0x10, 0x10, 0xa6, 0x0a, 0x39, 0x7d, 0x9b,
    };
    uint8_t irk[16];
    for (int i = 0; i < 16; i++)
        irk[i] = irk_msb[15 - i];

    g_assert_true(proximity_address_matches(irk, "70:81:94:0D:FB:AA"));
    g_assert_true(proximity_address_matches(irk, "70:81:94:0d:fb:aa"));
    g_assert_false(proximity_address_matches(irk, "70:81:94:0D:FB:AB"));
    /* Not a resolvable private address (top bits must be 01) */
    g_assert_false(proximity_address_matches(irk, "30:81:94:0D:FB:AA"));
    g_assert_false(proximity_address_matches(irk, "not an address"));
    g_assert_false(proximity_address_matches(NULL, "70:81:94:0D:FB:AA"));
}

static void test_autoconnect_rules(void)
{
    const struct {
        uint8_t state;
        bool in_ear;
        bool ears_trigger;
        bool playback_trigger;
    } cases[] = {
        /* Free AirPods: connect */
        { PROXIMITY_CONN_DISCONNECTED, true, true, true },
        /* Idle on another device: connecting takes them from it, so only
         * when playback starts here */
        { PROXIMITY_CONN_IDLE, true, false, true },
        /* In use elsewhere: never */
        { PROXIMITY_CONN_MUSIC, true, false, false },
        { PROXIMITY_CONN_CALL, true, false, false },
        { PROXIMITY_CONN_RINGING, true, false, false },
        { PROXIMITY_CONN_HANGING_UP, true, false, false },
        { 0x42, true, false, false },
        /* Not worn: never */
        { PROXIMITY_CONN_DISCONNECTED, false, false, false },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        ProximityInfo info = {
            .model = 0x2420,
            .in_ear = cases[i].in_ear,
            .connection_state = cases[i].state,
        };
        g_assert_cmpint(autoconnect_allowed(&info, AUTOCONNECT_TRIGGER_EARS), ==,
                        cases[i].ears_trigger);
        g_assert_cmpint(autoconnect_allowed(&info, AUTOCONNECT_TRIGGER_PLAYBACK), ==,
                        cases[i].playback_trigger);
    }
}

static void test_worth_waiting(void)
{
    ProximityInfo music = { .model = 0x2420, .in_ear = true, .connection_state = PROXIMITY_CONN_MUSIC };
    ProximityInfo call = { .model = 0x2420, .in_ear = true, .connection_state = PROXIMITY_CONN_CALL };
    ProximityInfo not_worn = { .model = 0x2420, .in_ear = false, .connection_state = PROXIMITY_CONN_MUSIC };

    /* Music just paused on the iPhone still reads "music" for 2-3 s */
    g_assert_true(autoconnect_worth_waiting(&music));
    g_assert_false(autoconnect_worth_waiting(&call));
    g_assert_false(autoconnect_worth_waiting(&not_worn));
}

/* The advert carries Apple's product ID little-endian: the model enum must
 * use the bytes as they come (0x2027 is sent 27 20) */
static void test_model_ids(void)
{
    static const struct {
        uint8_t bytes[2];
        AirPodsModel model;
    } cases[] = {
        { { 0x24, 0x20 }, AIRPODS_MODEL_PRO_2_USBC },  /* Captured */
        { { 0x27, 0x20 }, AIRPODS_MODEL_PRO_3 },
        { { 0x36, 0x20 }, AIRPODS_MODEL_5 },
        { { 0x30, 0x20 }, AIRPODS_MODEL_5_WIRELESS_CHARGING },
        { { 0x2D, 0x20 }, AIRPODS_MODEL_MAX_2 },
    };
    uint8_t advert[sizeof(advert_music)];
    ProximityInfo info;

    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        memcpy(advert, advert_music, sizeof(advert));
        advert[3] = cases[i].bytes[0];
        advert[4] = cases[i].bytes[1];
        g_assert_true(proximity_parse(advert, sizeof(advert), &info));
        g_assert_cmpuint(info.model, ==, cases[i].model);
    }

    g_assert_true(airpods_model_has_ble_case(AIRPODS_MODEL_PRO_3));
    g_assert_true(airpods_model_is_headphones(AIRPODS_MODEL_MAX_2));
    g_assert_false(airpods_model_has_ble_case(AIRPODS_MODEL_MAX_2));
}

/* An advert encrypted here with a made-up key (the real ENC key is a
 * secret): right pod primary, so the pods arrive swapped */
static void test_decrypt_battery(void)
{
    const uint8_t key[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    /* [?] [primary = right: 64 %] [left: 70 %, charging] [case: unknown] */
    const uint8_t plain[16] = { 0x00, 64, 0x80 | 70, 0x7F };
    uint8_t advert[sizeof(advert_music)];
    ProximityInfo info;
    ProximityBattery battery;

    memcpy(advert, advert_music, sizeof(advert));
    advert[5] &= ~0x20;   /* Right pod primary */
    aes128_encrypt_block(key, plain, advert + sizeof(advert) - 16);

    g_assert_true(proximity_parse(advert, sizeof(advert), &info));
    g_assert_false(info.primary_left);
    g_assert_true(proximity_decrypt_battery(key, advert, sizeof(advert), &info, &battery));
    g_assert_cmpint(battery.left, ==, 70);
    g_assert_true(battery.left_charging);
    g_assert_cmpint(battery.right, ==, 64);
    g_assert_false(battery.right_charging);
    g_assert_cmpint(battery.case_level, ==, -1);
    g_assert_false(battery.case_charging);

    /* Too short to hold the encrypted block */
    g_assert_false(proximity_decrypt_battery(key, advert, 20, &info, &battery));
}

/* The case's advert, behind another entry, encrypted with a made-up key */
static void test_decrypt_case(void)
{
    const uint8_t key[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    const uint8_t other_key[16] = { 0 };
    /* Case at 49 %, charging; pods unknown */
    uint8_t plain[16] = { 0x10, 0x20, 0x0E, 0x80 | 49, 0xFF, 0xFF, 0x50, 0x01 };
    uint8_t advert[23] = { 0x12, 0x02, 0x64, 0x00, 0x07, 0x11, 0x06 };
    int level;
    bool charging;

    aes128_encrypt_block(key, plain, advert + 7);
    g_assert_true(proximity_decrypt_case(key, advert, sizeof(advert), &level, &charging));
    g_assert_cmpint(level, ==, 49);
    g_assert_true(charging);

    /* Another case, or noise: rejected */
    g_assert_false(proximity_decrypt_case(other_key, advert, sizeof(advert), &level, &charging));
    plain[9] = 0x01;
    aes128_encrypt_block(key, plain, advert + 7);
    g_assert_false(proximity_decrypt_case(key, advert, sizeof(advert), &level, &charging));

    /* Truncated, and the pods' advert */
    g_assert_false(proximity_decrypt_case(key, advert, sizeof(advert) - 1, &level, &charging));
    g_assert_false(proximity_decrypt_case(key, advert_music, sizeof(advert_music), &level, &charging));
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/proximity/parse", test_parse);
    g_test_add_func("/proximity/address-resolution", test_address_resolution);
    g_test_add_func("/proximity/autoconnect-rules", test_autoconnect_rules);
    g_test_add_func("/proximity/worth-waiting", test_worth_waiting);
    g_test_add_func("/proximity/model-ids", test_model_ids);
    g_test_add_func("/proximity/decrypt-battery", test_decrypt_battery);
    g_test_add_func("/proximity/decrypt-case", test_decrypt_case);

    return g_test_run();
}
