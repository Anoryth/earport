/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "sleep_pause.h"

#include <glib.h>

/* Confidence the AirPods must reach, also sent to them */
#define SLEEP_CONFIDENCE_THRESHOLD 65

/* Time between the detection and the pause: the AirPods also report
 * sleep for a user sitting still, who will touch the computer meanwhile */
#ifndef SLEEP_COOL_OFF_MS
#define SLEEP_COOL_OFF_MS (10 * 60 * 1000)
#endif

struct SleepPause {
    SleepPauseCallbacks callbacks;
    void *user_data;

    guint cool_off_id;
    int rewind_seconds;        /* AirPods' estimate when sleep was detected */
    gint64 detected_us;        /* When it was detected */
};

static void send_msg(SleepPause *sp, uint8_t msg_type, uint8_t value)
{
    uint8_t msg[AAP_SLEEP_MSG_SIZE];
    aap_build_sleep_detection_msg(msg_type, value, msg);
    sp->callbacks.send(msg, sizeof(msg), sp->user_data);
}

static void cancel_cool_off(SleepPause *sp)
{
    if (sp->cool_off_id > 0) {
        g_source_remove(sp->cool_off_id);
        sp->cool_off_id = 0;
    }
}

static gboolean cool_off_done_cb(gpointer user_data)
{
    SleepPause *sp = user_data;
    sp->cool_off_id = 0;

    /* Turned off meanwhile */
    if (!sp->callbacks.is_enabled(sp->user_data))
        return G_SOURCE_REMOVE;

    /* Used the computer meanwhile: awake after all. Tell the AirPods, and
     * arm the detection again. */
    int64_t idle = sp->callbacks.idle_ms(sp->user_data);
    if (idle >= 0 && idle < SLEEP_COOL_OFF_MS) {
        g_message("Sleep detection: the computer was used meanwhile, not pausing");
        send_msg(sp, AAP_SLEEP_MSG_RESET, AAP_SLEEP_RESET_USER_ACTIVE);
        send_msg(sp, AAP_SLEEP_MSG_THRESHOLD, SLEEP_CONFIDENCE_THRESHOLD);
        return G_SOURCE_REMOVE;
    }

    /* Back to when the user fell asleep: the AirPods' estimate, plus the
     * time playback kept going since */
    gint64 elapsed_s = (g_get_monotonic_time() - sp->detected_us + G_USEC_PER_SEC / 2) / G_USEC_PER_SEC;
    int rewind = sp->rewind_seconds + (int)elapsed_s;
    g_message("Sleep detection: pausing playback, rewinding %d s", rewind);
    sp->callbacks.pause_media(rewind, sp->user_data);
    return G_SOURCE_REMOVE;
}

SleepPause *sleep_pause_new(const SleepPauseCallbacks *callbacks, void *user_data)
{
    SleepPause *sp = g_new0(SleepPause, 1);
    sp->callbacks = *callbacks;
    sp->user_data = user_data;
    return sp;
}

void sleep_pause_free(SleepPause *sp)
{
    if (sp == NULL)
        return;
    cancel_cool_off(sp);
    g_free(sp);
}

void sleep_pause_setting_changed(SleepPause *sp, bool enabled)
{
    if (enabled)
        send_msg(sp, AAP_SLEEP_MSG_THRESHOLD, SLEEP_CONFIDENCE_THRESHOLD);
    else
        cancel_cool_off(sp);
}

void sleep_pause_handle(SleepPause *sp, const AapSleepDetection *msg)
{
    if (msg->msg_type != AAP_SLEEP_MSG_STATUS)
        return;

    g_message("Sleep detection: %s, confidence %u, rewind %d s",
              msg->status == AAP_SLEEP_STATUS_ASLEEP ? "asleep" : "awake",
              msg->confidence, msg->rewind_seconds);

    if (!sp->callbacks.is_enabled(sp->user_data))
        return;

    if (msg->status != AAP_SLEEP_STATUS_ASLEEP) {
        if (sp->cool_off_id > 0)
            g_message("Sleep detection: awake again, not pausing");
        cancel_cool_off(sp);
        return;
    }

    if (msg->confidence < SLEEP_CONFIDENCE_THRESHOLD)
        return;

    /* The first report dates the sleep; later ones change nothing */
    if (sp->cool_off_id == 0) {
        sp->rewind_seconds = msg->rewind_seconds;
        sp->detected_us = g_get_monotonic_time();
        g_message("Sleep detection: pausing in %d min unless the computer is used",
                  SLEEP_COOL_OFF_MS / 60000);
        sp->cool_off_id = g_timeout_add(SLEEP_COOL_OFF_MS, cool_off_done_cb, sp);
    }
}

void sleep_pause_session_ended(SleepPause *sp)
{
    cancel_cool_off(sp);
}
