/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "audio_route.h"

#include <gio/gio.h>
#include <stdbool.h>

/* $1: the address as in PipeWire's sink names ("AC_07_75_F0_31_02"),
 * $2: "streams", "output", "any" or "profile" (see audio_route.h). Exit
 * status: 0 restarted, 2 no AirPods output (not in A2DP for "profile"), 3
 * nothing playing to it, 4 no muted output (pactl failing for "profile").
 * The streams move to the muted output, so nothing plays on the speakers
 * meanwhile. */
static const char RESTART_SCRIPT[] =
    "if [ \"$2\" = profile ]; then\n"
    "  card=bluez_card.$1\n"
    "  profile=$(pactl list cards | awk -v c=\"$card\" "
    "'$1 == \"Name:\" { on = ($2 == c) } on && $1 == \"Active\" { print $3; exit }')\n"
    "  case \"$profile\" in a2dp*) ;; *) exit 2;; esac\n"
    "  pactl set-card-profile \"$card\" off && sleep 0.5 && "
    "pactl set-card-profile \"$card\" \"$profile\" || exit 4\n"
    "  exit 0\n"
    "fi\n"
    "sink=$(pactl list sinks short | awk -v p=\"bluez_output.$1.\" 'index($2, p) == 1 { print $1; exit }')\n"
    "[ -n \"$sink\" ] || exit 2\n"
    "restart_output() {\n"
    "  pactl suspend-sink \"$sink\" 1 && sleep 0.5 && pactl suspend-sink \"$sink\" 0 || exit 4\n"
    "  exit 0\n"
    "}\n"
    "[ \"$2\" = output ] && restart_output\n"
    "inputs=$(pactl list sink-inputs | awk -v s=\"$sink\" '"
    "/^Sink Input #/ { id = substr($3, 2) } "
    "/^\\tSink: / { on = ($2 == s) } "
    "/^\\tCorked: no/ { if (on) print id }')\n"
    "if [ -z \"$inputs\" ]; then\n"
    "  [ \"$2\" = any ] && restart_output\n"
    "  exit 3\n"
    "fi\n"
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
    bool profile = GPOINTER_TO_INT(user_data);
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
        g_message(profile ? "Restarted the AirPods' A2DP profile"
                          : "Restarted the sound playing to the AirPods");
        break;
    case 2:
    case 3:
        g_message(profile ? "Nothing to restart: the AirPods are not in A2DP"
                          : "Nothing playing to the AirPods to restart");
        break;
    default:
        g_message("Could not restart the AirPods audio (pactl missing or failing)");
        break;
    }
}

void audio_route_restart(const char *airpods_address, const char *mode)
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
                                                    "earport-audio-route", name, mode, NULL);
    g_object_unref(launcher);
    g_free(name);

    if (proc == NULL) {
        g_warning("Could not restart the AirPods audio: %s", error->message);
        g_error_free(error);
        return;
    }

    running = true;
    g_message("Restarting the sound to the AirPods (%s)", mode);
    g_subprocess_wait_async(proc, NULL, on_restart_done,
                            GINT_TO_POINTER(g_strcmp0(mode, "profile") == 0));
    g_object_unref(proc);
}
