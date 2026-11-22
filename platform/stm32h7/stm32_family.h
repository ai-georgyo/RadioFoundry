// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32H7 half of platform/stm32/platform.h, included only there: the ST HAL of this family and the names the
// shared STM32 code (platform/stm32) uses for what differs between the families. Every name here exists in each
// family's stm32_family.h. Included before platform.h declares anything: a macro here may name what platform.h
// defines (it expands at its use), an inline function may not.
#ifndef STM32_FAMILY_H
#define STM32_FAMILY_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32h7xx_hal.h"
#include "clock_math.h"		// stm32_clock_t: the board's clock table
#include "soc.h"		// the reset hooks, stm32h7_soc_init

// main.c, after the ROM entry check and the stack fill, before the watchdog starts: interrupts and the vector table
// as a reset leaves them (the ROM bootloader may have started the image), the instruction cache (soc.c)
#define STM32_SOC_INIT()		stm32h7_soc_init()

// gpio.c: the ports' clock enable register, and the slope stm32_pin_out / _in / _af give a pin. Low: 12 MHz, 16 ns
// edges at 3.3 V (DS12110 Table 53), enough for every bit-banged line and kinder to a receiver next to it. A bus that
// needs more raises its pins with stm32_pin_speed()
#define STM32_GPIO_CLKEN		RCC->AHB4ENR
#define STM32_GPIO_SPEED		GPIO_SPEED_FREQ_LOW

// watchdog.c: the independent watchdog, and its stop while a debugger halts the core
#define STM32_IWDG			IWDG1
#define STM32_IWDG_DEBUG_FREEZE()	(DBGMCU->APB4FZ1 |= DBGMCU_APB4FZ1_DBG_IWDG1)

// hrc/dmr_glue.c: the EXTI lines' pending register (the lost-interrupt record)
#define STM32_EXTI_PENDING		EXTI->PR1

// time.c, before the cycle counter is written: the Cortex-M7's DWT takes no write until its lock is opened
#define STM32_DWT_UNLOCK()		(DWT->LAR = 0xC5ACCE55u)

// sysboot.c chip(): the family byte, and the option bytes with the RDP byte in bits 15:8
#define STM32_CHIP_FAMILY		HAL_CHIP_STM32H7
#define STM32_OPTBYTES()		FLASH->OPTSR_CUR
// sysboot.c reg(): the Cortex-M7 has no switch that takes a write around its write buffer. reg() tests the imprecise
// bus fault flag after its barrier, which is what catches a refused write here
#define STM32_REG_WRITE_PRECISE(on)	do { (void)(on); } while (0)

// sysboot.c, crash_stm32.c: the backup domain (RTC backup registers, backup SRAM). The power interface has no clock
// gate on this family and the RTC's bus clock is on from reset (RCC_APB4ENR RTCAPBEN), so there is nothing to turn
// on first; then the write protection off and on, the backup SRAM's clock and address
#define STM32_BACKUP_CLOCK_ON()		do { } while (0)
// The unlock waits, with a bound, until DBP reads back set, as ST's HAL does before its first write to the domain:
// RM0433 does not say that a write right behind the bit is taken. The callers write at once (the ROM entry request
// and its clearing in sysboot.c are each the first write after a reset, which clears DBP), and a request that can't
// be cleared is never acted on. One read when the bit is set already
static inline void stm32h7_backup_unlock(void)
{
	PWR->CR1 |= PWR_CR1_DBP;
	for (uint32_t i = 0; i < 1000u && (PWR->CR1 & PWR_CR1_DBP) == 0; i++) {
	}
}
#define STM32_BACKUP_UNLOCK()		stm32h7_backup_unlock()
#define STM32_BACKUP_LOCK()		(PWR->CR1 &= ~PWR_CR1_DBP)
#define STM32_BKPSRAM_ON() \
	do { \
		RCC->AHB4ENR |= RCC_AHB4ENR_BKPRAMEN; \
		(void)RCC->AHB4ENR; \
	} while (0)
#define STM32_BKPSRAM_BASE		D3_BKPSRAM_BASE

// sysboot.c reboot(): whether the board allows the software entry to the ROM bootloader, and the board's outputs to
// their reset state before a reset (both hooks: soc.h)
#define STM32_ROM_ENTRY_OK()		board_rom_entry_ok()
#define STM32_RESET_PREPARE()		board_reset_prepare()
// The last statement before the reset. This family's RAMs have ECC, and the last incomplete word written to one
// before a reset is not kept unless another incomplete word is written to the same RAM at another address (RM0433
// 2.4). The crash record and the soft mark are copied into the backup SRAM just before the reset and the copy may end
// in byte stores, so one byte goes to that RAM's last byte, which nothing else uses. The macro makes the RAM
// writable itself: in a build without the crash log nothing before it has, and what a write does with the RAM's clock
// off is not documented. NVIC_SystemReset() starts with the barrier that takes the store out of the core
#define STM32_KEPT_RAM_FLUSH() \
	do { \
		STM32_BKPSRAM_ON(); \
		STM32_BACKUP_UNLOCK(); \
		*(volatile uint8_t *)(D3_BKPSRAM_BASE + 0xFFFu) = 0; \
	} while (0)

// crash_stm32.c: the end of the internal flash (2 MB in two banks, STM32H743xI), and the RAM a stack or a task block
// can be in (DTCM 128 KB, AXI SRAM 512 KB)
#define STM32_FLASH_HI			0x08200000u
#define STM32_IS_RAM(a) \
	(((a) >= 0x20000000u && (a) < 0x20020000u) || ((a) >= 0x24000000u && (a) < 0x24080000u))
// The reset cause: RCC_RSR (RM0433 8.7.39)
#define STM32_RESET_FLAGS		RCC->RSR
#define STM32_RESET_FLAGS_CLEAR()	(RCC->RSR |= RCC_RSR_RMVF)
#define STM32_RSTF_WATCHDOG		(RCC_RSR_IWDG1RSTF | RCC_RSR_WWDG1RSTF)
#define STM32_RSTF_SOFTWARE		RCC_RSR_SFTRSTF
#define STM32_RSTF_POWER		RCC_RSR_PORRSTF
#define STM32_RSTF_BROWNOUT		RCC_RSR_BORRSTF
#define STM32_RSTF_PIN			RCC_RSR_PINRSTF

// A DMA stream's memory address must be in the AXI SRAM: DMA1 and DMA2 can't reach the DTCM, and the D2 domain's
// RAMs, which they could reach, have their clocks off (soc.c)
#define STM32_DMA_REACHABLE(p)		(((uintptr_t)(p) >> 24) == 0x24u)

// hrc/hrc_bus.c: a quarter of a microsecond, keeping the bit-banged clock near the 1-2 MHz the chip is known to take.
// On the cycle counter, not a counted loop: a pass of one takes 1 to 2 cycles on this dual-issue core
#define STM32_HRC_BUS_DELAY()		stm32_wait_ns(250u)

#endif
