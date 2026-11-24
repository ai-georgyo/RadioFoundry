// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 stream encryption (m17.md 6): the scrambler's three LFSRs, with a jump to any frame number for a late joiner,
// and the AES-CTR counter layout on the generic cipher (core/crypto/aes.h). Each covers the 16 STREAM bytes of a frame,
// never the FN or the LICH, and never the authentication or signature frames of a signed stream. Bit order: the first
// key stream bit on the MSB of STREAM byte 0 (m17.md 13). Pure functions; keys and LFSR states are secrets the caller
// wipes. Scrambler: CONFIG_M17_SCRAMBLER; AES: CONFIG_M17_AES.
#ifndef RFOUNDRY_M17_CRYPT_H
#define RFOUNDRY_M17_CRYPT_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "m17.h"
#if CONFIG_M17_AES
#include "../crypto/aes.h"
#endif

#if CONFIG_M17_SCRAMBLER
// Fibonacci LFSR of 8, 16 or 24 bits (m17.md 6.1): taps D7 D5 D4 D3 / D15 D14 D12 D3 / D23 D22 D21 D16
typedef struct {
	uint32_t reg;		// D0 in bit 0
	uint8_t bits;		// 8, 16, 24; 0 = no key
} m17_scr_t;

// Seed of 1 to 3 bytes (big endian, leading zero bytes counted: the length sets the size). False for another length or
// an all-zero seed. The subtype the size sends (M17_SCR_*): m17_scr_sub
bool m17_scr_init(m17_scr_t *s, const uint8_t *seed, uint8_t len);
// The same check without making a register (no copy of the seed's bits anywhere): what refuses a key before any
// frame, in either direction
bool m17_scr_key_ok(const uint8_t *seed, uint8_t len);
static inline uint8_t m17_scr_sub(const m17_scr_t *s)
{
	return s->bits == 8 ? M17_SCR_8 : s->bits == 16 ? M17_SCR_16 : M17_SCR_24;
}
// The register 128 x (fn & 0x7FFF) steps after the seed (a late joiner, m17.md 6.1), by GF(2) matrix powers: a few
// thousand word operations whatever fn, inside one frame time. The matrices are the caller's working memory (the
// engine's work union), never the provider task's stack
typedef struct {
	uint32_t m[2][24];	// the step matrix's power and its square being made: one column word per register bit
} m17_scr_work_t;
void m17_scr_seek(m17_scr_t *s, const uint8_t *seed, uint8_t len, uint16_t fn, m17_scr_work_t *w);
// 128 steps: the frame's key stream XORed into stream
void m17_scr_frame(m17_scr_t *s, uint8_t stream[M17_STREAM_BYTES]);
#endif

#if CONFIG_M17_AES
#define M17_AES_NONCE_RANDOM	10u	// bytes of the nonce's random part

// META of an AES stream (m17.md 6.2): the timestamp (seconds since 2020-01-01 UTC, 0 without a clock) and 80 random
// bits from entropy_get
void m17_aes_nonce(uint32_t ts, const uint8_t rnd[M17_AES_NONCE_RANDOM], uint8_t meta[M17_META_BYTES]);
// The AES subtype for a key length (16, 24, 32), or 0xFF for another
uint8_t m17_aes_sub(uint8_t key_len);
// stream ^= AES_K(META || FN), FN as sent (its end bit included, m17.md 13)
void m17_aes_frame(const aes_ctx_t *k, const uint8_t meta[M17_META_BYTES], uint16_t fn,
		   uint8_t stream[M17_STREAM_BYTES]);
#endif

#endif
