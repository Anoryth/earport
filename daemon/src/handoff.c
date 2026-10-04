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

struct Handoff {
    HandoffCallbacks callbacks;
    void *user_data;
    bool enabled;

    bool taken;                 /* Another device had them since this one */
    guint keep_id;              /* Another device has them: kept? */
    guint restart_id;           /* Playback started: sound back shortly */
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

static gboolean restart_cb(gpointer user_data)
{
    Handoff *handoff = user_data;
    handoff->restart_id = 0;
    handoff->callbacks.restart_audio(handoff->user_data);
    return G_SOURCE_REMOVE;
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
    cancel(&handoff->keep_id);
    cancel(&handoff->restart_id);
    g_free(handoff);
}

void handoff_set_enabled(Handoff *handoff, bool enabled)
{
    handoff->enabled = enabled;
    if (!enabled) {
        cancel(&handoff->keep_id);
        cancel(&handoff->restart_id);
        handoff->taken = false;
    }
}

void handoff_source_changed(Handoff *handoff, const char *source)
{
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
            handoff->callbacks.restart_audio(handoff->user_data);
        }
    } else {
        /* Back here, or disconnected */
        cancel(&handoff->keep_id);
        cancel(&handoff->restart_id);
        handoff->taken = false;
    }
}

void handoff_playback_started(Handoff *handoff)
{
    if (!handoff->enabled || !handoff->taken)
        return;

    g_message("Handoff: playback started here, getting the AirPods back");
    cancel(&handoff->restart_id);
    handoff->restart_id = g_timeout_add(HANDOFF_PLAY_DELAY_MS, restart_cb, handoff);
}
