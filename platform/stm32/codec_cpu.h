// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The processor state the stock codec needs (docs/spec/codec-placement.md, "STM32 radios"): the two traps of the
// core's configuration and control register off. The start-up clears them on every STM32 family (main.c: a
// bootloader's jump to the image is no reset), and the host makes sure once more, before its first codec call
// (hrc/vocoder.c); nothing may set either bit afterwards, and nothing in the tree does.
// No hardware here: the host tests run it (tests/host/codecDescTest.c).
#ifndef RFOUNDRY_STM32_CODEC_CPU_H
#define RFOUNDRY_STM32_CODEC_CPU_H

#include <stdbool.h>
#include <stdint.h>

#define CODEC_CCR_UNALIGN_TRP	(1u << 3)	// SCB->CCR
#define CODEC_CCR_DIV_0_TRP	(1u << 4)
#define CODEC_CCR_TRAPS		(CODEC_CCR_UNALIGN_TRP | CODEC_CCR_DIV_0_TRP)

// The codec loads words at unaligned addresses and divides without a test for zero: both traps off, their reset
// value. True when one was set and the register was written: the caller then issues the barriers
static inline bool codec_cpu_traps_off(volatile uint32_t *ccr)
{
	const uint32_t was = *ccr;
	if ((was & CODEC_CCR_TRAPS) == 0) {
		return false;
	}
	*ccr = was & ~CODEC_CCR_TRAPS;
	return true;
}

#endif
