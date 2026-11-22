// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32H7 half of sysboot.c, included there once: where the ROM bootloader is, and the way into it (AN2606 4.1
// and 63.1).
#define SYSMEM_BASE		0x1FF00000u	// system memory, 128 KB
#define SYSMEM_END		0x1FF20000u
#define ROM_VECTORS		0x1FF09800u	// "the bootloader start address": its stack pointer and reset vector
#define ROM_SP_LO		0x20000000u	// the loader's RAM: the DTCM and the AXI SRAM (AN2606 Table 145)
#define ROM_SP_HI		0x24080000u

// From stm32_sysboot_check(), with the board's outputs in their reset-time state, interrupts off, SysTick and the
// NVIC cleared. Nothing has to be undone first: the check runs before STM32_SOC_INIT() and the board's clock set-up,
// so the HSI is the clock and the PLLs and both caches are off, as after a reset, and nothing but the board's
// reset-time pin work has run; the watchdog has not been started. This family has no memory remap: the loader's own
// table address goes into VTOR. Never returns to its caller
static inline __attribute__((always_inline)) void stm32_rom_enter(void)
{
	uint32_t sp = *(volatile const uint32_t *)ROM_VECTORS;
	uint32_t pc = *(volatile const uint32_t *)(ROM_VECTORS + 4u);
	if (sp < ROM_SP_LO || sp > ROM_SP_HI || pc < SYSMEM_BASE || pc >= SYSMEM_END || (pc & 1u) == 0) {
		NVIC_SystemReset();	// not a ROM vector table: boot normally (the flag is gone)
	}
	SCB->VTOR = ROM_VECTORS;
	__DSB();
	__ISB();
	__enable_irq();		// the loader needs interrupts and does not enable them itself (AN2606 4.1); none is
				// enabled or pending now
	__asm volatile("msr msp, %0\n\tbx %1" : : "r"(sp), "r"(pc) : "memory");
}
