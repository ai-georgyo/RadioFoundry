// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// STM32 system services (hal_system.h): chip identity and option bytes, memory reads, SoC registers, reboot, and the
// software entry to the MCU's ROM bootloader (docs/spec/hardware-dm1701.md "Software entry to a bootloader").
//
// ROM DFU entry: the request stores a flag in an RTC backup register (kept through a system reset, unlike RAM, which
// a bootloader below the image may use before the application starts) and resets. The next start reaches main() as
// usual (through that bootloader where the board keeps one), and stm32_sysboot_check(), first thing in main(), finds
// the flag, clears it (so a failed entry boots normally next time), puts the board in its reset-time pin state
// (board_sysboot_prepare: the power latch stays high) and enters the ROM the family's way (sysboot_soc.h,
// stm32_rom_enter: on the F4 clocks and peripherals back to their reset state, system memory mapped at 0, then the
// ROM's reset vector).
//
// What differs between the families is in stm32_family.h (the STM32_* names), in sysboot_soc.h (where the ROM is and
// the way into it) and in mem_window.h (what the debug link may read and write).
#include <string.h>
#include "config.h"
#include "platform.h"
#include "board.h"
#include "hal_system.h"
#include "mem_window.h"
#if CONFIG_CRASH_LOG
#include "hal_crash.h"
#endif

#define SYSBOOT_MAGIC		0xB007DF11u	// in RTC_BKP19R: enter the ROM bootloader on the next start
#define SPIN_LIMIT		1000000u

__attribute__((weak)) void board_sysboot_prepare(void)
{
}

static bool chip(hal_chip_t *c)
{
	c->idcode = DBGMCU->IDCODE;
	c->flash_kb = *(volatile const uint16_t *)FLASHSIZE_BASE;
	c->uid[0] = *(volatile const uint32_t *)(UID_BASE + 0u);
	c->uid[1] = *(volatile const uint32_t *)(UID_BASE + 4u);
	c->uid[2] = *(volatile const uint32_t *)(UID_BASE + 8u);
	c->optcr = STM32_OPTBYTES();
	c->family = STM32_CHIP_FAMILY;
	return true;
}

// Whole aligned words are read one 32-bit load at a time: that is what a register block needs (mem_window.h refuses
// anything else there), and memcpy may use byte loads
static void read(uint32_t addr, void *dst, size_t n)
{
	if (((addr | n) & 3u) != 0) {
		memcpy(dst, (const void *)(uintptr_t)addr, n);
		return;
	}
	uint8_t *d = dst;
	for (size_t i = 0; i < n; i += 4u) {
		uint32_t v = *(volatile const uint32_t *)(uintptr_t)(addr + i);
		memcpy(d + i, &v, 4u);
	}
}

static bool mem_window(unsigned i, hal_mem_window_t *w)
{
	return stm32_mem_window_for(i, w, *(volatile const uint16_t *)FLASHSIZE_BASE);
}

#if CONFIG_DEBUGLINK
#define BUS_ERRORS	(SCB_CFSR_PRECISERR_Msk | SCB_CFSR_IMPRECISERR_Msk | SCB_CFSR_BFARVALID_Msk)

// CHIP_REG's SoC registers (mem_window.h). The write and its read back run at priority -1 (FAULTMASK), so no handler
// changes the register between them, and with data bus faults ignored there (CCR.BFHFNMIGN): a reserved address inside
// the windows is refused instead of crashing the radio. The write goes around the write buffer where the core has
// the switch (STM32_REG_WRITE_PRECISE: ACTLR.DISDEFWBUF on the Cortex-M4), so its fault, if any, is precise and lands
// here rather than after FAULTMASK is lifted; an imprecise one is still caught by the flag test after the barrier
static bool reg(uint32_t addr, uint32_t *val, bool write)
{
	if (!stm32_reg_allowed(addr)) {
		return false;
	}
	uint32_t fm = __get_FAULTMASK();
	__disable_fault_irq();
	uint32_t before = SCB->CFSR & BUS_ERRORS;
	SCB->CCR |= SCB_CCR_BFHFNMIGN_Msk;
	__DSB();
	__ISB();
	if (write) {
		STM32_REG_WRITE_PRECISE(true);
		__DSB();
		__ISB();
		*(volatile uint32_t *)(uintptr_t)addr = *val;
		__DSB();
		STM32_REG_WRITE_PRECISE(false);
	}
	uint32_t v = *(volatile const uint32_t *)(uintptr_t)addr;
	__DSB();
	SCB->CCR &= ~SCB_CCR_BFHFNMIGN_Msk;
	uint32_t err = SCB->CFSR & BUS_ERRORS & ~before;
	if (err != 0) {
		SCB->CFSR = err;	// write 1 to clear: only the flags this access set
	}
	__DSB();
	__ISB();
	__set_FAULTMASK(fm);
	if (err != 0) {
		return false;
	}
	*val = v;
	return true;
}
#endif

