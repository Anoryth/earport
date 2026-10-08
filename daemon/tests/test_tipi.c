/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Sharing the AirPods with an Apple device, with made-up addresses and the
 * delays shortened in meson.build (reconnection: 20 ms then 30 ms, giving
 * up: 150 ms, same list again: 50 ms, pods switching: 100 ms)
 */

#include <glib.h>
#include <string.h>

#include "tipi.h"

#define ME    "11:22:33:44:55:66"
#define PHONE "AA:BB:CC:DD:EE:01"

typedef struct {
    int sent;
    uint8_t last[32];
    size_t last_len;
    int reconnects;
    int reconnecting;           /* Times announced, and ended */
    int reconnected;
} Calls;

static bool send_packet(const uint8_t *data, size_t len, void *user_data)
{
    Calls *calls = user_data;
    calls->sent++;
    memcpy(calls->last, data, len);
    calls->last_len = len;
    return true;
}

static void reconnect(void *user_data)
{
    ((Calls *)user_data)->reconnects++;
}

static void reconnecting(bool on, void *user_data)
{
    Calls *calls = user_data;
    if (on)
        calls->reconnecting++;
    else
        calls->reconnected++;
}

static const TipiCallbacks callbacks = {
    .send = send_packet,
    .reconnect = reconnect,
    .reconnecting = reconnecting,
};

static void run_for(guint ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
}

static AapHosts table(uint8_t my_status, uint8_t my_flags, uint8_t phone_status,
                      uint8_t phone_flags)
{
    AapHosts hosts = { .count = 2 };
    g_strlcpy(hosts.hosts[0].address, PHONE, sizeof(hosts.hosts[0].address));
    hosts.hosts[0].status = phone_status;
    hosts.hosts[0].flags = phone_flags;
    g_strlcpy(hosts.hosts[1].address, ME, sizeof(hosts.hosts[1].address));
    hosts.hosts[1].status = my_status;
    hosts.hosts[1].flags = my_flags;
    return hosts;
}

static Tipi *new_session(Calls *calls)
{
    Tipi *tipi = tipi_new(&callbacks, calls);
    tipi_set_enabled(tipi, true);
    tipi_link_opened(tipi);
    return tipi;
}

/* Another device connected: both listed, the one playing first, once */
static void test_list(void)
{
    Calls calls = { 0 };
    Tipi *tipi = new_session(&calls);
    const uint8_t phone_first[] = {
        0x04, 0x00, 0x04, 0x00, 0x14, 0x00, 0x02,
        0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
    };

    /* Alone: nothing to list */
    AapHosts hosts = table(2, 0x04, 0, 0x01);
    tipi_hosts_changed(tipi, &hosts, ME);
    g_assert_cmpint(calls.sent, ==, 0);

    hosts = table(2, 0x04, 2, 0x13);
    tipi_hosts_changed(tipi, &hosts, ME);
    g_assert_cmpint(calls.sent, ==, 1);
    g_assert_cmpmem(calls.last, calls.last_len, phone_first, sizeof(phone_first));

    /* Not taken yet, or taken: not again */
    tipi_hosts_changed(tipi, &hosts, ME);
    hosts = table(2, 0x05, 2, 0x13);
    tipi_hosts_changed(tipi, &hosts, ME);
    g_assert_cmpint(calls.sent, ==, 1);

    /* Playing here: this computer first */
    hosts = table(2, 0x07, 2, 0x11);
    tipi_hosts_changed(tipi, &hosts, ME);
    g_assert_cmpint(calls.sent, ==, 2);
    g_assert_cmpmem(calls.last + 7, 6, phone_first + 13, 6);

    /* Forgotten by the pod taking over, the other device dropped: again */
    AapHosts alone = { .count = 1 };
    g_strlcpy(alone.hosts[0].address, ME, sizeof(alone.hosts[0].address));
    alone.hosts[0].status = 2;
    alone.hosts[0].flags = 0x06;
    tipi_hosts_changed(tipi, &alone, ME);
    g_assert_cmpint(calls.sent, ==, 2);     /* Just sent */
    run_for(60);
    tipi_hosts_changed(tipi, &alone, ME);
    g_assert_cmpint(calls.sent, ==, 3);
    g_assert_cmpmem(calls.last + 13, 6, phone_first + 7, 6);

    /* New session, playing here from the start */
    tipi_link_opened(tipi);
    hosts = table(2, 0x06, 2, 0x10);
    tipi_hosts_changed(tipi, &hosts, ME);
    g_assert_cmpint(calls.sent, ==, 4);
    g_assert_cmpmem(calls.last + 7, 6, phone_first + 13, 6);

    /* Own address unknown, or disabled: nothing */
    tipi_link_opened(tipi);
    tipi_hosts_changed(tipi, &hosts, "");
    tipi_set_enabled(tipi, false);
    tipi_hosts_changed(tipi, &hosts, ME);
    g_assert_cmpint(calls.sent, ==, 4);
    tipi_free(tipi);
}

