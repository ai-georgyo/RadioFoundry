// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 stream encryption (docs/spec/m17.md 6): the scrambler's LFSRs with the jump a late joiner needs, and the AES-CTR
// counter layout over core/crypto/aes.c. Each half builds with its own switch (CONFIG_M17_SCRAMBLER, CONFIG_M17_AES);
// with both off this file is empty. Without a key both fail closed: the STREAM bytes are zeroed, never sent or played
// as they came
#include "config.h"

#if CONFIG_M17_SCRAMBLER || CONFIG_M17_AES

#include <string.h>
#include "m17_crypt.h"
#include "../crypto/crypto_wipe.h"

#if CONFIG_M17_SCRAMBLER

// The feedback taps as register masks, D0 in bit 0 (m17.md 6.1): D7 D5 D4 D3; D15 D14 D12 D3; D23 D22 D21 D16
#define SCR_TAPS_8	0x000000B8u
#define SCR_TAPS_16	0x0000D008u
#define SCR_TAPS_24	0x00E10000u

static uint32_t scr_taps(uint8_t bits)
{
	return bits == 8 ? SCR_TAPS_8 : bits == 16 ? SCR_TAPS_16 : SCR_TAPS_24;
}

static uint32_t scr_mask(uint8_t bits)
{
	return (1u << bits) - 1u;
}

// XOR of the bits of x (at most 24 set: the register): folded down to 4 bits, then the parity of those from a 16-bit
// table in a constant, no branch and no library call
static uint32_t parity24(uint32_t x)
{
	x ^= x >> 16;
	x ^= x >> 8;
	x ^= x >> 4;
	return (0x6996u >> (x & 0xFu)) & 1u;
}

// One step: the feedback is the key stream bit and enters D0 as the register shifts toward D(n-1)
static uint32_t scr_step(uint32_t reg, uint32_t taps, uint32_t mask, uint32_t *bit)
{
	uint32_t fb = parity24(reg & taps);

	*bit = fb;
	return ((reg << 1) | fb) & mask;
}

bool m17_scr_key_ok(const uint8_t *seed, uint8_t len)
{
	uint8_t any = 0;

	if (!seed || len < 1u || len > 3u) {
		return false;
	}
	for (uint8_t i = 0; i < len; i++) {
		any |= seed[i];
	}
	return any != 0;	// all zeros never leaves zero: no key stream at all
}

bool m17_scr_init(m17_scr_t *s, const uint8_t *seed, uint8_t len)
{
	uint32_t reg = 0;

	s->reg = 0;
	s->bits = 0;
	if (!m17_scr_key_ok(seed, len)) {
		return false;
	}
	// Big endian: seed bit n-1 lands in D(n-1). Leading zero bytes count toward the length, so 00 01 is a 16-bit key
	for (uint8_t i = 0; i < len; i++) {
		reg = (reg << 8) | seed[i];
	}
	s->reg = reg;
	s->bits = (uint8_t)(8u * len);
	return true;
}

void m17_scr_frame(m17_scr_t *s, uint8_t stream[M17_STREAM_BYTES])
{
	uint32_t taps, mask, reg, bit;

	if (s->bits != 8u && s->bits != 16u && s->bits != 24u) {
		memset(stream, 0, M17_STREAM_BYTES);
		return;
	}
	taps = scr_taps(s->bits);
	mask = scr_mask(s->bits);
	reg = s->reg;
	// The first key stream bit goes on the most significant bit of byte 0 (m17.md 13). Each bit is XORed in as it is
	// made, so no key stream byte is left behind in memory
	for (unsigned i = 0; i < M17_STREAM_BYTES; i++) {
		for (unsigned b = 8; b-- > 0;) {
			reg = scr_step(reg, taps, mask, &bit);
			stream[i] ^= (uint8_t)(bit << b);
		}
	}
	s->reg = reg;
}

// A GF(2) matrix of the register's width as its columns: m[j] is the image of the register with only bit j set. The
// register is linear in its start state, so applying the matrix is the XOR of the columns of the bits that are set
static uint32_t mat_apply(const uint32_t *m, uint8_t bits, uint32_t v)
{
	uint32_t r = 0;

	for (uint8_t j = 0; j < bits; j++) {
		r ^= m[j] & (0u - ((v >> j) & 1u));
	}
	return r;
}

