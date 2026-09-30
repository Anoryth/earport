/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * AAP link tests over a fake Bluetooth transport (fake_bluetooth.c), with
 * delays shortened at build time (see meson.build): handshake, send queue,
 * notification retries, silent re-open, reconnection backoff.
 */

#include <glib.h>
#include <string.h>

#include "aap_link.h"
#include "fake_bluetooth.h"

/* Same values as passed by meson.build */
#define GAP_MS 2
#define RETRY_MS 20
#define RECONNECT_MS 10
#define SILENT_MS 30

#define ADDRESS "00:11:22:33:44:55"

typedef struct {
    int connected;
    int disconnected;
    int batteries;
    AapLink *link;
} Fixture;

/* Battery packet from AirPods Pro 2 USB-C, both pods in the ears */
static const uint8_t battery[] = {
    0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03,
    0x04, 0x01, 0x53, 0x02, 0x01,
    0x02, 0x01, 0x53, 0x02, 0x01,
    0x08, 0x01, 0x00, 0x04, 0x01,
};

static const uint8_t features_ack[] = { 0x04, 0x00, 0x04, 0x00, 0x2B, 0x00, 0x01, 0x22, 0x00 };

static void on_connected(const char *address, const char *name, void *user_data)
{
    Fixture *f = user_data;
    g_assert_cmpstr(address, ==, ADDRESS);
    g_assert_cmpstr(name, ==, "AirPods Pro");
    f->connected++;
}

static void on_disconnected(void *user_data)
{
    Fixture *f = user_data;
    f->disconnected++;
}

static void on_packet(const AapParsedPacket *packet, void *user_data)
{
    Fixture *f = user_data;
    if (packet->type == AAP_PKT_TYPE_BATTERY)
        f->batteries++;
}

static void fixture_setup(Fixture *f, gconstpointer data)
{
    (void)data;
    static const AapLinkCallbacks callbacks = {
        .connected = on_connected,
        .disconnected = on_disconnected,
        .packet = on_packet,
    };

    fake_bt_reset();
    *f = (Fixture) { 0 };
    f->link = aap_link_new(&callbacks, f);
}

static void fixture_teardown(Fixture *f, gconstpointer data)
{
    (void)data;
    aap_link_free(f->link);
}

static gboolean set_flag(gpointer user_data)
{
    *(bool *)user_data = true;
    return G_SOURCE_REMOVE;
}

/* Run the main loop until the condition holds (or fail after a second) */
#define RUN_UNTIL(cond) do { \
    gint64 deadline_ = g_get_monotonic_time() + G_USEC_PER_SEC; \
    while (!(cond)) { \
        g_assert_cmpint(g_get_monotonic_time(), <, deadline_); \
        g_main_context_iteration(NULL, FALSE); \
        g_usleep(500); \
    } \
} while (0)

/* Let the link's timers run for a while */
static void run_for(guint ms)
{
    bool done = false;
    g_timeout_add(ms, set_flag, &done);
    while (!done)
        g_main_context_iteration(NULL, TRUE);
}

static guint count_sent(const uint8_t *packet, size_t len)
{
    return fake_bt_count_sent(packet, len);
}

#define HANDSHAKES() count_sent(AAP_PKT_HANDSHAKE, AAP_HANDSHAKE_SIZE)
#define SET_FEATURES() count_sent(AAP_PKT_SET_FEATURES, AAP_SET_FEATURES_SIZE)
#define NOTIF_REQUESTS() count_sent(AAP_PKT_REQUEST_NOTIFICATIONS, AAP_REQUEST_NOTIF_SIZE)

static void assert_sent(guint index, const uint8_t *packet, size_t len)
{
    gsize size;
    g_assert_cmpuint(index, <, fake_bt.sent->len);
    const uint8_t *data = g_bytes_get_data(fake_bt.sent->pdata[index], &size);
    g_assert_cmpmem(data, size, packet, len);
}

/* ============================================================================
 * Session start
 * ========================================================================== */

static void test_handshake(Fixture *f, gconstpointer data)
{
    (void)data;

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    g_assert_true(aap_link_is_connected(f->link));
    g_assert_cmpint(f->connected, ==, 1);

    /* The AirPods get a moment before the handshake */
    g_assert_cmpuint(fake_bt.sent->len, ==, 0);

    run_for(10 * GAP_MS);
    assert_sent(0, AAP_PKT_HANDSHAKE, AAP_HANDSHAKE_SIZE);
    assert_sent(1, AAP_PKT_SET_FEATURES, AAP_SET_FEATURES_SIZE);
    assert_sent(2, AAP_PKT_REQUEST_NOTIFICATIONS, AAP_REQUEST_NOTIF_SIZE);
}

