// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The modem interrupt lines of the STM32 glue (platform/stm32/hrc/hrc_lines.h): the masks, the "in a row" flag, and
// the one-by-one pick of the three lines' bits. Built twice: with the default lines (0, 1, 2) and, as
// hrcLinesAltTest, with the system and RF TX lines crossed (-DSTM32_HRC_LINE_SYS=3 -DSTM32_HRC_LINE_RFTX=2).
#include <stdint.h>
#include <stdio.h>
#include "../../platform/stm32/hrc/hrc_lines.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

#define IRQ_FIRST	6u	// EXTI0's interrupt number on the STM32 families: lines 0..4 are IRQs 6..10

// What the glue's lost-interrupt record wants from an NVIC word: the three lines' bits, each found on its own
static uint32_t three_bits(uint32_t word)
{
	uint32_t ts = (word >> (IRQ_FIRST + (unsigned)STM32_HRC_LINE_TS)) & 1u;
	uint32_t sys = (word >> (IRQ_FIRST + (unsigned)STM32_HRC_LINE_SYS)) & 1u;
	uint32_t rftx = (word >> (IRQ_FIRST + (unsigned)STM32_HRC_LINE_RFTX)) & 1u;
	return ts | sys << 1 | rftx << 2;
}

int main(void)
{
	// The masks are the pins: one bit each, at the line's number
	CHECK(INT_TS == 1u << STM32_HRC_LINE_TS);
	CHECK(INT_SYS == 1u << STM32_HRC_LINE_SYS);
	CHECK(INT_RFTX == 1u << STM32_HRC_LINE_RFTX);
	CHECK((INT_TS & INT_SYS) == 0 && (INT_TS & INT_RFTX) == 0 && (INT_SYS & INT_RFTX) == 0);
	CHECK((INT_TS | INT_SYS | INT_RFTX) <= 0x1Fu);	// lines 0..4 only: a vector each

#if STM32_HRC_LINE_TS == 0 && STM32_HRC_LINE_SYS == 1 && STM32_HRC_LINE_RFTX == 2
	// The default: pins 0, 1, 2
	CHECK(INT_TS == 0x01u && INT_SYS == 0x02u && INT_RFTX == 0x04u);
	CHECK(STM32_HRC_LINES_ADJACENT == 1);
#elif STM32_HRC_LINE_TS == 0 && STM32_HRC_LINE_SYS == 3 && STM32_HRC_LINE_RFTX == 2
	// System and RF TX crossed: three pins, not in a row
	CHECK(INT_TS == 0x01u && INT_SYS == 0x08u && INT_RFTX == 0x04u);
	CHECK(STM32_HRC_LINES_ADJACENT == 0);
#else
#error "hrcLinesTest: a line set this test has no expectations for"
#endif

	// The flag says what it means: system right after timeslot, RF TX right after system
	CHECK(STM32_HRC_LINES_ADJACENT ==
	      (STM32_HRC_LINE_SYS == STM32_HRC_LINE_TS + 1 && STM32_HRC_LINE_RFTX == STM32_HRC_LINE_TS + 2));

	// The pick, on every combination of the five vectors' bits with the neighbors set and clear: always the three
	// lines' bits in the order timeslot, system, RF TX, and never a neighbor's
	for (uint32_t lines = 0; lines < 32u; lines++) {
		for (uint32_t fill = 0; fill < 2u; fill++) {
			uint32_t word = (lines << IRQ_FIRST) | (fill ? ~(0x1Fu << IRQ_FIRST) : 0u);
			uint32_t got = stm32_hrc_lines_pick(word, IRQ_FIRST);
			CHECK(got == three_bits(word));
			CHECK(((got >> 0) & 1u) == ((lines >> STM32_HRC_LINE_TS) & 1u));
			CHECK(((got >> 1) & 1u) == ((lines >> STM32_HRC_LINE_SYS) & 1u));
			CHECK(((got >> 2) & 1u) == ((lines >> STM32_HRC_LINE_RFTX) & 1u));
#if STM32_HRC_LINES_ADJACENT
			// In a row, the glue's one shift and mask gives the same
			CHECK(got == ((word >> (IRQ_FIRST + (unsigned)STM32_HRC_LINE_TS)) & 7u));
#endif
		}
	}
	// Each line alone lands on its own bit
	CHECK(stm32_hrc_lines_pick(INT_TS, 0) == 1u);
	CHECK(stm32_hrc_lines_pick(INT_SYS, 0) == 2u);
	CHECK(stm32_hrc_lines_pick(INT_RFTX, 0) == 4u);
	CHECK(stm32_hrc_lines_pick(INT_TS | INT_SYS | INT_RFTX, 0) == 7u);
	CHECK(stm32_hrc_lines_pick(~(INT_TS | INT_SYS | INT_RFTX), 0) == 0u);

#if STM32_HRC_LINES_ADJACENT
	printf("hrcLinesTest: %s\n", failures ? "FAILED" : "passed");
#else
	printf("hrcLinesAltTest: %s\n", failures ? "FAILED" : "passed");
#endif
	return failures != 0;
}
