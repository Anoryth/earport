/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * BLE discovery through BlueZ, reporting Apple manufacturer data.
 * Standard BlueZ API only: no experimental features, no VendorID change.
 */

#ifndef BLE_SCANNER_H
#define BLE_SCANNER_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>

/* Whether an address is worth reporting (e.g. resolves with our IRK), given
 * its first Apple manufacturer data. Called once per address and cached. */
typedef bool (*BleAddressFilter)(const char *address, const uint8_t *data,
                                 size_t len, void *user_data);

/* Apple manufacturer data (company 0x004C) seen for a matching address */
typedef void (*BleAdvertCallback)(const char *address, const uint8_t *data,
                                  size_t len, void *user_data);

typedef struct BleScanner BleScanner;

BleScanner *ble_scanner_new(const char *adapter_path,
                            BleAddressFilter filter,
                            BleAdvertCallback callback,
                            void *user_data);

void ble_scanner_free(BleScanner *scanner);

/* Start/stop LE discovery (asynchronous, errors are only logged) */
void ble_scanner_start(BleScanner *scanner);
void ble_scanner_stop(BleScanner *scanner);
bool ble_scanner_is_running(const BleScanner *scanner);

/* Forget which addresses matched (e.g. after the IRK changed) */
void ble_scanner_reset_filter(BleScanner *scanner);

#endif /* BLE_SCANNER_H */
