/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "battery_estimator.h"
#include <math.h>
#include <string.h>

static void session_reset(DischargeSession *session)
{
    memset(session, 0, sizeof(*session));
    session->first_change_sec = -1;
}

/* Discharge rate (%/h) of a session, false if it is too short or bogus */
static bool session_rate(const DischargeSession *session, double *rate)
{
    if (session->n < 3 ||
        session->last_change_sec - session->first_change_sec < BATTERY_ESTIMATOR_MIN_SESSION_SEC ||
        session->first_change_level - session->last_level < BATTERY_ESTIMATOR_MIN_SESSION_DROP)
        return false;

    double n = session->n;
    double denominator = n * session->sum_tt - session->sum_t * session->sum_t;
    if (denominator <= 0)
        return false;

    /* Least-squares slope of level over time; levels go down */
    double slope = -(n * session->sum_tl - session->sum_t * session->sum_l) / denominator;
    if (slope < BATTERY_ESTIMATOR_MIN_RATE || slope > BATTERY_ESTIMATOR_MAX_RATE)
        return false;

    *rate = slope;
    return true;
}

/* Learn from a finished session, then forget it */
static bool session_finish(BatteryEstimator *estimator, DischargeSession *session)
{
    double rate;
    bool learned = false;

    if (session->active && session_rate(session, &rate)) {
        if (estimator->drain_rate <= 0) {
            estimator->drain_rate = rate;
        } else {
            double duration = session->last_change_sec - session->first_change_sec;
            double weight = BATTERY_ESTIMATOR_LEARNING_RATE *
                            fmin(1.0, duration / BATTERY_ESTIMATOR_FULL_WEIGHT_SEC);
            estimator->drain_rate = (1 - weight) * estimator->drain_rate + weight * rate;
        }
        learned = true;
    }

    session_reset(session);
    return learned;
}

void battery_estimator_init(BatteryEstimator *estimator, double drain_rate)
{
    estimator->drain_rate = drain_rate > 0 ? drain_rate : 0;
    for (int i = 0; i < BATTERY_POD_COUNT; i++)
        session_reset(&estimator->pods[i]);
}

bool battery_estimator_update(BatteryEstimator *estimator, BatteryPod pod,
                              int64_t now_sec, int level, bool discharging)
{
    DischargeSession *session = &estimator->pods[pod];

    if (!discharging || level < 0)
        return session_finish(estimator, session);

    if (session->active && level > session->last_level) {
        /* Charged in between: this is a new session */
        bool learned = session_finish(estimator, session);
        session->active = true;
        session->last_level = level;
        return learned;
    }

    if (!session->active) {
        session_reset(session);
        session->active = true;
        session->last_level = level;
        return false;
    }

    if (level < session->last_level) {
        /* Time counts from the first drop: the starting level may have
         * been reached long before the session began */
        if (session->first_change_sec < 0) {
            session->first_change_sec = now_sec;
            session->first_change_level = level;
        }

        double t = (now_sec - session->first_change_sec) / 3600.0;
        session->n++;
        session->sum_t += t;
        session->sum_l += level;
        session->sum_tt += t * t;
        session->sum_tl += t * level;
        session->last_change_sec = now_sec;
        session->last_level = level;
    }

    return false;
}

bool battery_estimator_finish(BatteryEstimator *estimator)
{
    bool learned = false;
    for (int i = 0; i < BATTERY_POD_COUNT; i++)
        learned |= session_finish(estimator, &estimator->pods[i]);
    return learned;
}

int battery_estimator_minutes_left(const BatteryEstimator *estimator, int level)
{
    if (level < 0)
        return -1;

    double rate = estimator->drain_rate;

    /* Nothing learned yet: fall back on a running session if long enough */
    if (rate <= 0) {
        for (int i = 0; i < BATTERY_POD_COUNT; i++) {
            double session_value;
            if (session_rate(&estimator->pods[i], &session_value) && session_value > rate)
                rate = session_value;  /* The faster pod runs out first */
        }
        if (rate <= 0)
            return -1;
    }

    return (int)lround(level / rate * 60.0);
}
