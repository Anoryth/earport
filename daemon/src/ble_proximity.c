/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "ble_proximity.h"
#include "aes128.h"
#include <stdio.h>
#include <string.h>

#define PROXIMITY_TYPE 0x07
#define PROXIMITY_MIN_SIZE 11

/* Status byte (5) */
#define STATUS_POD_IN_EAR_1  0x02
#define STATUS_BOTH_IN_CASE  0x04
#define STATUS_POD_IN_EAR_2  0x08
#define STATUS_PRIMARY_LEFT  0x20

/* Encrypted block at the end of the advert: [?] [primary pod] [other pod]
 * [case], each level & 0x7F (0x7F unknown), bit 7 = charging */
#define ENCRYPTED_SIZE 16
#define LEVEL_UNKNOWN 0x7F

bool proximity_parse(const uint8_t *data, size_t len, ProximityInfo *info)
{
    /* 07 [length] [01 = paired] [model:2] [status] [batteries:2] [lid]
     * [color] [connection state] [encrypted payload...] */
    if (data == NULL || len < PROXIMITY_MIN_SIZE || data[0] != PROXIMITY_TYPE)
        return false;

    /* Pairing mode adverts use another layout */
    if (data[2] != 0x01)
        return false;

    uint8_t status = data[5];
    info->model = (uint16_t)((data[3] << 8) | data[4]);
    info->in_ear = (status & (STATUS_POD_IN_EAR_1 | STATUS_POD_IN_EAR_2)) != 0;
    info->both_in_case = (status & STATUS_BOTH_IN_CASE) != 0;
    info->connection_state = data[10];
    info->primary_left = (status & STATUS_PRIMARY_LEFT) != 0;
    return true;
}

static void read_level(uint8_t byte, int *level, bool *charging)
{
    *level = (byte & 0x7F) == LEVEL_UNKNOWN ? -1 : (byte & 0x7F);
    *charging = *level >= 0 && (byte & 0x80) != 0;
}

bool proximity_decrypt_battery(const uint8_t *enc_key, const uint8_t *data, size_t len,
                               const ProximityInfo *info, ProximityBattery *battery)
{
    if (enc_key == NULL || len < PROXIMITY_MIN_SIZE + ENCRYPTED_SIZE)
        return false;

    uint8_t plain[ENCRYPTED_SIZE];
    aes128_decrypt_block(enc_key, data + len - ENCRYPTED_SIZE, plain);

    /* Primary pod first: swapped when the right one is primary */
    int left_index = info->primary_left ? 1 : 2;
    int right_index = info->primary_left ? 2 : 1;
    read_level(plain[left_index], &battery->left, &battery->left_charging);
    read_level(plain[right_index], &battery->right, &battery->right_charging);
    read_level(plain[3], &battery->case_level, &battery->case_charging);
    return true;
}

/* Case advert: an entry of the Apple data, [07] [11] [06] [encrypted:16] */
#define CASE_ENTRY_SIZE 0x11
#define CASE_ENTRY_KIND 0x06

static bool level_byte_valid(uint8_t byte)
{
    return (byte & 0x7F) <= 100 || (byte & 0x7F) == LEVEL_UNKNOWN;
}

bool proximity_decrypt_case(const uint8_t *enc_key, const uint8_t *data, size_t len,
                            int *level, bool *charging)
{
    if (enc_key == NULL || data == NULL)
        return false;

    /* Apple data is a list of [type] [length] [value] entries */
    for (size_t i = 0; i + 2 <= len; i += 2 + data[i + 1]) {
        if (data[i] != PROXIMITY_TYPE || data[i + 1] != CASE_ENTRY_SIZE ||
            i + 2 + CASE_ENTRY_SIZE > len || data[i + 2] != CASE_ENTRY_KIND)
            continue;

        uint8_t plain[ENCRYPTED_SIZE];
        aes128_decrypt_block(enc_key, data + i + 3, plain);

        /* With another key, the block decrypts to noise: these bytes are
         * zero, and the levels in range, only with the right one */
        static const uint8_t zeros[4] = { 0 };
        if (memcmp(plain + 8, zeros, sizeof(zeros)) != 0 ||
            !level_byte_valid(plain[3]) || !level_byte_valid(plain[4]) ||
            !level_byte_valid(plain[5]))
            return false;

        read_level(plain[3], level, charging);
        return true;
    }
    return false;
}

bool proximity_address_matches(const uint8_t *irk, const char *address)
{
    unsigned int b[6];
    if (irk == NULL || address == NULL ||
        sscanf(address, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return false;

    /* Resolvable private addresses start with bits 01 */
    if ((b[0] & 0xC0) != 0x40)
        return false;

    /* Bluetooth Core Spec Vol 3 Part H 2.2.2: hash = ah(IRK, prand) =
     * AES-128(IRK, 0^104 || prand) mod 2^24, everything most significant
     * byte first. AirPods send the IRK least significant byte first. */
    uint8_t key[16], block[16] = { 0 }, out[16];
    for (int i = 0; i < 16; i++)
        key[i] = irk[15 - i];
    block[13] = (uint8_t)b[0];
    block[14] = (uint8_t)b[1];
    block[15] = (uint8_t)b[2];

    aes128_encrypt_block(key, block, out);

    return out[13] == b[3] && out[14] == b[4] && out[15] == b[5];
}

bool autoconnect_worth_waiting(const ProximityInfo *info)
{
    /* A call is not paused in a few seconds */
    return info->in_ear && info->connection_state == PROXIMITY_CONN_MUSIC;
}

bool autoconnect_allowed(const ProximityInfo *info, AutoConnectTrigger trigger)
{
    /* Audio must not jump to AirPods lying on the desk */
    if (!info->in_ear)
        return false;

    switch (info->connection_state) {
    case PROXIMITY_CONN_DISCONNECTED:
        return true;
    case PROXIMITY_CONN_IDLE:
        return trigger == AUTOCONNECT_TRIGGER_PLAYBACK;
    default:
        /* Music, call, ringing on another device, or unknown: never */
        return false;
    }
}
