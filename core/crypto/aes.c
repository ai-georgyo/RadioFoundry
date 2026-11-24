// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AES forward cipher (FIPS 197 sections 5.1 and 5.2) and counter mode (NIST SP 800-38A 6.5, B.1), docs/spec/crypto.md.
// One 256-byte table, the S-box: the rounds compute MixColumns with xtime instead of the 4 KB of T-tables a faster AES
// keeps, since flash is the scarce resource and the callers ask little: M17 one block per 40 ms frame, DMR AES at
// most 9 per 60 ms voice burst
#include "config.h"

#if CONFIG_CRYPTO_AES

#include <string.h>
#include "crypto/aes.h"
#include "crypto/crypto_wipe.h"

// FIPS 197 figure 7 (5.1.1: the multiplicative inverse in GF(2^8), then the affine transformation)
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

// Multiplication by x in GF(2^8) mod x^8 + x^4 + x^3 + x + 1 (FIPS 197 4.2.1), with a mask instead of a branch so the
// time doesn't depend on the secret byte's top bit
static inline uint8_t xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ (0x1bu & (0u - (unsigned)(x >> 7))));
}

static bool has_key(const aes_ctx_t *ctx)
{
	return ctx->nr == 10 || ctx->nr == 12 || ctx->nr == 14;
}

// The state is FIPS 197's column-major array: byte r + 4c is row r, column c (3.4)
static void add_round_key(uint8_t s[AES_BLOCK], const uint8_t *rk)
{
	for (unsigned i = 0; i < AES_BLOCK; i++) {
		s[i] ^= rk[i];
	}
}

// SubBytes (5.1.1) and ShiftRows (5.1.2) in one pass: row r rotates left by r columns
static void sub_shift(uint8_t s[AES_BLOCK])
{
	uint8_t t;

	s[0] = sbox[s[0]];
	s[4] = sbox[s[4]];
	s[8] = sbox[s[8]];
	s[12] = sbox[s[12]];

	t = s[1];
	s[1] = sbox[s[5]];
	s[5] = sbox[s[9]];
	s[9] = sbox[s[13]];
	s[13] = sbox[t];

	t = s[2];
	s[2] = sbox[s[10]];
	s[10] = sbox[t];
	t = s[6];
	s[6] = sbox[s[14]];
	s[14] = sbox[t];

	t = s[15];
	s[15] = sbox[s[11]];
	s[11] = sbox[s[7]];
	s[7] = sbox[s[3]];
	s[3] = sbox[t];
}

// MixColumns (5.1.3): s'0 = 2a0 ^ 3a1 ^ a2 ^ a3 = a0 ^ u ^ 2(a0 ^ a1) with u the XOR of the column, and the same
// rotated for the other rows: four xtimes a column instead of a multiplication table
static void mix_columns(uint8_t s[AES_BLOCK])
{
	for (unsigned c = 0; c < AES_BLOCK; c += 4) {
		uint8_t a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];
		uint8_t u = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
		s[c] = (uint8_t)(a0 ^ u ^ xtime((uint8_t)(a0 ^ a1)));
		s[c + 1] = (uint8_t)(a1 ^ u ^ xtime((uint8_t)(a1 ^ a2)));
		s[c + 2] = (uint8_t)(a2 ^ u ^ xtime((uint8_t)(a2 ^ a3)));
		s[c + 3] = (uint8_t)(a3 ^ u ^ xtime((uint8_t)(a3 ^ a0)));
	}
}

// KeyExpansion (5.2) on bytes: word i = word i - Nk ^ temp, temp = word i - 1, through RotWord, SubWord and Rcon
// where i is a multiple of Nk, through SubWord alone where i mod Nk is 4 for a 256-bit key. pos is i mod Nk, counted
// (no division: a core without a divider would call the library). The round keys are built in place in ctx; the one
// temporary word is wiped
bool aes_init(aes_ctx_t *ctx, const uint8_t *key, size_t len)
{
	unsigned nk = (unsigned)(len / 4u), words, pos = 0;
	uint8_t rcon = 0x01, t[4];

	if (!ctx) {
		return false;
	}
	if (!key || (len != 16u && len != 24u && len != 32u)) {
		crypto_wipe(ctx, sizeof *ctx);
		return false;
	}
	ctx->nr = (uint8_t)(nk + 6u);
	words = 4u * (ctx->nr + 1u);
	memcpy(ctx->rk, key, len);
	for (unsigned i = nk; i < words; i++) {
		memcpy(t, &ctx->rk[4u * (i - 1u)], sizeof t);
		if (pos == 0) {
			uint8_t t0 = t[0];
			t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
			t[1] = sbox[t[2]];
			t[2] = sbox[t[3]];
			t[3] = sbox[t0];
			rcon = xtime(rcon);
		} else if (nk > 6u && pos == 4u) {
			for (unsigned j = 0; j < 4u; j++) {
				t[j] = sbox[t[j]];
			}
		}
		for (unsigned j = 0; j < 4u; j++) {
			ctx->rk[4u * i + j] = (uint8_t)(ctx->rk[4u * (i - nk) + j] ^ t[j]);
		}
		if (++pos == nk) {
			pos = 0;
		}
	}
	// A 128- or 192-bit key leaves the end of rk unused: wipe it, or a longer key's schedule from before would stay
	crypto_wipe(&ctx->rk[4u * words], sizeof ctx->rk - 4u * words);
	crypto_wipe(t, sizeof t);
	return true;
}

// Cipher (5.1). Without a key (aes_init failed or aes_wipe ran) the output is all zeros, never the input
void aes_encrypt(const aes_ctx_t *ctx, const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK])
{
	uint8_t s[AES_BLOCK];

	if (!has_key(ctx)) {
		memset(out, 0, AES_BLOCK);
		return;
	}
	memcpy(s, in, sizeof s);
	add_round_key(s, ctx->rk);
	for (unsigned r = 1; r < ctx->nr; r++) {
		sub_shift(s);
		mix_columns(s);
		add_round_key(s, &ctx->rk[AES_BLOCK * r]);
	}
	sub_shift(s);
	add_round_key(s, &ctx->rk[AES_BLOCK * ctx->nr]);
	memcpy(out, s, sizeof s);
	crypto_wipe(s, sizeof s);
}

// The 128-bit counter + 1 mod 2^128, big-endian; every byte is touched, so the time doesn't show where a carry stops
static void ctr_next(uint8_t ctr[AES_BLOCK])
{
	unsigned carry = 1;

	for (unsigned i = AES_BLOCK; i-- > 0;) {
		carry += ctr[i];
		ctr[i] = (uint8_t)carry;
		carry >>= 8;
	}
}

// A partial last block uses up its counter value: the next call starts on a fresh block (the header's contract).
// Without a key the data is zeroed: failing closed, a caller that missed aes_init's false never sends the plaintext
void aes_ctr(const aes_ctx_t *ctx, uint8_t ctr[AES_BLOCK], uint8_t *data, size_t n)
{
	uint8_t ks[AES_BLOCK];

	if (!has_key(ctx)) {
		if (n) {
			memset(data, 0, n);
		}
		return;
	}
	while (n) {
		size_t m = n < AES_BLOCK ? n : AES_BLOCK;
		aes_encrypt(ctx, ctr, ks);
		ctr_next(ctr);
		for (size_t i = 0; i < m; i++) {
			data[i] ^= ks[i];
		}
		data += m;
		n -= m;
	}
	crypto_wipe(ks, sizeof ks);
}

void aes_wipe(aes_ctx_t *ctx)
{
	if (ctx) {
		crypto_wipe(ctx, sizeof *ctx);
	}
}

#endif