static void backup_write(uint32_t v)
{
	STM32_BACKUP_CLOCK_ON();
	STM32_BACKUP_UNLOCK();
	RTC->BKP19R = v;
	(void)RTC->BKP19R;
}

// The modes reboot() takes. Also hal_system_t.reboot_ok, which the debug link asks before it answers: one rule, so
// what it is told and what reboot() does can't differ. The ROM entry only where the board allows it (on some the
// ROM's pins meet other outputs)
static bool reboot_ok(uint8_t mode)
{
	if (mode == HAL_REBOOT_ROM_DFU) {
		return STM32_ROM_ENTRY_OK();
	}
	return mode == HAL_REBOOT_NORMAL;
}

static bool reboot(uint8_t mode)
{
	if (!reboot_ok(mode)) {
		return false;
	}
	if (mode == HAL_REBOOT_ROM_DFU) {
		backup_write(SYSBOOT_MAGIC);
		if (RTC->BKP19R != SYSBOOT_MAGIC) {
			return false;
		}
	}
	__disable_irq();
	board_rf_safe();	// a reset leaves the pins floating until the next start drives them: transmitter off first
#if CONFIG_CRASH_LOG
	hal_crash_soft_mark();	// asked for: not the watchdog (crash-log.md 1.3)
#endif
	STM32_RESET_PREPARE();	// a board whose outputs outlive an MCU reset puts them in their reset state
	// Last before the reset: what was just written to the RAM a reset keeps (the soft mark; a crash record on the
	// way to its restart, hal_crash_restart) is in it for certain. Every asked-for reset and crash restart passes here
	STM32_KEPT_RAM_FLUSH();
	NVIC_SystemReset();
	return false;	// not reached
}

#if CONFIG_SUPPORT_INFO
// The linker's: the application's base (board.mk APP_BASE) and the end of its .bin (common.ld)
extern const uint8_t __app_base[], __image_end[];

static void image(uint32_t *addr, uint32_t *len)
{
	*addr = (uint32_t)(uintptr_t)__app_base;
	*len = (uint32_t)(__image_end - __app_base);
}
#endif

const hal_system_t stm32_system = {
	.chip = chip,
	.read = read,
	.mem_window = mem_window,
	.reboot = reboot,
	.reboot_ok = reboot_ok,
	.irq_stack_free = stm32_irq_stack_free,	// irq_stack.c
#if CONFIG_DEBUGLINK
	.reg = reg,
#endif
#if CONFIG_SUPPORT_INFO
	.image = image,
#endif
#if CONFIG_STUB_ENTRY
	.stub_region = stm32_stub_region,	// stub_entry.c
	.enter_stub = stm32_enter_stub,
	.stub_run_addr = 0x20000000u,		// the staging buffer is copied to the SRAM base at entry
	.stub_image_ok = stm32_stub_image_ok,	// with the IWDG running: only a stub that feeds it
#endif
};

// The family's half: stm32_rom_enter(), the way into the ROM bootloader, and what that needs (it may use SPIN_LIMIT)
#include "sysboot_soc.h"

void stm32_sysboot_check(void)
{
	STM32_BACKUP_CLOCK_ON();
	if (RTC->BKP19R != SYSBOOT_MAGIC) {
		return;
	}
	backup_write(0);
	STM32_BACKUP_LOCK();
	if (RTC->BKP19R != 0) {
		return;	// can't clear it: entering anyway would loop into the ROM on every start
	}

	board_rf_safe();
	board_sysboot_prepare();

	__disable_irq();
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;
	for (unsigned i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
		NVIC->ICER[i] = 0xFFFFFFFFu;
		NVIC->ICPR[i] = 0xFFFFFFFFu;
	}

	stm32_rom_enter();
	__builtin_unreachable();
}
