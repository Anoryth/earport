/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Waiting in tests for something that must happen: up to 2 s, so that a
 * loaded machine (package builders) only makes them slower. Fixed delays
 * stay for what must not happen.
 */

#ifndef TEST_WAIT_H
#define TEST_WAIT_H

#include <glib.h>

#define WAIT_FOR(condition)                                                  \
    do {                                                                     \
        gint64 wait_end_ = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;      \
        while (!(condition) && g_get_monotonic_time() < wait_end_)           \
            g_main_context_iteration(NULL, FALSE);                           \
    } while (0)

#endif /* TEST_WAIT_H */
