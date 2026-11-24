// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The entropy pool and the nonce counter (core/crypto/entropy.c, entropy_boot.c; docs/spec/crypto.md 8) against
// entropy.h's contract: sources and credits, readiness with and without a hardware generator (a fake one here, which
// can fail whole, part way or overrun), output that never repeats across reseeds, the nonce counter across simulated
// start-ups and draws (the 4,096th draw, the bump at the last quarter, a start-up whose boot count wasn't saved), the
// refusals (a weak pool with no chip ID or no credited stir), a boot count that went back, and the wipes. entropy.c is
// included, so the test sees its state; the settings store and the microsecond clock are fakes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/crypto/entropy.c"
#include "crypto/entropy_boot.h"
#include "settings/settings.h"

static unsigned checks, failed;

#define CHECK(c, ...)                                                                                                \
	do {                                                                                                         \
		checks++;                                                                                            \
		if (!(c)) {                                                                                          \
			failed++;                                                                                    \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                  \
			printf(__VA_ARGS__);                                                                         \
			printf("\n");                                                                                \
		}                                                                                                    \
	} while (0)

static int all_zero(const void *p, size_t n)
{
	const uint8_t *b = p;

	for (size_t i = 0; i < n; i++) {
		if (b[i] != 0) {
			return 0;
		}
	}
	return 1;
}

static uint32_t counter_of(const uint8_t *nonce, size_t n)
{
	return (uint32_t)nonce[n - 4] << 24 | (uint32_t)nonce[n - 3] << 16 | (uint32_t)nonce[n - 2] << 8 | nonce[n - 1];
}

// --- A fake hardware generator ---

enum { TRNG_OK, TRNG_FAIL, TRNG_SHORT, TRNG_OVERRUN };
static int trng_mode;
static unsigned trng_calls;
static uint32_t trng_state = 0x12345678u;

static size_t fake_trng(uint8_t *buf, size_t n)
{
	trng_calls++;
	if (trng_mode == TRNG_FAIL) {
		return 0;
	}
	size_t give = trng_mode == TRNG_SHORT ? n / 2u : n;
	for (size_t i = 0; i < give; i++) {
		trng_state ^= trng_state << 13;
		trng_state ^= trng_state >> 17;
		trng_state ^= trng_state << 5;
		buf[i] = (uint8_t)trng_state;
	}
	return trng_mode == TRNG_OVERRUN ? n + 1u : give;
}

