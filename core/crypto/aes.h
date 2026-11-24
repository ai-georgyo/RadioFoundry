// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AES (FIPS 197), the forward cipher only, for 128-, 192- and 256-bit keys, and counter mode on it (NIST SP 800-38A
// 6.5). Counter mode never runs the inverse cipher, so there is none. Generic, the one AES in the tree: M17's counter
// layout is core/m17's (m17_crypt.h), DMR AES's OFB keystream core/dmr's (dmrAes.h). Table-free beyond the 256-byte
// S-box; no RTOS call. Built with CONFIG_CRYPTO_AES (derived from CONFIG_M17_AES or CONFIG_DMR_AES).
//
// The expanded key is a secret: keep the context in static or lent memory, never on a task stack, and aes_wipe it when
// the stream ends.
#ifndef RFOUNDRY_AES_H
#define RFOUNDRY_AES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AES_BLOCK	16u
#define AES_KEY_MAX	32u
#define AES_ROUNDS_MAX	14u

typedef struct {
	uint8_t rk[AES_BLOCK * (AES_ROUNDS_MAX + 1)];	// round keys (FIPS 197 5.2), 176 / 208 / 240 bytes used
	uint8_t nr;					// rounds: 10, 12, 14; 0 = no key
} aes_ctx_t;

// Expand key (16, 24 or 32 bytes). False for another length (ctx then holds no key)
bool aes_init(aes_ctx_t *ctx, const uint8_t *key, size_t len);
// One block, in and out may be the same
void aes_encrypt(const aes_ctx_t *ctx, const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK]);
// Counter mode: data ^= E(ctr), E(ctr + 1), ... for n bytes, ctr a 128-bit big-endian counter incremented per block
// (SP 800-38A B.1, the whole block as the counter) and left at the next unused value. A caller whose counter layout
// differs (a nonce with a frame number) builds its block and calls aes_encrypt
void aes_ctr(const aes_ctx_t *ctx, uint8_t ctr[AES_BLOCK], uint8_t *data, size_t n);
void aes_wipe(aes_ctx_t *ctx);

#endif
