/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "handoff.h"

#include <glib.h>

/* Longer than an iPhone sound or notification taking the AirPods */
#ifndef HANDOFF_KEEP_MS
#define HANDOFF_KEEP_MS 3000
#endif

/* Leaves the player time to send its sound again */
#ifndef HANDOFF_PLAY_DELAY_MS
#define HANDOFF_PLAY_DELAY_MS 500
#endif

/* Leaves the other device time to let the AirPods go: restarting too early
 * leaves PipeWire's output stuck */
#ifndef HANDOFF_SETTLE_MS
#define HANDOFF_SETTLE_MS 1000
#endif

/* The AirPods should be back by then, otherwise try once more */
#ifndef HANDOFF_CHECK_MS
#define HANDOFF_CHECK_MS 2500
#endif

struct Handoff {
    HandoffCallbacks callbacks;
    void *user_data;
    bool enabled;

    bool taken;                 /* Another device had them since this one */
    bool here;                  /* They play from this computer */
    guint keep_id;              /* Another device has them: kept? */
    guint restart_id;           /* Sound back shortly */
    guint check_id;             /* Back after the restart? */
    bool tried_again;
};

static void cancel(guint *id)
{
    if (*id > 0) {
        g_source_remove(*id);
        *id = 0;
    }
}

static gboolean keep_done_cb(gpointer user_data)
{
    Handoff *handoff = user_data;
    handoff->keep_id = 0;
    g_message("Handoff: another device kept the AirPods, pausing here");
    handoff->callbacks.pause_players(handoff->user_data);
    return G_SOURCE_REMOVE;
}

static gboolean check_cb(gpointer user_data)
{
    Handoff *handoff = user_data;
    handoff->check_id = 0;
    if (handoff->here || handoff->tried_again)
        return G_SOURCE_REMOVE;

    g_message("Handoff: the AirPods didn't come back, trying again");
    handoff->tried_again = true;
    handoff->callbacks.restart_audio(true, handoff->user_data);
    handoff->check_id = g_timeout_add(HANDOFF_CHECK_MS, check_cb, handoff);
    return G_SOURCE_REMOVE;
}

static gboolean restart_cb(gpointer user_data)
{
    Handoff *handoff = user_data;
    handoff->restart_id = 0;
    handoff->tried_again = false;
    handoff->callbacks.restart_audio(false, handoff->user_data);
    cancel(&handoff->check_id);
    handoff->check_id = g_timeout_add(HANDOFF_CHECK_MS, check_cb, handoff);
    return G_SOURCE_REMOVE;
}

static void restart_in(Handoff *handoff, guint ms)
{
    cancel(&handoff->restart_id);
    handoff->restart_id = g_timeout_add(ms, restart_cb, handoff);
}

static void cancel_all(Handoff *handoff)
{
    cancel(&handoff->keep_id);
    cancel(&handoff->restart_id);
    cancel(&handoff->check_id);
}

Handoff *handoff_new(const HandoffCallbacks *callbacks, void *user_data)
{
    Handoff *handoff = g_new0(Handoff, 1);
    handoff->callbacks = *callbacks;
    handoff->user_data = user_data;
    return handoff;
}

void handoff_free(Handoff *handoff)
{
    if (handoff == NULL)
        return;
    cancel_all(handoff);
    g_free(handoff);
}

void handoff_set_enabled(Handoff *handoff, bool enabled)
{
    handoff->enabled = enabled;
    if (!enabled) {
        cancel_all(handoff);
        handoff->taken = false;
    }
}

void handoff_source_changed(Handoff *handoff, const char *source)
{
    handoff->here = g_strcmp0(source, "computer") == 0;
    if (!handoff->enabled)
        return;

    if (g_strcmp0(source, "other") == 0) {
        handoff->taken = true;
        if (handoff->keep_id == 0)
            handoff->keep_id = g_timeout_add(HANDOFF_KEEP_MS, keep_done_cb, handoff);
    } else if (g_strcmp0(source, "none") == 0) {
        /* Only for a moment: what was playing here plays again */
        if (handoff->keep_id > 0) {
            cancel(&handoff->keep_id);
            g_message("Handoff: another device only had the AirPods for a moment");
            restart_in(handoff, HANDOFF_SETTLE_MS);
        }
    } else {
        /* Back here, or disconnected */
        cancel_all(handoff);
        handoff->taken = false;
    }
}

void handoff_playback_started(Handoff *handoff)
{
    if (!handoff->enabled || !handoff->taken)
        return;

    g_message("Handoff: playback started here, getting the AirPods back");
    restart_in(handoff, HANDOFF_PLAY_DELAY_MS);
}