static void test_battery_stops_retries(Fixture *f, gconstpointer data)
{
    (void)data;

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    fake_bt_receive(battery, sizeof(battery));
    g_assert_cmpint(f->batteries, ==, 1);

    run_for(8 * RETRY_MS);
    g_assert_cmpuint(SET_FEATURES(), ==, 1);
    g_assert_cmpint(fake_bt.connect_count, ==, 1);
}

/* Notification requests not sent right after SET_FEATURES: retries send
 * the pair, the reaction to the acknowledgement sends a request alone */
static guint lone_notif_requests(void)
{
    guint count = 0;
    for (guint i = 0; i < fake_bt.sent->len; i++) {
        gsize size, prev_size = 0;
        const uint8_t *data = g_bytes_get_data(fake_bt.sent->pdata[i], &size);
        const uint8_t *prev = i > 0 ? g_bytes_get_data(fake_bt.sent->pdata[i - 1], &prev_size) : NULL;
        bool is_request = size == AAP_REQUEST_NOTIF_SIZE &&
                          memcmp(data, AAP_PKT_REQUEST_NOTIFICATIONS, size) == 0;
        bool after_features = prev != NULL && prev_size == AAP_SET_FEATURES_SIZE &&
                              memcmp(prev, AAP_PKT_SET_FEATURES, prev_size) == 0;
        if (is_request && !after_features)
            count++;
    }
    return count;
}

static void test_features_ack_requests_again(Fixture *f, gconstpointer data)
{
    (void)data;

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    run_for(5 * GAP_MS);
    g_assert_cmpuint(lone_notif_requests(), ==, 0);

    /* The request may have arrived before SET_FEATURES was processed */
    fake_bt_receive(features_ack, sizeof(features_ack));
    run_for(5 * GAP_MS);
    g_assert_cmpuint(lone_notif_requests(), ==, 1);

    /* Not needed any more once notifications flow */
    fake_bt_receive(battery, sizeof(battery));
    fake_bt_receive(features_ack, sizeof(features_ack));
    run_for(5 * GAP_MS);
    g_assert_cmpuint(lone_notif_requests(), ==, 1);
}

static void test_proximity_keys_requested(Fixture *f, gconstpointer data)
{
    (void)data;

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    fake_bt_receive(battery, sizeof(battery));
    run_for(60);
    g_assert_cmpuint(count_sent(AAP_PKT_REQUEST_PROXIMITY_KEYS, AAP_PROXIMITY_KEYS_REQ_SIZE), ==, 1);
}

/* ============================================================================
 * Keeping the link useful
 * ========================================================================== */

/* No battery: retries, then one silent re-open, hidden from clients */
static void test_silent_reopen(Fixture *f, gconstpointer data)
{
    (void)data;

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    RUN_UNTIL(fake_bt.connect_count == 2);
    run_for(10 * GAP_MS);

    g_assert_cmpint(f->connected, ==, 1);
    g_assert_cmpint(f->disconnected, ==, 0);
    g_assert_cmpuint(HANDSHAKES(), ==, 2);
    /* Handshake, 5 retries, handshake of the new link */
    g_assert_cmpuint(SET_FEATURES(), >=, 7);

    /* Only once per BlueZ connection */
    run_for(12 * RETRY_MS);
    g_assert_cmpint(fake_bt.connect_count, ==, 2);
    g_assert_cmpint(f->disconnected, ==, 0);
}

static void test_silent_reopen_restores_battery(Fixture *f, gconstpointer data)
{
    (void)data;

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    RUN_UNTIL(fake_bt.connect_count == 2);

    fake_bt_receive(battery, sizeof(battery));
    run_for(10 * GAP_MS);  /* Handshake of the new link still queued */
    guint set_features = SET_FEATURES();
    run_for(8 * RETRY_MS);
    g_assert_cmpuint(SET_FEATURES(), ==, set_features);
    g_assert_cmpint(f->batteries, ==, 1);
}

/* ============================================================================
 * Reconnection
 * ========================================================================== */

static void test_refused_then_backoff(Fixture *f, gconstpointer data)
{
    (void)data;

    fake_bt.refuse_connects = 2;
    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    g_assert_cmpint(fake_bt.connect_count, ==, 1);
    g_assert_false(aap_link_is_connected(f->link));

    /* Retried after 10 ms, then 20 ms */
    run_for(RECONNECT_MS + 5);
    g_assert_cmpint(fake_bt.connect_count, ==, 2);
    run_for(2 * RECONNECT_MS + 5);
    g_assert_cmpint(fake_bt.connect_count, ==, 3);

    g_assert_true(aap_link_is_connected(f->link));
    g_assert_cmpint(f->connected, ==, 1);
}

static void test_gives_up(Fixture *f, gconstpointer data)
{
    (void)data;

    fake_bt.refuse_connects = 100;
    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");

    /* 10 + 20 + 40 + 80 + 160 ms */
    run_for(31 * RECONNECT_MS + 50);
    g_assert_cmpint(fake_bt.connect_count, ==, 6);
    run_for(20 * RECONNECT_MS);
    g_assert_cmpint(fake_bt.connect_count, ==, 6);
    g_assert_cmpint(f->connected, ==, 0);
}

