/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Stand-in for bluetooth.c, linked into tests instead of it: same API and
 * same state transitions (connect is synchronous: CONNECTING then
 * CONNECTED, or ERROR then back to DISCONNECTED), no socket. The test
 * decides whether connections succeed, feeds received packets and reads
 * back what was sent.
 */

#include "fake_bluetooth.h"

struct BluetoothConnection {
    BluetoothState state;
    BtDataCallback data_callback;
    void *data_user_data;
    BtStateCallback state_callback;
    void *state_user_data;
};

FakeBluetooth fake_bt;

void fake_bt_reset(void)
{
    if (fake_bt.sent != NULL)
        g_ptr_array_unref(fake_bt.sent);
    fake_bt = (FakeBluetooth) {
        .sent = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref),
    };
}

static void set_state(BluetoothConnection *conn, BluetoothState state, const char *error)
{
    conn->state = state;
    if (conn->state_callback)
        conn->state_callback(state, error, conn->state_user_data);
}

BluetoothConnection *bt_connection_new(void)
{
    BluetoothConnection *conn = g_new0(BluetoothConnection, 1);
    conn->state = BT_STATE_DISCONNECTED;
    fake_bt.conn = conn;
    return conn;
}

void bt_connection_free(BluetoothConnection *conn)
{
    if (conn == NULL)
        return;
    bt_connection_disconnect(conn);
    if (fake_bt.conn == conn)
        fake_bt.conn = NULL;
    g_free(conn);
}

void bt_connection_set_data_callback(BluetoothConnection *conn, BtDataCallback callback,
                                     void *user_data)
{
    conn->data_callback = callback;
    conn->data_user_data = user_data;
}

void bt_connection_set_state_callback(BluetoothConnection *conn, BtStateCallback callback,
                                      void *user_data)
{
    conn->state_callback = callback;
    conn->state_user_data = user_data;
}

bool bt_connection_connect(BluetoothConnection *conn, const char *address)
{
    (void)address;
    if (conn->state != BT_STATE_DISCONNECTED)
        return false;

    fake_bt.connect_count++;
    if (fake_bt.refuse_connects > 0) {
        fake_bt.refuse_connects--;
        set_state(conn, BT_STATE_ERROR, "Connection refused");
        conn->state = BT_STATE_DISCONNECTED;
        return false;
    }

    set_state(conn, BT_STATE_CONNECTING, NULL);
    set_state(conn, BT_STATE_CONNECTED, NULL);
    return true;
}

void bt_connection_disconnect(BluetoothConnection *conn)
{
    if (conn->state != BT_STATE_DISCONNECTED)
        set_state(conn, BT_STATE_DISCONNECTED, NULL);
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
    if (conn->state != BT_STATE_CONNECTED)
        return -1;
    g_ptr_array_add(fake_bt.sent, g_bytes_new(data, len));
    return (ssize_t)len;
}

int bt_connection_get_fd(BluetoothConnection *conn)
{
    (void)conn;
    return -1;
}

bool bt_connection_attach_to_mainloop(BluetoothConnection *conn, GMainContext *context)
{
    (void)conn;
    (void)context;
    return true;
}

void bt_connection_detach_from_mainloop(BluetoothConnection *conn)
{
    (void)conn;
}

void fake_bt_receive(const uint8_t *data, size_t len)
{
    BluetoothConnection *conn = fake_bt.conn;
    g_assert_nonnull(conn);
    g_assert_cmpint(conn->state, ==, BT_STATE_CONNECTED);
    if (conn->data_callback)
        conn->data_callback(data, len, conn->data_user_data);
}

void fake_bt_peer_closes(void)
{
    g_assert_nonnull(fake_bt.conn);
    bt_connection_disconnect(fake_bt.conn);
}

guint fake_bt_count_sent(const uint8_t *prefix, size_t len)
{
    guint count = 0;
    for (guint i = 0; i < fake_bt.sent->len; i++) {
        gsize size;
        const uint8_t *data = g_bytes_get_data(fake_bt.sent->pdata[i], &size);
        if (size >= len && memcmp(data, prefix, len) == 0)
            count++;
    }
    return count;
}
