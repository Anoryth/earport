/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "audio_route.h"

#include <gio/gio.h>
#include <stdbool.h>

/* $1: the address as in PipeWire's sink names ("AC_07_75_F0_31_02").
 * Exit status: 0 restarted, 2 no AirPods output, 3 nothing playing to it,
 * 4 no muted output. The streams move to the muted output, so nothing
 * plays on the speakers meanwhile. */
static const char RESTART_SCRIPT[] =
    "sink=$(pactl list sinks short | awk -v p=\"bluez_output.$1.\" 'index($2, p) == 1 { print $1; exit }')\n"
    "[ -n \"$sink\" ] || exit 2\n"
    "inputs=$(pactl list sink-inputs | awk -v s=\"$sink\" '"
    "/^Sink Input #/ { id = substr($3, 2) } "
    "/^\\tSink: / { on = ($2 == s) } "
    "/^\\tCorked: no/ { if (on) print id }')\n"
    "[ -n \"$inputs\" ] || exit 3\n"
    "module=$(pactl load-module module-null-sink sink_name=earport_handoff "
    "sink_properties=device.description=EarPort) || exit 4\n"
    "sleep 0.3\n"
    "for i in $inputs; do pactl move-sink-input \"$i\" earport_handoff; done\n"
    "sleep 0.3\n"
    "for i in $inputs; do pactl move-sink-input \"$i\" \"$sink\"; done\n"
    "sleep 0.5\n"
    "pactl unload-module \"$module\"\n";

static bool running;

static void on_restart_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GSubprocess *proc = G_SUBPROCESS(source);

    running = false;
    if (!g_subprocess_wait_finish(proc, res, &error)) {
        g_warning("Could not restart the AirPods audio: %s", error->message);
        g_error_free(error);
        return;
    }

    switch (g_subprocess_get_exit_status(proc)) {
    case 0:
        g_message("Restarted the sound playing to the AirPods");
        break;
    case 2:
    case 3:
        g_debug("Nothing playing to the AirPods to restart");
        break;
    default:
        g_message("Could not restart the AirPods audio (pactl missing or failing)");
        break;
    }
}

void audio_route_restart(const char *airpods_address)
{
    if (running || airpods_address == NULL)
        return;

    char *name = g_strdup(airpods_address);
    g_strdelimit(name, ":", '_');

    GError *error = NULL;
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                              G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    /* pactl output is parsed: untranslated */
    g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
    GSubprocess *proc = g_subprocess_launcher_spawn(launcher, &error, "sh", "-c", RESTART_SCRIPT,
                                                    "earport-audio-route", name, NULL);
    g_object_unref(launcher);
    g_free(name);

    if (proc == NULL) {
        g_warning("Could not restart the AirPods audio: %s", error->message);
        g_error_free(error);
        return;
    }

    running = true;
    g_subprocess_wait_async(proc, NULL, on_restart_done, NULL);
    g_object_unref(proc);
}
