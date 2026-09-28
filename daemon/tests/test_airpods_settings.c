/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * AirPods settings table and state storage tests
 */

#include <glib.h>

#include "airpods_settings.h"
#include "airpods_state.h"

/* ============================================================================
 * Settings table
 * ========================================================================== */

static void test_lookup(void)
{
    const AirPodsSettingDef *def;

    def = airpods_setting_by_key("OneBudANC");
    g_assert_nonnull(def);
    g_assert_cmpuint(def->id, ==, 0x1B);
    g_assert_true(airpods_setting_by_id(0x1B) == def);

    g_assert_nonnull(airpods_setting_by_key("PersonalizedVolume"));
    g_assert_nonnull(airpods_setting_by_key("SleepDetection"));
    g_assert_nonnull(airpods_setting_by_key("PressSpeed"));
    g_assert_nonnull(airpods_setting_by_key("PressHoldDuration"));
    g_assert_nonnull(airpods_setting_by_key("VolumeSwipe"));
    g_assert_nonnull(airpods_setting_by_key("VolumeSwipeSpeed"));

    /* Features with a dedicated D-Bus API are not generic settings */
    g_assert_null(airpods_setting_by_id(0x0D));
    g_assert_null(airpods_setting_by_id(0x28));
    g_assert_null(airpods_setting_by_key("NoSuchSetting"));
    g_assert_null(airpods_setting_by_key(NULL));
}

static void test_bool_encoding(void)
{
    const AirPodsSettingDef *def = airpods_setting_by_key("VolumeSwipe");
    uint8_t byte = 0;

    g_assert_cmpint(def->kind, ==, AIRPODS_SETTING_BOOL);

    /* Same convention as conversational awareness: 01 = on, 02 = off */
    g_assert_true(airpods_setting_encode(def, 1, &byte));
    g_assert_cmpuint(byte, ==, 0x01);
    g_assert_true(airpods_setting_encode(def, 0, &byte));
    g_assert_cmpuint(byte, ==, 0x02);
    g_assert_false(airpods_setting_encode(def, 2, &byte));

    g_assert_cmpint(airpods_setting_decode(def, 0x01), ==, 1);
    g_assert_cmpint(airpods_setting_decode(def, 0x02), ==, 0);
    g_assert_cmpint(airpods_setting_decode(def, 0x00), ==, -1);
}

static void test_choice_encoding(void)
{
    /* Press speed: 0 = default, 1 = slower, 2 = slowest */
    const AirPodsSettingDef *def = airpods_setting_by_key("PressSpeed");
    uint8_t byte = 0xFF;

    g_assert_cmpint(def->kind, ==, AIRPODS_SETTING_CHOICE);

    g_assert_true(airpods_setting_encode(def, 2, &byte));
    g_assert_cmpuint(byte, ==, 0x02);
    g_assert_false(airpods_setting_encode(def, 3, &byte));
    g_assert_false(airpods_setting_encode(def, -1, &byte));

    g_assert_cmpint(airpods_setting_decode(def, 0x00), ==, 0);
    g_assert_cmpint(airpods_setting_decode(def, 0x07), ==, -1);
}

/* ============================================================================
 * State storage
 * ========================================================================== */

static void test_state_settings(void)
{
    AirPodsState state;
    uint8_t byte = 0;

    airpods_state_init(&state);

    /* Nothing announced yet */
    g_assert_false(airpods_state_get_setting(&state, 0x26, &byte));

    g_assert_true(airpods_state_set_setting(&state, 0x26, 0x01));
    g_assert_true(airpods_state_get_setting(&state, 0x26, &byte));
    g_assert_cmpuint(byte, ==, 0x01);

    /* Same value again: no change to report */
    g_assert_false(airpods_state_set_setting(&state, 0x26, 0x01));
    g_assert_true(airpods_state_set_setting(&state, 0x26, 0x02));

    /* Unknown IDs are ignored */
    g_assert_false(airpods_state_set_setting(&state, 0x3E, 0x02));
    g_assert_false(airpods_state_get_setting(&state, 0x3E, &byte));

    /* A disconnect forgets what the AirPods announced */
    airpods_state_reset(&state);
    g_assert_false(airpods_state_get_setting(&state, 0x26, &byte));

    airpods_state_cleanup(&state);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/settings/lookup", test_lookup);
    g_test_add_func("/settings/bool-encoding", test_bool_encoding);
    g_test_add_func("/settings/choice-encoding", test_choice_encoding);
    g_test_add_func("/settings/state", test_state_settings);

    return g_test_run();
}
