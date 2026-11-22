// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The modem's three interrupt lines (hrc/dmr_glue.c): which EXTI line each one is on. Board parameters: the lines are
// pins of one port (stm32_hrc_dmr_board_t.int_port), pins 0, 1 and 2 unless the board's board.mk says otherwise in
// MCU_DEFS (-DSTM32_HRC_LINE_SYS=3). A line is 0..4: those have an interrupt vector each on every STM32 family, which
// the glue's three handlers need. No hardware here: the host tests build it (tests/host/hrcLinesTest.c).
#ifndef STM32_HRC_LINES_H
#define STM32_HRC_LINES_H

#include <stdint.h>

// Plain digits, no suffix or parentheses: the glue pastes them into the vector and interrupt names (EXTI<n>_IRQn)
#ifndef STM32_HRC_LINE_TS
#define STM32_HRC_LINE_TS	0	// timeslot
#endif
#ifndef STM32_HRC_LINE_SYS
#define STM32_HRC_LINE_SYS	1	// system
#endif
#ifndef STM32_HRC_LINE_RFTX
#define STM32_HRC_LINE_RFTX	2	// RF TX
#endif

_Static_assert(STM32_HRC_LINE_TS >= 0 && STM32_HRC_LINE_TS <= 4, "the timeslot line needs its own vector (EXTI0-4)");
_Static_assert(STM32_HRC_LINE_SYS >= 0 && STM32_HRC_LINE_SYS <= 4, "the system line needs its own vector (EXTI0-4)");
_Static_assert(STM32_HRC_LINE_RFTX >= 0 && STM32_HRC_LINE_RFTX <= 4, "the RF TX line needs its own vector (EXTI0-4)");
_Static_assert(STM32_HRC_LINE_TS != STM32_HRC_LINE_SYS && STM32_HRC_LINE_TS != STM32_HRC_LINE_RFTX &&
	       STM32_HRC_LINE_SYS != STM32_HRC_LINE_RFTX, "three lines, three pins");

// The lines as pin masks of the port and as bits of the EXTI registers (the same numbers)
#define INT_TS		(1u << STM32_HRC_LINE_TS)
#define INT_SYS		(1u << STM32_HRC_LINE_SYS)
#define INT_RFTX	(1u << STM32_HRC_LINE_RFTX)

// 1: timeslot, system, RF TX are three lines in a row, so their bits in any register sit side by side in that order
#if STM32_HRC_LINE_SYS == STM32_HRC_LINE_TS + 1 && STM32_HRC_LINE_RFTX == STM32_HRC_LINE_TS + 2
#define STM32_HRC_LINES_ADJACENT	1
#else
#define STM32_HRC_LINES_ADJACENT	0
#endif

// The three lines' bits of a register word that has EXTI line n at bit (first + n), picked one by one into bits 0, 1
// and 2: timeslot, system, RF TX. For the lines that are not in a row (an NVIC word: first = EXTI0's interrupt number)
static inline uint32_t stm32_hrc_lines_pick(uint32_t word, unsigned first)
{
	return ((word >> (first + STM32_HRC_LINE_TS)) & 1u) | ((word >> (first + STM32_HRC_LINE_SYS)) & 1u) << 1 |
	       ((word >> (first + STM32_HRC_LINE_RFTX)) & 1u) << 2;
}

#endif
