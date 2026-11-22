/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* Connect Systems CS7000 M17 PLUS, STM32H743VI in LQFP100 (2 MB of flash in two banks,
 * docs/spec/hardware-cs7000p.md). The image starts at the flash base (__app_start, board.mk APP_BASE): it is
 * installed through the SoC's ROM bootloader and no loader is kept below it. The vector table, stack rule and
 * start-up are the shared ones (platform/stm32/common.ld).
 *
 * FLASH: bank 1 only, 1 MB. Bank 2 (0x08100000 up) stays unused, so no tool ever asks the ROM bootloader to erase
 * it: version 9.1 of that loader leaves a bank 2 erase early, and a flash command after it can hang (AN2606 Table
 * 146). Which version a radio has is not known. The flashing tools read the application region from this script
 * (tools/boards.py). No codec is placed by the linker.
 *
 * RAM: the AXI SRAM, where the DMA controllers can reach a buffer (.data, .bss, the main stack at its top). It ends
 * 32 bytes short: the ROM bootloader's jump to an image needs the image's stack pointer more than 16 bytes below
 * the end of the RAM (AN2606 Table 146, versions 9.0 to 9.2).
 *
 * CCMRAM: the DTCM, this SoC's CPU-only RAM (no DMA reaches it; the region keeps the shared script's name):
 * RFOUNDRY_CPU_RAM state and the RTOS heap with every task stack.
 *
 * Not used: SRAM1-3 (their clocks stay off), SRAM4, the ITCM. The 4 KB backup SRAM at 0x38800000 holds the crash
 * record and is addressed directly (platform/stm32/crash_stm32.c), not through a region. */

MEMORY
{
	FLASH   (rx)  : ORIGIN = __app_start, LENGTH = 0x08100000 - __app_start	/* bank 1 */
	RAM     (xrw) : ORIGIN = 0x24000000, LENGTH = 512K - 32	/* AXI SRAM; SP more than 16 bytes below its end */
	CCMRAM  (rw)  : ORIGIN = 0x20000000, LENGTH = 128K	/* DTCM */
}

INCLUDE common.ld
