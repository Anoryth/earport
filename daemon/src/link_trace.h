/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Logs what BlueZ says about the AirPods' links, to understand cuts: why
 * they disconnected (BlueZ's reason) and the state of this computer's audio
 * stream to them (playing, idle...).
 */

#ifndef LINK_TRACE_H
#define LINK_TRACE_H

typedef struct LinkTrace LinkTrace;

LinkTrace *link_trace_new(void);
void link_trace_free(LinkTrace *trace);

/* The AirPods followed: their BlueZ object path, NULL for none */
void link_trace_set_device(LinkTrace *trace, const char *device_path);

#endif /* LINK_TRACE_H */
