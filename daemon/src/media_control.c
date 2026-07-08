/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 LibrePods Contributors
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

struct MediaControl {
    GDBusConnection *connection;
    EarPauseMode ear_pause_mode;

    /* Track which players we paused */
    GList *paused_players;     /* List of player names (strings) that we paused */

    /* Previous ear state for edge detection */
    bool prev_left_in_ear;
    bool prev_right_in_ear;
    bool prev_state_valid;
};

/* ============================================================================
 * Helper functions
 * ========================================================================== */

static GList *get_mpris_players(MediaControl *mc)
{
    GList *players = NULL;
    GError *error = NULL;

    GVariant *result = g_dbus_connection_call_sync(
        mc->connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ListNames",
        NULL,
        G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error);

    if (error != NULL) {
        g_warning("Failed to list D-Bus names: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    GVariantIter *iter;
    const gchar *name;
    g_variant_get(result, "(as)", &iter);

    while (g_variant_iter_loop(iter, "&s", &name)) {
        if (g_str_has_prefix(name, MPRIS_DBUS_NAME_PREFIX)) {
            players = g_list_append(players, g_strdup(name));
        }
    }

    g_variant_iter_free(iter);
    g_variant_unref(result);

    return players;
}

static gchar *get_player_playback_status(MediaControl *mc, const gchar *player_name)
{
    GError *error = NULL;
    gchar *status = NULL;

    GVariant *result = g_dbus_connection_call_sync(
        mc->connection,
        player_name,
        MPRIS_DBUS_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Get",
        g_variant_new("(ss)", MPRIS_PLAYER_INTERFACE, "PlaybackStatus"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error);

    if (error != NULL) {
        g_debug("Failed to get playback status from %s: %s", player_name, error->message);
        g_error_free(error);
        return NULL;
    }

    GVariant *variant;
    g_variant_get(result, "(v)", &variant);
    status = g_strdup(g_variant_get_string(variant, NULL));
    g_variant_unref(variant);
    g_variant_unref(result);

    return status;
}

static bool player_pause(MediaControl *mc, const gchar *player_name)
{
    GError *error = NULL;

    g_dbus_connection_call_sync(
        mc->connection,
        player_name,
        MPRIS_DBUS_PATH,
        MPRIS_PLAYER_INTERFACE,
        "Pause",
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error);

    if (error != NULL) {
        g_debug("Failed to pause %s: %s", player_name, error->message);
        g_error_free(error);
        return false;
    }

    g_message("Paused media player: %s", player_name);
    return true;
}

static bool player_play(MediaControl *mc, const gchar *player_name)
{
    GError *error = NULL;

    g_dbus_connection_call_sync(
        mc->connection,
        player_name,
        MPRIS_DBUS_PATH,
        MPRIS_PLAYER_INTERFACE,
        "Play",
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error);

    if (error != NULL) {
        g_debug("Failed to play %s: %s", player_name, error->message);
        g_error_free(error);
        return false;
    }

    g_message("Resumed media player: %s", player_name);
    return true;
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
    mc->paused_players = NULL;
    mc->prev_state_valid = false;

    return mc;
}

void media_control_free(MediaControl *mc)
{
    if (mc == NULL) {
        return;
    }

    /* Free paused players list */
    g_list_free_full(mc->paused_players, g_free);

    if (mc->connection) {
        g_object_unref(mc->connection);
    }

    g_free(mc);
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

    /* Clear previous paused list */
    g_list_free_full(mc->paused_players, g_free);
    mc->paused_players = NULL;

    /* Get all MPRIS players */
    GList *players = get_mpris_players(mc);

    for (GList *l = players; l != NULL; l = l->next) {
        const gchar *player_name = l->data;

        /* Check if player is currently playing */
        gchar *status = get_player_playback_status(mc, player_name);
        if (status != NULL && g_strcmp0(status, "Playing") == 0) {
            /* Pause this player and remember it */
            if (player_pause(mc, player_name)) {
                mc->paused_players = g_list_append(mc->paused_players, g_strdup(player_name));
            }
        }
        g_free(status);
    }

    g_list_free_full(players, g_free);
}

void media_control_resume(MediaControl *mc)
{
    if (mc == NULL || mc->connection == NULL) {
        return;
    }

    /* Resume only players that we paused */
    for (GList *l = mc->paused_players; l != NULL; l = l->next) {
        const gchar *player_name = l->data;
        player_play(mc, player_name);
    }

    /* Clear the paused list */
    g_list_free_full(mc->paused_players, g_free);
    mc->paused_players = NULL;
}