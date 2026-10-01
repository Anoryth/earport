/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * "Pause Media When Falling Asleep": the AirPods report that the user fell
 * asleep, with a confidence; the host decides. Like on an iPhone, nothing
 * happens at once: after a cool-off period without the user touching the
 * computer, playback is paused and rewound a little.
 */

#ifndef SLEEP_PAUSE_H
#define SLEEP_PAUSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aap_protocol.h"

typedef struct {
    /* Send a packet to the AirPods */
    void (*send)(const uint8_t *data, size_t len, void *user_data);
    /* Whether the setting is on, on the AirPods */
    bool (*is_enabled)(void *user_data);
    /* Milliseconds since the last keyboard or mouse input, -1 if unknown */
    int64_t (*idle_ms)(void *user_data);
    /* Pause what is playing and replay the last seconds */
    void (*pause_media)(int rewind_seconds, void *user_data);
} SleepPauseCallbacks;

typedef struct SleepPause SleepPause;

SleepPause *sleep_pause_new(const SleepPauseCallbacks *callbacks, void *user_data);
void sleep_pause_free(SleepPause *sp);

/* The AirPods reported the setting (on or off) */
void sleep_pause_setting_changed(SleepPause *sp, bool enabled);

/* A sleep detection message from the AirPods */
void sleep_pause_handle(SleepPause *sp, const AapSleepDetection *msg);

/* The AirPods disconnected: forget a pending pause */
void sleep_pause_session_ended(SleepPause *sp);

#endif /* SLEEP_PAUSE_H */
