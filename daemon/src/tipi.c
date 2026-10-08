/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "tipi.h"

#include <glib.h>

/* Connection attempts after being dropped, from the drop: the AirPods
 * finish switching pods first (an iPhone comes back after about 6 s) */
#ifndef TIPI_RECONNECT_FIRST_MS
#define TIPI_RECONNECT_FIRST_MS 2000
#endif
#ifndef TIPI_RECONNECT_NEXT_MS
#define TIPI_RECONNECT_NEXT_MS 5000
#endif
#define TIPI_RECONNECT_TRIES 3

/* The primary pod came out this recently: the AirPods drop a host while
 * switching pods, about 1 s later */
#ifndef TIPI_SWAP_WINDOW_MS
#define TIPI_SWAP_WINDOW_MS 5000
#endif

/* Dropped again this soon after coming back: let it be */
#ifndef TIPI_AGAIN_MS
#define TIPI_AGAIN_MS 10000
#endif

/* After the last attempt, before giving up: BlueZ answers by then */
#ifndef TIPI_RECONNECT_GIVE_UP_MS
#define TIPI_RECONNECT_GIVE_UP_MS 10000
#endif

/* The same list again only after this, and so many per session at most:
 * the AirPods answer with the table, possibly before taking it */
#ifndef TIPI_LIST_REPEAT_MS
#define TIPI_LIST_REPEAT_MS 2000
#endif
#define TIPI_LISTS_MAX 8

/* In the hosts table */
#define HOST_CONNECTED 0x02     /* Status */
#define HOST_LISTED    0x01     /* Flags: kept with the other one */
#define HOST_PLAYS     0x02     /* Flags: the one they play from */

struct Tipi {
    TipiCallbacks callbacks;
    void *user_data;
    bool enabled;

    /* Session */
    bool known;                 /* This computer is in the table */
    uint8_t my_flags;
    char other[18];             /* Other host, connected or connecting */
    bool other_connected;
    char partner[18];           /* Other host seen connected, kept */
    char listed_with[18];       /* Last list sent */
    bool listed_first;          /* With this computer first */
    gint64 listed_us;
    int lists;
    bool primary_in;
    bool secondary_in;
    gint64 primary_out_us;      /* When the primary pod came out, 0 if in */
    gint64 back_us;             /* When reconnected after a drop */

    guint reconnect_id;
    int tries;
    bool reconnecting;
};

static void set_reconnecting(Tipi *tipi, bool reconnecting)
{
    if (tipi->reconnecting == reconnecting)
        return;
    tipi->reconnecting = reconnecting;
    if (tipi->callbacks.reconnecting != NULL)
        tipi->callbacks.reconnecting(reconnecting, tipi->user_data);
}

static void cancel_reconnect(Tipi *tipi)
{
    if (tipi->reconnect_id > 0) {
        g_source_remove(tipi->reconnect_id);
        tipi->reconnect_id = 0;
    }
    tipi->tries = 0;
    set_reconnecting(tipi, false);
}

static void reset_session(Tipi *tipi)
{
    tipi->known = false;
    tipi->my_flags = 0;
    tipi->other[0] = '\0';
    tipi->other_connected = false;
    tipi->partner[0] = '\0';
    tipi->listed_with[0] = '\0';
    tipi->listed_first = false;
    tipi->listed_us = 0;
    tipi->lists = 0;
    tipi->primary_in = false;
    tipi->secondary_in = false;
    tipi->primary_out_us = 0;
}

Tipi *tipi_new(const TipiCallbacks *callbacks, void *user_data)
{
    Tipi *tipi = g_new0(Tipi, 1);
    tipi->callbacks = *callbacks;
    tipi->user_data = user_data;
    return tipi;
}

void tipi_free(Tipi *tipi)
{
    if (tipi == NULL)
        return;
    tipi->callbacks.reconnecting = NULL;    /* Nobody to tell any more */
    cancel_reconnect(tipi);
    g_free(tipi);
}

void tipi_set_enabled(Tipi *tipi, bool enabled)
{
    tipi->enabled = enabled;
    if (!enabled)
        cancel_reconnect(tipi);
}

void tipi_link_opened(Tipi *tipi)
{
    tipi->back_us = tipi->reconnecting ? g_get_monotonic_time() : 0;
    cancel_reconnect(tipi);
    reset_session(tipi);
}

/* Both listed, the one playing first. Again when that changes, and when
 * the AirPods forgot it: the pod taking over as primary may not know it. */
