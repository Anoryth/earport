/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * What this computer does when the AirPods switch between it and an Apple
 * device (once they take it for one, see apple_identity.h), like a Mac:
 * - another device keeps them: pause the players here, the user moved on;
 *   if it lets them go soon after (an iPhone announcing a notification),
 *   or after a call however long, resume them;
 * - another device only had them for a moment (an iPhone sound, say): get
 *   the sound playing here back;
 * - playback starts here after another device had them: get the sound back
 *   too, the stream stays silent otherwise.
 * Getting the sound back and pausing are left to the callbacks.
 */

#ifndef HANDOFF_H
#define HANDOFF_H

#include <stdbool.h>

/* How to restart the sound playing to the AirPods */
typedef enum {
    HANDOFF_RESTART_STREAMS,    /* Restart what plays to them */
    HANDOFF_RESTART_OUTPUT,     /* Restart their output (second try) */
    HANDOFF_RESTART_ANY,        /* The streams, or the output if none plays */
} HandoffRestart;

typedef struct {
    /* Pause the players playing here */
    void (*pause_players)(void *user_data);
    /* Resume the ones pause_players paused */
    void (*resume_players)(void *user_data);
    /* Restart what plays here to the AirPods, so that they switch back */
    void (*restart_audio)(HandoffRestart how, void *user_data);
} HandoffCallbacks;

typedef struct Handoff Handoff;

Handoff *handoff_new(const HandoffCallbacks *callbacks, void *user_data);
void handoff_free(Handoff *handoff);

void handoff_set_enabled(Handoff *handoff, bool enabled);

/* The device the AirPods play from: "computer", "other", "none", or NULL
 * once disconnected (a static string: compared, not copied); call: it
 * plays a call */
void handoff_source_changed(Handoff *handoff, const char *source, bool call);

/* A player started playing here */
void handoff_playback_started(Handoff *handoff);

/* The user asks for the AirPods here: resume what was paused for another
 * device and get the sound back */
void handoff_use_here(Handoff *handoff);

#endif /* HANDOFF_H */
