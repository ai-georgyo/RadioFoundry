// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32H7's own start-up and identity (soc.c, reset.S, clock.c), included by stm32_family.h: the hooks a board
// may give for the time around a reset, the core set-up main() runs, and what the views say about the chip. Also read
// by the assembler (reset.S): everything but the register numbers below is C only.
#ifndef STM32H7_SOC_H
#define STM32H7_SOC_H

// The two registers Reset_Handler writes and reads before any C can run (RM0433 6.8.2, 6.8.4), by number: the
// assembler has no device header. soc.c asserts each against the device header's
#define STM32H7_PWR_CSR1_ADDR		0x58024804
#define STM32H7_PWR_CR3_ADDR		0x5802480C
#define STM32H7_PWR_CR3_LDOEN_BIT	0x00000002
#define STM32H7_PWR_CSR1_ACTVOSRDY_BIT	0x00002000
// Passes of the handler's wait for the supply to report its level. A bound, not a time: on the order of a tenth of a
// second at the reset clock, for a flag that is expected long before
#define STM32H7_SUPPLY_WAIT_PASSES	0x100000

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>

// Board hook, weak default: nothing. A board with outputs that are undefined at reset (a latched expander, a select
// line without a pull) puts them in their safe state here. Called once, from the reset handler, before .data and .bss
// exist: interrupts masked, HSI 64 MHz, the RCC at its reset values, FPU on. No access to .data, .bss or the heap:
// register writes and const data only, no SystemCoreClock, no stm32_wait_ns() (the cycle counter is not running).
// Returns in well under a millisecond
void board_reset_safe(void);
// Board hook, weak default: nothing (STM32_RESET_PREPARE()). Called by the shared reboot() with interrupts off, after
// board_rf_safe(), right before the system reset; also on a crash restart, from the fault handler. A board whose
// outputs outlive an MCU reset writes their reset state here: the next code to run may be the ROM bootloader (BOOT0
// is sampled at every reset), which writes none of them
void board_reset_prepare(void);
// Board hook, weak default: true (STM32_ROM_ENTRY_OK()). Whether the software entry to the ROM bootloader is allowed:
// a board on which the loader's pins meet other outputs says no (the loader drives its serial ports' pins,
// AN2606 Table 145). The key-at-power-on route is not asked
bool board_rom_entry_ok(void);

// From reset.S only, after ST's SystemInit: a backup domain that kept random content through a fast power cycle is
// reset (ES0392 2.2.32). Registers only
void stm32h7_reset_early(void);
// STM32_SOC_INIT(), from main(): interrupts and the vector table as a reset leaves them, whoever started the image,
// then interrupts on; the instruction cache on (the data cache and the MPU stay off)
void stm32h7_soc_init(void);
// DBGMCU REV_ID: 0x1003 = Y, 0x2003 = V
uint16_t stm32h7_rev_id(void);
// The ROM bootloader's version, the byte at 0x1FF1E7FE (AN2606 Table 3): 0x90 = V9.0
uint8_t stm32h7_rom_loader_id(void);

// Which tree stm32_clock_init() ended on (clock.c): the board's table from its crystal; the same rates from the HSI
// through PLL1 (the crystal or its PLL did not start, or the board's table has no crystal); the bare 64 MHz HSI
// (no PLL locked, or the table is outside the limits). STM32H7_CLOCK_HSI before the call
typedef enum { STM32H7_CLOCK_HSE, STM32H7_CLOCK_HSI_PLL, STM32H7_CLOCK_HSI } stm32h7_clock_source_t;
stm32h7_clock_source_t stm32h7_clock_source(void);
#endif

#endif
