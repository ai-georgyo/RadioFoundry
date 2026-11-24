// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Random bytes for keys' companions that must never repeat: AES-CTR nonces (M17's 80 random bits a transmission,
// docs/spec/m17.md 6.2). Generic, any radio: a pool that hashes in whatever the core can see (the SoC's hardware
// generator where the platform has one, hal_system_t.entropy; else timing jitter of events, received noise, the chip's
// unique ID), and an HMAC_DRBG (hmac_sha256.h) seeded and reseeded from it that the callers read. Without a hardware
// generator the quality is what the stirring gives: the pool says how much it was credited, and says which source it
// has (debug link crypto.entropy). Not for signing nonces: P-256 signs with RFC 6979 (p256.h) and needs none.
//
// A nonce must never repeat under one key, and a pool stirred for a few seconds after start-up on a radio without a
// hardware generator can't promise that alone. So entropy_nonce ends every nonce with a 32-bit counter that never
// repeats on this radio: the boot count the caller persists (settings store, bumped once per start-up) in its top 20
// bits and the nonces drawn since start-up in its low 12 (a 4,096th draw asks the caller to persist the next boot
// count first: entropy_nonce_want_bump). Two radios sharing a key differ in the random bytes before it, which the
// chip's unique ID already seeds differently.
//
// One task only: the radio core's (it stirs on its tick and reads at key-up). No RTOS call, no locking. Built with
// CONFIG_ENTROPY (derived from CONFIG_M17_AES).
#ifndef RFOUNDRY_ENTROPY_H
#define RFOUNDRY_ENTROPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../hal/hal_system.h"

// entropy_source()
enum {
	ENTROPY_SRC_NONE,	// not started
	ENTROPY_SRC_WEAK,	// stirred, but less than ENTROPY_READY_BITS credited yet
	ENTROPY_SRC_POOL,	// stirred enough to count as seeded (no hardware generator)
	ENTROPY_SRC_TRNG,	// seeded from the SoC's generator, and reseeded from it
};

#define ENTROPY_READY_BITS	256u	// credited before the pool counts as seeded

// At start-up, radio task: sys may be NULL (no hal_system_t) or have no entropy op. uid: the chip's unique ID or other
// per-radio constant (hal_chip_t.uid), mixed in but never credited. boot: this start-up's boot count, already bumped
// and persisted by the caller (boot_saved true), or the best it has when the write failed (boot_saved false): mixed in
// (never credited) and the base of entropy_nonce's counter
void entropy_init(const hal_system_t *sys, const void *uid, size_t uid_len, uint32_t boot, bool boot_saved);
// Mix in event data (a time stamp in microseconds, an RSSI, a block of received soft values). credit_bits: how much
// unpredictability the caller vouches for, conservatively (a free-running microsecond counter read at a key press: 2;
// a received noise block: 1 per 8 bytes; a constant: 0)
void entropy_mix(const void *p, size_t n, uint16_t credit_bits);
static inline void entropy_mix_u32(uint32_t v, uint16_t credit_bits)
{
	entropy_mix(&v, sizeof v, credit_bits);
}
// n bytes from the generator (reseeded from the pool when the pool grew, and from the hardware generator each call
// where there is one). Always fills out; false when the pool is not seeded yet (ENTROPY_SRC_WEAK or NONE): the caller
// decides whether that is good enough (an M17 nonce uses it anyway and counts it, m17.md)
bool entropy_get(uint8_t *out, size_t n);
// A nonce of n >= 8 bytes: n - 4 bytes from the generator, then the counter (big endian). False, with out zeroed,
// when it can't promise the nonce is new: the counter is spent (4,096 draws on one boot count), or the pool is not
// seeded (ENTROPY_SRC_WEAK or NONE) and either the boot count was not saved, no chip ID went in, or no credited stir
// came in since entropy_init. An encrypting transmitter then refuses (radio_m17.c: TXDENY_ENC_NOT_READY). True
// otherwise, even from a weak pool: the counter keeps it unique on this radio while the boot count only goes up, and
// the stir (the caller's key-up time) keeps it apart from an earlier start-up's when the count went back (crypto.md 8.3)
bool entropy_nonce(uint8_t *out, size_t n);
// The counter is at its last quarter: the caller persists boot + 1 and calls entropy_rebase with it (radio task)
bool entropy_nonce_want_bump(void);
void entropy_rebase(uint32_t boot, bool boot_saved);
uint8_t entropy_source(void);		// ENTROPY_SRC_*
uint32_t entropy_credited(void);	// bits credited since start (saturates), for the debug link

#endif
