/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Handoff decisions, with the delays shortened in meson.build (keep:
 * 100 ms, restart after playback: 20 ms, settle: 30 ms, check: 80 ms)
 */

#include <glib.h>

#include "handoff.h"

typedef struct {
    int paused;
    int restarted;
    int tried_again;
} Calls;

static void pause_players(void *user_data)
{
    ((Calls *)user_data)->paused++;
}

static void restart_audio(bool again, void *user_data)
{
    Calls *calls = user_data;
    if (again)
        calls->tried_again++;
    else
        calls->restarted++;
}

static const HandoffCallbacks callbacks = {
    .pause_players = pause_players,
    .restart_audio = restart_audio,
};

static void run_for(guint ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
}

/* An Apple device keeps the AirPods: pause here */
static void test_kept(void)
{
    Calls calls = { 0 };
    Handoff *handoff = handoff_new(&callbacks, &calls);
    handoff_set_enabled(handoff, true);

    handoff_source_changed(handoff, "computer");
    handoff_source_changed(handoff, "other");
    run_for(200);
    g_assert_cmpint(calls.paused, ==, 1);
    g_assert_cmpint(calls.restarted, ==, 0);

    /* Paused by the user there: nothing more here */
    handoff_source_changed(handoff, "none");
    run_for(50);
    g_assert_cmpint(calls.restarted, ==, 0);

    /* Until playback starts here again; back at once, no second try */
    handoff_playback_started(handoff);
    run_for(50);
    g_assert_cmpint(calls.restarted, ==, 1);
    handoff_source_changed(handoff, "computer");
    run_for(200);
    g_assert_cmpint(calls.tried_again, ==, 0);
    handoff_free(handoff);
}

/* Only for a moment: the sound here comes back, nothing paused */
static void test_moment(void)
{
    Calls calls = { 0 };
    Handoff *handoff = handoff_new(&callbacks, &calls);
    handoff_set_enabled(handoff, true);

    handoff_source_changed(handoff, "computer");
    handoff_source_changed(handoff, "other");
    run_for(30);
    handoff_source_changed(handoff, "none");
    run_for(60);
    g_assert_cmpint(calls.paused, ==, 0);
    g_assert_cmpint(calls.restarted, ==, 1);

    /* Not back: one more try, then no more */
    run_for(300);
    g_assert_cmpint(calls.tried_again, ==, 1);
    g_assert_cmpint(calls.restarted, ==, 1);
    handoff_free(handoff);
}

/* Back here, playback starting needs nothing; and nothing at all when off */
static void test_back_and_disabled(void)
{
    Calls calls = { 0 };
    Handoff *handoff = handoff_new(&callbacks, &calls);
    handoff_set_enabled(handoff, true);

    handoff_source_changed(handoff, "other");
    handoff_source_changed(handoff, "computer");
    handoff_playback_started(handoff);
    run_for(200);
    g_assert_cmpint(calls.paused, ==, 0);
    g_assert_cmpint(calls.restarted, ==, 0);

    /* Disconnected while another device had them: forgotten */
    handoff_source_changed(handoff, "other");
    handoff_source_changed(handoff, NULL);
    run_for(200);
    g_assert_cmpint(calls.paused, ==, 0);

    handoff_set_enabled(handoff, false);
    handoff_source_changed(handoff, "other");
    handoff_playback_started(handoff);
    run_for(200);
    g_assert_cmpint(calls.paused, ==, 0);
    g_assert_cmpint(calls.restarted, ==, 0);
    handoff_free(handoff);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/handoff/kept", test_kept);
    g_test_add_func("/handoff/moment", test_moment);
    g_test_add_func("/handoff/back-and-disabled", test_back_and_disabled);

    return g_test_run();
}
