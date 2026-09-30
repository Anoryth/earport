/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Research log: when enabled (research_log=true in daemon.conf, not shown
 * in the preferences), every packet the parser doesn't know and every
 * setting we don't expose is logged with its raw bytes, so days of normal
 * use can be compared with what happened (sleep, calls, gestures...).
 */

#ifndef RESEARCH_H
#define RESEARCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aap_protocol.h"

void research_set_enabled(bool enabled);

/* A packet received from the AirPods, with what the parser made of it */
void research_observe(const uint8_t *data, size_t len, AapParseResult result,
                      const AapParsedPacket *packet);

#endif /* RESEARCH_H */
