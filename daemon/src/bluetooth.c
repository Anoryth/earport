/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "bluetooth.h"
#include "aap_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

struct BluetoothConnection {
    int socket_fd;
    BluetoothState state;
    char *address;

    BtDataCallback data_callback;
    void *data_user_data;

    BtStateCallback state_callback;
    void *state_user_data;

    GSource *source;
    uint8_t recv_buffer[BT_MAX_PACKET_SIZE];
};

BluetoothConnection *bt_connection_new(void)
{
    BluetoothConnection *conn = g_new0(BluetoothConnection, 1);
    conn->socket_fd = -1;
    conn->state = BT_STATE_DISCONNECTED;
    conn->address = NULL;
    conn->source = NULL;
    return conn;
}

void bt_connection_free(BluetoothConnection *conn)
{
    if (conn == NULL)
        return;

    bt_connection_disconnect(conn);
    g_free(conn->address);
    g_free(conn);
}

void bt_connection_set_data_callback(BluetoothConnection *conn,
                                      BtDataCallback callback,
                                      void *user_data)
{
    conn->data_callback = callback;
    conn->data_user_data = user_data;
}

void bt_connection_set_state_callback(BluetoothConnection *conn,
                                       BtStateCallback callback,
                                       void *user_data)
{
    conn->state_callback = callback;
    conn->state_user_data = user_data;
}

static void set_state(BluetoothConnection *conn, BluetoothState state, const char *error)
{
    conn->state = state;
    if (conn->state_callback) {
        conn->state_callback(state, error, conn->state_user_data);
    }
}

bool bt_connection_connect(BluetoothConnection *conn, const char *address)
{
    if (conn->state != BT_STATE_DISCONNECTED) {
        g_warning("Cannot connect: already connected or connecting");
        return false;
    }

    /* Create L2CAP socket */
    conn->socket_fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (conn->socket_fd < 0) {
        int err = errno;
        g_warning("Failed to create L2CAP socket: %s", strerror(err));
        set_state(conn, BT_STATE_ERROR, strerror(err));
        /* Leave the state machine ready for another attempt */
        conn->state = BT_STATE_DISCONNECTED;
        return false;
    }

    /* Set socket options */
    struct l2cap_options opts;
    socklen_t optlen = sizeof(opts);
    if (getsockopt(conn->socket_fd, SOL_L2CAP, L2CAP_OPTIONS, &opts, &optlen) == 0) {
        opts.imtu = BT_MAX_PACKET_SIZE;
        opts.omtu = BT_MAX_PACKET_SIZE;
        setsockopt(conn->socket_fd, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts));
    }

    /* Prepare destination address */
    struct sockaddr_l2 addr;
    memset(&addr, 0, sizeof(addr));
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = htobs(AIRPODS_L2CAP_PSM);
    str2ba(address, &addr.l2_bdaddr);

    g_free(conn->address);
    conn->address = g_strdup(address);

    set_state(conn, BT_STATE_CONNECTING, NULL);

    g_message("Connecting to %s on PSM 0x%04X...", address, AIRPODS_L2CAP_PSM);

    /* Connect (blocking for now, could be made async) */
    if (connect(conn->socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err = errno;
        g_warning("Failed to connect to %s: %s", address, strerror(err));
        close(conn->socket_fd);
        conn->socket_fd = -1;
        set_state(conn, BT_STATE_ERROR, strerror(err));
        /* Leave the state machine ready for another attempt */
        conn->state = BT_STATE_DISCONNECTED;
        return false;
    }

    /* Set non-blocking after connect */
    int flags = fcntl(conn->socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(conn->socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        g_warning("Failed to set non-blocking mode: %s", strerror(errno));
    }

    g_message("Connected to %s", address);
    set_state(conn, BT_STATE_CONNECTED, NULL);

    return true;
}

void bt_connection_disconnect(BluetoothConnection *conn)
{
    if (conn->source) {
        g_source_destroy(conn->source);
        g_source_unref(conn->source);
        conn->source = NULL;
    }

    if (conn->socket_fd >= 0) {
        close(conn->socket_fd);
        conn->socket_fd = -1;
    }

    if (conn->state != BT_STATE_DISCONNECTED) {
        set_state(conn, BT_STATE_DISCONNECTED, NULL);
    }
}

bool bt_connection_is_connected(BluetoothConnection *conn)
{
    return conn->state == BT_STATE_CONNECTED;
}

BluetoothState bt_connection_get_state(BluetoothConnection *conn)
{
    return conn->state;
}

ssize_t bt_connection_send(BluetoothConnection *conn, const uint8_t *data, size_t len)
{
    if (conn->socket_fd < 0 || conn->state != BT_STATE_CONNECTED) {
        g_warning("Cannot send: not connected");
        return -1;
    }

    aap_debug_print_packet("TX", data, len);

    ssize_t sent = send(conn->socket_fd, data, len, 0);
    if (sent < 0) {
        g_warning("Send failed: %s", strerror(errno));
        if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
            bt_connection_disconnect(conn);
        }
    }

    return sent;
}

