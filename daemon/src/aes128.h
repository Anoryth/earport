/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Minimal AES-128 block encryption (FIPS-197), enough to resolve BLE
 * random addresses. Built in so that prebuilt daemon binaries need
 * nothing but GLib at runtime. Not constant-time: only used on public
 * advertisement data with a key that never leaves this machine.
 */

#ifndef AES128_H
#define AES128_H

#include <stdint.h>

/* Encrypt one 16-byte block; in and out may be the same buffer */
void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

#endif /* AES128_H */
