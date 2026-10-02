/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "aes128.h"
#include <stdbool.h>
#include <string.h>

static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

/* Multiplication by x in GF(2^8) */
static uint8_t xtime(uint8_t b)
{
    return (uint8_t)((b << 1) ^ ((b & 0x80) ? 0x1b : 0x00));
}

/* 11 round keys of 16 bytes (FIPS-197 5.2) */
static void expand_key(const uint8_t key[16], uint8_t round_keys[176])
{
    uint8_t rcon = 0x01;

    memcpy(round_keys, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, &round_keys[i - 4], 4);
        if (i % 16 == 0) {
            /* RotWord, SubWord, Rcon */
            uint8_t first = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++)
            round_keys[i + j] = round_keys[i - 16 + j] ^ t[j];
    }
}

/* SubBytes and ShiftRows together; the state is column-major */
static void sub_shift(uint8_t s[16])
{
    uint8_t t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            t[4 * c + r] = sbox[s[4 * ((c + r) % 4) + r]];
    memcpy(s, t, 16);
}

static void mix_columns(uint8_t s[16])
{
    for (int c = 0; c < 4; c++) {
        uint8_t *col = &s[4 * c];
        uint8_t all = col[0] ^ col[1] ^ col[2] ^ col[3];
        uint8_t first = col[0];
        col[0] ^= all ^ xtime(col[0] ^ col[1]);
        col[1] ^= all ^ xtime(col[1] ^ col[2]);
        col[2] ^= all ^ xtime(col[2] ^ col[3]);
        col[3] ^= all ^ xtime(col[3] ^ first);
    }
}

static void add_round_key(uint8_t s[16], const uint8_t *round_key)
{
    for (int i = 0; i < 16; i++)
        s[i] ^= round_key[i];
}

void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t round_keys[176], s[16];

    expand_key(key, round_keys);
    memcpy(s, in, 16);

    add_round_key(s, round_keys);
    for (int round = 1; round < 10; round++) {
        sub_shift(s);
        mix_columns(s);
        add_round_key(s, &round_keys[16 * round]);
    }
    sub_shift(s);
    add_round_key(s, &round_keys[160]);

    memcpy(out, s, 16);
}

/* ============================================================================
 * Decryption (FIPS-197 5.3)
 * ========================================================================== */

/* The inverse S-box, derived from the S-box rather than typed again */
static const uint8_t *inv_sbox(void)
{
    static uint8_t inv[256];
    static bool ready;

    if (!ready) {
        for (int i = 0; i < 256; i++)
            inv[sbox[i]] = (uint8_t)i;
        ready = true;
    }
    return inv;
}

/* Multiplication in GF(2^8) */
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    while (b) {
        if (b & 1)
            p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

/* InvShiftRows and InvSubBytes together */
static void inv_shift_sub(uint8_t s[16])
{
    const uint8_t *inv = inv_sbox();
    uint8_t t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            t[4 * ((c + r) % 4) + r] = inv[s[4 * c + r]];
    memcpy(s, t, 16);
}

static void inv_mix_columns(uint8_t s[16])
{
    for (int c = 0; c < 4; c++) {
        uint8_t *col = &s[4 * c];
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
        col[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
        col[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
        col[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
    }
}

void aes128_decrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t round_keys[176], s[16];

    expand_key(key, round_keys);
    memcpy(s, in, 16);

    add_round_key(s, &round_keys[160]);
    for (int round = 9; round >= 1; round--) {
        inv_shift_sub(s);
        add_round_key(s, &round_keys[16 * round]);
        inv_mix_columns(s);
    }
    inv_shift_sub(s);
    add_round_key(s, round_keys);

    memcpy(out, s, 16);
}
