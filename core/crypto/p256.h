// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ECDSA over P-256 (secp256r1) with SHA-256 hashes (docs/spec/p256.md; FIPS 186-5, SP 800-186, SEC 1, RFC 6979),
// written for the two 32-bit cores the tree runs on: 8 x 32-bit limbs in Montgomery form, Jacobian points with a = -3.
// Signing derives its nonce from the key and the hash (RFC 6979), so it needs no random source, and runs in constant
// time (a fixed 4-bit window over signed digits, table reads by masked scan). Verifying is variable time (everything in
// it is public) and accepts a high s (p256.md 8: M17's own test signature has one).
//
// Encodings are SEC 1 without the leading 04: a public key is X || Y (64 bytes, big endian), a compressed key 02 / 03
// || X (33 bytes), a signature r || s (64 bytes), a private key d (32 bytes, big endian).
//
// The 32x32 -> 64 multiply is picked by CPU, as core/dsp/fxp.h does: UMULL on Arm, 16-bit halves on C-SKY (no HI/LO
// unit: the context switch does not save it), plain C on the host; host tests run the halves path too (FXP_SPLIT16).
// Memory: the caller's scratch (p256_scratch_t, lent: in M17 the voice path's codec scratch while no voice runs). Every
// call wipes the secret parts of it before returning. No RTOS call, no heap, no recursion. P-256's own frames stay
// under 256 bytes of stack; p256_sign and p256_rfc6979_k add the HMAC-SHA-256 under RFC 6979 and need about 350-380
// bytes (docs/spec/p256.md 14.4). Built with CONFIG_CRYPTO_P256 (derived from CONFIG_M17_SIG); p256.c is in the
// Makefile's HOT_SRC (-O2).
#ifndef RFOUNDRY_P256_H
#define RFOUNDRY_P256_H

#include <stdbool.h>
#include <stdint.h>

#define P256_BYTES		32u
#define P256_PUB_BYTES		64u	// X || Y
#define P256_PUB_C_BYTES	33u	// 02 / 03 || X
#define P256_SIG_BYTES		64u	// r || s

// Working memory: the signing window's 8-point table, the verifier's precomputed points and NAF digits, RFC 6979's
// HMAC state, field temporaries. Part of the M17 memory plan: it must fit C2_SCRATCH_BYTES (codec2.h)
#define P256_SCRATCH_BYTES	1536u
typedef struct { uint32_t w[P256_SCRATCH_BYTES / 4]; } p256_scratch_t;

// A private key: 1 <= d <= n - 1 (p256.md 9). False for 0 and anything at or above n
bool p256_priv_valid(const uint8_t d[P256_BYTES]);
// Q = d * G (constant time). False when d is not a valid private key
bool p256_pub_from_priv(const uint8_t d[P256_BYTES], uint8_t q[P256_PUB_BYTES], p256_scratch_t *s);
// A public key on the curve, coordinates below p, not the point at infinity (SP 800-186 D.1.1, p256.md 9); the
// all-zero key is refused
bool p256_pub_valid(const uint8_t q[P256_PUB_BYTES]);
// 33 bytes to 64 (y from x, p = 3 mod 4: one exponentiation) and back. False when x is not on the curve
bool p256_pub_decompress(const uint8_t c[P256_PUB_C_BYTES], uint8_t q[P256_PUB_BYTES]);
void p256_pub_compress(const uint8_t q[P256_PUB_BYTES], uint8_t c[P256_PUB_C_BYTES]);

// Sign hash h (32 bytes, used as the message representative as FIPS 186-5 6.4.1 does for a 256-bit hash) with d; the
// nonce from RFC 6979 3.2 (HMAC-SHA-256). False for an invalid d, or r or s of 0 (probability about 2^-256: refuse,
// as FIPS 186-5 6.4.1 step 11 allows). The same d and h always give the same signature
bool p256_sign(const uint8_t d[P256_BYTES], const uint8_t h[P256_BYTES], uint8_t rs[P256_SIG_BYTES],
	       p256_scratch_t *s);
// The same with a given nonce k (1 <= k <= n - 1): the NIST CAVP known-k vectors. Never with a k from anywhere but a
// test or RFC 6979
bool p256_sign_k(const uint8_t d[P256_BYTES], const uint8_t h[P256_BYTES], const uint8_t k[P256_BYTES],
		 uint8_t rs[P256_SIG_BYTES], p256_scratch_t *s);
// RFC 6979's k for d and h, alone (a test of the derivation: p256.md 12, the M17 vector's k)
bool p256_rfc6979_k(const uint8_t d[P256_BYTES], const uint8_t h[P256_BYTES], uint8_t k[P256_BYTES],
		    p256_scratch_t *s);
// Verify r || s over h with q: true only for a valid signature (r and s in [1, n-1], q valid, R not infinity and
// x(R) mod n = r)
bool p256_verify(const uint8_t q[P256_PUB_BYTES], const uint8_t h[P256_BYTES], const uint8_t rs[P256_SIG_BYTES],
		 p256_scratch_t *s);

void p256_wipe(p256_scratch_t *s);

#endif
