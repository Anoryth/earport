/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * The sound restart script, run for real against a fake pactl (canned sink
 * and stream lists, actions logged) and a sleep that doesn't wait, both
 * first in PATH
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "audio_route.h"

#define AIRPODS "00:00:00:00:00:0A"

static char *dir;
static GPtrArray *messages;

static const char FAKE_PACTL[] =
    "#!/bin/sh\n"
    "case \"$1\" in\n"
    "list) case \"$2\" in sinks) cat \"$FAKE_PACTL_DIR/sinks\";;\n"
    "  cards) cat \"$FAKE_PACTL_DIR/cards\";; *) cat \"$FAKE_PACTL_DIR/inputs\";; esac;;\n"
    "load-module) echo \"$*\" >> \"$FAKE_PACTL_DIR/log\"\n"
    "  [ -e \"$FAKE_PACTL_DIR/fail-load\" ] && exit 1; echo 42;;\n"
    "*) echo \"$*\" >> \"$FAKE_PACTL_DIR/log\";;\n"
    "esac\n";

/* pactl list sinks short: the AirPods (7), another pair (8), speakers */
static const char SINKS[] =
    "1\talsa_output.pci-0000_00_1f.3.analog-stereo\tPipeWire\ts32le 2ch 48000Hz\tIDLE\n"
    "7\tbluez_output.00_00_00_00_00_0A.1\tPipeWire\ts16le 2ch 48000Hz\tRUNNING\n"
    "8\tbluez_output.00_00_00_00_00_0B.1\tPipeWire\ts16le 2ch 48000Hz\tRUNNING\n";

/* pactl list sink-inputs: playing (20) and paused (21) on the AirPods,
 * playing on the speakers (22) and on a sink whose id starts the same (23) */
static const char INPUTS[] =
    "Sink Input #20\n\tDriver: PipeWire\n\tSink: 7\n\tCorked: no\n\n"
    "Sink Input #21\n\tDriver: PipeWire\n\tSink: 7\n\tCorked: yes\n\n"
    "Sink Input #22\n\tDriver: PipeWire\n\tSink: 1\n\tCorked: no\n\n"
    "Sink Input #23\n\tDriver: PipeWire\n\tSink: 77\n\tCorked: no\n";

/* pactl list cards: another pair in HFP, then the AirPods in A2DP (AAC) */
static const char CARDS_A2DP[] =
    "Card #50\n\tName: bluez_card.00_00_00_00_00_0B\n\tActive Profile: headset-head-unit\n\n"
    "Card #51\n\tName: bluez_card.00_00_00_00_00_0A\n\tProfiles:\n"
    "\t\ta2dp-sink: High Fidelity Playback (A2DP Sink, codec AAC)\n"
    "\tActive Profile: a2dp-sink\n";

static const char CARDS_HFP[] =
    "Card #51\n\tName: bluez_card.00_00_00_00_00_0A\n\tActive Profile: headset-head-unit\n";

static const char PAUSED_ONLY[] =
    "Sink Input #21\n\tDriver: PipeWire\n\tSink: 7\n\tCorked: yes\n\n"
    "Sink Input #22\n\tDriver: PipeWire\n\tSink: 1\n\tCorked: no\n";

static void log_handler(const gchar *domain, GLogLevelFlags level, const gchar *message,
                        gpointer user_data)
{
    (void)user_data;
    g_ptr_array_add(messages, g_strdup(message));
    g_log_default_handler(domain, level, message, NULL);
}

static void write_file(const char *name, const char *contents)
{
    g_autofree char *path = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(path, contents, -1, NULL));
}

static void setup(const char *sinks, const char *inputs, bool fail_load)
{
    write_file("sinks", sinks);
    write_file("inputs", inputs);
    write_file("cards", CARDS_A2DP);
    write_file("log", "");
    g_autofree char *fail = g_build_filename(dir, "fail-load", NULL);
    if (fail_load)
        write_file("fail-load", "");
    else
        g_unlink(fail);
    g_ptr_array_set_size(messages, 0);
}

static bool logged(const char *prefix)
{
    for (guint i = 0; i < messages->len; i++)
        if (g_str_has_prefix(messages->pdata[i], prefix))
            return true;
    return false;
}

/* Runs the restart to its end; returns the pactl actions */
static char *restart(const char *mode)
{
    audio_route_restart(AIRPODS, mode);
    gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (!logged("Restarted") && !logged("Nothing") && !logged("Could not") &&
           g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);

    g_autofree char *path = g_build_filename(dir, "log", NULL);
    char *actions = NULL;
    g_assert_true(g_file_get_contents(path, &actions, NULL, NULL));
    return actions;
}

/* The streams playing to the AirPods go through a muted output and back,
 * the others stay */
static void test_streams(void)
{
    setup(SINKS, INPUTS, false);
    g_autofree char *actions = restart("streams");
    g_assert_true(logged("Restarted the sound playing to the AirPods"));
    g_assert_cmpstr(actions, ==,
                    "load-module module-null-sink sink_name=earport_handoff "
                    "sink_properties=device.description=EarPort\n"
                    "move-sink-input 20 earport_handoff\n"
                    "move-sink-input 20 7\n"
                    "unload-module 42\n");
}

