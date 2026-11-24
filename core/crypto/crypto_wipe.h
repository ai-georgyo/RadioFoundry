// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Clearing secrets (keys, nonces, round keys, DRBG state) so the compiler can't drop the stores as dead: a volatile
// write per byte. Every module in core/crypto and every caller holding a secret wipes it when done (docs/spec/p256.md
// 10: secrets live in static or lent memory, never on a task stack, and never reach the debug link or a crash record).
#ifndef RFOUNDRY_CRYPTO_WIPE_H
#define RFOUNDRY_CRYPTO_WIPE_H

#include <stddef.h>
#include <stdint.h>

static inline void crypto_wipe(void *p, size_t n)
{
	volatile uint8_t *v = (volatile uint8_t *)p;

	while (n--)
		*v++ = 0;
}

// Equal in time independent of where the bytes differ (MACs, signatures compared against a secret)
static inline int crypto_equal(const void *a, const void *b, size_t n)
{
	const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
	uint8_t d = 0;

	for (size_t i = 0; i < n; i++)
		d |= (uint8_t)(x[i] ^ y[i]);
	return d == 0;
}

#endif
