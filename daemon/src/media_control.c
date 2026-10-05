/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Media control via MPRIS D-Bus interface
 */

#include "media_control.h"
#include <gio/gio.h>
#include <string.h>

#define MPRIS_DBUS_NAME_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_DBUS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"

/* Players are called asynchronously, and a frozen one (e.g. a stuck
 * browser) must not hold the others back for long */
#define PLAYER_CALL_TIMEOUT_MS 2000

struct MediaControl {
    GDBusConnection *connection;
    EarPauseMode ear_pause_mode;

    /* Track which players we paused */
    GList *paused_players;     /* List of player names (strings) that we paused */
    GList *handoff_players;    /* Paused because another device took the AirPods */
    guint resume_count;        /* Resumes so far, to spot pauses that land after one */
    GCancellable *cancellable; /* Pending player calls, cancelled on free */

    /* Previous ear state for edge detection */
    bool prev_left_in_ear;
    bool prev_right_in_ear;
    bool prev_state_valid;

    /* Any MPRIS player starting to play */
    guint playback_subscription_id;
    MediaPlaybackStartedCallback playback_started_callback;
    MediaPausedForSleepCallback paused_for_sleep_callback;
    void *paused_for_sleep_user_data;
    void *playback_started_user_data;
};

static void on_player_properties_changed(GDBusConnection *connection G_GNUC_UNUSED,
                                         const gchar *sender_name G_GNUC_UNUSED,
                                         const gchar *object_path G_GNUC_UNUSED,
                                         const gchar *interface_name G_GNUC_UNUSED,
                                         const gchar *signal_name G_GNUC_UNUSED,
                                         GVariant *parameters,
                                         gpointer user_data)
{
    MediaControl *mc = user_data;
    GVariant *changed = NULL;
    const gchar *status = NULL;

    if (mc->playback_started_callback == NULL)
        return;

    g_variant_get(parameters, "(&s@a{sv}@as)", NULL, &changed, NULL);
    if (g_variant_lookup(changed, "PlaybackStatus", "&s", &status) &&
        g_strcmp0(status, "Playing") == 0)
        mc->playback_started_callback(mc->playback_started_user_data);
    g_variant_unref(changed);
}

/* ============================================================================
 * Helper functions
 * ========================================================================== */

/* A pause in progress on one player */
typedef struct {
    MediaControl *mc;
    char *player;
    guint resume_count;        /* mc->resume_count when the pause started */
    bool for_sleep;            /* Not resumed with the ears; rewound */
    bool for_handoff;          /* Not resumed with the ears either */
    int rewind_seconds;
} PauseOp;

static void pause_op_free(PauseOp *op)
{
    g_free(op->player);
    g_free(op);
}

/* Whether a call failed because the MediaControl is gone (op/mc unusable) */
static bool call_cancelled(GError *error)
{
    return g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void player_call(MediaControl *mc, const char *player, const char *method,
                        GAsyncReadyCallback callback, gpointer user_data)
{
    g_dbus_connection_call(mc->connection, player, MPRIS_DBUS_PATH, MPRIS_PLAYER_INTERFACE,
                           method, NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                           PLAYER_CALL_TIMEOUT_MS, mc->cancellable, callback, user_data);
}

static void on_play_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    char *player = user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (reply != NULL) {
        g_message("Resumed media player: %s", player);
        g_variant_unref(reply);
    } else {
        if (!call_cancelled(error))
            g_debug("Failed to play %s: %s", player, error->message);
        g_error_free(error);
    }
    g_free(player);
}

static void player_play(MediaControl *mc, const char *player)
{
    player_call(mc, player, "Play", on_play_done, g_strdup(player));
}

static void on_seek_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    char *player = user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (reply != NULL) {
        g_message("Rewound media player: %s", player);
        g_variant_unref(reply);
    } else {
        if (!call_cancelled(error))
            g_message("Could not rewind %s: %s", player, error->message);
        g_error_free(error);
    }
    g_free(player);
}

