// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HMAC-SHA-256 (FIPS 198-1 section 4, RFC 2104) and HMAC_DRBG (NIST SP 800-90A Rev. 1 10.1.2, no prediction
// resistance, no derivation function, no additional input), docs/spec/crypto.md. SHA-256 is core/support/sha256.c
#include "config.h"

#if CONFIG_CRYPTO_HMAC

#include <string.h>
#include "crypto/hmac_sha256.h"
#include "crypto/crypto_wipe.h"

#define HMAC_BLOCK	64u	// SHA-256's input block, B in FIPS 198-1

// FIPS 198-1 steps 1-3 and 4/7 up to the message: K0 (the key, hashed when longer than a block, zero padded), then
// the inner state after K0 ^ ipad and the outer after K0 ^ opad. A long key is hashed in h->inner, then wiped:
// sha256_init doesn't clear the block buffer, where the padding left the key's last bytes, and a copy of the keyed
// context would carry them. The key block is wiped
void hmac_sha256_init(hmac_sha256_t *h, const uint8_t *key, size_t len)
{
	uint8_t k0[HMAC_BLOCK];

	memset(k0, 0, sizeof k0);
	if (len > HMAC_BLOCK) {
		sha256_init(&h->inner);
		sha256_update(&h->inner, key, len);
		sha256_final(&h->inner, k0);
		crypto_wipe(&h->inner, sizeof h->inner);
	} else if (len) {
		memcpy(k0, key, len);
	}
	for (unsigned i = 0; i < HMAC_BLOCK; i++) {
		k0[i] ^= 0x36u;
	}
	sha256_init(&h->inner);
	sha256_update(&h->inner, k0, sizeof k0);
	for (unsigned i = 0; i < HMAC_BLOCK; i++) {
		k0[i] ^= 0x36u ^ 0x5cu;
	}
	sha256_init(&h->outer);
	sha256_update(&h->outer, k0, sizeof k0);
	crypto_wipe(k0, sizeof k0);
}

// An empty message may come as NULL: it never reaches sha256_update's memcpy
void hmac_sha256_update(hmac_sha256_t *h, const void *data, size_t len)
{
	if (len) {
		sha256_update(&h->inner, data, len);
	}
}

// Steps 6-9: the outer hash over the inner one. The context is wiped: spent, it holds the keyed outer state still
void hmac_sha256_final(hmac_sha256_t *h, uint8_t mac[HMAC_SHA256_LEN])
{
	uint8_t inner[SHA256_LEN];

	sha256_final(&h->inner, inner);
	sha256_update(&h->outer, inner, sizeof inner);
	sha256_final(&h->outer, inner);
	crypto_wipe(h, sizeof *h);
	memcpy(mac, inner, sizeof inner);
	crypto_wipe(inner, sizeof inner);
}

void hmac_sha256(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t mac[HMAC_SHA256_LEN])
{
	hmac_sha256_t h;

	hmac_sha256_init(&h, key, klen);
	hmac_sha256_update(&h, data, len);
	hmac_sha256_final(&h, mac);	// wipes h
}

void hmac_sha256_wipe(hmac_sha256_t *h)
{
	if (h) {
		crypto_wipe(h, sizeof *h);
	}
}

// HMAC(K, V || sep || data) into out, sep < 0 for none. out may be K or V: both are read before out is written
static void drbg_mac(const uint8_t k[HMAC_SHA256_LEN], const uint8_t v[HMAC_SHA256_LEN], int sep, const uint8_t *data,
	size_t len, uint8_t out[HMAC_SHA256_LEN])
{
	hmac_sha256_t h;

	hmac_sha256_init(&h, k, HMAC_SHA256_LEN);
	hmac_sha256_update(&h, v, HMAC_SHA256_LEN);
	if (sep >= 0) {
		uint8_t b = (uint8_t)sep;
		hmac_sha256_update(&h, &b, 1);
	}
	hmac_sha256_update(&h, data, len);
	hmac_sha256_final(&h, out);	// wipes h
}

// HMAC_DRBG_Update (10.1.2.2)
static void drbg_update(hmac_drbg_t *g, const uint8_t *data, size_t len)
{
	drbg_mac(g->k, g->v, 0x00, data, len, g->k);
	drbg_mac(g->k, g->v, -1, NULL, 0, g->v);
	if (!len) {
		return;
	}
	drbg_mac(g->k, g->v, 0x01, data, len, g->k);
	drbg_mac(g->k, g->v, -1, NULL, 0, g->v);
}

// Instantiate (10.1.2.3): K = 0x00.., V = 0x01.., Update(seed material)
void hmac_drbg_seed(hmac_drbg_t *g, const uint8_t *seed, size_t len)
{
	memset(g->k, 0x00, sizeof g->k);
	memset(g->v, 0x01, sizeof g->v);
	drbg_update(g, seed, len);
	g->reseeds = 0;
}

// Reseed (10.1.2.4): Update(entropy input), the generate count starts again
void hmac_drbg_reseed(hmac_drbg_t *g, const uint8_t *seed, size_t len)
{
	drbg_update(g, seed, len);
	g->reseeds = 0;
}

// Generate (10.1.2.5): V = HMAC(K, V) per 32 bytes out, then Update with no input. The whole 32-byte V goes through
// out only as far as n reaches; the rest stays in g, which is secret anyway
void hmac_drbg_generate(hmac_drbg_t *g, uint8_t *out, size_t n)
{
	while (n) {
		size_t m = n < HMAC_SHA256_LEN ? n : HMAC_SHA256_LEN;
		drbg_mac(g->k, g->v, -1, NULL, 0, g->v);
		memcpy(out, g->v, m);
		out += m;
		n -= m;
	}
	drbg_update(g, NULL, 0);
	g->reseeds++;
}

void hmac_drbg_wipe(hmac_drbg_t *g)
{
	if (g) {
		crypto_wipe(g, sizeof *g);
	}
}

#endif