/* Playback here with another device connected: this computer plays */
static void test_playback(void)
{
    Calls calls = { 0 };
    Tipi *tipi = new_session(&calls);
    const uint8_t plays[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06, 0x01, 0x00, 0x00, 0x00 };

    AapHosts hosts = table(2, 0x05, 2, 0x13);
    tipi_hosts_changed(tipi, &hosts, ME);
    calls.sent = 0;                     /* The list, sent on each session */
    tipi_playback_started(tipi);
    g_assert_cmpint(calls.sent, ==, 1);
    g_assert_cmpmem(calls.last, calls.last_len, plays, sizeof(plays));

    /* Already the one playing, or alone: nothing to say */
    hosts = table(2, 0x07, 2, 0x11);
    tipi_hosts_changed(tipi, &hosts, ME);
    int sent = calls.sent;
    tipi_playback_started(tipi);
    g_assert_cmpint(calls.sent, ==, sent);
    hosts = table(2, 0x04, 0, 0x01);
    tipi_hosts_changed(tipi, &hosts, ME);
    sent = calls.sent;
    tipi_playback_started(tipi);
    g_assert_cmpint(calls.sent, ==, sent);
    tipi_free(tipi);
}

/* The primary pod comes out, the other one stays in */
static void swap_pods(Tipi *tipi)
{
    tipi_ear_changed(tipi, true, true);
    tipi_ear_changed(tipi, false, true);
}

/* Dropped for the device playing as the pods switch: back, a few tries
 * until the link opens */
static void test_dropped(void)
{
    Calls calls = { 0 };
    Tipi *tipi = new_session(&calls);

    AapHosts hosts = table(2, 0x05, 2, 0x13);
    tipi_hosts_changed(tipi, &hosts, ME);
    swap_pods(tipi);
    hosts = table(0, 0x05, 2, 0x13);
    tipi_hosts_changed(tipi, &hosts, ME);
    tipi_link_closed(tipi, true);
    run_for(10);
    g_assert_cmpint(calls.reconnects, ==, 0);
    run_for(30);
    g_assert_cmpint(calls.reconnects, ==, 1);
    run_for(100);
    g_assert_cmpint(calls.reconnects, ==, 3);
    g_assert_cmpint(calls.reconnecting, ==, 1);
    g_assert_cmpint(calls.reconnected, ==, 0);
    run_for(150);                       /* Gave up */
    g_assert_cmpint(calls.reconnected, ==, 1);

    /* Back at the first try: no more */
    hosts = table(2, 0x05, 2, 0x13);
    tipi_link_opened(tipi);
    tipi_hosts_changed(tipi, &hosts, ME);
    swap_pods(tipi);
    tipi_link_closed(tipi, true);
    run_for(30);
    tipi_link_opened(tipi);
    g_assert_cmpint(calls.reconnected, ==, 2);
    run_for(100);
    g_assert_cmpint(calls.reconnects, ==, 4);

    /* Dropped again right after: let it be */
    tipi_hosts_changed(tipi, &hosts, ME);
    swap_pods(tipi);
    tipi_link_closed(tipi, true);
    run_for(100);
    g_assert_cmpint(calls.reconnects, ==, 4);
    tipi_free(tipi);
}

/* Left otherwise: stays disconnected */
static void test_not_dropped(void)
{
    Calls calls = { 0 };
    Tipi *tipi = new_session(&calls);

    /* Taken by another device, pods in place: the user chose it */
    AapHosts hosts = table(2, 0x05, 2, 0x13);
    tipi_hosts_changed(tipi, &hosts, ME);
    tipi_ear_changed(tipi, true, true);
    tipi_link_closed(tipi, true);

    /* The primary pod came out a while ago */
    tipi_link_opened(tipi);
    tipi_hosts_changed(tipi, &hosts, ME);
    swap_pods(tipi);
    run_for(120);
    tipi_link_closed(tipi, true);

    /* Playing here */
    tipi_link_opened(tipi);
    hosts = table(2, 0x07, 2, 0x11);
    tipi_hosts_changed(tipi, &hosts, ME);
    swap_pods(tipi);
    tipi_link_closed(tipi, true);

    /* Put away */
    tipi_link_opened(tipi);
    hosts = table(2, 0x05, 2, 0x13);
    tipi_hosts_changed(tipi, &hosts, ME);
    tipi_ear_changed(tipi, true, false);
    tipi_ear_changed(tipi, false, false);
    tipi_link_closed(tipi, true);

    /* Disconnected from here */
    tipi_link_opened(tipi);
    tipi_hosts_changed(tipi, &hosts, ME);
    swap_pods(tipi);
    tipi_link_closed(tipi, false);

    /* Alone */
    tipi_link_opened(tipi);
    hosts = table(2, 0x04, 0, 0x01);
    tipi_hosts_changed(tipi, &hosts, ME);
    swap_pods(tipi);
    tipi_link_closed(tipi, true);

    /* Disabled */
    tipi_link_opened(tipi);
    hosts = table(2, 0x05, 2, 0x13);
    tipi_hosts_changed(tipi, &hosts, ME);
    swap_pods(tipi);
    tipi_set_enabled(tipi, false);
    tipi_link_closed(tipi, true);

    run_for(150);
    g_assert_cmpint(calls.reconnects, ==, 0);
    g_assert_cmpint(calls.reconnecting, ==, 0);
    tipi_free(tipi);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/tipi/list", test_list);
    g_test_add_func("/tipi/playback", test_playback);
    g_test_add_func("/tipi/dropped", test_dropped);
    g_test_add_func("/tipi/not-dropped", test_not_dropped);

    return g_test_run();
}
