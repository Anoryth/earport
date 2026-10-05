/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Media control via MPRIS D-Bus interface
 * Handles pause/play when AirPods are removed from ears
 */

#ifndef MEDIA_CONTROL_H
#define MEDIA_CONTROL_H

#include <glib.h>
#include <stdbool.h>

/* Media control context */
typedef struct MediaControl MediaControl;

/* Called when any MPRIS player starts playing */
typedef void (*MediaPlaybackStartedCallback)(void *user_data);

/* Called for each player paused because the user fell asleep */
typedef void (*MediaPausedForSleepCallback)(const char *player, void *user_data);

/* Ear detection mode for auto-pause behavior */
typedef enum {
    EAR_PAUSE_DISABLED = 0,    /* Don't pause on ear removal */
    EAR_PAUSE_ONE_OUT = 1,     /* Pause when one pod is removed */
    EAR_PAUSE_BOTH_OUT = 2,    /* Pause when both pods are removed */
} EarPauseMode;

/* Create new media control instance */
MediaControl *media_control_new(void);

/* Free media control instance */
void media_control_free(MediaControl *mc);

/* Set ear pause mode */
void media_control_set_ear_pause_mode(MediaControl *mc, EarPauseMode mode);

/* Get current ear pause mode */
EarPauseMode media_control_get_ear_pause_mode(MediaControl *mc);

/* Update ear detection state - will trigger pause/play as needed */
void media_control_on_ear_detection_changed(MediaControl *mc,
                                            bool left_in_ear,
                                            bool right_in_ear);

/* Pause all playing media players (asynchronous: returns at once, the
 * players answer from the main loop) */
void media_control_pause_all(MediaControl *mc);

/* Be told when a player starts playing (e.g. to connect the AirPods) */
void media_control_set_playback_started_callback(MediaControl *mc,
                                                 MediaPlaybackStartedCallback callback,
                                                 void *user_data);

/* Pause the players playing because the user fell asleep: not resumed
 * when the AirPods go back in, and rewound by this many seconds */
void media_control_pause_for_sleep(MediaControl *mc, int rewind_seconds);

void media_control_set_paused_for_sleep_callback(MediaControl *mc,
                                                 MediaPausedForSleepCallback callback,
                                                 void *user_data);

/* Pause the players playing because another device took the AirPods: not
 * resumed when the AirPods go back in */
void media_control_pause_for_handoff(MediaControl *mc);

/* Resume the players paused by media_control_pause_for_handoff */
void media_control_resume_handoff(MediaControl *mc);

/* Resume media players that were paused by us */
void media_control_resume(MediaControl *mc);

#endif /* MEDIA_CONTROL_H */