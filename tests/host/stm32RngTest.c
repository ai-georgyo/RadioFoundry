// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4 hardware generator's driver (platform/stm32f4/rng.c; docs/spec/crypto.md 8.4) against a scripted RNG
// block (stubs/stm32_rng/stm32f4xx_hal.h): the continuous test (a repeated word is a fault), a seed or clock fault part
// way, and the word kept between reads for that test, which must never be one the read gave out (crypto.md 8.2: the
// generator's words that went into the pool don't stay in RAM). rng.c is included, so the test sees that word
#include <stdio.h>
#include <string.h>
#include "../../platform/stm32f4/rng.c"

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

fake_rcc_t fake_rcc;
static fake_rng_t regs;
static uint32_t state = 0x2545F491u;
static unsigned accesses;	// register accesses since the script was set
static unsigned fault_at;	// from this access on the status shows a seed error (0: never)
static bool stuck;		// the data register repeats one value

// Every access gives a fresh data word (an xorshift: no two alike) and a status of ready, or the scripted fault
fake_rng_t *fake_rng(void)
{
	accesses++;
	if (!stuck) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
	}
	regs.DR = state;
	regs.SR = fault_at != 0 && accesses >= fault_at ? RNG_SR_SECS : RNG_SR_DRDY;
	return &regs;
}

static void script(unsigned fault, bool repeat)
{
	accesses = 0;
	fault_at = fault;
	stuck = repeat;
}

// Whether the kept word is one of the words a read gave out, as rng.c packs them (little endian). A last word given
// only in part counts by the bytes that went out
static bool kept_word_given(const uint8_t *buf, size_t n)
{
	for (size_t i = 0; i < n; i += 4u) {
		size_t k = n - i < 4u ? n - i : 4u;
		uint32_t mask = k == 4u ? 0xFFFFFFFFu : (1u << (8u * k)) - 1u;
		uint32_t w = 0;
		for (size_t j = 0; j < k; j++) {
			w |= (uint32_t)buf[i + j] << (8u * j);
		}
		if (w == (last & mask)) {
			return true;
		}
	}
	return false;
}

int main(void)
{
	uint8_t buf[32];

	// A whole read: 32 bytes, the clock on, and the word kept for the next comparison is none of the 8 given
	script(0, false);
	CHECK(stm32_rng_read(buf, sizeof buf) == sizeof buf, "a whole read");
	CHECK((fake_rcc.AHB2ENR & RCC_AHB2ENR_RNGEN) != 0 && on, "the generator started");
	CHECK(have_last && !kept_word_given(buf, sizeof buf), "the kept word was given out");
	// Again, without a restart: the comparison carries over, and still nothing given stays
	script(0, false);
	CHECK(stm32_rng_read(buf, sizeof buf) == sizeof buf && !kept_word_given(buf, sizeof buf), "a second read");
	// A length that isn't a whole word
	CHECK(stm32_rng_read(buf, 7) == 7u && !kept_word_given(buf, 7), "7 bytes: the kept word was given in part");

	// A word equal to the one before: the continuous test fails the read
	script(0, true);
	CHECK(stm32_rng_read(buf, sizeof buf) == 0, "a stuck generator gave bytes");
	CHECK(!have_last, "the comparison starts again after a repeat");
	// A seed error part way voids the read
	script(9, false);
	CHECK(stm32_rng_read(buf, sizeof buf) == 0, "a fault part way gave bytes");
	// A seed error on the spare word, after the 32 bytes are in: the read is voided too. The read takes 2 accesses
	// a word (status, data): the first word after the restart primes, 8 are given, the spare's status is the 19th
	script(0, false);
	stm32_rng_read(buf, sizeof buf);	// a good read first, so no restart's priming word is needed
	script(17, false);
	CHECK(stm32_rng_read(buf, sizeof buf) == 0, "a fault on the spare word gave the read");
	script(0, false);
	CHECK(stm32_rng_read(buf, sizeof buf) == sizeof buf, "back after the fault");
	printf("stm32RngTest: %u checks, %u failed\n", checks, failed);
	return failed ? 1 : 0;
}
