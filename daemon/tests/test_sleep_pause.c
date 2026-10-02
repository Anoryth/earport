/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Pause when falling asleep: confidence threshold, cool-off period (made
 * short by meson.build), cancelled by waking up or by using the computer
 */

#include <glib.h>
#include <string.h>

#include "sleep_pause.h"

#define COOL_OFF_MS 1000   /* Same value as passed by meson.build */

typedef struct {
    SleepPause *sp;
    bool enabled;
    int64_t idle_ms;
    int pauses;
    int rewind;
    GPtrArray *sent;
} Fixture;

static void on_send(const uint8_t *data, size_t len, void *user_data)
{
    Fixture *f = user_data;
    g_ptr_array_add(f->sent, g_bytes_new(data, len));
}

static bool is_enabled(void *user_data)
{
    return ((Fixture *)user_data)->enabled;
}

static int64_t idle_ms(void *user_data)
{
    return ((Fixture *)user_data)->idle_ms;
}

static void pause_media(int rewind_seconds, void *user_data)
{
    Fixture *f = user_data;
    f->pauses++;
    f->rewind = rewind_seconds;
}

static void fixture_setup(Fixture *f, gconstpointer data)
{
    (void)data;
    static const SleepPauseCallbacks callbacks = {
        .send = on_send,
        .is_enabled = is_enabled,
        .idle_ms = idle_ms,
        .pause_media = pause_media,
    };
    *f = (Fixture) { .enabled = true, .idle_ms = 60 * 60 * 1000 };
    f->sent = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
    f->sp = sleep_pause_new(&callbacks, f);
}

static void fixture_teardown(Fixture *f, gconstpointer data)
{
    (void)data;
    sleep_pause_free(f->sp);
    g_ptr_array_unref(f->sent);
}

static gboolean set_flag(gpointer user_data)
{
    *(bool *)user_data = true;
    return G_SOURCE_REMOVE;
}

static void run_for(guint ms)
{
    bool done = false;
    g_timeout_add(ms, set_flag, &done);
    while (!done)
        g_main_context_iteration(NULL, TRUE);
}

static void status(Fixture *f, uint8_t state, uint8_t confidence, int rewind)
{
    AapSleepDetection msg = {
        .msg_type = AAP_SLEEP_MSG_STATUS, .status = state,
        .confidence = confidence, .rewind_seconds = rewind,
    };
    sleep_pause_handle(f->sp, &msg);
}

static bool sent(Fixture *f, uint8_t msg_type, uint8_t value)
{
    uint8_t expected[AAP_SLEEP_MSG_SIZE];
    aap_build_sleep_detection_msg(msg_type, value, expected);
    for (guint i = 0; i < f->sent->len; i++) {
        gsize len;
        const uint8_t *data = g_bytes_get_data(f->sent->pdata[i], &len);
        if (len == sizeof(expected) && memcmp(data, expected, len) == 0)
            return true;
    }
    return false;
}

static void test_threshold_sent(Fixture *f, gconstpointer data)
{
    (void)data;
    sleep_pause_setting_changed(f->sp, true);
    g_assert_true(sent(f, AAP_SLEEP_MSG_THRESHOLD, 65));
}

/* Not at once: after the cool-off, rewound to when the user fell asleep:
 * the AirPods' estimate plus the cool-off */
static void test_pause_after_cool_off(Fixture *f, gconstpointer data)
{
    (void)data;
    status(f, AAP_SLEEP_STATUS_ASLEEP, 65, 50);
    g_assert_cmpint(f->pauses, ==, 0);

    /* A later report doesn't move the starting point */
    run_for(COOL_OFF_MS / 2);
    status(f, AAP_SLEEP_STATUS_ASLEEP, 70, 5);

    run_for(COOL_OFF_MS / 2 + 300);
    g_assert_cmpint(f->pauses, ==, 1);
    g_assert_cmpint(f->rewind, ==, 50 + COOL_OFF_MS / 1000);
}

static void test_low_confidence(Fixture *f, gconstpointer data)
{
    (void)data;
    status(f, AAP_SLEEP_STATUS_ASLEEP, 64, 0);
    run_for(COOL_OFF_MS + 300);
    g_assert_cmpint(f->pauses, ==, 0);
}

static void test_awake_again(Fixture *f, gconstpointer data)
{
    (void)data;
    status(f, AAP_SLEEP_STATUS_ASLEEP, 65, 0);
    run_for(COOL_OFF_MS / 2);
    status(f, 0x02, 64, 0);
    run_for(COOL_OFF_MS + 300);
    g_assert_cmpint(f->pauses, ==, 0);
}

/* Sitting still at the computer is not sleeping */
static void test_computer_used(Fixture *f, gconstpointer data)
{
    (void)data;
    f->idle_ms = 10;   /* Input during the cool-off */
    status(f, AAP_SLEEP_STATUS_ASLEEP, 65, 0);
    run_for(COOL_OFF_MS + 300);
    g_assert_cmpint(f->pauses, ==, 0);
    g_assert_true(sent(f, AAP_SLEEP_MSG_RESET, AAP_SLEEP_RESET_USER_ACTIVE));
    g_assert_true(sent(f, AAP_SLEEP_MSG_THRESHOLD, 65));
}

/* Not on GNOME: no idle time, the AirPods decide */
static void test_idle_unknown(Fixture *f, gconstpointer data)
{
    (void)data;
    f->idle_ms = -1;
    status(f, AAP_SLEEP_STATUS_ASLEEP, 65, 0);
    run_for(COOL_OFF_MS + 300);
    g_assert_cmpint(f->pauses, ==, 1);
}

static void test_setting_off(Fixture *f, gconstpointer data)
{
    (void)data;
    f->enabled = false;
    status(f, AAP_SLEEP_STATUS_ASLEEP, 65, 0);
    run_for(COOL_OFF_MS + 300);
    g_assert_cmpint(f->pauses, ==, 0);
}

static void test_disconnected_meanwhile(Fixture *f, gconstpointer data)
{
    (void)data;
    status(f, AAP_SLEEP_STATUS_ASLEEP, 65, 0);
    sleep_pause_session_ended(f->sp);
    run_for(COOL_OFF_MS + 300);
    g_assert_cmpint(f->pauses, ==, 0);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
    g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

    ADD("/sleep/threshold-sent", test_threshold_sent);
    ADD("/sleep/pause-after-cool-off", test_pause_after_cool_off);
    ADD("/sleep/low-confidence", test_low_confidence);
    ADD("/sleep/awake-again", test_awake_again);
    ADD("/sleep/computer-used", test_computer_used);
    ADD("/sleep/idle-unknown", test_idle_unknown);
    ADD("/sleep/setting-off", test_setting_off);
    ADD("/sleep/disconnected-meanwhile", test_disconnected_meanwhile);

    return g_test_run();
}