/* Nothing to restart: no AirPods output, or nothing playing to it */
static void test_nothing_playing(void)
{
    setup("1\talsa_output.pci-0000_00_1f.3.analog-stereo\tPipeWire\ts32le 2ch 48000Hz\tIDLE\n",
          INPUTS, false);
    g_autofree char *no_output = restart("any");
    g_assert_true(logged("Nothing playing to the AirPods"));
    g_assert_cmpstr(no_output, ==, "");

    setup(SINKS, PAUSED_ONLY, false);
    g_autofree char *paused = restart("streams");
    g_assert_true(logged("Nothing playing to the AirPods"));
    g_assert_cmpstr(paused, ==, "");
}

/* The output itself: asked for, or when no stream plays ("any") */
static void test_output(void)
{
    setup(SINKS, INPUTS, false);
    g_autofree char *asked = restart("output");
    g_assert_true(logged("Restarted"));
    g_assert_cmpstr(asked, ==, "suspend-sink 7 1\nsuspend-sink 7 0\n");

    setup(SINKS, PAUSED_ONLY, false);
    g_autofree char *nothing_playing = restart("any");
    g_assert_true(logged("Restarted"));
    g_assert_cmpstr(nothing_playing, ==, "suspend-sink 7 1\nsuspend-sink 7 0\n");
}

/* Their A2DP profile off and back on, the same; not when they are in HFP */
static void test_profile(void)
{
    setup(SINKS, INPUTS, false);
    g_autofree char *actions = restart("profile");
    g_assert_true(logged("Restarted the AirPods' A2DP profile"));
    g_assert_cmpstr(actions, ==, "set-card-profile bluez_card.00_00_00_00_00_0A off\n"
                                 "set-card-profile bluez_card.00_00_00_00_00_0A a2dp-sink\n");

    setup(SINKS, INPUTS, false);
    write_file("cards", CARDS_HFP);
    g_autofree char *in_hfp = restart("profile");
    g_assert_true(logged("Nothing to restart: the AirPods are not in A2DP"));
    g_assert_cmpstr(in_hfp, ==, "");
}

/* No muted output to go through: given up, streams left alone */
static void test_load_fails(void)
{
    setup(SINKS, INPUTS, true);
    g_autofree char *actions = restart("streams");
    g_assert_true(logged("Could not restart the AirPods audio (pactl missing or failing)"));
    g_assert_null(strstr(actions, "move-sink-input"));
}

/* One at a time; no address, nothing */
static void test_one_at_a_time(void)
{
    setup(SINKS, INPUTS, false);
    audio_route_restart(NULL, "streams");
    g_assert_cmpuint(messages->len, ==, 0);

    audio_route_restart(AIRPODS, "streams");
    g_autofree char *actions = restart("output");
    g_assert_null(strstr(actions, "suspend-sink"));
    guint starts = 0;
    for (guint i = 0; i < messages->len; i++)
        if (g_str_has_prefix(messages->pdata[i], "Restarting the sound"))
            starts++;
    g_assert_cmpuint(starts, ==, 1);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    messages = g_ptr_array_new_with_free_func(g_free);
    g_log_set_handler(NULL, G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_WARNING, log_handler, NULL);

    dir = g_dir_make_tmp("earport-audio-route-XXXXXX", NULL);
    g_assert_nonnull(dir);
    write_file("pactl", FAKE_PACTL);
    write_file("sleep", "#!/bin/sh\n");
    g_autofree char *pactl = g_build_filename(dir, "pactl", NULL);
    g_autofree char *sleep = g_build_filename(dir, "sleep", NULL);
    g_assert_cmpint(g_chmod(pactl, 0755), ==, 0);
    g_assert_cmpint(g_chmod(sleep, 0755), ==, 0);
    g_autofree char *path = g_strdup_printf("%s:%s", dir, g_getenv("PATH"));
    g_setenv("PATH", path, TRUE);
    g_setenv("FAKE_PACTL_DIR", dir, TRUE);

    g_test_add_func("/audio-route/streams", test_streams);
    g_test_add_func("/audio-route/nothing-playing", test_nothing_playing);
    g_test_add_func("/audio-route/output", test_output);
    g_test_add_func("/audio-route/profile", test_profile);
    g_test_add_func("/audio-route/load-fails", test_load_fails);
    g_test_add_func("/audio-route/one-at-a-time", test_one_at_a_time);
    int ret = g_test_run();

    const char *files[] = { "pactl", "sleep", "sinks", "inputs", "log", "fail-load" };
    for (gsize i = 0; i < G_N_ELEMENTS(files); i++) {
        g_autofree char *file = g_build_filename(dir, files[i], NULL);
        g_unlink(file);
    }
    g_rmdir(dir);
    g_free(dir);
    g_ptr_array_unref(messages);
    return ret;
}
