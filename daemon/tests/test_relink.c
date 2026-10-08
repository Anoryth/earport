/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Reconnecting the AirPods on purpose, with the delays shortened in
 * meson.build (settle: 20 ms, pause: 10 ms, step: 100 ms)
 */

#include <glib.h>

#include "relink.h"

typedef struct {
    int disconnects;
    int connects;
    int started;
    int finished;
} Calls;

static void disconnect_airpods(void *user_data)
{
    ((Calls *)user_data)->disconnects++;
}

static void connect_airpods(void *user_data)
{
    ((Calls *)user_data)->connects++;
}

static void reconnecting(bool on, void *user_data)
{
    Calls *calls = user_data;
    if (on)
        calls->started++;
    else
        calls->finished++;
}

static const RelinkCallbacks callbacks = {
    .disconnect = disconnect_airpods,
    .connect = connect_airpods,
    .reconnecting = reconnecting,
};

static void run_for(guint ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
}

/* Twice: disconnect, connect, and again, announced as one reconnection */
static void test_twice(void)
{
    Calls calls = { 0 };
    Relink *relink = relink_new(&callbacks, &calls);

    relink_start(relink, 2);
    g_assert_cmpint(calls.started, ==, 1);
    run_for(30);
    g_assert_cmpint(calls.disconnects, ==, 1);

    /* BlueZ signals the disconnection before it is done: wait for its
     * answer */
    relink_link_closed(relink);
    run_for(20);
    g_assert_cmpint(calls.connects, ==, 0);
    relink_disconnect_done(relink);
    run_for(20);
    g_assert_cmpint(calls.connects, ==, 1);
    relink_connect_done(relink, true);
    relink_link_opened(relink);
    g_assert_cmpint(calls.finished, ==, 0);

    run_for(30);
    g_assert_cmpint(calls.disconnects, ==, 2);
    relink_disconnect_done(relink);
    run_for(20);
    g_assert_cmpint(calls.connects, ==, 2);
    relink_link_opened(relink);
    g_assert_cmpint(calls.finished, ==, 1);
    g_assert_cmpint(calls.started, ==, 1);

    /* Done: the next connections are ordinary ones */
    relink_link_closed(relink);
    relink_link_opened(relink);
    relink_connect_done(relink, false);
    run_for(50);
    g_assert_cmpint(calls.disconnects, ==, 2);
    g_assert_cmpint(calls.connects, ==, 2);
    relink_free(relink);
}

/* Connecting fails, or the link closes again: a few more tries */
static void test_connect_again(void)
{
    Calls calls = { 0 };
    Relink *relink = relink_new(&callbacks, &calls);

    relink_start(relink, 1);
    run_for(30);
    relink_disconnect_done(relink);
    run_for(20);
    g_assert_cmpint(calls.connects, ==, 1);
    relink_connect_done(relink, false);
    run_for(20);
    g_assert_cmpint(calls.connects, ==, 2);
    relink_link_closed(relink);
    run_for(20);
    g_assert_cmpint(calls.connects, ==, 3);

    /* No more */
    relink_connect_done(relink, false);
    relink_link_closed(relink);
    run_for(20);
    g_assert_cmpint(calls.connects, ==, 3);
    relink_link_opened(relink);
    g_assert_cmpint(calls.finished, ==, 1);
    relink_free(relink);
}

/* The AirPods don't answer: given up, not stuck */
static void test_give_up(void)
{
    Calls calls = { 0 };
    Relink *relink = relink_new(&callbacks, &calls);

    relink_start(relink, 1);
    run_for(30);
    g_assert_cmpint(calls.disconnects, ==, 1);
    run_for(120);
    g_assert_cmpint(calls.finished, ==, 1);

    /* Closing later changes nothing */
    relink_link_closed(relink);
    run_for(30);
    g_assert_cmpint(calls.connects, ==, 0);
    relink_free(relink);
}

/* Gone before being disconnected: connected back all the same */
static void test_gone_meanwhile(void)
{
    Calls calls = { 0 };
    Relink *relink = relink_new(&callbacks, &calls);

    relink_start(relink, 1);
    relink_link_closed(relink);
    run_for(40);
    g_assert_cmpint(calls.disconnects, ==, 0);
    g_assert_cmpint(calls.connects, ==, 1);
    relink_link_opened(relink);
    g_assert_cmpint(calls.finished, ==, 1);
    relink_free(relink);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/relink/twice", test_twice);
    g_test_add_func("/relink/connect-again", test_connect_again);
    g_test_add_func("/relink/give-up", test_give_up);
    g_test_add_func("/relink/gone-meanwhile", test_gone_meanwhile);

    return g_test_run();
}