/* L2CAP dropped while the AirPods are still connected to BlueZ */
static void test_peer_closes(Fixture *f, gconstpointer data)
{
    (void)data;

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    fake_bt_receive(battery, sizeof(battery));
    fake_bt_peer_closes();
    g_assert_cmpint(f->disconnected, ==, 1);

    run_for(RECONNECT_MS + 5);
    g_assert_cmpint(fake_bt.connect_count, ==, 2);
    g_assert_cmpint(f->connected, ==, 2);
}

static void test_bluez_disconnect(Fixture *f, gconstpointer data)
{
    (void)data;

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    fake_bt_receive(battery, sizeof(battery));
    aap_link_device_disconnected(f->link);
    g_assert_cmpint(f->disconnected, ==, 1);
    g_assert_false(aap_link_is_connected(f->link));

    run_for(5 * RECONNECT_MS);
    g_assert_cmpint(fake_bt.connect_count, ==, 1);
}

/* The silent re-open is refused: now the link is really down */
static void test_silent_reopen_refused(Fixture *f, gconstpointer data)
{
    (void)data;

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    fake_bt.refuse_connects = 100;
    RUN_UNTIL(fake_bt.connect_count == 2);
    g_assert_cmpint(f->disconnected, ==, 1);

    /* Reported once, not again when BlueZ loses them */
    aap_link_device_disconnected(f->link);
    g_assert_cmpint(f->disconnected, ==, 1);
}

/* The AirPods leave while the link is closed for the silent re-open:
 * clients still think they are connected, so it must be reported */
static void test_bluez_disconnect_during_reopen(Fixture *f, gconstpointer data)
{
    (void)data;

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    RUN_UNTIL(!aap_link_is_connected(f->link));
    g_assert_cmpint(f->disconnected, ==, 0);

    aap_link_device_disconnected(f->link);
    g_assert_cmpint(f->disconnected, ==, 1);

    run_for(2 * SILENT_MS);
    g_assert_cmpint(fake_bt.connect_count, ==, 1);
}

/* ============================================================================
 * Send queue
 * ========================================================================== */

static void test_send_needs_link(Fixture *f, gconstpointer data)
{
    (void)data;
    const uint8_t anc[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00 };

    g_assert_false(aap_link_send(f->link, anc, sizeof(anc)));

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    g_assert_true(aap_link_send(f->link, anc, sizeof(anc)));
    run_for(10 * GAP_MS);
    g_assert_cmpuint(count_sent(anc, sizeof(anc)), ==, 1);
}

/* Packets queued for a link that went down are not sent on the next one */
static void test_queue_cleared(Fixture *f, gconstpointer data)
{
    (void)data;

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    fake_bt_peer_closes();
    g_assert_cmpuint(fake_bt.sent->len, ==, 0);

    run_for(RECONNECT_MS + 10 * GAP_MS);
    g_assert_cmpint(fake_bt.connect_count, ==, 2);
    g_assert_cmpuint(HANDSHAKES(), ==, 1);
}

/* Stopping the service is not losing the AirPods */
static void test_free_is_silent(Fixture *f, gconstpointer data)
{
    (void)data;

    aap_link_device_connected(f->link, ADDRESS, "AirPods Pro");
    aap_link_free(f->link);
    f->link = NULL;
    g_assert_cmpint(f->disconnected, ==, 0);

    /* No timer left behind on the freed link */
    run_for(10 * RETRY_MS);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    /* The link logs warnings on purpose (refused connections, giving up) */
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

#define ADD(path, func) \
    g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

    ADD("/link/handshake", test_handshake);
    ADD("/link/battery-stops-retries", test_battery_stops_retries);
    ADD("/link/features-ack-requests-again", test_features_ack_requests_again);
    ADD("/link/proximity-keys-requested", test_proximity_keys_requested);
    ADD("/link/silent-reopen", test_silent_reopen);
    ADD("/link/silent-reopen-restores-battery", test_silent_reopen_restores_battery);
    ADD("/link/refused-then-backoff", test_refused_then_backoff);
    ADD("/link/gives-up", test_gives_up);
    ADD("/link/peer-closes", test_peer_closes);
    ADD("/link/bluez-disconnect", test_bluez_disconnect);
    ADD("/link/silent-reopen-refused", test_silent_reopen_refused);
    ADD("/link/bluez-disconnect-during-reopen", test_bluez_disconnect_during_reopen);
    ADD("/link/send-needs-link", test_send_needs_link);
    ADD("/link/queue-cleared", test_queue_cleared);
    ADD("/link/free-is-silent", test_free_is_silent);

    return g_test_run();
}
