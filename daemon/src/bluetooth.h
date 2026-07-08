/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Bluetooth L2CAP connection management
 */

#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>

/* AirPods L2CAP PSM */
#define AIRPODS_L2CAP_PSM 0x1001

/* AirPods Service UUID */
#define AIRPODS_UUID "74ec2172-0bad-4d01-8f77-997b2be0722a"

/* Maximum packet size */
#define BT_MAX_PACKET_SIZE 1024

/* Connection states */
typedef enum {
    BT_STATE_DISCONNECTED,
    BT_STATE_CONNECTING,
    BT_STATE_CONNECTED,
    BT_STATE_ERROR,
} BluetoothState;

/* Callback types */
typedef void (*BtDataCallback)(const uint8_t *data, size_t len, void *user_data);
typedef void (*BtStateCallback)(BluetoothState state, const char *error, void *user_data);

/* Bluetooth connection context */
typedef struct BluetoothConnection BluetoothConnection;

/**
 * Create a new Bluetooth connection context
 *
 * @return New connection context or NULL on error
 */
BluetoothConnection *bt_connection_new(void);

/**
 * Free Bluetooth connection context
 */
void bt_connection_free(BluetoothConnection *conn);

/**
 * Set data received callback
 */
void bt_connection_set_data_callback(BluetoothConnection *conn,
                                      BtDataCallback callback,
                                      void *user_data);

/**
 * Set state change callback
 */
void bt_connection_set_state_callback(BluetoothConnection *conn,
                                       BtStateCallback callback,
                                       void *user_data);

/**
 * Connect to AirPods device
 *
 * @param conn Connection context
 * @param address Bluetooth MAC address (XX:XX:XX:XX:XX:XX)
 * @return true if connection initiated, false on error
 */
bool bt_connection_connect(BluetoothConnection *conn, const char *address);

/**
 * Disconnect from device
 */
void bt_connection_disconnect(BluetoothConnection *conn);

/**
 * Check if connected
 */
bool bt_connection_is_connected(BluetoothConnection *conn);

/**
 * Get current connection state
 */
BluetoothState bt_connection_get_state(BluetoothConnection *conn);

/**
 * Send data to device
 *
 * @param conn Connection context
 * @param data Data to send
 * @param len Data length
 * @return Number of bytes sent, or -1 on error
 */
ssize_t bt_connection_send(BluetoothConnection *conn, const uint8_t *data, size_t len);

/**
 * Send handshake packet
 */
bool bt_connection_send_handshake(BluetoothConnection *conn);

/**
 * Send request notifications packet
 */
bool bt_connection_send_request_notifications(BluetoothConnection *conn);

/**
 * Send set features packet
 */
bool bt_connection_send_set_features(BluetoothConnection *conn);

/**
 * Get file descriptor for event loop integration
 * Returns -1 if not connected
 */
int bt_connection_get_fd(BluetoothConnection *conn);

/**
 * Attach connection to GLib main loop
 * This sets up a GSource to monitor the socket
 */
bool bt_connection_attach_to_mainloop(BluetoothConnection *conn, GMainContext *context);

/**
 * Detach from main loop
 */
void bt_connection_detach_from_mainloop(BluetoothConnection *conn);

#endif /* BLUETOOTH_H */