bool bt_connection_send_handshake(BluetoothConnection *conn)
{
    ssize_t sent = bt_connection_send(conn, AAP_PKT_HANDSHAKE, AAP_HANDSHAKE_SIZE);
    return sent == AAP_HANDSHAKE_SIZE;
}

bool bt_connection_send_request_notifications(BluetoothConnection *conn)
{
    ssize_t sent = bt_connection_send(conn, AAP_PKT_REQUEST_NOTIFICATIONS, AAP_REQUEST_NOTIF_SIZE);
    return sent == AAP_REQUEST_NOTIF_SIZE;
}

bool bt_connection_send_set_features(BluetoothConnection *conn)
{
    ssize_t sent = bt_connection_send(conn, AAP_PKT_SET_FEATURES, AAP_SET_FEATURES_SIZE);
    return sent == AAP_SET_FEATURES_SIZE;
}

int bt_connection_get_fd(BluetoothConnection *conn)
{
    return conn->socket_fd;
}

/* GSource callbacks for main loop integration */
typedef struct {
    GSource source;
    BluetoothConnection *conn;
    GPollFD poll_fd;
} BtSource;

static gboolean bt_source_prepare(GSource *source G_GNUC_UNUSED, gint *timeout)
{
    *timeout = -1;
    return FALSE;
}

static gboolean bt_source_check(GSource *source)
{
    BtSource *bt_source = (BtSource *)source;
    return (bt_source->poll_fd.revents & G_IO_IN) != 0;
}

static gboolean bt_source_dispatch(GSource *source,
                                    GSourceFunc callback G_GNUC_UNUSED,
                                    gpointer user_data G_GNUC_UNUSED)
{
    BtSource *bt_source = (BtSource *)source;
    BluetoothConnection *conn = bt_source->conn;

    if (bt_source->poll_fd.revents & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        g_warning("Socket error or hangup");
        bt_connection_disconnect(conn);
        return G_SOURCE_REMOVE;
    }

    if (bt_source->poll_fd.revents & G_IO_IN) {
        ssize_t len = recv(conn->socket_fd, conn->recv_buffer, BT_MAX_PACKET_SIZE, 0);

        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                g_warning("Receive error: %s", strerror(errno));
                bt_connection_disconnect(conn);
                return G_SOURCE_REMOVE;
            }
        } else if (len == 0) {
            g_message("Connection closed by peer");
            bt_connection_disconnect(conn);
            return G_SOURCE_REMOVE;
        } else {
            aap_debug_print_packet("RX", conn->recv_buffer, len);

            if (conn->data_callback) {
                conn->data_callback(conn->recv_buffer, len, conn->data_user_data);
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

static void bt_source_finalize(GSource *source G_GNUC_UNUSED)
{
    /* Nothing to clean up */
}

static GSourceFuncs bt_source_funcs = {
    .prepare = bt_source_prepare,
    .check = bt_source_check,
    .dispatch = bt_source_dispatch,
    .finalize = bt_source_finalize,
};

bool bt_connection_attach_to_mainloop(BluetoothConnection *conn, GMainContext *context)
{
    if (conn->socket_fd < 0) {
        g_warning("Cannot attach: not connected");
        return false;
    }

    if (conn->source != NULL) {
        g_warning("Already attached to main loop");
        return false;
    }

    BtSource *bt_source = (BtSource *)g_source_new(&bt_source_funcs, sizeof(BtSource));
    bt_source->conn = conn;
    bt_source->poll_fd.fd = conn->socket_fd;
    bt_source->poll_fd.events = G_IO_IN | G_IO_HUP | G_IO_ERR;
    bt_source->poll_fd.revents = 0;

    g_source_add_poll((GSource *)bt_source, &bt_source->poll_fd);
    g_source_attach((GSource *)bt_source, context);

    conn->source = (GSource *)bt_source;

    return true;
}

void bt_connection_detach_from_mainloop(BluetoothConnection *conn)
{
    if (conn->source) {
        g_source_destroy(conn->source);
        g_source_unref(conn->source);
        conn->source = NULL;
    }
}
