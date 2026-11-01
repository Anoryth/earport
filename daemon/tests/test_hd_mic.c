/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * HD microphone: the automatic gain, and the system's FFmpeg opening the
 * AAC-ELD decoder (a distribution package fails here, not on users). No
 * PipeWire: package builders don't run it.
 */

#include <glib.h>

#include "hd_mic.h"

/* Soft speech gets the full +24 dB; a frame that would clip lowers the gain
 * at once, never under x1; it comes back slowly */
static void test_gain(void)
{
    g_assert_cmpfloat(hd_mic_next_gain(16, 0.005f), ==, 16);
    g_assert_cmpfloat(hd_mic_next_gain(16, 0), ==, 16);

    float loud = hd_mic_next_gain(16, 0.25f);
    g_assert_cmpfloat_with_epsilon(loud, 2, 1e-6);
    g_assert_cmpfloat(0.25f * loud, <=, 1);
    g_assert_cmpfloat(hd_mic_next_gain(16, 0.9f), ==, 1);

    float gain = loud;
    int frames = 0;
    while (gain < 16 && frames < 1000) {
        gain = hd_mic_next_gain(gain, 0.005f);
        frames++;
    }
    /* 7.5 ms frames: from x2 to x16 in about 1.5 s */
    g_assert_cmpint(frames, >, 150);
    g_assert_cmpint(frames, <, 250);
}

static void test_decoder(void)
{
    g_assert_true(hd_mic_can_decode());
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/hd-mic/gain", test_gain);
    g_test_add_func("/hd-mic/decoder", test_decoder);
    return g_test_run();
}
