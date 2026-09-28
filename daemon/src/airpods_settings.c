/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "airpods_settings.h"
#include <glib.h>

/* Values checked on AirPods Pro 2 USB-C against the iPhone settings screens:
 * every scale is 0 = default, 1 and 2 = the next steps. */
static const AirPodsSettingDef settings[AIRPODS_SETTING_COUNT] = {
    { "OneBudANC",          0x1B, AIRPODS_SETTING_BOOL,   0 },
    { "PersonalizedVolume", 0x26, AIRPODS_SETTING_BOOL,   0 },
    { "SleepDetection",     0x35, AIRPODS_SETTING_BOOL,   0 },
    { "PressSpeed",         0x17, AIRPODS_SETTING_CHOICE, 2 },  /* default, slower, slowest */
    { "PressHoldDuration",  0x18, AIRPODS_SETTING_CHOICE, 2 },  /* default, shorter, shortest */
    { "VolumeSwipe",        0x25, AIRPODS_SETTING_BOOL,   0 },
    { "VolumeSwipeSpeed",   0x23, AIRPODS_SETTING_CHOICE, 2 },  /* default, longer, longest */
};

const AirPodsSettingDef *airpods_setting_at(int index)
{
    if (index < 0 || index >= AIRPODS_SETTING_COUNT)
        return NULL;
    return &settings[index];
}

int airpods_setting_index(const AirPodsSettingDef *def)
{
    for (int i = 0; i < AIRPODS_SETTING_COUNT; i++) {
        if (def == &settings[i])
            return i;
    }
    return -1;
}

const AirPodsSettingDef *airpods_setting_by_key(const char *key)
{
    if (key == NULL)
        return NULL;

    for (int i = 0; i < AIRPODS_SETTING_COUNT; i++) {
        if (g_strcmp0(settings[i].key, key) == 0)
            return &settings[i];
    }
    return NULL;
}

const AirPodsSettingDef *airpods_setting_by_id(uint8_t id)
{
    for (int i = 0; i < AIRPODS_SETTING_COUNT; i++) {
        if (settings[i].id == id)
            return &settings[i];
    }
    return NULL;
}

bool airpods_setting_encode(const AirPodsSettingDef *def, int value, uint8_t *byte)
{
    switch (def->kind) {
    case AIRPODS_SETTING_BOOL:
        if (value != 0 && value != 1)
            return false;
        *byte = value ? 0x01 : 0x02;
        return true;

    case AIRPODS_SETTING_CHOICE:
        if (value < 0 || value > def->max)
            return false;
        *byte = (uint8_t)value;
        return true;
    }
    return false;
}

int airpods_setting_decode(const AirPodsSettingDef *def, uint8_t byte)
{
    switch (def->kind) {
    case AIRPODS_SETTING_BOOL:
        if (byte == 0x01)
            return 1;
        if (byte == 0x02)
            return 0;
        return -1;

    case AIRPODS_SETTING_CHOICE:
        return byte <= def->max ? byte : -1;
    }
    return -1;
}