static void on_pause_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    PauseOp *op = user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (reply == NULL) {
        if (!call_cancelled(error))
            g_debug("Failed to pause %s: %s", op->player, error->message);
        g_error_free(error);
        pause_op_free(op);
        return;
    }
    g_variant_unref(reply);

    MediaControl *mc = op->mc;
    g_message("Paused media player: %s", op->player);

    if (op->for_handoff) {
        if (g_list_find_custom(mc->handoff_players, op->player, (GCompareFunc)g_strcmp0) == NULL)
            mc->handoff_players = g_list_append(mc->handoff_players, g_strdup(op->player));
        pause_op_free(op);
        return;
    }

    if (op->for_sleep) {
        /* Replay what was missed while falling asleep (offset in µs) */
        if (op->rewind_seconds > 0) {
            g_dbus_connection_call(mc->connection, op->player, MPRIS_DBUS_PATH,
                                   MPRIS_PLAYER_INTERFACE, "Seek",
                                   g_variant_new("(x)", -(gint64)op->rewind_seconds * G_USEC_PER_SEC),
                                   NULL, G_DBUS_CALL_FLAGS_NONE, PLAYER_CALL_TIMEOUT_MS,
                                   mc->cancellable, on_seek_done, g_strdup(op->player));
        }
        if (mc->paused_for_sleep_callback != NULL)
            mc->paused_for_sleep_callback(op->player, mc->paused_for_sleep_user_data);
        pause_op_free(op);
        return;
    }

    if (mc->resume_count != op->resume_count) {
        /* The pods went back in while the player was being paused */
        player_play(mc, op->player);
    } else if (g_list_find_custom(mc->paused_players, op->player, (GCompareFunc)g_strcmp0) == NULL) {
        mc->paused_players = g_list_append(mc->paused_players, g_strdup(op->player));
    }
    pause_op_free(op);
}

static void on_status_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    PauseOp *op = user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (reply == NULL) {
        if (!call_cancelled(error))
            g_debug("Failed to get playback status from %s: %s", op->player, error->message);
        g_error_free(error);
        pause_op_free(op);
        return;
    }

    GVariant *status = NULL;
    g_variant_get(reply, "(v)", &status);
    bool playing = g_strcmp0(g_variant_get_string(status, NULL), "Playing") == 0;
    g_variant_unref(status);
    g_variant_unref(reply);

    /* Only the players actually playing are paused, and later resumed */
    if (playing && (op->for_sleep || op->for_handoff ||
                    op->mc->resume_count == op->resume_count))
        player_call(op->mc, op->player, "Pause", on_pause_done, op);
    else
        pause_op_free(op);
}

/* What to do with the players found playing */
typedef struct {
    MediaControl *mc;
    bool for_sleep;
    bool for_handoff;
    int rewind_seconds;
} PauseRequest;

static void on_list_names_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    PauseRequest *request = user_data;
    MediaControl *mc = request->mc;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (reply == NULL) {
        if (!call_cancelled(error))
            g_warning("Failed to list D-Bus names: %s", error->message);
        g_error_free(error);
        g_free(request);
        return;
    }

    GVariantIter *iter;
    const gchar *name;
    g_variant_get(reply, "(as)", &iter);
    while (g_variant_iter_loop(iter, "&s", &name)) {
        if (!g_str_has_prefix(name, MPRIS_DBUS_NAME_PREFIX))
            continue;

        PauseOp *op = g_new0(PauseOp, 1);
        op->mc = mc;
        op->player = g_strdup(name);
        op->resume_count = mc->resume_count;
        op->for_sleep = request->for_sleep;
        op->for_handoff = request->for_handoff;
        op->rewind_seconds = request->rewind_seconds;
        g_dbus_connection_call(mc->connection, name, MPRIS_DBUS_PATH, DBUS_PROPERTIES_INTERFACE,
                               "Get", g_variant_new("(ss)", MPRIS_PLAYER_INTERFACE, "PlaybackStatus"),
                               G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE,
                               PLAYER_CALL_TIMEOUT_MS, mc->cancellable, on_status_done, op);
    }
    g_variant_iter_free(iter);
    g_variant_unref(reply);
    g_free(request);
}

