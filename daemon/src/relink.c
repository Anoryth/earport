/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "relink.h"

#include <glib.h>

/* Before disconnecting: the new identity is published by then, and the
 * AirPods have settled since connecting */
#ifndef RELINK_SETTLE_MS
#define RELINK_SETTLE_MS 2000
#endif

/* Disconnected: connecting again right away may find BlueZ busy */
#ifndef RELINK_PAUSE_MS
#define RELINK_PAUSE_MS 1000
#endif

/* Connection attempts per reconnection */
#define RELINK_CONNECT_TRIES 3

/* No answer from a step by then: give up */
#ifndef RELINK_STEP_MS
#define RELINK_STEP_MS 30000
#endif

typedef enum {
    RELINK_IDLE,
    RELINK_SETTLING,            /* Connected, disconnecting next */
    RELINK_DISCONNECTING,
    RELINK_PAUSING,             /* Disconnected, connecting next */
    RELINK_CONNECTING,
} RelinkStep;

struct Relink {
    RelinkCallbacks callbacks;
    void *user_data;
    RelinkStep step;
    int remaining;
    int connect_tries;
    guint timer_id;             /* Next step, or giving up */
};

static void cancel_timer(Relink *relink)
{
    if (relink->timer_id > 0) {
        g_source_remove(relink->timer_id);
        relink->timer_id = 0;
    }
}

static void finish(Relink *relink, const char *how)
{
    cancel_timer(relink);
    bool was_running = relink->step != RELINK_IDLE;
    relink->step = RELINK_IDLE;
    relink->remaining = 0;
    if (was_running) {
        g_message("Reconnecting the AirPods: %s", how);
        if (relink->callbacks.reconnecting != NULL)
            relink->callbacks.reconnecting(false, relink->user_data);
    }
}

static gboolean on_give_up(gpointer user_data)
{
    Relink *relink = user_data;
    relink->timer_id = 0;
    finish(relink, "no answer, giving up");
    return G_SOURCE_REMOVE;
}

static void wait_for_step(Relink *relink, RelinkStep step)
{
    cancel_timer(relink);
    relink->step = step;
    relink->timer_id = g_timeout_add(RELINK_STEP_MS, on_give_up, relink);
}

static gboolean on_settled(gpointer user_data)
{
    Relink *relink = user_data;
    relink->timer_id = 0;
    g_message("Reconnecting the AirPods so that they read this computer's identity (%d left)",
              relink->remaining);
    wait_for_step(relink, RELINK_DISCONNECTING);
    relink->callbacks.disconnect(relink->user_data);
    return G_SOURCE_REMOVE;
}

static gboolean on_paused(gpointer user_data)
{
    Relink *relink = user_data;
    relink->timer_id = 0;
    relink->connect_tries++;
    wait_for_step(relink, RELINK_CONNECTING);
    relink->callbacks.connect(relink->user_data);
    return G_SOURCE_REMOVE;
}

static void pause_then_connect(Relink *relink)
{
    cancel_timer(relink);
    relink->step = RELINK_PAUSING;
    relink->timer_id = g_timeout_add(RELINK_PAUSE_MS, on_paused, relink);
}

static void settle(Relink *relink)
{
    cancel_timer(relink);
    relink->step = RELINK_SETTLING;
    relink->connect_tries = 0;
    relink->timer_id = g_timeout_add(RELINK_SETTLE_MS, on_settled, relink);
}

Relink *relink_new(const RelinkCallbacks *callbacks, void *user_data)
{
    Relink *relink = g_new0(Relink, 1);
    relink->callbacks = *callbacks;
    relink->user_data = user_data;
    return relink;
}

void relink_free(Relink *relink)
{
    if (relink == NULL)
        return;
    relink->callbacks.reconnecting = NULL;  /* Nobody to tell any more */
    finish(relink, "stopped");
    g_free(relink);
}

void relink_start(Relink *relink, int times)
{
    if (times <= 0)
        return;
    bool was_running = relink->step != RELINK_IDLE;
    relink->remaining = times;
    settle(relink);
    if (!was_running && relink->callbacks.reconnecting != NULL)
        relink->callbacks.reconnecting(true, relink->user_data);
}

void relink_link_opened(Relink *relink)
{
    if (relink->step != RELINK_CONNECTING)
        return;
    relink->remaining--;
    if (relink->remaining > 0)
        settle(relink);
    else
        finish(relink, "done");
}

void relink_disconnect_done(Relink *relink)
{
    /* BlueZ answers once the link is down, its signals come earlier */
    if (relink->step == RELINK_DISCONNECTING)
        pause_then_connect(relink);
}

void relink_connect_done(Relink *relink, bool connected)
{
    if (relink->step == RELINK_CONNECTING && !connected &&
        relink->connect_tries < RELINK_CONNECT_TRIES)
        pause_then_connect(relink);
}

void relink_link_closed(Relink *relink)
{
    /* Gone before being disconnected, or again while connecting: connect
     * them back all the same */
    if (relink->step == RELINK_SETTLING ||
        (relink->step == RELINK_CONNECTING && relink->connect_tries < RELINK_CONNECT_TRIES))
        pause_then_connect(relink);
}
