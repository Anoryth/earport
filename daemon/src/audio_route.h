/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Gets this computer's sound back to the AirPods after another device had
 * them. While an Apple device plays, the AirPods stop this computer's
 * stream and PipeWire leaves the AirPods output in error; a stream still
 * playing stays silent until it is recreated. Moving the streams to a muted
 * output and straight back restarts it: the AirPods then switch back to
 * this computer, as when a Mac starts playing.
 *
 * Done with pactl (PipeWire's PulseAudio tools), so that the service needs
 * nothing more than GLib; without it, the sound just doesn't come back on
 * its own.
 */

#ifndef AUDIO_ROUTE_H
#define AUDIO_ROUTE_H

/* Restart the streams playing to the AirPods with this address, if any
 * (asynchronous, at most one at a time) */
void audio_route_restart(const char *airpods_address);

#endif /* AUDIO_ROUTE_H */
