// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// STM32H7 start-up beside the reset handler (reset.S), and the chip's identity (soc.h): the backup domain check that
// runs before .data and .bss exist, the weak board hooks, and the core set-up main() calls as STM32_SOC_INIT().
//
// Caches and MPU: the instruction cache is on, the data cache and the MPU stay off. With the data cache off no DMA
// buffer needs cache maintenance, 32-byte alignment or an MPU region, and Arm erratum 1259864 (ES0392 2.1.1) has
// nothing to hit. The task stacks (the RTOS heap) and the hot statics are in the DTCM, which is never cached; the main
// stack (start-up and every interrupt), .data and .bss are in the AXI SRAM, which would be. The D2 domain's RAMs
// (SRAM1-3) are unused and nothing turns their clocks on.
#include <stddef.h>
#include "platform.h"

// reset.S works on the supply registers by number: the numbers are the device header's
_Static_assert(PWR_BASE + offsetof(PWR_TypeDef, CR3) == STM32H7_PWR_CR3_ADDR, "PWR_CR3");
_Static_assert(PWR_BASE + offsetof(PWR_TypeDef, CSR1) == STM32H7_PWR_CSR1_ADDR, "PWR_CSR1");
_Static_assert(PWR_CR3_LDOEN == STM32H7_PWR_CR3_LDOEN_BIT, "PWR_CR3 LDOEN");
_Static_assert(PWR_CSR1_ACTVOSRDY == STM32H7_PWR_CSR1_ACTVOSRDY_BIT, "PWR_CSR1 ACTVOSRDY");

#define ROM_LOADER_ID_ADDR	0x1FF1E7FEu	// AN2606 Table 3: the bootloader's version byte
#define PKG_LQFP208_TFBGA240	0x8u		// SYSCFG_PKGR PKG: one code for both (RM0433 12.3.11)

extern const uint32_t g_pfnVectors[];	// the vector table (ST's start-up file), at the image's first word

// ES0392 2.2.32, every silicon revision: with VBAT on the VDD supply a fast power cycle can skip the backup domain's
// reset and leave RCC_BDCR and the RTC backup registers with random content. A random LSEON hands PC14 / PC15 to the
// oscillator whatever the GPIO registers say (RM0433 11.3.14), and a random BKP19R is what the ROM entry check
// (sysboot.c) reads. ST's workaround is to reset the domain by software on the first start after power came up.
//
// "Power came up" is PORRSTF or BORRSTF with neither a software reset nor a watchdog reset since: nothing of ours is
// in the domain yet then. A software reset must keep its BKP19R request, also in a build that never clears the flags
// (they are cleared by crash_stm32.c, which is left its RMVF: the crash log wants the cause). An LSE that is on or in
// bypass is never this platform's state, so that resets the domain whatever the cause.
//
// From Reset_Handler, before .data and .bss: registers only
void stm32h7_reset_early(void)
{
	const uint32_t rsr = RCC->RSR;
	const bool power_up = (rsr & (RCC_RSR_PORRSTF | RCC_RSR_BORRSTF)) != 0 &&
			      (rsr & (RCC_RSR_SFTRSTF | RCC_RSR_IWDG1RSTF | RCC_RSR_WWDG1RSTF)) == 0;
	if (!power_up && (RCC->BDCR & (RCC_BDCR_LSEON | RCC_BDCR_LSEBYP)) == 0) {
		return;
	}
	STM32_BACKUP_UNLOCK();		// the domain's registers take writes (the bit has read back set, or a bound ran out)
	RCC->BDCR = RCC_BDCR_BDRST;	// the reset, and every other bit of the register to 0
	(void)RCC->BDCR;		// the read makes the reset long enough (the erratum's sequence)
	RCC->BDCR = 0;
	STM32_BACKUP_LOCK();
}

__attribute__((weak)) void board_reset_safe(void)
{
}

__attribute__((weak)) void board_reset_prepare(void)
{
}

__attribute__((weak)) bool board_rom_entry_ok(void)
{
	return true;
}

// The state a reset gives, whoever started the image: the ROM bootloader's jump to an image it has written can
// leave its SysTick, its USB interrupt and its own vector table behind (AN2606 4.1: the registers it used "must be
// reconfigured in the user application"). Reset_Handler masked interrupts; here they are cleared, then let through
void stm32h7_soc_init(void)
{
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;
	for (unsigned i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
		NVIC->ICER[i] = 0xFFFFFFFFu;
		NVIC->ICPR[i] = 0xFFFFFFFFu;
	}
	// A tick or a context switch left pending would run the RTOS's handlers before the scheduler exists
	SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
	SCB->VTOR = (uint32_t)(uintptr_t)g_pfnVectors;	// SystemInit set it too (USER_VECT_TAB_ADDRESS, platform.mk)
	// CCR's two traps: main.c cleared them before this, for every family
	__DSB();
	__ISB();
	__enable_irq();

	SCB_EnableICache();	// invalidates it first; a no-op when it is already on

	// PC2 and PC3 are joined to the PC2_C / PC3_C pads through analog switches (SYSCFG_PMCR PC2SO and PC3SO, RM0433
	// 12.3.1). The package decides: closed, 0, is the reset value where only the _C pads exist, open where PC2 and
	// PC3 have pads of their own too (the TFBGA240; its package code is the LQFP208's, SYSCFG_PKGR 12.3.11). Every
	// system reset restores it; written again only where it is known, in case a loader that jumped here changed it
	__HAL_RCC_SYSCFG_CLK_ENABLE();
	if ((SYSCFG->PKGR & SYSCFG_PKGR_PKG) != PKG_LQFP208_TFBGA240) {
		SYSCFG->PMCR &= ~(SYSCFG_PMCR_PC2SO | SYSCFG_PMCR_PC3SO);
	}
}

uint16_t stm32h7_rev_id(void)
{
	return (uint16_t)((DBGMCU->IDCODE & DBGMCU_IDCODE_REV_ID) >> DBGMCU_IDCODE_REV_ID_Pos);
}

uint8_t stm32h7_rom_loader_id(void)
{
	return *(volatile const uint8_t *)ROM_LOADER_ID_ADDR;
}
