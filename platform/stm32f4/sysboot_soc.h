// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4 half of sysboot.c, included there once: where the ROM bootloader is, and the way into it. Uses
// SPIN_LIMIT of the including file.
#define SYSMEM_BASE		0x1FFF0000u
#define SYSMEM_END		0x1FFF7800u

static void spin_until(volatile uint32_t *reg, uint32_t mask, uint32_t want)
{
	for (uint32_t i = 0; i < SPIN_LIMIT && (*reg & mask) != want; i++) {
	}
}

// From stm32_sysboot_check(), with the board's pins in their reset-time state, interrupts off, SysTick and the NVIC
// cleared: clocks and peripherals back to their reset state, system memory mapped at 0, then the ROM's reset vector.
// Never returns to its caller
static inline __attribute__((always_inline)) void stm32_rom_enter(void)
{
	// Clocks back to HSI 16 MHz, HSE and the PLLs off and at their reset values (the stock bootloader leaves
	// HSE/PLL running; the ROM sets its own clocks up)
	RCC->CR |= RCC_CR_HSION;
	spin_until(&RCC->CR, RCC_CR_HSIRDY, RCC_CR_HSIRDY);
	RCC->CFGR = 0;
	spin_until(&RCC->CFGR, RCC_CFGR_SWS, 0);
	RCC->CR &= ~(RCC_CR_PLLON | RCC_CR_PLLI2SON | RCC_CR_CSSON);
	spin_until(&RCC->CR, RCC_CR_PLLRDY | RCC_CR_PLLI2SRDY, 0);
	RCC->CR &= ~RCC_CR_HSEON;
	spin_until(&RCC->CR, RCC_CR_HSERDY, 0);
	RCC->CR &= ~RCC_CR_HSEBYP;
	RCC->PLLCFGR = 0x24003010u;
	RCC->PLLI2SCFGR = 0x20003000u;
	RCC->CIR = 0x00FF0000u;	// clear the ready flags, interrupts off
	RCC->CIR = 0;
	FLASH->ACR = 0;		// 0 wait states is right for 16 MHz; caches off

	// Peripherals to their reset state, except the GPIO ports (the board's pin levels above, the power latch)
	RCC->AHB1RSTR = RCC_AHB1RSTR_DMA1RST | RCC_AHB1RSTR_DMA2RST | RCC_AHB1RSTR_OTGHRST | RCC_AHB1RSTR_CRCRST;
	RCC->AHB2RSTR = 0xFFFFFFFFu;
	RCC->AHB3RSTR = 0xFFFFFFFFu;
	RCC->APB1RSTR = 0xFFFFFFFFu;
	RCC->APB2RSTR = 0xFFFFFFFFu;
	RCC->AHB1RSTR = 0;
	RCC->AHB2RSTR = 0;
	RCC->AHB3RSTR = 0;
	RCC->APB1RSTR = 0;
	RCC->APB2RSTR = 0;
	RCC->AHB1ENR &= 0x000001FFu;	// GPIOA..GPIOI only
	RCC->AHB2ENR = 0;
	RCC->AHB3ENR = 0;
	RCC->APB1ENR = 0;
	RCC->APB2ENR = RCC_APB2ENR_SYSCFGEN;
	(void)RCC->APB2ENR;

	uint32_t sp = *(volatile const uint32_t *)SYSMEM_BASE;
	uint32_t pc = *(volatile const uint32_t *)(SYSMEM_BASE + 4u);
	if (sp < 0x20000000u || sp > 0x20020000u || pc < SYSMEM_BASE || pc >= SYSMEM_END) {
		NVIC_SystemReset();	// not a ROM vector table: boot normally (the flag is gone)
	}
	SYSCFG->MEMRMP = SYSCFG_MEMRMP_MEM_MODE_0;	// system flash at 0
	SCB->VTOR = 0;
	__DSB();
	__ISB();
	__enable_irq();		// the reset state; nothing can interrupt now (NVIC cleared, SysTick off)
	__asm volatile("msr msp, %0\n\tbx %1" : : "r"(sp), "r"(pc) : "memory");
}
