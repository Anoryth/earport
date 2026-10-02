/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Publishes the AirPods' battery to BlueZ (BatteryProviderManager1), so
 * that GNOME shows it natively (Settings > Power, Bluetooth panel) like
 * for any other Bluetooth device.
 */

#ifndef BATTERY_PROVIDER_H
#define BATTERY_PROVIDER_H

typedef struct BatteryProvider BatteryProvider;

/* NULL without a system bus */
BatteryProvider *battery_provider_new(void);
void battery_provider_free(BatteryProvider *bp);

/* Battery of the AirPods at this address, on this adapter; a negative
 * percentage removes it (disconnected, unknown) */
void battery_provider_update(BatteryProvider *bp, const char *adapter_path,
                             const char *address, int percentage);

#endif /* BATTERY_PROVIDER_H */
