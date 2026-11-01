/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * How long the user has left this computer alone: GNOME's idle monitor,
 * else the idle hint of the user's graphical session in logind (other
 * desktops).
 */

#ifndef IDLE_H
#define IDLE_H

#include <stdbool.h>
#include <stdint.h>

/* Milliseconds since the last keyboard or mouse input, -1 if unknown */
int64_t idle_ms(void);

/* Idle time from logind's session idle hint (monotonic microseconds), -1
 * when no one ever set the hint */
int64_t idle_from_logind(bool idle_hint, uint64_t idle_since_us, uint64_t now_us);

#endif /* IDLE_H */
