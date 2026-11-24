// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The entropy pool and the nonce counter (entropy.h, docs/spec/crypto.md 8). Everything that comes in (the chip's ID,
// the boot count, the hardware generator's words, the caller's stirring) is hashed into one SHA-256 context, the pool.
// The generator is an HMAC_DRBG (hmac_sha256.h): instantiated from the pool's digest at entropy_init and reseeded
// from it whenever the pool took something since, which entropy_get checks first. With a hardware generator each
// entropy_get reads a fresh seed's worth from it into the pool before that. Only what the caller vouches for, and a
// whole read of the hardware generator, counts toward ENTROPY_READY_BITS; the ID and the boot count never do.
//
// The radio core's task only: no lock, no RTOS call. The pool and the generator's K and V are secrets: static memory,
// wiped where they are replaced; the digest and the generator's words pass through the stack and are wiped there.
#include "config.h"

#if CONFIG_ENTROPY

#include <string.h>
#include "crypto/entropy.h"
#include "crypto/hmac_sha256.h"
#include "crypto/crypto_wipe.h"

#define TRNG_BYTES	32u			// one read of the hardware generator: a seed of the generator's strength
#define TRNG_CREDIT	(TRNG_BYTES * 8u)	// what a whole read counts for
#define DRAW_BITS	12u			// the counter's low bits: nonces drawn on one boot count
#define DRAWS		(1u << DRAW_BITS)
#define DRAWS_BUMP	(DRAWS - DRAWS / 4u)	// from here on the caller persists the next boot count
#define BOOT_MAX	0xFFFFFu		// the counter's top 20 bits; a larger boot count can't be told apart

static struct {
	sha256_t pool;				// what came in since the last (re)seed
	hmac_drbg_t drbg;
	size_t (*trng)(uint8_t *buf, size_t n);	// hal_system_t.entropy, NULL without one
	uint32_t credited;			// bits credited since entropy_init, saturating
	uint32_t boot;				// the counter's base (entropy_init, entropy_rebase)
	uint16_t draws;				// nonces drawn on it
	uint8_t src;				// ENTROPY_SRC_*
	bool grew;				// the pool took something the generator hasn't
	bool boot_saved;
	bool uid;				// a chip ID went in at entropy_init
	bool stirred;				// a credited stir (entropy_mix) since entropy_init
} e;

static void credit(uint32_t bits)
{
	e.credited = e.credited > UINT32_MAX - bits ? UINT32_MAX : e.credited + bits;
	if (e.src == ENTROPY_SRC_WEAK && e.credited >= ENTROPY_READY_BITS) {
		e.src = ENTROPY_SRC_POOL;
	}
}

static void pool_mix(const void *p, size_t n, uint32_t bits)
{
	if (p == NULL || n == 0) {
		return;
	}
	sha256_update(&e.pool, p, n);
	e.grew = true;
	credit(bits);
}

// The pool's digest into the generator, the pool emptied: on a SHA-256 context the block buffer keeps the last input
// bytes after the final, so the whole context is wiped before it starts again
static void pool_drain(bool instantiate)
{
	uint8_t d[SHA256_LEN];

	sha256_final(&e.pool, d);
	crypto_wipe(&e.pool, sizeof e.pool);
	sha256_init(&e.pool);
	if (instantiate) {
		hmac_drbg_seed(&e.drbg, d, sizeof d);
	} else {
		hmac_drbg_reseed(&e.drbg, d, sizeof d);
	}
	crypto_wipe(d, sizeof d);
	e.grew = false;
}

