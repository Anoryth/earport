/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * What this computer does when the AirPods switch between it and an Apple
 * device (once they take it for one, see apple_identity.h), like a Mac:
 * - another device keeps them: pause the players here, the user moved on;
 * - another device only had them for a moment (an iPhone sound, say): get
 *   the sound playing here back;
 * - playback starts here after another device had them: get the sound back
 *   too, the stream stays silent otherwise.
 * Getting the sound back and pausing are left to the callbacks.
 */

#ifndef HANDOFF_H
#define HANDOFF_H

#include <stdbool.h>

typedef struct {
    /* Pause the players playing here */
    void (*pause_players)(void *user_data);
    /* Restart what plays here to the AirPods, so that they switch back */
    void (*restart_audio)(void *user_data);
} HandoffCallbacks;

typedef struct Handoff Handoff;

Handoff *handoff_new(const HandoffCallbacks *callbacks, void *user_data);
void handoff_free(Handoff *handoff);

void handoff_set_enabled(Handoff *handoff, bool enabled);

/* The device the AirPods play from: "computer", "other", "none", or NULL
 * once disconnected */
void handoff_source_changed(Handoff *handoff, const char *source);

/* A player started playing here */
void handoff_playback_started(Handoff *handoff);

#endif /* HANDOFF_H */
