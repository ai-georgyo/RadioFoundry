// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32H7's debug link windows (platform/stm32h7/mem_window.h): what READ_MEM may read, through the debug link's
// own gate (core/debuglink/dl_chip.c), and which SoC registers CHIP_REG may touch. RAM and peripherals are never
// readable as memory.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "debuglink/dl_chip.h"
#include "../../platform/stm32h7/mem_window.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static uint16_t flash_kb;	// the flash size word the windows are asked with

static bool window(unsigned i, hal_mem_window_t *w)
{
	return stm32_mem_window_for(i, w, flash_kb);
}

static bool readable(uint32_t addr, uint32_t len)
{
	return dl_mem_allowed(addr, len, window);
}

static void test_windows(void)
{
	hal_mem_window_t w;

	// Four windows: the flash by its size word, system memory, the ID words, the system control block
	flash_kb = 2048;
	CHECK(window(0, &w) && w.lo == 0x08000000u && w.hi == 0x081FFFFFu && !w.words);
	CHECK(window(1, &w) && w.lo == 0x1FF00000u && w.hi == 0x1FF1E7FFu && !w.words);
	CHECK(window(2, &w) && w.lo == 0x1FF1E800u && w.hi == 0x1FF1E8FFu && !w.words);
	CHECK(window(3, &w) && w.lo == 0xE000ED00u && w.hi == 0xE000ED3Fu && w.words);
	CHECK(!window(4, &w));
	CHECK(!window(5, &w));
	CHECK(!window(0xFFFFFFFFu, &w));

	// The flash window follows the size word; a word that can't be a size counts as the largest part, 2 MB
	flash_kb = 1024;
	CHECK(window(0, &w) && w.lo == 0x08000000u && w.hi == 0x080FFFFFu);
	flash_kb = 16;
	CHECK(window(0, &w) && w.hi == 0x08003FFFu);
	flash_kb = 15;
	CHECK(window(0, &w) && w.hi == 0x081FFFFFu);
	flash_kb = 0;
	CHECK(window(0, &w) && w.hi == 0x081FFFFFu);
	flash_kb = 2049;
	CHECK(window(0, &w) && w.hi == 0x081FFFFFu);
	flash_kb = 0xFFFF;
	CHECK(window(0, &w) && w.hi == 0x081FFFFFu);
	// The other windows don't move with it
	CHECK(window(1, &w) && w.lo == 0x1FF00000u);
}