static void pause_playing(MediaControl *mc, bool for_sleep, int rewind_seconds)
{
    PauseRequest *request = g_new0(PauseRequest, 1);
    request->mc = mc;
    request->for_sleep = for_sleep;
    request->rewind_seconds = rewind_seconds;

    /* For each player: playing? -> pause */
    g_dbus_connection_call(mc->connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "ListNames", NULL, G_VARIANT_TYPE("(as)"),
                           G_DBUS_CALL_FLAGS_NONE, PLAYER_CALL_TIMEOUT_MS, mc->cancellable,
                           on_list_names_done, request);
}

/* ============================================================================
 * Public API
 * ========================================================================== */

MediaControl *media_control_new(void)
{
    MediaControl *mc = g_new0(MediaControl, 1);
    GError *error = NULL;

    mc->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error != NULL) {
        g_warning("Failed to connect to session bus: %s", error->message);
        g_error_free(error);
        g_free(mc);
        return NULL;
    }

    mc->ear_pause_mode = EAR_PAUSE_ONE_OUT;  /* Default: pause when one pod is removed */
    mc->cancellable = g_cancellable_new();
    mc->paused_players = NULL;
    mc->prev_state_valid = false;

    mc->playback_subscription_id = g_dbus_connection_signal_subscribe(
        mc->connection,
        NULL,
        DBUS_PROPERTIES_INTERFACE,
        "PropertiesChanged",
        MPRIS_DBUS_PATH,
        MPRIS_PLAYER_INTERFACE,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_player_properties_changed,
        mc,
        NULL);

    return mc;
}

void media_control_free(MediaControl *mc)
{
    if (mc == NULL) {
        return;
    }

    /* Pending calls must not reach the freed MediaControl */
    g_cancellable_cancel(mc->cancellable);
    g_object_unref(mc->cancellable);

    /* Free paused players list */
    g_list_free_full(mc->paused_players, g_free);
    g_list_free_full(mc->handoff_players, g_free);

    if (mc->connection) {
        if (mc->playback_subscription_id > 0)
            g_dbus_connection_signal_unsubscribe(mc->connection, mc->playback_subscription_id);
        g_object_unref(mc->connection);
    }

    g_free(mc);
}

void media_control_set_playback_started_callback(MediaControl *mc,
                                                 MediaPlaybackStartedCallback callback,
                                                 void *user_data)
{
    if (mc == NULL)
        return;
    mc->playback_started_callback = callback;
    mc->playback_started_user_data = user_data;
}

void media_control_set_ear_pause_mode(MediaControl *mc, EarPauseMode mode)
{
    if (mc != NULL) {
        mc->ear_pause_mode = mode;
        g_message("Ear pause mode set to: %d", mode);
    }
}

EarPauseMode media_control_get_ear_pause_mode(MediaControl *mc)
{
    return mc ? mc->ear_pause_mode : EAR_PAUSE_DISABLED;
}