static void update_list(Tipi *tipi, const char *my_address)
{
    bool mine_first = (tipi->my_flags & HOST_PLAYS) != 0;
    bool same = g_ascii_strcasecmp(tipi->listed_with, tipi->partner) == 0 &&
                tipi->listed_first == mine_first;
    if (same && (tipi->my_flags & HOST_LISTED))
        return;
    gint64 now = g_get_monotonic_time();
    if ((same && now - tipi->listed_us < TIPI_LIST_REPEAT_MS * 1000) ||
        tipi->lists >= TIPI_LISTS_MAX)
        return;

    const char *addresses[2];
    addresses[0] = mine_first ? my_address : tipi->partner;
    addresses[1] = mine_first ? tipi->partner : my_address;
    uint8_t packet[AAP_PRIORITY_LIST_MAX_SIZE];
    size_t len = aap_build_priority_list(addresses, 2, packet);
    if (len == 0)
        return;

    g_message("Asking the AirPods to keep this computer with the other device (%s first)",
              mine_first ? "this computer" : "the other device");
    if (!tipi->callbacks.send(packet, len, tipi->user_data))
        return;
    g_strlcpy(tipi->listed_with, tipi->partner, sizeof(tipi->listed_with));
    tipi->listed_first = mine_first;
    tipi->listed_us = now;
    tipi->lists++;
}

void tipi_hosts_changed(Tipi *tipi, const AapHosts *hosts, const char *my_address)
{
    tipi->known = false;
    tipi->other[0] = '\0';
    tipi->other_connected = false;
    for (uint8_t i = 0; i < hosts->count; i++) {
        const AapHost *host = &hosts->hosts[i];
        if (my_address[0] != '\0' && g_ascii_strcasecmp(host->address, my_address) == 0) {
            tipi->known = true;
            tipi->my_flags = host->flags;
        } else if (host->status != 0 && !tipi->other_connected) {
            /* Rather the one connected */
            g_strlcpy(tipi->other, host->address, sizeof(tipi->other));
            tipi->other_connected = host->status == HOST_CONNECTED;
        }
    }

    if (tipi->other_connected)
        g_strlcpy(tipi->partner, tipi->other, sizeof(tipi->partner));
    if (tipi->enabled && tipi->known && tipi->partner[0] != '\0')
        update_list(tipi, my_address);
}

void tipi_ear_changed(Tipi *tipi, bool primary_in, bool secondary_in)
{
    if (tipi->primary_in && !primary_in)
        tipi->primary_out_us = g_get_monotonic_time();
    else if (primary_in)
        tipi->primary_out_us = 0;
    tipi->primary_in = primary_in;
    tipi->secondary_in = secondary_in;
}

void tipi_playback_started(Tipi *tipi)
{
    if (!tipi->enabled || !tipi->known || tipi->other[0] == '\0' ||
        (tipi->my_flags & HOST_PLAYS))
        return;

    const uint8_t plays = 0x01;
    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_control_cmd(AAP_CTRL_OWNS_CONNECTION, &plays, 1, packet);
    g_message("Telling the AirPods this computer plays");
    tipi->callbacks.send(packet, sizeof(packet), tipi->user_data);
}

static gboolean on_give_up(gpointer user_data)
{
    Tipi *tipi = user_data;
    tipi->reconnect_id = 0;
    g_message("The AirPods stay with the other device");
    set_reconnecting(tipi, false);
    return G_SOURCE_REMOVE;
}

static gboolean on_reconnect(gpointer user_data)
{
    Tipi *tipi = user_data;
    tipi->tries++;
    g_message("Connecting the AirPods again (%d/%d)", tipi->tries, TIPI_RECONNECT_TRIES);
    tipi->callbacks.reconnect(tipi->user_data);

    if (tipi->tries >= TIPI_RECONNECT_TRIES)
        tipi->reconnect_id = g_timeout_add(TIPI_RECONNECT_GIVE_UP_MS, on_give_up, tipi);
    else
        tipi->reconnect_id = g_timeout_add(TIPI_RECONNECT_NEXT_MS, on_reconnect, tipi);
    return G_SOURCE_REMOVE;
}

void tipi_link_closed(Tipi *tipi, bool left)
{
    /* Dropped for the device playing as the pods switched, still listening */
    gint64 now = g_get_monotonic_time();
    bool swapping = tipi->secondary_in && tipi->primary_out_us > 0 &&
                    now - tipi->primary_out_us < TIPI_SWAP_WINDOW_MS * 1000;
    bool again = tipi->back_us > 0 && now - tipi->back_us < TIPI_AGAIN_MS * 1000;
    bool dropped = tipi->enabled && left && tipi->known && tipi->other[0] != '\0' &&
                   !(tipi->my_flags & HOST_PLAYS) && swapping && !again;
    reset_session(tipi);
    if (!dropped || tipi->reconnect_id > 0)
        return;

    g_message("The AirPods left this computer for the other device: connecting again");
    tipi->tries = 0;
    tipi->reconnect_id = g_timeout_add(TIPI_RECONNECT_FIRST_MS, on_reconnect, tipi);
    set_reconnecting(tipi, true);
}
