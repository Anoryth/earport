/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * AirPods settings exposed through the generic Settings D-Bus API.
 * Each one is a control command (04 00 04 00 09 00 [ID] [value] ...) that
 * the AirPods announce right after the handshake.
 */

#ifndef AIRPODS_SETTINGS_H
#define AIRPODS_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    AIRPODS_SETTING_BOOL,    /* 01 = on, 02 = off */
    AIRPODS_SETTING_CHOICE,  /* 0..max, 0 = default */
} AirPodsSettingKind;

typedef struct {
    const char *key;         /* D-Bus key in the Settings property */
    uint8_t id;              /* Control command identifier */
    AirPodsSettingKind kind;
    uint8_t max;             /* Highest value for choices */
} AirPodsSettingDef;

/* Number of known settings (size of the state storage) */
#define AIRPODS_SETTING_COUNT 7

/* Setting at a given index (0..AIRPODS_SETTING_COUNT-1) */
const AirPodsSettingDef *airpods_setting_at(int index);

/* Index of a setting in the table, -1 if unknown */
int airpods_setting_index(const AirPodsSettingDef *def);

/* Look a setting up by D-Bus key or control ID, NULL if unknown */
const AirPodsSettingDef *airpods_setting_by_key(const char *key);
const AirPodsSettingDef *airpods_setting_by_id(uint8_t id);

/* Convert a D-Bus value (0/1 for booleans) to the byte sent to the AirPods.
 * Returns false if the value is out of range. */
bool airpods_setting_encode(const AirPodsSettingDef *def, int value, uint8_t *byte);

/* Convert a byte from the AirPods to a D-Bus value, -1 if unexpected */
int airpods_setting_decode(const AirPodsSettingDef *def, uint8_t byte);

#endif /* AIRPODS_SETTINGS_H */
