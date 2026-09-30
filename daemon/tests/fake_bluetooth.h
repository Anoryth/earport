/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Test controls of the fake Bluetooth transport (fake_bluetooth.c)
 */

#ifndef FAKE_BLUETOOTH_H
#define FAKE_BLUETOOTH_H

#include <glib.h>
#include <string.h>

#include "bluetooth.h"

typedef struct {
    BluetoothConnection *conn;   /* Last connection created */
    int connect_count;           /* L2CAP connection attempts */
    int refuse_connects;         /* Refuse this many next attempts */
    GPtrArray *sent;             /* GBytes, in sending order */
} FakeBluetooth;

extern FakeBluetooth fake_bt;

void fake_bt_reset(void);

/* The AirPods send a packet */
void fake_bt_receive(const uint8_t *data, size_t len);

/* The AirPods close the L2CAP channel */
void fake_bt_peer_closes(void);

/* Packets sent that start with these bytes */
guint fake_bt_count_sent(const uint8_t *prefix, size_t len);

#endif /* FAKE_BLUETOOTH_H */
