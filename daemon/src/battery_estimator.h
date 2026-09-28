/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Remaining listening time estimate.
 *
 * Battery levels drop by 1% every 2-5 minutes, so the discharge rate
 * measured over a few minutes is mostly noise. Instead, each pair of AirPods
 * learns its own typical discharge rate from past listening sessions and
 * the estimate is simply level / learned rate. The rate of a session is the
 * slope of a linear regression over its level changes; sessions are blended
 * into the learned rate with a weight growing with their duration.
 *
 * No rated battery life is needed: until a first session has been learned,
 * the estimate comes from the current session once it is long enough, and
 * is unknown before that.
 */

#ifndef BATTERY_ESTIMATOR_H
#define BATTERY_ESTIMATOR_H

#include <stdint.h>
#include <stdbool.h>

/* A session teaches the rate (or gives a first estimate) only once it is
 * long enough to be reliable */
#define BATTERY_ESTIMATOR_MIN_SESSION_SEC (20 * 60)
#define BATTERY_ESTIMATOR_MIN_SESSION_DROP 3

/* Weight of the latest session in the learned rate, reached for sessions
 * of BATTERY_ESTIMATOR_FULL_WEIGHT_SEC or more (proportional below) */
#define BATTERY_ESTIMATOR_LEARNING_RATE 0.4
#define BATTERY_ESTIMATOR_FULL_WEIGHT_SEC (60 * 60)

/* Plausible discharge rates (%/h); anything outside is a bogus session */
#define BATTERY_ESTIMATOR_MIN_RATE 2.0
#define BATTERY_ESTIMATOR_MAX_RATE 60.0

typedef enum {
    BATTERY_POD_LEFT,
    BATTERY_POD_RIGHT,
    BATTERY_POD_COUNT,
} BatteryPod;

/* Discharge session of one pod. Level changes feed running sums for a
 * least-squares fit (time in hours since the first change), so no sample
 * needs to be stored. */
typedef struct {
    bool active;
    int last_level;
    int64_t first_change_sec;   /* -1 until the level first drops */
    int first_change_level;
    int64_t last_change_sec;
    int n;
    double sum_t, sum_l, sum_tt, sum_tl;
} DischargeSession;

typedef struct {
    double drain_rate;          /* Learned discharge rate, %/h; 0 = none yet */
    DischargeSession pods[BATTERY_POD_COUNT];
} BatteryEstimator;

void battery_estimator_init(BatteryEstimator *estimator, double drain_rate);

/* Feed a battery report for one pod (time in seconds, monotonic).
 * Returns true if a finished session changed the learned rate. */
bool battery_estimator_update(BatteryEstimator *estimator, BatteryPod pod,
                              int64_t now_sec, int level, bool discharging);

/* End every running session, e.g. on disconnection.
 * Returns true if the learned rate changed. */
bool battery_estimator_finish(BatteryEstimator *estimator);

/* Listening time left, in minutes, for a battery level; -1 if unknown
 * (nothing learned yet and no long enough session running) */
int battery_estimator_minutes_left(const BatteryEstimator *estimator, int level);

#endif /* BATTERY_ESTIMATOR_H */