// A seed's worth from the hardware generator into the pool. Only a whole read is credited and makes the source TRNG:
// a short one means the generator stopped on a fault part way (its words still go in, uncredited)
static void trng_mix(void)
{
	uint8_t b[TRNG_BYTES];

	if (e.trng == NULL) {
		return;
	}
	size_t got = e.trng(b, sizeof b);
	if (got > sizeof b) {
		got = 0;	// an op that overran its buffer: nothing it said counts
	}
	bool whole = got == sizeof b;
	pool_mix(b, got, whole ? TRNG_CREDIT : 0u);
	crypto_wipe(b, sizeof b);
	if (whole) {
		e.src = ENTROPY_SRC_TRNG;
	}
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

void entropy_init(const hal_system_t *sys, const void *uid, size_t uid_len, uint32_t boot, bool boot_saved)
{
	// The personalization string (SP 800-90A 8.7.1) tells this pool's generator from any other user of the DRBG
	static const char pers[] = "RadioFoundry entropy pool 1";
	uint8_t b[5];

	crypto_wipe(&e, sizeof e);	// a second start drops the first's pool and generator
	sha256_init(&e.pool);
	e.src = ENTROPY_SRC_WEAK;
	e.trng = sys != NULL ? sys->entropy : NULL;
	e.boot = boot;
	e.boot_saved = boot_saved;
	pool_mix(pers, sizeof pers - 1u, 0u);
	pool_mix(uid, uid_len, 0u);	// tells two radios apart, but anyone may read it: never credited
	e.uid = uid != NULL && uid_len != 0;
	put_le32(b, boot);
	b[4] = boot_saved ? 1u : 0u;
	pool_mix(b, sizeof b, 0u);
	trng_mix();
	pool_drain(true);
}

void entropy_mix(const void *p, size_t n, uint16_t credit_bits)
{
	if (e.src == ENTROPY_SRC_NONE) {
		return;		// before entropy_init: there is no pool yet
	}
	// A caller can't vouch for more bits than it gave
	uint32_t bits = credit_bits;
	if (n < 8192u && bits > n * 8u) {
		bits = (uint32_t)n * 8u;
	}
	pool_mix(p, n, bits);
	if (p != NULL && n != 0 && bits != 0) {
		e.stirred = true;
	}
}

bool entropy_get(uint8_t *out, size_t n)
{
	if (e.src == ENTROPY_SRC_NONE) {
		entropy_init(NULL, NULL, 0, 0, false);	// always fills: from a pool of nothing, which says so
	}
	trng_mix();
	if (e.grew) {
		pool_drain(false);
	}
	if (out != NULL && n != 0) {
		hmac_drbg_generate(&e.drbg, out, n);
	}
	return e.src == ENTROPY_SRC_POOL || e.src == ENTROPY_SRC_TRNG;
}

bool entropy_nonce(uint8_t *out, size_t n)
{
	if (out == NULL) {
		return false;
	}
	if (n < 8u || e.src == ENTROPY_SRC_NONE || e.draws >= DRAWS || e.boot > BOOT_MAX) {
		memset(out, 0, n);
		return false;
	}
	bool seeded = entropy_get(out, n - 4u);
	// A weak pool's random part is set by what went in at start-up unless something unpredictable came in since
	// (crypto.md 8.3). Refused: an unsaved boot count (it may come again at the next start-up, with a pool stirred
	// the same way); no chip ID (the counter alone doesn't keep two radios sharing a key apart); no credited stir
	// since start-up (a boot count that went back, a settings store reset or erased, would repeat the nonces whole)
	if (!seeded && (!e.boot_saved || !e.uid || !e.stirred)) {
		crypto_wipe(out, n);
		return false;
	}
	uint32_t c = (e.boot << DRAW_BITS) | e.draws;
	e.draws++;
	out[n - 4u] = (uint8_t)(c >> 24);
	out[n - 3u] = (uint8_t)(c >> 16);
	out[n - 2u] = (uint8_t)(c >> 8);
	out[n - 1u] = (uint8_t)c;
	return true;
}

bool entropy_nonce_want_bump(void)
{
	return e.src != ENTROPY_SRC_NONE && e.draws >= DRAWS_BUMP;
}

void entropy_rebase(uint32_t boot, bool boot_saved)
{
	if (e.src == ENTROPY_SRC_NONE || boot < e.boot) {
		return;		// a counter never goes back: the values under a lower boot count may be spent
	}
	if (boot != e.boot) {
		e.boot = boot;
		e.draws = 0;
		uint8_t b[4];
		put_le32(b, boot);
		pool_mix(b, sizeof b, 0u);
	}
	e.boot_saved = boot_saved;	// the same count: only whether it is stored now
}

uint8_t entropy_source(void)
{
	return e.src;
}

uint32_t entropy_credited(void)
{
	return e.credited;
}

#endif
