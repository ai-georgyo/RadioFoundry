// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HMAC-SHA-256 (FIPS 198-1, RFC 2104) on the tree's SHA-256 (core/support/sha256.h), streaming and one-shot. The
// inner and outer states are kept after the key block, so a caller that MACs many messages under one key (RFC 6979's
// loop, the entropy pool's DRBG) copies them instead of hashing the key again. Built with CONFIG_CRYPTO_HMAC (derived:
// CRYPTO_P256 or ENTROPY). A context with a key in it is a secret: hmac_sha256_wipe it.
#ifndef RFOUNDRY_HMAC_SHA256_H
#define RFOUNDRY_HMAC_SHA256_H

#include <stddef.h>
#include <stdint.h>
#include "../support/sha256.h"

#define HMAC_SHA256_LEN	SHA256_LEN

typedef struct {
	sha256_t inner;		// after K ^ ipad, then the message so far
	sha256_t outer;		// after K ^ opad
} hmac_sha256_t;

// Key of any length (longer than a block: hashed first, FIPS 198-1 step 2)
void hmac_sha256_init(hmac_sha256_t *h, const uint8_t *key, size_t len);
void hmac_sha256_update(hmac_sha256_t *h, const void *data, size_t len);
// The MAC; h is spent (init or copy a keyed one again)
void hmac_sha256_final(hmac_sha256_t *h, uint8_t mac[HMAC_SHA256_LEN]);
void hmac_sha256(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t mac[HMAC_SHA256_LEN]);
void hmac_sha256_wipe(hmac_sha256_t *h);

// HMAC_DRBG with SHA-256 (NIST SP 800-90A 10.1.2), no prediction resistance and no derivation function: the entropy
// pool's generator (entropy.h). seed: entropy input || nonce || personalization, as one string
typedef struct {
	uint8_t k[HMAC_SHA256_LEN];
	uint8_t v[HMAC_SHA256_LEN];
	uint32_t reseeds;	// generate calls since the last (re)seed
} hmac_drbg_t;

void hmac_drbg_seed(hmac_drbg_t *g, const uint8_t *seed, size_t len);
void hmac_drbg_reseed(hmac_drbg_t *g, const uint8_t *seed, size_t len);
// n bytes out (any n; SP 800-90A limits one request to 2^16 bytes, far above what the callers ask)
void hmac_drbg_generate(hmac_drbg_t *g, uint8_t *out, size_t n);
void hmac_drbg_wipe(hmac_drbg_t *g);

#endif