void media_control_on_ear_detection_changed(MediaControl *mc,
                                            bool left_in_ear,
                                            bool right_in_ear)
{
    if (mc == NULL || mc->ear_pause_mode == EAR_PAUSE_DISABLED) {
        return;
    }

    bool should_pause = false;
    bool should_resume = false;

    /* Calculate current state based on mode */
    bool pods_out = false;
    bool pods_in = false;

    switch (mc->ear_pause_mode) {
    case EAR_PAUSE_ONE_OUT:
        /* Pause if at least one pod is removed */
        pods_out = !left_in_ear || !right_in_ear;
        pods_in = left_in_ear && right_in_ear;
        break;

    case EAR_PAUSE_BOTH_OUT:
        /* Pause only if both pods are removed */
        pods_out = !left_in_ear && !right_in_ear;
        pods_in = left_in_ear || right_in_ear;
        break;

    default:
        return;
    }

    /* Detect transitions (edge detection) */
    if (mc->prev_state_valid) {
        bool prev_pods_out = false;

        switch (mc->ear_pause_mode) {
        case EAR_PAUSE_ONE_OUT:
            prev_pods_out = !mc->prev_left_in_ear || !mc->prev_right_in_ear;
            break;
        case EAR_PAUSE_BOTH_OUT:
            prev_pods_out = !mc->prev_left_in_ear && !mc->prev_right_in_ear;
            break;
        default:
            break;
        }

        /* Transition from in-ear to out-of-ear: pause */
        if (!prev_pods_out && pods_out) {
            should_pause = true;
        }

        /* Transition from out-of-ear to in-ear: resume */
        if (prev_pods_out && pods_in) {
            should_resume = true;
        }
    }

    /* Update previous state */
    mc->prev_left_in_ear = left_in_ear;
    mc->prev_right_in_ear = right_in_ear;
    mc->prev_state_valid = true;

    /* Execute actions */
    if (should_pause) {
        g_message("Ear detection: pods removed, pausing media");
        media_control_pause_all(mc);
    } else if (should_resume) {
        g_message("Ear detection: pods inserted, resuming media");
        media_control_resume(mc);
    }
}

void media_control_pause_all(MediaControl *mc)
{
    if (mc == NULL || mc->connection == NULL) {
        return;
    }

    /* Forget the players paused last time: this is a new pause */
    g_list_free_full(mc->paused_players, g_free);
    mc->paused_players = NULL;

    /* Then pause the players playing, and remember them */
    pause_playing(mc, false, 0);
}

void media_control_set_paused_for_sleep_callback(MediaControl *mc,
                                                 MediaPausedForSleepCallback callback,
                                                 void *user_data)
{
    if (mc == NULL)
        return;
    mc->paused_for_sleep_callback = callback;
    mc->paused_for_sleep_user_data = user_data;
}

void media_control_pause_for_handoff(MediaControl *mc)
{
    if (mc == NULL || mc->connection == NULL)
        return;

    /* A new handoff: the players to resume are the ones paused now */
    g_list_free_full(mc->handoff_players, g_free);
    mc->handoff_players = NULL;

    PauseRequest *request = g_new0(PauseRequest, 1);
    request->mc = mc;
    request->for_handoff = true;
    g_dbus_connection_call(mc->connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "ListNames", NULL, G_VARIANT_TYPE("(as)"),
                           G_DBUS_CALL_FLAGS_NONE, PLAYER_CALL_TIMEOUT_MS, mc->cancellable,
                           on_list_names_done, request);
}

void media_control_resume_handoff(MediaControl *mc)
{
    if (mc == NULL || mc->connection == NULL)
        return;

    for (GList *l = mc->handoff_players; l != NULL; l = l->next) {
        g_message("Resuming media player: %s", (const char *)l->data);
        player_play(mc, l->data);
    }
    g_list_free_full(mc->handoff_players, g_free);
    mc->handoff_players = NULL;
}

void media_control_pause_for_sleep(MediaControl *mc, int rewind_seconds)
{
    if (mc == NULL || mc->connection == NULL)
        return;
    pause_playing(mc, true, rewind_seconds);
}

void media_control_resume(MediaControl *mc)
{
    if (mc == NULL || mc->connection == NULL) {
        return;
    }

    /* Pauses still in flight will resume their player themselves */
    mc->resume_count++;

    /* Resume only players that we paused */
    for (GList *l = mc->paused_players; l != NULL; l = l->next) {
        player_play(mc, l->data);
    }

    /* Clear the paused list */
    g_list_free_full(mc->paused_players, g_free);
    mc->paused_players = NULL;
}
