/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Logs what BlueZ says about the AirPods' links, to understand cuts: why
 * they disconnected (BlueZ's reason) and the state of this computer's audio
 * stream to them (playing, idle...). Tells who closed the link.
 */

#ifndef LINK_TRACE_H
#define LINK_TRACE_H

#include <stdbool.h>

typedef struct LinkTrace LinkTrace;

/* The link to the AirPods closed; left: they closed it or stopped
 * answering (they do both when another device takes them), rather than
 * this computer closing it */
typedef void (*LinkTraceClosedCallback)(bool left, void *user_data);

LinkTrace *link_trace_new(void);
void link_trace_free(LinkTrace *trace);

/* The AirPods followed: their BlueZ object path, NULL for none */
void link_trace_set_device(LinkTrace *trace, const char *device_path);

/* Needs BlueZ 5.73 or later, which tells why a link closed */
void link_trace_set_closed_callback(LinkTrace *trace, LinkTraceClosedCallback callback,
                                    void *user_data);

#endif /* LINK_TRACE_H */
