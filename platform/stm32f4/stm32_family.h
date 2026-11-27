// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4 half of platform/stm32/platform.h, included only there: the ST HAL of this family and the names the
// shared STM32 code (platform/stm32) uses for what differs between the families. Every name here exists in each
// family's stm32_family.h; here each one is the text the code had when the STM32F4 was the only family.
#ifndef STM32_FAMILY_H
#define STM32_FAMILY_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h"

typedef struct {
	uint32_t hse_hz;
	bool hse_bypass;	// external clock fed in, no crystal
	uint8_t pll_m, pll_p, pll_q;
	uint16_t pll_n;
	uint32_t ahb_div, apb1_div, apb2_div;	// RCC_SYSCLK_DIVx / RCC_HCLK_DIVx values
	uint32_t flash_latency;			// FLASH_LATENCY_x
	bool lse;				// 32.768 kHz crystal fitted
} stm32_clock_t;

// main.c, after the ROM entry check and the stack fill, before the watchdog starts: nothing to set up on this family
#define STM32_SOC_INIT()		do { } while (0)

// gpio.c: the ports' clock enable register, and the slope stm32_pin_out / _in / _af give a pin
#define STM32_GPIO_CLKEN		RCC->AHB1ENR
#define STM32_GPIO_SPEED		GPIO_SPEED_FREQ_VERY_HIGH

// watchdog.c: the independent watchdog, and its stop while a debugger halts the core
#define STM32_IWDG			IWDG
#define STM32_IWDG_DEBUG_FREEZE()	(DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP)

// hrc/dmr_glue.c: the EXTI lines' pending register (the lost-interrupt record)
#define STM32_EXTI_PENDING		EXTI->PR

// time.c, before the cycle counter is written: the DWT has no lock on the Cortex-M4
#define STM32_DWT_UNLOCK()		do { } while (0)

// sysboot.c chip(): the family byte, and the option bytes with the RDP byte in bits 15:8
#define STM32_CHIP_FAMILY		HAL_CHIP_STM32F4
#define STM32_OPTBYTES()		FLASH->OPTCR
// sysboot.c reg(): a register write around the write buffer, so its bus fault is precise (ACTLR.DISDEFWBUF)
#define STM32_REG_WRITE_PRECISE(on) \
	do { \
		if (on) { \
			SCnSCB->ACTLR |= SCnSCB_ACTLR_DISDEFWBUF_Msk; \
		} else { \
			SCnSCB->ACTLR &= ~SCnSCB_ACTLR_DISDEFWBUF_Msk; \
		} \
	} while (0)

// sysboot.c, crash_stm32.c: the backup domain (RTC backup registers, backup SRAM): the power interface's clock, the
// write protection off and on, the backup SRAM's clock and address
#define STM32_BACKUP_CLOCK_ON() \
	do { \
		RCC->APB1ENR |= RCC_APB1ENR_PWREN; \
		(void)RCC->APB1ENR; \
	} while (0)
#define STM32_BACKUP_UNLOCK()		(PWR->CR |= PWR_CR_DBP)
#define STM32_BACKUP_LOCK()		(PWR->CR &= ~PWR_CR_DBP)
#define STM32_BKPSRAM_ON() \
	do { \
		RCC->AHB1ENR |= RCC_AHB1ENR_BKPSRAMEN; \
		(void)RCC->AHB1ENR; \
	} while (0)
#define STM32_BKPSRAM_BASE		BKPSRAM_BASE

// sysboot.c reboot(): whether the board allows the software entry to the ROM bootloader (always, here), the board's
// outputs to their reset state before a reset (the pins float through one, here), and the flush of the RAM a reset
// keeps (no ECC here: a write is in the RAM when it is made)
#define STM32_ROM_ENTRY_OK()		true
#define STM32_RESET_PREPARE()		do { } while (0)
#define STM32_KEPT_RAM_FLUSH()		do { } while (0)

// crash_stm32.c: the end of the internal flash (1 MB, STM32F405xG), and the RAM a stack or a task block can be in
// (main SRAM 128 KB, CCM 64 KB)
#define STM32_FLASH_HI			0x08100000u
#define STM32_IS_RAM(a) \
	(((a) >= 0x20000000u && (a) < 0x20020000u) || ((a) >= 0x10000000u && (a) < 0x10010000u))
// hrc/vocoder.c: the program flash has no error correction, so a read of it never faults
#define STM32_FLASH_ECC			0
// The reset cause: RCC_CSR (RM0090 7.3.21)
#define STM32_RESET_FLAGS		RCC->CSR
#define STM32_RESET_FLAGS_CLEAR()	(RCC->CSR |= RCC_CSR_RMVF)
#define STM32_RSTF_WATCHDOG		(RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF)
#define STM32_RSTF_SOFTWARE		RCC_CSR_SFTRSTF
#define STM32_RSTF_POWER		RCC_CSR_PORRSTF
#define STM32_RSTF_BROWNOUT		RCC_CSR_BORRSTF
#define STM32_RSTF_PIN			RCC_CSR_PINRSTF

// A DMA stream's memory address must be in main SRAM: the DMA controllers can't reach the CCM
#define STM32_DMA_REACHABLE(p)		(((uintptr_t)(p) >> 24) == 0x20u)

// hrc/hrc_bus.c: about 110 ns at 72 MHz on top of the GPIO writes, keeping the bit-banged clock near the 1-2 MHz the
// chip is known to take
#define STM32_HRC_BUS_DELAY() \
	do { \
		__NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); \
	} while (0)

#endif
