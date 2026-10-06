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
#include <glib-unix.h>

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

    /* Connection in progress */
    guint connect_watch_id;
    guint connect_timeout_id;
};

/* The connection runs in the background (non-blocking socket watched from
 * the main loop): a slow or absent device must not freeze the service.
 * The kernel gives up on its own, but not always quickly. */
#define CONNECT_TIMEOUT_SEC 20

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

/* "AC:07:75:F0:31:02" -> bdaddr_t, least significant byte first. Done here
 * rather than with libbluetooth's str2ba(), the only function the daemon
 * used from it: the binary then needs nothing but GLib at runtime. */
static bool parse_bdaddr(const char *address, bdaddr_t *bdaddr)
{
    unsigned int b[6];
    char extra;

    if (address == NULL ||
        sscanf(address, "%2x:%2x:%2x:%2x:%2x:%2x%c",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &extra) != 6)
        return false;

    for (int i = 0; i < 6; i++)
        bdaddr->b[5 - i] = (uint8_t)b[i];
    return true;
}

static void stop_connecting(BluetoothConnection *conn)
{
    if (conn->connect_watch_id > 0) {
        g_source_remove(conn->connect_watch_id);
        conn->connect_watch_id = 0;
    }
    if (conn->connect_timeout_id > 0) {
        g_source_remove(conn->connect_timeout_id);
        conn->connect_timeout_id = 0;
    }
}

static void connect_failed(BluetoothConnection *conn, const char *error)
{
    stop_connecting(conn);
    g_warning("Failed to connect to %s: %s", conn->address, error);
    if (conn->socket_fd >= 0) {
        close(conn->socket_fd);
        conn->socket_fd = -1;
    }
    set_state(conn, BT_STATE_ERROR, error);
    /* Leave the state machine ready for another attempt */
    conn->state = BT_STATE_DISCONNECTED;
}

static void connect_succeeded(BluetoothConnection *conn)
{
    stop_connecting(conn);
    g_message("Connected to %s", conn->address);
    set_state(conn, BT_STATE_CONNECTED, NULL);
}

static gboolean on_connect_ready(gint fd, GIOCondition condition, gpointer user_data)
{
    (void)condition;
    BluetoothConnection *conn = user_data;
    int err = 0;
    socklen_t len = sizeof(err);

    conn->connect_watch_id = 0;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        err = errno;

    if (err == 0)
        connect_succeeded(conn);
    else
        connect_failed(conn, strerror(err));
    return G_SOURCE_REMOVE;
}

static gboolean on_connect_timeout(gpointer user_data)
{
    BluetoothConnection *conn = user_data;
    conn->connect_timeout_id = 0;
    connect_failed(conn, "Connection timed out");
    return G_SOURCE_REMOVE;
}

bool bt_connection_connect(BluetoothConnection *conn, const char *address)
{
    if (conn->state != BT_STATE_DISCONNECTED) {
        g_warning("Cannot connect: already connected or connecting");
        return false;
    }

    bdaddr_t bdaddr;
    if (!parse_bdaddr(address, &bdaddr)) {
        g_warning("Cannot connect: invalid address '%s'", address ? address : "");
        return false;
    }

    g_free(conn->address);
    conn->address = g_strdup(address);

    /* Create L2CAP socket, non-blocking from the start */
    conn->socket_fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK, BTPROTO_L2CAP);
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

    /* Wait for an encrypted link: some AirPods (seen with Pro 3) refuse the
     * channel on a link not encrypted yet ("Permission denied") */
    struct bt_security security = { .level = BT_SECURITY_MEDIUM };
    if (setsockopt(conn->socket_fd, SOL_BLUETOOTH, BT_SECURITY, &security, sizeof(security)) < 0)
        g_debug("Could not ask for an encrypted link: %s", strerror(errno));

    /* Prepare destination address */
    struct sockaddr_l2 addr;
    memset(&addr, 0, sizeof(addr));
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = htobs(AIRPODS_L2CAP_PSM);
    bacpy(&addr.l2_bdaddr, &bdaddr);

    set_state(conn, BT_STATE_CONNECTING, NULL);

    g_message("Connecting to %s on PSM 0x%04X...", address, AIRPODS_L2CAP_PSM);

    if (connect(conn->socket_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        connect_succeeded(conn);
        return true;
    }

    if (errno != EINPROGRESS) {
        connect_failed(conn, strerror(errno));
        return false;
    }

    /* Writable once connected, or with SO_ERROR set if it failed */
    conn->connect_watch_id = g_unix_fd_add(conn->socket_fd, G_IO_OUT | G_IO_ERR | G_IO_HUP,
                                           on_connect_ready, conn);
    conn->connect_timeout_id = g_timeout_add_seconds(CONNECT_TIMEOUT_SEC, on_connect_timeout, conn);
    return true;
}

void bt_connection_disconnect(BluetoothConnection *conn)
{
    stop_connecting(conn);

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
