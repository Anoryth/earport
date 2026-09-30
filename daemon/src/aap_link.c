/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "aap_link.h"
#include "bluetooth.h"
#include "research.h"

#include <glib.h>

/* L2CAP reconnection: AirPods frequently refuse the first L2CAP connect
 * right after the BlueZ link comes up, so retry with exponential backoff. */
#define RECONNECT_MAX_ATTEMPTS 5
#define RECONNECT_BASE_DELAY_SEC 2

/* When another Apple device (e.g. a nearby iPhone) is also connected to the
 * AirPods, they sometimes ignore our notification request: control commands
 * still flow, but battery and ear detection never arrive. Re-send the
 * request until the first battery packet shows up. */
#define NOTIF_RETRY_MAX_ATTEMPTS 5
#define NOTIF_RETRY_INTERVAL_SEC 2

/* If retries are not enough, re-open the L2CAP link once: a fresh
 * connection has been seen to restore battery updates. */
#define SILENT_RECONNECT_DELAY_MS 500

/* Keys to recognize the AirPods' BLE adverts (automatic connection) */
#define PROXIMITY_KEYS_DELAY_SEC 2

/* The AirPods drop packets sent back to back: space them out, and give
 * them a moment after the L2CAP connection before the handshake */
#define SEND_GAP_MS 50
#define HANDSHAKE_DELAY_MS 100

/* Opcode of the AirPods' acknowledgement of our SET_FEATURES packet */
#define AAP_OPCODE_FEATURES_ACK 0x2B

struct AapLink {
    AapLinkCallbacks callbacks;
    void *user_data;

    BluetoothConnection *bt_conn;
    char *address;
    char *name;
    bool bluez_connected;        /* Device still connected at BlueZ level */

    guint reconnect_timeout_id;
    int reconnect_attempts;

    /* Notification request retries (until the first battery packet) */
    guint notif_retry_timeout_id;
    int notif_retry_attempts;
    bool battery_received;

    /* Silent L2CAP reconnect when battery info never arrives */
    bool silent_reconnect;       /* Reconnect in progress, hidden from clients */
    bool silent_reconnect_done;  /* Already tried for this BlueZ connection */
    guint silent_reconnect_id;

    guint keys_request_id;

    /* Outgoing packets (GBytes), sent SEND_GAP_MS apart */
    GQueue send_queue;
    gint64 next_send_us;         /* Earliest time for the next packet */
    guint send_timer_id;
};

static void connect_link(AapLink *link);

/* ============================================================================
 * Send queue
 * ========================================================================== */

static gboolean send_timer_cb(gpointer user_data);

/* Send the next packet if its time has come, or wait for it */
static void pump_send_queue(AapLink *link)
{
    if (link->send_timer_id > 0 || g_queue_is_empty(&link->send_queue))
        return;

    gint64 now = g_get_monotonic_time();
    if (now < link->next_send_us) {
        guint delay_ms = (guint)((link->next_send_us - now + 999) / 1000);
        link->send_timer_id = g_timeout_add(delay_ms, send_timer_cb, link);
        return;
    }

    GBytes *packet = g_queue_pop_head(&link->send_queue);
    gsize len;
    const uint8_t *data = g_bytes_get_data(packet, &len);
    bt_connection_send(link->bt_conn, data, len);
    g_bytes_unref(packet);

    link->next_send_us = now + SEND_GAP_MS * 1000;
    pump_send_queue(link);
}

static gboolean send_timer_cb(gpointer user_data)
{
    AapLink *link = user_data;
    link->send_timer_id = 0;
    pump_send_queue(link);
    return G_SOURCE_REMOVE;
}

static void clear_send_queue(AapLink *link)
{
    if (link->send_timer_id > 0) {
        g_source_remove(link->send_timer_id);
        link->send_timer_id = 0;
    }
    g_queue_clear_full(&link->send_queue, (GDestroyNotify)g_bytes_unref);
}

