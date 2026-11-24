// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4's hardware random number generator, RNG (hal_system_t.entropy, CONFIG_ENTROPY; RM0090 section 24): an
// analog noise source giving a 32-bit word every 40 cycles of PLL48CLK, the 48 MHz clock USB runs on, which every
// STM32F4 board sets up (PLL Q in its stm32_clock_t). The core conditions the words (core/crypto/entropy.c); here they
// are only fetched and checked:
// - a seed error (SECS: the noise source stuck, or too few transitions) or a clock error (CECS: PLL48CLK too slow
//   against HCLK, or stopped) gives 0 bytes. A seed error restarts the generator (RNGEN cleared and set, SEIS
//   cleared), as RM0090 24.3.2 says; a clock error clears CEIS and waits for the clock;
// - the continuous test RM0090 24.3.1 asks for: the first word after the start is kept only to compare, and a word
//   equal to the one before it is a fault. The word kept between reads is never one that was output: each read ends
//   with one more word drawn only to compare, so no seed bits stay in static RAM once the pool has them;
// - a word that isn't ready in about 20 us (no PLL48CLK: the PLL never locked) is a fault too. A word takes under a
//   microsecond at 48 MHz, so a read of 32 bytes is done in about 10 us and never waits longer than that per word.
// One caller at a time (the radio core's task); no interrupt.
#include "config.h"

#if CONFIG_ENTROPY
#include "platform.h"

// DRDY polls per word: a poll is a peripheral read and a branch, some 6 to 8 cycles, so 20 to 27 us at 72 MHz (and
// still over ten words' time on a faster HCLK, a word being 40 cycles of PLL48CLK)
#define RNG_SPINS	240u

static bool on;			// the clock and the generator are enabled
static bool have_last;		// last holds the word before (the continuous test)
static uint32_t last;

static void rng_start(void)
{
	RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN;
	(void)RCC->AHB2ENR;	// the clock is on before the first register access
	RNG->CR = RNG_CR_RNGEN;	// no interrupt: polled
	on = true;
	have_last = false;
}

// Faults: SEIS and CEIS are cleared by writing 0 (rc_w0), only the ones read (one raised since stays set, written 1);
// a seed error needs the generator restarted
static void rng_fault(uint32_t sr)
{
	RNG->SR = ~(sr & (RNG_SR_SEIS | RNG_SR_CEIS)) & (RNG_SR_SEIS | RNG_SR_CEIS);
	if ((sr & (RNG_SR_SECS | RNG_SR_SEIS)) != 0) {
		RNG->CR = 0;
		RNG->CR = RNG_CR_RNGEN;
	}
	have_last = false;	// the next word starts the comparison again
}

// One word that passed the checks, false on a fault; it becomes last, so a word that is output must not be the read's
// final one (stm32_rng_read draws a spare)
static bool rng_word(uint32_t *w)
{
	for (;;) {
		uint32_t sr = 0;
		uint32_t i;
		for (i = 0; i < RNG_SPINS; i++) {
			sr = RNG->SR;
			if ((sr & (RNG_SR_DRDY | RNG_SR_SECS | RNG_SR_CECS | RNG_SR_SEIS | RNG_SR_CEIS)) != 0) {
				break;
			}
		}
		if ((sr & (RNG_SR_SECS | RNG_SR_CECS | RNG_SR_SEIS | RNG_SR_CEIS)) != 0) {
			rng_fault(sr);
			return false;
		}
		if (i == RNG_SPINS) {
			return false;	// no clock
		}
		uint32_t v = RNG->DR;
		if (!have_last) {
			last = v;	// the first word after a start: only to compare against
			have_last = true;
			continue;
		}
		bool same = v == last;
		last = v;
		if (same) {
			have_last = false;
			return false;
		}
		*w = v;
		return true;
	}
}

size_t stm32_rng_read(uint8_t *buf, size_t n)
{
	if (!on) {
		rng_start();
	}
	size_t got = 0;
	while (got < n) {
		uint32_t w;
		if (!rng_word(&w)) {
			return 0;	// a fault part way: nothing of this read counts (the caller wipes buf)
		}
		for (unsigned k = 0; k < 4u && got < n; k++) {
			buf[got++] = (uint8_t)(w >> (8u * k));
		}
		*(volatile uint32_t *)&w = 0;
	}
	// The spare: it replaces the output word in last and goes nowhere. A fault here voids the read as one part way
	// does (the bytes already in buf were never handed over).
	uint32_t spare;
	bool ok = rng_word(&spare);
	*(volatile uint32_t *)&spare = 0;
	return ok ? got : 0;
}

#endif
