// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The pure part of the STM32H7 I2S data direction probe (platform/stm32h7/hrc/i2s_dir.h): which sense is tried
// first for each wiring, and the verdict for every outcome of the two passes. The receive pad is given to the block
// only on a verdict of OK or SWAPPED, so every outcome that proves nothing must be one of the others.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../../platform/stm32h7/hrc/i2s_dir.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

#define E	STM32H7_I2S_PROBE_EDGES

// RM0433 Table 426, full duplex: IOSWP 0 drives the SDO pad, IOSWP 1 the SDI pad. Pass 0 is that sense for the pad
// the board transmits on, pass 1 the other
static void test_sense_order(void)
{
	CHECK(stm32h7_i2s_try_ioswp(false, 0) == false);	// transmit on SDO: IOSWP 0 first
	CHECK(stm32h7_i2s_try_ioswp(false, 1) == true);
	CHECK(stm32h7_i2s_try_ioswp(true, 0) == true);		// transmit on SDI: IOSWP 1 first
	CHECK(stm32h7_i2s_try_ioswp(true, 1) == false);
	// The two passes always differ, and any pass number but 0 is the second sense
	for (unsigned tx_on_sdi = 0; tx_on_sdi < 2; tx_on_sdi++) {
		CHECK(stm32h7_i2s_try_ioswp(tx_on_sdi != 0, 0) != stm32h7_i2s_try_ioswp(tx_on_sdi != 0, 1));
		CHECK(stm32h7_i2s_try_ioswp(tx_on_sdi != 0, 2) == stm32h7_i2s_try_ioswp(tx_on_sdi != 0, 1));
	}
}

static void test_verdicts(void)
{
	// A transmit pad something else drives: busy, whatever the counts say (no pass is run then)
	CHECK(stm32h7_i2s_verdict(false, 0, 0) == STM32H7_I2S_DIR_BUSY);
	CHECK(stm32h7_i2s_verdict(false, 1000, 0) == STM32H7_I2S_DIR_BUSY);
	CHECK(stm32h7_i2s_verdict(false, 0, 1000) == STM32H7_I2S_DIR_BUSY);
	CHECK(stm32h7_i2s_verdict(false, 1000, 1000) == STM32H7_I2S_DIR_BUSY);
	// Pass 0 drove the pad and pass 1 did not: the block is as Table 426 says
	CHECK(stm32h7_i2s_verdict(true, 800, 0) == STM32H7_I2S_DIR_OK);
	// Only pass 1: the other sense
	CHECK(stm32h7_i2s_verdict(true, 0, 800) == STM32H7_I2S_DIR_SWAPPED);
	// Neither
	CHECK(stm32h7_i2s_verdict(true, 0, 0) == STM32H7_I2S_DIR_NONE);
	// Both: the pad moves whichever way the block is set, so it is not the block that moves it. Nothing is proven
	CHECK(stm32h7_i2s_verdict(true, 800, 800) == STM32H7_I2S_DIR_BUSY);
	CHECK(stm32h7_i2s_verdict(true, 0xFFFF, 0xFFFF) == STM32H7_I2S_DIR_BUSY);
	// No outcome gives the value that means "not probed yet"
	for (unsigned is_free = 0; is_free < 2; is_free++) {
		for (unsigned a = 0; a < 2; a++) {
			for (unsigned b = 0; b < 2; b++) {
				const stm32h7_i2s_dir_t v = stm32h7_i2s_verdict(is_free != 0, a ? 800 : 0, b ? 800 : 0);
				CHECK(v != STM32H7_I2S_DIR_UNKNOWN);
			}
		}
	}
}

static void test_threshold(void)
{
	// The threshold itself counts as driven, one less does not: a few edges of noise on a pulled-down pad prove
	// nothing
	CHECK(E >= 2);
	CHECK(stm32h7_i2s_verdict(true, E, 0) == STM32H7_I2S_DIR_OK);
	CHECK(stm32h7_i2s_verdict(true, E - 1, 0) == STM32H7_I2S_DIR_NONE);
	CHECK(stm32h7_i2s_verdict(true, 0, E) == STM32H7_I2S_DIR_SWAPPED);
	CHECK(stm32h7_i2s_verdict(true, 0, E - 1) == STM32H7_I2S_DIR_NONE);
	CHECK(stm32h7_i2s_verdict(true, E - 1, E - 1) == STM32H7_I2S_DIR_NONE);
	// Noise in one pass does not hide the other pass's answer, and does not make it "both"
	CHECK(stm32h7_i2s_verdict(true, E, E - 1) == STM32H7_I2S_DIR_OK);
	CHECK(stm32h7_i2s_verdict(true, E - 1, E) == STM32H7_I2S_DIR_SWAPPED);
	CHECK(stm32h7_i2s_verdict(true, E, E) == STM32H7_I2S_DIR_BUSY);
	// The window sees far more edges than the threshold from a running block: an 8 kHz frame of two 32-bit channels
	// has at least one edge in each, so 4 ms give 64 or more
	CHECK(STM32H7_I2S_PROBE_MS * 8u * 2u >= 4u * E);
}

// What the half file relies on when it gives the receive pad to the block: a proven direction comes from a free line
// with exactly one pass that drove it, and from no other outcome, at every count around the threshold and at the
// counter's ends
static void test_proven_only_by_one_pass(void)
{
	static const uint16_t counts[] = { 0, 1, E - 1, E, E + 1, 800, 0xFFFE, 0xFFFF };
	const unsigned n = sizeof counts / sizeof counts[0];
	for (unsigned is_free = 0; is_free < 2; is_free++) {
		for (unsigned a = 0; a < n; a++) {
			for (unsigned b = 0; b < n; b++) {
				const stm32h7_i2s_dir_t v = stm32h7_i2s_verdict(is_free != 0, counts[a], counts[b]);
				const bool drove0 = counts[a] >= E;
				const bool drove1 = counts[b] >= E;
				CHECK((v == STM32H7_I2S_DIR_OK) == (is_free != 0 && drove0 && !drove1));
				CHECK((v == STM32H7_I2S_DIR_SWAPPED) == (is_free != 0 && !drove0 && drove1));
			}
		}
	}
}

int main(void)
{
	test_sense_order();
	test_verdicts();
	test_threshold();
	test_proven_only_by_one_pass();
	if (failures) {
		printf("stm32h7I2sDirTest: %d FAILED\n", failures);
		return 1;
	}
	printf("stm32h7I2sDirTest: all passed\n");
	return 0;
}