// The jump: P = M^128 (one frame of steps), built by stepping each unit register 128 times; then the seed goes through
// P^(2^k) for each bit k set in the FN, the powers made by squaring. Powers of one matrix commute, so the order of the
// factors doesn't matter and the register itself is the accumulator: two of the work's matrices (the power and the
// square being made) are all it takes. The work holds only powers of the public step matrix, nothing of the key; the
// register, the secret, stays in a local until it is stored
void m17_scr_seek(m17_scr_t *s, const uint8_t *seed, uint8_t len, uint16_t fn, m17_scr_work_t *w)
{
	uint32_t *p = w->m[0], *q = w->m[1], *t;
	uint32_t taps, mask, reg, bit;
	uint8_t bits;

	if (!m17_scr_init(s, seed, len)) {
		return;
	}
	fn &= M17_FN_MASK;	// the end bit is not a frame count (m17.md 6.1)
	if (fn == 0) {
		return;
	}
	bits = s->bits;
	taps = scr_taps(bits);
	mask = scr_mask(bits);
	for (uint8_t j = 0; j < bits; j++) {
		uint32_t v = 1u << j;
		for (unsigned k = 0; k < 8u * M17_STREAM_BYTES; k++) {
			v = scr_step(v, taps, mask, &bit);
		}
		p[j] = v;
	}
	reg = s->reg;
	for (;;) {
		if (fn & 1u) {
			reg = mat_apply(p, bits, reg);
		}
		fn >>= 1;
		if (fn == 0) {
			break;
		}
		// q = p * p, column by column: p applied to each of p's columns
		for (uint8_t j = 0; j < bits; j++) {
			q[j] = mat_apply(p, bits, p[j]);
		}
		t = p;
		p = q;
		q = t;
	}
	s->reg = reg;
}

#endif // CONFIG_M17_SCRAMBLER

#if CONFIG_M17_AES

_Static_assert(M17_META_BYTES + 2u == AES_BLOCK, "the counter block is META and the FN");
_Static_assert(M17_STREAM_BYTES == AES_BLOCK, "one block of key stream a frame");

void m17_aes_nonce(uint32_t ts, const uint8_t rnd[M17_AES_NONCE_RANDOM], uint8_t meta[M17_META_BYTES])
{
	meta[0] = (uint8_t)(ts >> 24);
	meta[1] = (uint8_t)(ts >> 16);
	meta[2] = (uint8_t)(ts >> 8);
	meta[3] = (uint8_t)ts;
	memcpy(&meta[4], rnd, M17_AES_NONCE_RANDOM);
}

uint8_t m17_aes_sub(uint8_t key_len)
{
	return key_len == 16u ? M17_AES_128 : key_len == 24u ? M17_AES_192 : key_len == 32u ? M17_AES_256 : 0xFFu;
}

// The counter block is META || FN (m17.md 6.2): FN written whole into the low 16 bits, never incremented, so nothing
// ever carries into the nonce. The block encrypted in place is the frame's key stream, wiped once XORed in. Without a
// key (aes_init failed, or aes_wipe ran) aes_encrypt gives zeros, which would send the plaintext: zero the frame instead
void m17_aes_frame(const aes_ctx_t *k, const uint8_t meta[M17_META_BYTES], uint16_t fn,
		   uint8_t stream[M17_STREAM_BYTES])
{
	uint8_t blk[AES_BLOCK];

	if (k->nr != 10u && k->nr != 12u && k->nr != 14u) {
		memset(stream, 0, M17_STREAM_BYTES);
		return;
	}
	memcpy(blk, meta, M17_META_BYTES);
	blk[M17_META_BYTES] = (uint8_t)(fn >> 8);
	blk[M17_META_BYTES + 1u] = (uint8_t)fn;
	aes_encrypt(k, blk, blk);
	for (unsigned i = 0; i < M17_STREAM_BYTES; i++) {
		stream[i] ^= blk[i];
	}
	crypto_wipe(blk, sizeof blk);
}

#endif // CONFIG_M17_AES

#endif