/* ============================================================================
 * L2CAP reconnection with exponential backoff
 * ========================================================================== */

static gboolean reconnect_timeout_cb(gpointer user_data)
{
    AapLink *link = user_data;
    link->reconnect_timeout_id = 0;

    if (!link->bluez_connected || link->address == NULL)
        return G_SOURCE_REMOVE;

    if (aap_link_is_connected(link))
        return G_SOURCE_REMOVE;

    g_message("L2CAP reconnect attempt %d/%d to %s",
              link->reconnect_attempts, RECONNECT_MAX_ATTEMPTS, link->address);

    /* On failure, the BT_STATE_ERROR callback schedules the next attempt */
    connect_link(link);

    return G_SOURCE_REMOVE;
}

static void schedule_reconnect(AapLink *link)
{
    if (link->reconnect_timeout_id > 0)
        return;

    if (!link->bluez_connected || link->address == NULL)
        return;

    if (link->reconnect_attempts >= RECONNECT_MAX_ATTEMPTS) {
        g_warning("Giving up L2CAP reconnection after %d attempts", link->reconnect_attempts);
        return;
    }

    guint delay = RECONNECT_BASE_DELAY_SEC << link->reconnect_attempts;  /* 2,4,8,16,32s */
    link->reconnect_attempts++;

    g_message("Scheduling L2CAP reconnect attempt %d/%d in %us",
              link->reconnect_attempts, RECONNECT_MAX_ATTEMPTS, delay);

    link->reconnect_timeout_id = g_timeout_add_seconds(delay, reconnect_timeout_cb, link);
}

static void cancel_reconnect(AapLink *link)
{
    if (link->reconnect_timeout_id > 0) {
        g_source_remove(link->reconnect_timeout_id);
        link->reconnect_timeout_id = 0;
    }
    link->reconnect_attempts = 0;
}

/* ============================================================================
 * Notification request retries
 * ========================================================================== */