static const hal_system_t sys_trng = { .entropy = fake_trng };
static const hal_system_t sys_none = { .entropy = NULL };
static const uint8_t uid_a[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
static const uint8_t uid_b[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13 };

// The microsecond clock entropy_boot.c stirs with: a fixed step a read, from 0 at each simulated power-up, so two
// start-ups that do the same see the same times
static uint32_t fake_us;
static uint32_t us_step = 137u;

uint32_t hal_micros(void)
{
	fake_us += us_step;
	return fake_us;
}

// What radio_m17.c does at key-up before it draws: the press time, 2 bits credited
static void keyup(uint32_t us)
{
	entropy_mix_u32(us, 2);
}

// Before entropy_init: no source, mixing does nothing, and a get still fills (from a pool of nothing) but says so
static void test_unstarted(void)
{
	uint8_t a[16], b[16];

	CHECK(entropy_source() == ENTROPY_SRC_NONE && entropy_credited() == 0, "unstarted");
	CHECK(!entropy_nonce_want_bump(), "unstarted bump");
	memset(a, 0xEE, sizeof a);
	CHECK(!entropy_nonce(a, sizeof a) && all_zero(a, sizeof a), "unstarted nonce zeroed");
	entropy_mix_u32(0xDEADBEEFu, 32);
	CHECK(entropy_source() == ENTROPY_SRC_NONE && entropy_credited() == 0, "unstarted mix ignored");
	entropy_rebase(9, true);
	CHECK(entropy_source() == ENTROPY_SRC_NONE, "unstarted rebase ignored");
	memset(a, 0, sizeof a);
	CHECK(!entropy_get(a, sizeof a) && !all_zero(a, sizeof a), "unstarted get fills, not seeded");
	CHECK(entropy_source() == ENTROPY_SRC_WEAK, "get started it");
	// The same nothing gives the same bytes: that is why it says it isn't seeded
	crypto_wipe(&e, sizeof e);
	memset(b, 0, sizeof b);
	CHECK(!entropy_get(b, sizeof b) && memcmp(a, b, sizeof a) == 0, "nothing is deterministic");
	crypto_wipe(&e, sizeof e);
}

// No hardware generator: only what the caller vouches for counts, capped at the bits it gave; ready at 256
static void test_credits(void)
{
	uint8_t out[32];

	entropy_init(&sys_none, uid_a, sizeof uid_a, 7, true);
	CHECK(entropy_source() == ENTROPY_SRC_WEAK && entropy_credited() == 0, "uid and boot not credited");
	CHECK(!entropy_get(out, sizeof out), "weak get false");
	entropy_mix_u32(1234, 2);
	CHECK(entropy_credited() == 2, "2 bits: %u", entropy_credited());
	entropy_mix_u32(5678, 1000);	// a word can't be worth more than 32 bits
	CHECK(entropy_credited() == 34, "capped at 32: %u", entropy_credited());
	CHECK(!entropy_get(out, 1) && !e.grew, "the get took the stirs in");
	entropy_mix(NULL, 10, 50);
	entropy_mix(out, 0, 50);
	CHECK(entropy_credited() == 34 && !e.grew, "nothing given, nothing credited");
	uint8_t block[64];
	memset(block, 0x5A, sizeof block);
	entropy_mix(block, sizeof block, 8);	// a received noise block: 1 bit per 8 bytes
	entropy_mix(block, sizeof block, 0);	// a constant
	CHECK(entropy_credited() == 42 && entropy_source() == ENTROPY_SRC_WEAK, "42");
	for (int i = 0; i < 106; i++) {
		entropy_mix_u32((uint32_t)i, 2);
	}
	CHECK(entropy_credited() == 254 && entropy_source() == ENTROPY_SRC_WEAK && !entropy_get(out, 8), "254: weak");
	entropy_mix_u32(99, 2);
	CHECK(entropy_credited() == 256 && entropy_source() == ENTROPY_SRC_POOL, "256: pool");
	CHECK(entropy_get(out, sizeof out), "pool get true");
	// Saturating
	e.credited = UINT32_MAX - 3u;
	entropy_mix_u32(1, 32);
	CHECK(entropy_credited() == UINT32_MAX, "saturates");
	// A long block's credit isn't capped by the 8-bits-a-byte rule's arithmetic
	static uint8_t big[9000];
	entropy_init(NULL, NULL, 0, 1, true);
	entropy_mix(big, sizeof big, 65535);
	CHECK(entropy_credited() == 65535 && entropy_source() == ENTROPY_SRC_POOL, "big block");
}

// With a hardware generator: seeded at start-up, a read on each get; its faults
static void test_trng(void)
{
	uint8_t out[16];

	trng_mode = TRNG_OK;
	trng_calls = 0;
	entropy_init(&sys_trng, uid_a, sizeof uid_a, 3, false);
	CHECK(entropy_source() == ENTROPY_SRC_TRNG && entropy_credited() == 256 && trng_calls == 1, "trng init");
	CHECK(entropy_get(out, sizeof out) && trng_calls == 2 && entropy_credited() == 512, "a read each get");
	CHECK(entropy_get(NULL, 0) && trng_calls == 3, "even a zero-length get reseeds");
	CHECK(e.drbg.reseeds == 0, "reseeded before the generate: %u", e.drbg.reseeds);

	// The generator fails at start-up: a pool like any other, and TRNG once it works again
	trng_mode = TRNG_FAIL;
	entropy_init(&sys_trng, uid_a, sizeof uid_a, 3, false);
	CHECK(entropy_source() == ENTROPY_SRC_WEAK && entropy_credited() == 0, "failed trng: weak");
	CHECK(!entropy_get(out, sizeof out), "failed trng: not seeded");
	trng_mode = TRNG_OK;
	CHECK(entropy_get(out, sizeof out) && entropy_source() == ENTROPY_SRC_TRNG, "trng back");
	// A fault later keeps the seed it had
	trng_mode = TRNG_FAIL;
	CHECK(entropy_get(out, sizeof out) && entropy_source() == ENTROPY_SRC_TRNG, "later fault keeps the seed");

	// Part way: its words go in uncredited
	trng_mode = TRNG_SHORT;
	entropy_init(&sys_trng, uid_a, sizeof uid_a, 3, false);
	CHECK(entropy_source() == ENTROPY_SRC_WEAK && entropy_credited() == 0, "short read: weak");
	// An op that says it wrote more than it was given: nothing it said counts
	trng_mode = TRNG_OVERRUN;
	entropy_init(&sys_trng, uid_a, sizeof uid_a, 3, false);
	CHECK(entropy_source() == ENTROPY_SRC_WEAK && entropy_credited() == 0, "overrun: weak");
	trng_mode = TRNG_OK;
}

// The uid and the boot count go in (different radios and start-ups get different bytes) though neither is credited;
// a hardware generator changes everything
static void test_inputs(void)
{
	uint8_t a[32], b[32], c[32], d[32], f[32];

	entropy_init(NULL, uid_a, sizeof uid_a, 5, true);
	entropy_get(a, sizeof a);
	entropy_init(NULL, uid_a, sizeof uid_a, 5, true);
	entropy_get(b, sizeof b);
	CHECK(memcmp(a, b, sizeof a) == 0, "same inputs, same bytes");
	entropy_init(NULL, uid_b, sizeof uid_b, 5, true);
	entropy_get(c, sizeof c);
	CHECK(memcmp(a, c, sizeof a) != 0, "the uid goes in");
	entropy_init(NULL, uid_a, sizeof uid_a, 6, true);
	entropy_get(d, sizeof d);
	CHECK(memcmp(a, d, sizeof a) != 0, "the boot count goes in");
	entropy_init(NULL, uid_a, sizeof uid_a, 5, false);
	entropy_get(f, sizeof f);
	CHECK(memcmp(a, f, sizeof a) != 0, "whether it was saved goes in");
	entropy_init(&sys_trng, uid_a, sizeof uid_a, 5, true);
	entropy_get(f, sizeof f);
	CHECK(memcmp(a, f, sizeof a) != 0, "the hardware generator goes in");
	// A stir changes the stream from the next get on
	entropy_init(NULL, uid_a, sizeof uid_a, 5, true);
	entropy_mix_u32(1, 0);
	entropy_get(f, sizeof f);
	CHECK(memcmp(a, f, sizeof a) != 0, "a stir goes in");
}

static int cmp16(const void *x, const void *y)
{
	return memcmp(x, y, 16);
}

// Outputs never repeat: thousands of draws through reseeds from stirring and from the generator, and none without
static void test_no_repeats(void)
{
	enum { N = 6000 };
	static uint8_t out[N][16];

	for (int k = 0; k < 2; k++) {
		entropy_init(k ? &sys_trng : NULL, uid_a, sizeof uid_a, 11, true);
		for (int i = 0; i < N; i++) {
			if (i % 3 == 0) {
				entropy_mix_u32((uint32_t)i * 2654435761u, 2);
			}
			entropy_get(out[i], sizeof out[i]);
		}
		qsort(out, N, sizeof out[0], cmp16);
		int dup = 0;
		for (int i = 1; i < N; i++) {
			dup += memcmp(out[i - 1], out[i], 16) == 0;
		}
		CHECK(dup == 0, "%d repeats (trng %d)", dup, k);
	}
}

// The nonce: n - 4 random bytes, then (boot << 12 | draw) big endian; the 4,096th draw and the refusal after it; the
// bump at the last quarter; the refusals
static void test_nonce(void)
{
	uint8_t a[14], b[14];

	entropy_init(NULL, uid_a, sizeof uid_a, 0x12345, true);	// weak, but saved: the counter keeps it new
	keyup(1000);
	CHECK(entropy_nonce(a, sizeof a) && counter_of(a, sizeof a) == (0x12345u << 12), "first: %08x",
	      counter_of(a, sizeof a));
	CHECK(entropy_nonce(b, sizeof b) && counter_of(b, sizeof b) == ((0x12345u << 12) | 1u), "second");
	CHECK(memcmp(a, b, 10) != 0, "random part differs");
	for (unsigned i = 2; i < 3072u; i++) {
		CHECK(!entropy_nonce_want_bump(), "no bump at %u", i);
		if (!entropy_nonce(a, sizeof a) || counter_of(a, sizeof a) != ((0x12345u << 12) | i)) {
			CHECK(false, "draw %u", i);
			break;
		}
	}
	CHECK(entropy_nonce_want_bump(), "bump at 3072");
	for (unsigned i = 3072; i < 4095u; i++) {
		entropy_nonce(a, sizeof a);
	}
	CHECK(entropy_nonce(a, sizeof a) && counter_of(a, sizeof a) == ((0x12345u << 12) | 0xFFFu), "4,096th draw");
	memset(a, 0xEE, sizeof a);
	CHECK(!entropy_nonce(a, sizeof a) && all_zero(a, sizeof a), "4,097th: spent, zeroed");
	CHECK(entropy_nonce_want_bump(), "still wants the bump");
	// A lower count is never taken; the same one only says it is stored
	entropy_rebase(0x12344, true);
	CHECK(!entropy_nonce(a, sizeof a), "a lower boot count is ignored");
	entropy_rebase(0x12345, true);
	CHECK(!entropy_nonce(a, sizeof a), "the same count doesn't reset the draws");
	entropy_rebase(0x12346, true);
	CHECK(!entropy_nonce_want_bump() && entropy_nonce(a, sizeof a) && counter_of(a, sizeof a) == (0x12346u << 12),
	      "rebased");

	// Sizes: 8 is the least (4 random, 4 counter); fewer, or no buffer, refused
	CHECK(entropy_nonce(b, 8) && counter_of(b, 8) == ((0x12346u << 12) | 1u), "8 bytes");
	memset(b, 0xEE, sizeof b);
	CHECK(!entropy_nonce(b, 7) && all_zero(b, 7) && b[7] == 0xEE, "7 bytes refused, zeroed");
	CHECK(!entropy_nonce(NULL, 14), "no buffer");
	CHECK(!entropy_nonce(b, 0), "no length");
	CHECK(entropy_nonce(b, sizeof b) && counter_of(b, sizeof b) == ((0x12346u << 12) | 2u), "refusals drew none");

	// A boot count past 20 bits can't be told apart: spent
	entropy_init(NULL, uid_a, sizeof uid_a, 0xFFFFFu, true);
	keyup(1000);
	CHECK(entropy_nonce(a, sizeof a) && counter_of(a, sizeof a) == 0xFFFFF000u, "the last boot count");
	entropy_rebase(0x100000u, true);
	memset(a, 0xEE, sizeof a);
	CHECK(!entropy_nonce(a, sizeof a) && all_zero(a, sizeof a), "past 20 bits: spent");

	// Not saved: refused while the pool is weak (a key-up stir doesn't change that), given once it is seeded, or with
	// a hardware generator
	entropy_init(NULL, uid_a, sizeof uid_a, 8, false);
	keyup(1000);
	memset(a, 0xEE, sizeof a);
	CHECK(!entropy_nonce(a, sizeof a) && all_zero(a, sizeof a), "unsaved, weak: refused");
	for (int i = 0; i < 128; i++) {
		entropy_mix_u32((uint32_t)i, 2);
	}
	CHECK(entropy_nonce(a, sizeof a) && counter_of(a, sizeof a) == (8u << 12), "unsaved, seeded pool: given (draw 0)");
	entropy_init(&sys_trng, uid_a, sizeof uid_a, 8, false);
	CHECK(entropy_nonce(a, sizeof a) && counter_of(a, sizeof a) == (8u << 12), "unsaved, trng: given");
	// The store got it later: the weak pool's nonces come again
	entropy_init(NULL, uid_a, sizeof uid_a, 8, false);
	keyup(1000);
	CHECK(!entropy_nonce(a, sizeof a), "unsaved");
	entropy_rebase(8, true);
	CHECK(entropy_nonce(a, sizeof a) && counter_of(a, sizeof a) == (8u << 12), "saved later");
	entropy_rebase(8, false);
	CHECK(!entropy_nonce(a, sizeof a), "and said unsaved again");
}

// The wipes: after start-up and after each get the pool holds no input (its block buffer would keep the last bytes);
// a second start replaces the generator; the random part of a refused nonce never leaves
static void test_wipe(void)
{
	uint8_t out[16];

	trng_mode = TRNG_OK;
	entropy_init(&sys_trng, uid_a, sizeof uid_a, 1, true);
	CHECK(all_zero(e.pool.block, sizeof e.pool.block) && e.pool.used == 0 && e.pool.bytes == 0 && !e.grew,
	      "pool empty after init");
	uint8_t k1[32], v1[32];
	memcpy(k1, e.drbg.k, 32);
	memcpy(v1, e.drbg.v, 32);
	entropy_mix_u32(0xA5A5A5A5u, 2);
	CHECK(e.grew && e.pool.used == 4, "a stir waits in the pool");
	entropy_get(out, sizeof out);
	CHECK(all_zero(e.pool.block, sizeof e.pool.block) && e.pool.used == 0 && !e.grew, "drained");
	CHECK(memcmp(k1, e.drbg.k, 32) != 0 && memcmp(v1, e.drbg.v, 32) != 0, "K and V moved on");
	entropy_init(NULL, uid_b, sizeof uid_b, 2, true);
	CHECK(memcmp(k1, e.drbg.k, 32) != 0 && e.trng == NULL && e.draws == 0, "a second start replaces all");
}

// --- The boot count through a fake settings store ---

static settings_t store_live;	// the live copy
static settings_t store_saved;	// what the store holds
static bool store_writes;	// a flash that takes records
static uint8_t store_load = SETTINGS_LOADED;
static unsigned store_flushes;

settings_t *settings_begin(void)
{
	return &store_live;
}

void settings_end(uint32_t now_ms)
{
	(void)now_ms;
}

static bool store_dirty(void)
{
	return memcmp(&store_live, &store_saved, sizeof store_live) != 0;
}

bool settings_flush(void)
{
	store_flushes++;
	if (store_writes) {
		store_saved = store_live;
	}
	return !store_dirty();
}

void settings_get_stats(settings_stats_t *s)
{
	memset(s, 0, sizeof *s);
	s->load = store_load;
	s->dirty = store_dirty();
}

// The settings task's save, some time later
static void store_save(void)
{
	if (store_writes) {
		store_saved = store_live;
	}
}

// A restart: RAM goes, the store stays
static void power_cycle(void)
{
	crypto_wipe(&e, sizeof e);
	store_live = store_saved;
	fake_us = 0;
}

static bool chip_ok(hal_chip_t *c)
{
	memset(c, 0, sizeof *c);
	c->uid[0] = 0x11111111u;
	c->uid[1] = 0x22222222u;
	c->uid[2] = 0x33333333u;
	return true;
}

static const hal_system_t sys_chip = { .chip = chip_ok };

#define SEEN_MAX 40000u
static uint32_t seen[SEEN_MAX];
static unsigned seen_n;

static int cmpu32(const void *x, const void *y)
{
	uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
	return a < b ? -1 : a > b;
}

static void draw(unsigned n)
{
	uint8_t nonce[14];

	for (unsigned i = 0; i < n; i++) {
		entropy_boot_poll(i);
		keyup(i * 7919u + 1u);
		if (entropy_nonce(nonce, sizeof nonce) && seen_n < SEEN_MAX) {
			seen[seen_n++] = counter_of(nonce, sizeof nonce);
		}
	}
}

static void test_boot(void)
{
	uint8_t nonce[14];

	memset(&store_live, 0, sizeof store_live);
	settings_set_boot_count(&store_live, 41);
	store_saved = store_live;
	store_writes = true;
	store_load = SETTINGS_LOADED;
	crypto_wipe(&e, sizeof e);
	entropy_boot_poll(0);	// before the start: nothing
	CHECK(entropy_source() == ENTROPY_SRC_NONE && settings_boot_count(&store_live) == 41 &&
	      entropy_boot_count() == 0, "poll before start");

	// Start-up: 41 read, 42 saved at once, the uid in
	power_cycle();
	store_flushes = 0;
	entropy_boot_start(&sys_chip, 0);
	CHECK(settings_boot_count(&store_saved) == 42 && store_flushes == 1 && entropy_boot_count() == 42 &&
	      entropy_boot_saved(), "start-up bump: %u", settings_boot_count(&store_saved));
	CHECK(!entropy_nonce(nonce, sizeof nonce), "weak, no key-up stir yet: refused");
	keyup(5000);
	CHECK(entropy_nonce(nonce, sizeof nonce) && counter_of(nonce, sizeof nonce) == (42u << 12), "on 42");
	uint8_t first[32], second[32];
	entropy_get(first, sizeof first);
	power_cycle();
	settings_set_boot_count(&store_live, 41);	// the same start-up again, but with no chip ID
	store_saved = store_live;
	entropy_boot_start(NULL, 0);
	entropy_nonce(nonce, sizeof nonce);
	entropy_get(second, sizeof second);
	CHECK(memcmp(first, second, sizeof first) != 0, "the chip ID went in");

	// Every nonce over several start-ups, with run-time bumps and saves that come late: no counter twice
	seen_n = 0;
	for (int boot = 0; boot < 6; boot++) {
		power_cycle();
		entropy_boot_start(&sys_chip, 0);
		uint32_t base = entropy_boot_count();
		draw(3071);
		CHECK(!entropy_nonce_want_bump() && settings_boot_count(&store_live) == base, "no bump yet");
		draw(1);	// the 3,072nd: the poll before the next draw stores the next count
		entropy_boot_poll(1);
		CHECK(settings_boot_count(&store_live) == base + 1u && settings_boot_count(&store_saved) == base &&
		      entropy_boot_count() == base,
		      "next count asked for, not stored yet");
		store_writes = false;	// the store is slow: the counter runs on to the end of this count
		draw(1100);
		CHECK(entropy_boot_count() == base && seen[seen_n - 1] == ((base << 12) | 0xFFFu), "ran to the end");
		CHECK(!entropy_nonce(nonce, sizeof nonce), "spent until the store has it");
		store_writes = true;
		store_save();
		entropy_boot_poll(2);
		CHECK(entropy_boot_count() == base + 1u && entropy_nonce(nonce, sizeof nonce) &&
		      counter_of(nonce, sizeof nonce) == ((base + 1u) << 12), "moved on");
		seen[seen_n++] = counter_of(nonce, sizeof nonce);
		draw((unsigned)boot * 300u);
	}
	qsort(seen, seen_n, sizeof seen[0], cmpu32);
	int dup = 0;
	for (unsigned i = 1; i < seen_n; i++) {
		dup += seen[i] == seen[i - 1];
	}
	CHECK(dup == 0 && seen_n > 6u * 4096u, "%d counters twice in %u", dup, seen_n);

	// A store that can't write (no flash, another firmware's area): not saved; nonces refused while the pool is
	// weak, and the bump never asked for
	power_cycle();
	store_writes = false;
	store_load = SETTINGS_RAM;
	uint32_t was = settings_boot_count(&store_saved);
	entropy_boot_start(&sys_chip, 0);
	CHECK(!entropy_boot_saved() && entropy_boot_count() == was + 1u && settings_boot_count(&store_saved) == was,
	      "unsaved");
	keyup(5000);
	CHECK(!entropy_nonce(nonce, sizeof nonce) && all_zero(nonce, sizeof nonce), "unsaved, weak: refused");
	entropy_boot_poll(0);
	CHECK(!entropy_boot_saved(), "RAM: never saved");
	// The area becomes ours (a claim): the next save stores it, and the poll sees that
	store_load = SETTINGS_DEFAULTS;
	entropy_boot_poll(0);
	CHECK(!entropy_boot_saved(), "dirty: not yet");
	store_writes = true;
	store_save();
	entropy_boot_poll(0);
	keyup(5000);
	CHECK(entropy_boot_saved() && entropy_nonce(nonce, sizeof nonce) &&
	      counter_of(nonce, sizeof nonce) == ((was + 1u) << 12), "saved late");
	// A restart before the save landed reads the same count again: the second start-up's nonces need a seeded pool
	power_cycle();
	store_writes = false;
	store_load = SETTINGS_FOREIGN;
	entropy_boot_start(&sys_chip, 0);
	uint32_t c1 = entropy_boot_count();
	power_cycle();
	entropy_boot_start(&sys_chip, 0);
	keyup(5000);
	CHECK(entropy_boot_count() == c1 && !entropy_boot_saved() && !entropy_nonce(nonce, sizeof nonce),
	      "the same unsaved count twice: refused while weak");
	power_cycle();
	entropy_boot_start(&sys_trng, 0);
	CHECK(entropy_boot_count() == c1 && entropy_nonce(nonce, sizeof nonce), "with the generator: given");

	// The count stops at its end rather than wrap to 0
	power_cycle();
	store_writes = true;
	store_load = SETTINGS_LOADED;
	settings_set_boot_count(&store_live, SETTINGS_BOOT_COUNT_MAX);
	store_saved = store_live;
	entropy_boot_start(NULL, 0);
	CHECK(entropy_boot_count() == SETTINGS_BOOT_COUNT_MAX &&
	      settings_boot_count(&store_saved) == SETTINGS_BOOT_COUNT_MAX &&
	      !entropy_nonce(nonce, sizeof nonce), "saturated: spent");
}

// A boot count that went back (a settings layout change, a claim, an erase: crypto.md 8.3) on a radio without a
// hardware generator. Two start-ups on the same count, chip ID and saved flag: with no credited stir the nonce is
// refused (it would be the earlier start-up's, whole); with the key-up time stirred in, the nonces differ unless the
// key-up fell on the same microsecond; a tick that ran a microsecond late is enough to tell the pools apart. No chip
// ID with a weak pool: refused, stir or not; a seeded pool or the hardware generator gives it
static void test_rollback(void)
{
	uint8_t n1[14], n2[14], n3[14], n4[14];

	// Start-up 1 and start-up 2 read the same stored count (the store was reset in between)
	memset(&store_live, 0, sizeof store_live);
	settings_set_boot_count(&store_live, 0);
	store_saved = store_live;
	store_writes = true;
	store_load = SETTINGS_LOADED;
	us_step = 137u;
	power_cycle();
	entropy_boot_start(&sys_chip, 0);
	for (uint32_t t = 0; t < 50; t++) {
		entropy_boot_poll(t);
	}
	memset(n1, 0xEE, sizeof n1);
	CHECK(!entropy_nonce(n1, sizeof n1) && all_zero(n1, sizeof n1) && e.draws == 0,
	      "rollback: weak, no credited stir: refused, no draw used");
	keyup(2345678u);
	CHECK(entropy_nonce(n1, sizeof n1) && counter_of(n1, sizeof n1) == (1u << 12), "start-up 1: given");

	memset(&store_live, 0, sizeof store_live);	// the reset: the count starts over
	store_saved = store_live;
	power_cycle();
	entropy_boot_start(&sys_chip, 0);
	for (uint32_t t = 0; t < 50; t++) {
		entropy_boot_poll(t);
	}
	CHECK(entropy_boot_count() == 1u && !entropy_nonce(n2, sizeof n2), "start-up 2: same count, refused unstirred");
	keyup(2345679u);	// a microsecond later than start-up 1's press
	CHECK(entropy_nonce(n2, sizeof n2) && counter_of(n2, sizeof n2) == (1u << 12), "start-up 2: given");
	CHECK(memcmp(n1, n2, 10) != 0, "rollback: the key-up time keeps the random part apart");

	// The same key-up microsecond, the same ticks: the same nonce. That is the limit of the guarantee (spec 8.3)
	memset(&store_live, 0, sizeof store_live);
	store_saved = store_live;
	power_cycle();
	entropy_boot_start(&sys_chip, 0);
	for (uint32_t t = 0; t < 50; t++) {
		entropy_boot_poll(t);
	}
	CHECK(!entropy_nonce(n3, sizeof n3), "start-up 3: refused unstirred");	// as start-up 1 did: a get of its own
	keyup(2345678u);
	CHECK(entropy_nonce(n3, sizeof n3) && memcmp(n1, n3, sizeof n1) == 0, "identical start-ups: identical nonce");
	// One tick a microsecond late, the same key-up time: the pools differ
	memset(&store_live, 0, sizeof store_live);
	store_saved = store_live;
	power_cycle();
	entropy_boot_start(&sys_chip, 0);
	for (uint32_t t = 0; t < 50; t++) {
		if (t == 30) {
			fake_us++;
		}
		entropy_boot_poll(t);
	}
	CHECK(!entropy_nonce(n4, sizeof n4), "start-up 4: refused unstirred");
	keyup(2345678u);
	CHECK(entropy_nonce(n4, sizeof n4) && memcmp(n1, n4, 10) != 0, "a tick's jitter keeps the random part apart");
	// Each later draw of the stirred start-up stays apart too: the generator's chain went another way
	keyup(9000000u);
	CHECK(entropy_nonce(n4, sizeof n4) && counter_of(n4, sizeof n4) == ((1u << 12) | 1u), "second draw");

	// No chip ID: weak is refused even saved and stirred; seeded or with the generator, given
	entropy_init(NULL, NULL, 0, 5, true);
	keyup(1234u);
	memset(n1, 0xEE, sizeof n1);
	CHECK(!entropy_nonce(n1, sizeof n1) && all_zero(n1, sizeof n1), "no chip ID, weak: refused");
	entropy_init(NULL, uid_a, 0, 5, true);
	keyup(1234u);
	CHECK(!entropy_nonce(n1, sizeof n1), "a chip ID of no bytes: refused");
	entropy_init(NULL, NULL, 0, 5, true);
	for (int i = 0; i < 128; i++) {
		keyup((uint32_t)i);
	}
	CHECK(entropy_source() == ENTROPY_SRC_POOL && entropy_nonce(n1, sizeof n1), "no chip ID, seeded pool: given");
	trng_mode = TRNG_OK;
	entropy_init(&sys_trng, NULL, 0, 5, true);
	CHECK(entropy_nonce(n1, sizeof n1), "no chip ID, generator: given (no stir needed)");
	// An uncredited stir is not a key-up: still refused
	entropy_init(NULL, uid_a, sizeof uid_a, 5, true);
	entropy_mix_u32(1234u, 0);
	CHECK(!entropy_nonce(n1, sizeof n1), "an uncredited stir doesn't count");
	entropy_mix(NULL, 4, 32);
	CHECK(!entropy_nonce(n1, sizeof n1), "nothing given doesn't count");
	keyup(1234u);
	CHECK(entropy_nonce(n1, sizeof n1), "a credited one does");
	// The flag is per start-up
	entropy_init(NULL, uid_a, sizeof uid_a, 6, true);
	CHECK(!entropy_nonce(n1, sizeof n1), "a new start-up needs its own stir");
}

int main(void)
{
	test_unstarted();
	test_credits();
	test_trng();
	test_inputs();
	test_no_repeats();
	test_nonce();
	test_wipe();
	test_boot();
	test_rollback();
	printf("entropyTest: %u checks, %u failed\n", checks, failed);
	return failed ? 1 : 0;
}
