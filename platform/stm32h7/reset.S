/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* STM32H7 reset handler. It takes the place of the weak Reset_Handler in ST's start-up file, which is built for its
 * vector table only. ST's handler waits for the supply without a bound and gives a board no chance to act before
 * .data and .bss are set up; this one bounds the wait and calls the platform's and the board's reset-time work first.
 *
 * Who enters here: a reset, or the ROM bootloader starting the image it has just written, with its own interrupts
 * and SysTick possibly still enabled (AN2606 Figure 80). So interrupts are masked first, and stay masked until
 * stm32h7_soc_init() (from main) has cleared what a loader may have left.
 *
 * Order, and why:
 *  1. The supply configuration. After a power-on reset the system is in Run* mode and RAM writes are not permitted
 *     until PWR_CR3 is written and ACTVOSRDY is set (RM0433 6.4.1): registers only, no stack. LDOEN is the only
 *     valid choice (no SMPS on this part) and is the reset value: the write is what counts, and it is the one write
 *     the register's low byte takes after a power-on reset. A ROM bootloader that ran before us has made it already
 *     and may have locked the register (AN2606 Table 145): the flag is then set and the wait ends at once. The
 *     voltage scale stays at its reset value, VOS3.
 *  2. ST's SystemInit: registers only. It turns the FPU on (code built for the hard float ABI may not run before it),
 *     returns the RCC to its reset state (HSI 64 MHz, whatever a loader left) and applies the revision Y AXI SRAM
 *     workaround (ES0392 2.2.10) the stack depends on.
 *  3. stm32h7_reset_early (soc.c), then the board's board_reset_safe (weak, soc.h has its contract): flash code and
 *     constants only, .data and .bss do not exist yet. A board's outputs get their safe state before the .bss clear.
 *  4. .data, .bss, the C library's constructors (ccm.c sets up the CPU-only RAM and the RTOS heap there), main. */

#include "soc.h"

	.syntax	unified
	.cpu	cortex-m7
	.thumb

	.section .text.Reset_Handler, "ax", %progbits
	.global	Reset_Handler
	.type	Reset_Handler, %function
Reset_Handler:
	cpsid	i
	ldr	sp, =_estack

	ldr	r0, =STM32H7_PWR_CR3_ADDR
	ldr	r1, [r0]
	orr	r1, r1, #STM32H7_PWR_CR3_LDOEN_BIT
	str	r1, [r0]
	ldr	r0, =STM32H7_PWR_CSR1_ADDR
	ldr	r2, =STM32H7_SUPPLY_WAIT_PASSES
1:	ldr	r1, [r0]
	tst	r1, #STM32H7_PWR_CSR1_ACTVOSRDY_BIT
	bne	2f
	subs	r2, r2, #1
	bne	1b
2:
	bl	SystemInit
	bl	stm32h7_reset_early
	bl	board_reset_safe

	/* .data from its load address in flash */
	ldr	r0, =_sdata
	ldr	r1, =_edata
	ldr	r2, =_sidata
	b	4f
3:	ldr	r3, [r2], #4
	str	r3, [r0], #4
4:	cmp	r0, r1
	bcc	3b

	/* .bss */
	ldr	r0, =_sbss
	ldr	r1, =_ebss
	movs	r3, #0
	b	6f
5:	str	r3, [r0], #4
6:	cmp	r0, r1
	bcc	5b

	bl	__libc_init_array
	bl	main
7:	b	7b
	.pool

	.size	Reset_Handler, . - Reset_Handler