static gboolean notif_retry_timeout_cb(gpointer user_data)
{
    AapLink *link = user_data;

    if (link->battery_received || !aap_link_is_connected(link)) {
        link->notif_retry_timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    if (link->notif_retry_attempts >= NOTIF_RETRY_MAX_ATTEMPTS) {
        link->notif_retry_timeout_id = 0;

        if (!link->silent_reconnect_done) {
            g_message("No battery info after %d notification requests, "
                      "re-opening the AirPods link", link->notif_retry_attempts);
            link->silent_reconnect = true;
            link->silent_reconnect_done = true;
            bt_connection_disconnect(link->bt_conn);
        } else {
            g_warning("No battery info after %d notification requests "
                      "(another Apple device may own the AirPods connection)",
                      link->notif_retry_attempts);
        }
        return G_SOURCE_REMOVE;
    }

    link->notif_retry_attempts++;
    g_message("No battery info yet, re-requesting notifications (%d/%d)",
              link->notif_retry_attempts, NOTIF_RETRY_MAX_ATTEMPTS);

    aap_link_send(link, AAP_PKT_SET_FEATURES, AAP_SET_FEATURES_SIZE);
    aap_link_send(link, AAP_PKT_REQUEST_NOTIFICATIONS, AAP_REQUEST_NOTIF_SIZE);

    return G_SOURCE_CONTINUE;
}

static void cancel_notif_retry(AapLink *link)
{
    if (link->notif_retry_timeout_id > 0) {
        g_source_remove(link->notif_retry_timeout_id);
        link->notif_retry_timeout_id = 0;
    }
}

static void start_notif_retry(AapLink *link)
{
    cancel_notif_retry(link);
    link->battery_received = false;
    link->notif_retry_attempts = 0;
    link->notif_retry_timeout_id = g_timeout_add_seconds(NOTIF_RETRY_INTERVAL_SEC,
                                                         notif_retry_timeout_cb, link);
}

/* ============================================================================
 * Session end
 * ========================================================================== */

/* Tell clients the AirPods are gone */
static void report_disconnected(AapLink *link)
{
    link->silent_reconnect = false;
    cancel_notif_retry(link);
    link->callbacks.disconnected(link->user_data);
}

static gboolean silent_reconnect_cb(gpointer user_data)
{
    AapLink *link = user_data;
    link->silent_reconnect_id = 0;

    if (!link->silent_reconnect)
        return G_SOURCE_REMOVE;

    if (link->bluez_connected && link->address != NULL)
        connect_link(link);
    else
        report_disconnected(link);

    return G_SOURCE_REMOVE;
}

/* ============================================================================
 * Bluetooth callbacks
 * ========================================================================== */

static gboolean request_proximity_keys_cb(gpointer user_data)
{
    AapLink *link = user_data;
    link->keys_request_id = 0;
    aap_link_send(link, AAP_PKT_REQUEST_PROXIMITY_KEYS, AAP_PROXIMITY_KEYS_REQ_SIZE);
    return G_SOURCE_REMOVE;
}

static void on_bt_data_received(const uint8_t *data, size_t len, void *user_data)
{
    AapLink *link = user_data;

    /* Our notification request may have reached the AirPods before they
     * processed SET_FEATURES: request again once they acknowledge it. */
    if (aap_has_valid_header(data, len) &&
        aap_get_opcode(data, len) == AAP_OPCODE_FEATURES_ACK &&
        !link->battery_received) {
        g_debug("Features acknowledged, requesting notifications again");
        aap_link_send(link, AAP_PKT_REQUEST_NOTIFICATIONS, AAP_REQUEST_NOTIF_SIZE);
    }

    AapParsedPacket packet;
    AapParseResult result = aap_parse_packet(data, len, &packet);
    research_observe(data, len, result, &packet);

    if (result != AAP_PARSE_OK) {
        if (result != AAP_PARSE_UNKNOWN_OPCODE) {
            g_debug("Failed to parse packet: %d", result);
        }
        return;
    }

    if (packet.type == AAP_PKT_TYPE_BATTERY) {
        link->battery_received = true;
        cancel_notif_retry(link);
    }

    link->callbacks.packet(&packet, link->user_data);
}

static void on_bt_state_changed(BluetoothState state, const char *error, void *user_data)
{
    AapLink *link = user_data;

    switch (state) {
    case BT_STATE_CONNECTED:
        g_message("Bluetooth connected, sending handshake...");
        cancel_reconnect(link);

        /* Attach to main loop for data reception */
        bt_connection_attach_to_mainloop(link->bt_conn, NULL);

        /* Initialization sequence */
        clear_send_queue(link);
        link->next_send_us = g_get_monotonic_time() + HANDSHAKE_DELAY_MS * 1000;
        aap_link_send(link, AAP_PKT_HANDSHAKE, AAP_HANDSHAKE_SIZE);
        aap_link_send(link, AAP_PKT_SET_FEATURES, AAP_SET_FEATURES_SIZE);
        aap_link_send(link, AAP_PKT_REQUEST_NOTIFICATIONS, AAP_REQUEST_NOTIF_SIZE);

        start_notif_retry(link);

        if (link->keys_request_id > 0)
            g_source_remove(link->keys_request_id);
        link->keys_request_id = g_timeout_add_seconds(PROXIMITY_KEYS_DELAY_SEC,
                                                      request_proximity_keys_cb, link);

        if (link->silent_reconnect) {
            /* Clients never saw the link go down: keep the current state
             * and don't announce a new connection. */
            link->silent_reconnect = false;
            g_message("AirPods link re-opened");
            break;
        }

        link->callbacks.connected(link->address, link->name, link->user_data);
        break;

    case BT_STATE_DISCONNECTED:
        g_message("Bluetooth disconnected");
        clear_send_queue(link);

        if (link->silent_reconnect) {
            if (link->silent_reconnect_id == 0)
                link->silent_reconnect_id = g_timeout_add(SILENT_RECONNECT_DELAY_MS,
                                                          silent_reconnect_cb, link);
            break;
        }

        report_disconnected(link);

        /* L2CAP dropped but the device is still connected at BlueZ level
         * (e.g. AirPods went idle): try to re-establish the link. */
        schedule_reconnect(link);
        break;

    case BT_STATE_ERROR:
        g_warning("Bluetooth error: %s", error ? error : "unknown");
        clear_send_queue(link);

        /* The silent reconnect failed: the link is really down now */
        if (link->silent_reconnect)
            report_disconnected(link);

        schedule_reconnect(link);
        break;

    default:
        break;
    }
}

/* ============================================================================
 * Public API
 * ========================================================================== */

static void connect_link(AapLink *link)
{
    if (aap_link_is_connected(link)) {
        g_message("Already connected, ignoring connect request");
        return;
    }

    if (link->bt_conn == NULL) {
        link->bt_conn = bt_connection_new();
        bt_connection_set_data_callback(link->bt_conn, on_bt_data_received, link);
        bt_connection_set_state_callback(link->bt_conn, on_bt_state_changed, link);
    }

    g_message("Connecting to AirPods: %s (%s)", link->name, link->address);

    if (!bt_connection_connect(link->bt_conn, link->address)) {
        g_warning("Failed to initiate connection");
    }
}

AapLink *aap_link_new(const AapLinkCallbacks *callbacks, void *user_data)
{
    AapLink *link = g_new0(AapLink, 1);
    g_queue_init(&link->send_queue);
    link->callbacks = *callbacks;
    link->user_data = user_data;
    return link;
}

void aap_link_free(AapLink *link)
{
    if (link == NULL)
        return;

    cancel_reconnect(link);
    cancel_notif_retry(link);
    clear_send_queue(link);
    if (link->silent_reconnect_id > 0)
        g_source_remove(link->silent_reconnect_id);
    if (link->keys_request_id > 0)
        g_source_remove(link->keys_request_id);

    /* Stopping is not losing the AirPods: no disconnection to report,
     * no reconnection to schedule */
    if (link->bt_conn != NULL) {
        bt_connection_set_state_callback(link->bt_conn, NULL, NULL);
        bt_connection_set_data_callback(link->bt_conn, NULL, NULL);
    }
    bt_connection_free(link->bt_conn);
    g_free(link->address);
    g_free(link->name);
    g_free(link);
}

void aap_link_device_connected(AapLink *link, const char *address, const char *name)
{
    link->bluez_connected = true;
    link->reconnect_attempts = 0;
    link->silent_reconnect_done = false;

    if (aap_link_is_connected(link)) {
        g_message("Already connected, ignoring connect request");
        return;
    }

    /* Dup first: address/name may alias the stored ones */
    char *address_copy = g_strdup(address);
    char *name_copy = g_strdup(name);
    g_free(link->address);
    g_free(link->name);
    link->address = address_copy;
    link->name = name_copy;

    connect_link(link);
}

void aap_link_device_disconnected(AapLink *link)
{
    link->bluez_connected = false;
    cancel_reconnect(link);

    /* The AirPods left during a silent reconnect: clients still think
     * they are connected, so report it now. */
    if (link->silent_reconnect)
        report_disconnected(link);

    if (link->bt_conn)
        bt_connection_disconnect(link->bt_conn);
}

bool aap_link_is_connected(AapLink *link)
{
    return link->bt_conn != NULL && bt_connection_is_connected(link->bt_conn);
}

bool aap_link_send(AapLink *link, const uint8_t *data, size_t len)
{
    if (!aap_link_is_connected(link))
        return false;

    g_queue_push_tail(&link->send_queue, g_bytes_new(data, len));
    pump_send_queue(link);
    return true;
}
