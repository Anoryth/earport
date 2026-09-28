/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Remaining listening time estimate tests
 */

#include <glib.h>

#include "battery_estimator.h"

/* Left pod level changes (seconds since the session start, level) logged by
 * the daemon with AirPods Pro 2 USB-C, noise cancellation on (2026-09-28) */
static const struct {
    int64_t sec;
    int level;
} real_session[] = {
    {    0, 100 }, {   40,  99 }, {  162,  98 }, {  345,  97 }, {  527,  96 },
    {  769,  95 }, { 1012,  94 }, { 1194,  93 }, { 1376,  92 }, { 1558,  91 },
    { 1801,  90 }, { 1983,  89 }, { 2044,  88 }, { 2165,  87 }, { 2408,  86 },
    { 2590,  85 }, { 2772,  84 }, { 3015,  83 },
};

static void feed_real_session(BatteryEstimator *e, int64_t offset)
{
    for (size_t i = 0; i < G_N_ELEMENTS(real_session); i++)
        battery_estimator_update(e, BATTERY_POD_LEFT, offset + real_session[i].sec,
                                 real_session[i].level, true);
}

static void test_first_session_sets_rate(void)
{
    BatteryEstimator e;
    battery_estimator_init(&e, 0);  /* Never seen these AirPods */

    feed_real_session(&e, 1000);
    g_assert_true(battery_estimator_finish(&e));

    /* Regression slope from the first drop: 19.35 %/h */
    g_assert_cmpfloat_with_epsilon(e.drain_rate, 19.349, 0.001);
}

static void test_session_blends_into_learned_rate(void)
{
    BatteryEstimator e;
    battery_estimator_init(&e, 25.0);

    feed_real_session(&e, 1000);
    g_assert_true(battery_estimator_finish(&e));

    /* 49.6 min session: weight 0.4 * 49.6/60 = 0.33 */
    double w = 0.4 * (2975.0 / 3600.0);
    g_assert_cmpfloat_with_epsilon(e.drain_rate, (1 - w) * 25.0 + w * 19.349, 0.001);
}

static void test_estimate_before_anything_learned(void)
{
    BatteryEstimator e;
    battery_estimator_init(&e, 0);

    /* First 10 minutes: not enough to say anything */
    for (size_t i = 0; real_session[i].sec <= 600; i++)
        battery_estimator_update(&e, BATTERY_POD_LEFT, real_session[i].sec,
                                 real_session[i].level, true);
    g_assert_cmpint(battery_estimator_minutes_left(&e, 96), ==, -1);

    /* After 20 minutes, the running session gives an estimate */
    feed_real_session(&e, 0);
    int minutes = battery_estimator_minutes_left(&e, 83);
    g_assert_cmpint(minutes, >, 83 * 60 / 25);
    g_assert_cmpint(minutes, <, 83 * 60 / 15);
}

static void test_charging_ends_session(void)
{
    BatteryEstimator e;
    battery_estimator_init(&e, 20.0);

    feed_real_session(&e, 0);
    /* Back in the case: the session is over and teaches the rate */
    g_assert_true(battery_estimator_update(&e, BATTERY_POD_LEFT, 3100, 83, false));
    g_assert_false(e.pods[BATTERY_POD_LEFT].active);
    g_assert_cmpfloat(e.drain_rate, !=, 20.0);
}

static void test_short_session_is_ignored(void)
{
    BatteryEstimator e;
    battery_estimator_init(&e, 20.0);

    /* 10 minutes, 3%: too short to be reliable */
    battery_estimator_update(&e, BATTERY_POD_RIGHT, 0, 80, true);
    battery_estimator_update(&e, BATTERY_POD_RIGHT, 60, 79, true);
    battery_estimator_update(&e, BATTERY_POD_RIGHT, 360, 78, true);
    battery_estimator_update(&e, BATTERY_POD_RIGHT, 660, 77, true);
    g_assert_false(battery_estimator_finish(&e));
    g_assert_cmpfloat(e.drain_rate, ==, 20.0);
}

static void test_level_increase_restarts_session(void)
{
    BatteryEstimator e;
    battery_estimator_init(&e, 20.0);

    battery_estimator_update(&e, BATTERY_POD_LEFT, 0, 60, true);
    battery_estimator_update(&e, BATTERY_POD_LEFT, 600, 58, true);
    /* Charged briefly between two reports: start over, learn nothing */
    battery_estimator_update(&e, BATTERY_POD_LEFT, 4000, 70, true);
    g_assert_cmpint(e.pods[BATTERY_POD_LEFT].last_level, ==, 70);
    g_assert_cmpint(e.pods[BATTERY_POD_LEFT].first_change_sec, ==, -1);
    g_assert_false(battery_estimator_finish(&e));
    g_assert_cmpfloat(e.drain_rate, ==, 20.0);
}

static void test_bogus_rate_is_ignored(void)
{
    BatteryEstimator e;
    battery_estimator_init(&e, 20.0);

    /* 50% in 21 minutes (~143 %/h) cannot be a real listening session */
    battery_estimator_update(&e, BATTERY_POD_LEFT, 0, 90, true);
    battery_estimator_update(&e, BATTERY_POD_LEFT, 60, 89, true);
    battery_estimator_update(&e, BATTERY_POD_LEFT, 700, 60, true);
    battery_estimator_update(&e, BATTERY_POD_LEFT, 60 + 21 * 60, 39, true);
    g_assert_false(battery_estimator_finish(&e));
    g_assert_cmpfloat(e.drain_rate, ==, 20.0);
}

static void test_minutes_left(void)
{
    BatteryEstimator e;
    battery_estimator_init(&e, 20.0);

    g_assert_cmpint(battery_estimator_minutes_left(&e, 50), ==, 150);
    g_assert_cmpint(battery_estimator_minutes_left(&e, 0), ==, 0);
    g_assert_cmpint(battery_estimator_minutes_left(&e, -1), ==, -1);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/estimator/first-session-sets-rate", test_first_session_sets_rate);
    g_test_add_func("/estimator/session-blends", test_session_blends_into_learned_rate);
    g_test_add_func("/estimator/estimate-before-learning", test_estimate_before_anything_learned);
    g_test_add_func("/estimator/charging-ends-session", test_charging_ends_session);
    g_test_add_func("/estimator/short-session-ignored", test_short_session_is_ignored);
    g_test_add_func("/estimator/level-increase-restarts", test_level_increase_restarts_session);
    g_test_add_func("/estimator/bogus-rate-ignored", test_bogus_rate_is_ignored);
    g_test_add_func("/estimator/minutes-left", test_minutes_left);

    return g_test_run();
}
