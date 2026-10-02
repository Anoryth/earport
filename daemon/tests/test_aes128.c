/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * AES-128 tests against the FIPS-197 vectors
 */

#include <glib.h>
#include <string.h>

#include "aes128.h"

static void test_fips197_appendix_b(void)
{
    const uint8_t key[16] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
    };
    const uint8_t plain[16] = {
        0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a, 0x30, 0x8d,
        0x31, 0x31, 0x98, 0xa2, 0xe0, 0x37, 0x07, 0x34,
    };
    const uint8_t expected[16] = {
        0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc, 0x09, 0xfb,
        0xdc, 0x11, 0x85, 0x97, 0x19, 0x6a, 0x0b, 0x32,
    };
    uint8_t out[16];

    aes128_encrypt_block(key, plain, out);
    g_assert_cmpmem(out, 16, expected, 16);
}

static void test_fips197_appendix_c1(void)
{
    uint8_t key[16], block[16];
    const uint8_t expected[16] = {
        0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
    };

    for (int i = 0; i < 16; i++) {
        key[i] = (uint8_t)i;
        block[i] = (uint8_t)(i * 0x11);
    }

    /* In place */
    aes128_encrypt_block(key, block, block);
    g_assert_cmpmem(block, 16, expected, 16);
}

/* FIPS-197 C.1, the other way, and back */
static void test_decrypt(void)
{
    uint8_t key[16], plain[16], block[16];
    const uint8_t cipher[16] = {
        0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
    };

    for (int i = 0; i < 16; i++) {
        key[i] = (uint8_t)i;
        plain[i] = (uint8_t)(i * 0x11);
    }

    aes128_decrypt_block(key, cipher, block);
    g_assert_cmpmem(block, 16, plain, 16);

    aes128_encrypt_block(key, block, block);
    aes128_decrypt_block(key, block, block);
    g_assert_cmpmem(block, 16, plain, 16);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/aes128/fips197-appendix-b", test_fips197_appendix_b);
    g_test_add_func("/aes128/fips197-appendix-c1", test_fips197_appendix_c1);
    g_test_add_func("/aes128/decrypt", test_decrypt);

    return g_test_run();
}
