/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * HD microphone: while the AirPods are connected and the option is on, a
 * PipeWire source "<their name> (HD)". Their AAC-ELD stream (64 kHz,
 * sent over AAP alongside A2DP, unlike the HFP microphone that degrades
 * playback) runs only while an application records from it: the links
 * leaving the source are counted in the PipeWire registry, the stream's own
 * state never tells when recording stops.
 *
 * Decoded with FFmpeg's AAC decoder, the system's (EarPort ships none).
 * Built only with libavcodec and libpipewire (meson option hd_mic);
 * otherwise hd_mic_new() returns NULL and the rest accepts NULL.
 */

#ifndef HD_MIC_H
#define HD_MIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aap_protocol.h"

typedef struct {
    /* Send a packet to the AirPods */
    void (*send)(const uint8_t *data, size_t len, void *user_data);
    /* The AirPods' stream started (an application records) or stopped */
    void (*capture_changed)(bool capturing, void *user_data);
} HdMicCallbacks;

typedef struct HdMic HdMic;

/* NULL when built without it, or when the AAC decoder cannot be opened */
HdMic *hd_mic_new(const HdMicCallbacks *callbacks, void *user_data);
void hd_mic_free(HdMic *mic);

void hd_mic_set_enabled(HdMic *mic, bool enabled);

/* The AirPods' name: the source is called "<name> (HD)" */
void hd_mic_set_name(HdMic *mic, const char *name);

/* The AAP link to the AirPods opened or closed */
void hd_mic_set_connected(HdMic *mic, bool connected);

/* A microphone audio packet from the AirPods */
void hd_mic_audio(HdMic *mic, const AapMicAudio *audio);

/* The gain for a frame with this peak (0-1) after one with 'gain' (exposed
 * for the tests; only built with the HD microphone) */
float hd_mic_next_gain(float gain, float peak);

#endif /* HD_MIC_H */