static void test_read_mem(void)
{
	flash_kb = 2048;
	// Flash: any byte range inside both banks, nothing across its ends
	CHECK(readable(0x08000000u, 1));
	CHECK(readable(0x08000001u, 3));
	CHECK(readable(0x080FFFFFu, 2));	// across the banks
	CHECK(readable(0x081FFFFFu, 1));
	CHECK(readable(0x081FFF00u, 0x100));
	CHECK(!readable(0x081FFF00u, 0x101));
	CHECK(!readable(0x08200000u, 1));
	CHECK(!readable(0x07FFFFFFu, 2));
	CHECK(!readable(0x08000000u, 0));
	flash_kb = 1024;
	CHECK(readable(0x080FFFFFu, 1));
	CHECK(!readable(0x080FFFFFu, 2));
	CHECK(!readable(0x08100000u, 1));
	flash_kb = 2048;

	// System memory: the ROM bootloader with its vectors (0x1FF09800) and its version byte (0x1FF1E7FE)
	CHECK(readable(0x1FF00000u, 4));
	CHECK(readable(0x1FF09800u, 8));
	CHECK(readable(0x1FF1E7FEu, 1));
	CHECK(readable(0x1FF1E7FFu, 1));
	CHECK(!readable(0x1FEFFFFFu, 1));
	// The ID words: unique ID, flash size, the calibration values; one request stays inside one window
	CHECK(readable(0x1FF1E800u, 12));
	CHECK(readable(0x1FF1E880u, 2));
	CHECK(readable(0x1FF1E8FFu, 1));
	CHECK(!readable(0x1FF1E7FFu, 2));
	CHECK(!readable(0x1FF1E8FFu, 2));
	CHECK(!readable(0x1FF1E900u, 1));
	CHECK(!readable(0x1FF20000u, 4));

	// The system control block: whole aligned words only
	CHECK(readable(0xE000ED00u, 4));	// CPUID
	CHECK(readable(0xE000ED28u, 4));	// CFSR
	CHECK(readable(0xE000ED00u, 0x40));
	CHECK(readable(0xE000ED3Cu, 4));
	CHECK(!readable(0xE000ED00u, 1));
	CHECK(!readable(0xE000ED01u, 4));
	CHECK(!readable(0xE000ED00u, 6));
	CHECK(!readable(0xE000ED3Cu, 8));
	CHECK(!readable(0xE000ED40u, 4));
	CHECK(!readable(0xE000ECFCu, 4));
	CHECK(!readable(0xE000E010u, 4));	// SysTick
	CHECK(!readable(0xE000E100u, 4));	// NVIC

	// Never RAM: ITCM, DTCM, AXI SRAM, the D2 and D3 RAMs, the backup RAM
	CHECK(!readable(0x00000000u, 4));
	CHECK(!readable(0x20000000u, 4));
	CHECK(!readable(0x2001FFFCu, 4));
	CHECK(!readable(0x24000000u, 4));
	CHECK(!readable(0x2407FFFCu, 4));
	CHECK(!readable(0x30000000u, 4));
	CHECK(!readable(0x38000000u, 4));
	CHECK(!readable(0x38800000u, 4));
	// Never a peripheral: a read there can clear a flag or pop a FIFO
	CHECK(!readable(0x40000000u, 4));
	CHECK(!readable(0x40013000u, 4));	// SPI1
	CHECK(!readable(0x40080000u, 4));	// the USB core
	CHECK(!readable(0x52002000u, 4));	// the flash interface
	CHECK(!readable(0x58024400u, 4));	// RCC
	CHECK(!readable(0x58024800u, 4));	// PWR
	CHECK(!readable(0x5C001000u, 4));	// DBGMCU
	CHECK(!readable(0xFFFFFFFCu, 4));
	CHECK(!readable(0xFFFFFFFFu, 2));	// no wrap
}

static void test_chip_reg(void)
{
	// The peripheral buses, first word to last
	CHECK(stm32_reg_allowed(0x40000000u));
	CHECK(stm32_reg_allowed(0x40013000u));
	CHECK(stm32_reg_allowed(0x52002000u));	// the flash interface is inside them on this family
	CHECK(stm32_reg_allowed(0x52004000u));	// and the memory controller
	CHECK(stm32_reg_allowed(0x58024400u));
	CHECK(stm32_reg_allowed(0x5C001000u));
	CHECK(stm32_reg_allowed(0x5FFFFFFCu));
	CHECK(!stm32_reg_allowed(0x3FFFFFFCu));
	CHECK(!stm32_reg_allowed(0x60000000u));
	// The core's private peripheral bus
	CHECK(stm32_reg_allowed(0xE0000000u));
	CHECK(stm32_reg_allowed(0xE000ED00u));
	CHECK(stm32_reg_allowed(0xE0001004u));	// the cycle counter
	CHECK(stm32_reg_allowed(0xE00FFFFCu));
	CHECK(!stm32_reg_allowed(0xDFFFFFFCu));
	CHECK(!stm32_reg_allowed(0xE0100000u));
	// Aligned words only
	CHECK(!stm32_reg_allowed(0x40000001u));
	CHECK(!stm32_reg_allowed(0x40000002u));
	CHECK(!stm32_reg_allowed(0xE000ED03u));
	// Never memory: flash, system memory, any RAM
	CHECK(!stm32_reg_allowed(0x00000000u));
	CHECK(!stm32_reg_allowed(0x08000000u));
	CHECK(!stm32_reg_allowed(0x1FF09800u));
	CHECK(!stm32_reg_allowed(0x20000000u));
	CHECK(!stm32_reg_allowed(0x24000000u));
	CHECK(!stm32_reg_allowed(0x30000000u));
	CHECK(!stm32_reg_allowed(0x38000000u));
	CHECK(!stm32_reg_allowed(0x38800000u));
	CHECK(!stm32_reg_allowed(0xFFFFFFFCu));
}

int main(void)
{
	test_windows();
	test_read_mem();
	test_chip_reg();
	printf("stm32h7WindowTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
