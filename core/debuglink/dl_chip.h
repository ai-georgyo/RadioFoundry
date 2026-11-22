// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's chip commands, the portable parts (host tested): option byte decoding by chip family, the READ_MEM
// address window check, the REBOOT safety gate (docs/spec/debuglink.md 3.9-3.11). The windows themselves are the
// platform's (hal_system_t.mem_window).
#ifndef DL_CHIP_H
#define DL_CHIP_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_system.h"

#define DL_MEM_READ_MAX		1000u	// READ_MEM bytes per reply

// A family whose option word (hal_chip_t.optcr) has the RDP byte, in bits 15:8 with the same three levels on each
// (STM32F4 FLASH_OPTCR, STM32H7 FLASH_OPTSR_CUR). Such a chip has a ROM bootloader that RDP level 0 leaves usable; any
// other family has neither, and its optcr is 0
static inline bool dl_chip_has_rdp(uint8_t family)
{
	return family == HAL_CHIP_STM32F4 || family == HAL_CHIP_STM32H7;
}

// The RDP byte, for a family that has it (dl_chip_has_rdp)
#define DL_RDP_LEVEL0_BYTE	0xAAu
#define DL_RDP_LEVEL2_BYTE	0xCCu
static inline uint8_t dl_rdp_byte(uint32_t optcr)
{
	return (uint8_t)(optcr >> 8);
}
// 0, 1 or 2
static inline uint8_t dl_rdp_level(uint32_t optcr)
{
	uint8_t b = dl_rdp_byte(optcr);
	return b == DL_RDP_LEVEL0_BYTE ? 0 : (b == DL_RDP_LEVEL2_BYTE ? 2 : 1);
}
// The rest are the STM32F4's fields only (STM32F405/407 FLASH_OPTCR: nWRP 27:16 one bit per sector 0..11, 0 =
// protected, BOR_LEV 3:2, WDG_SW 5, nRST_STOP 6, nRST_STDBY 7). Another family keeps its write protection in other
// registers and lays its user bits out differently: INFO_CHIP sends the word itself and decodes nothing more
// Write-protected sectors, bit n = sector n protected
static inline uint16_t dl_wrp_mask(uint32_t optcr)
{
	return (uint16_t)(~(optcr >> 16) & 0x0FFFu);
}
static inline uint8_t dl_bor_level(uint32_t optcr)
{
	return (uint8_t)((optcr >> 2) & 3u);
}
static inline uint8_t dl_user_bits(uint32_t optcr)
{
	return (uint8_t)((optcr >> 5) & 7u);	// bit 0 WDG_SW, 1 nRST_STOP, 2 nRST_STDBY
}

// INFO_CHIP's decoded option bytes, the six at reply offsets 23..28 (docs/spec/debuglink.md 3.9): the RDP level and
// byte for a family with RDP, then the write-protect mask (u16, low byte first), the BOR level and the user bits for
// the STM32F4. 0xFF for each one the family has no decode for.
// Kept to defaults, one switch and six stores: the command dispatcher this is inlined into sits at the compiler's size
// limit for inlining on the largest images, and a longer form here puts another command out of line (about 240 bytes)
#define DL_CHIP_OPT_LEN		6u
static inline void dl_chip_optbytes(const hal_chip_t *chip, uint8_t out[DL_CHIP_OPT_LEN])
{
	uint32_t opt = chip->optcr;
	uint8_t level = 0xFF, rdp = 0xFF, bor = 0xFF, user = 0xFF;
	uint16_t wrp = 0xFFFF;

	switch (chip->family) {
	case HAL_CHIP_STM32F4:
		wrp = dl_wrp_mask(opt);
		bor = dl_bor_level(opt);
		user = dl_user_bits(opt);
		// fall through
	case HAL_CHIP_STM32H7:
		level = dl_rdp_level(opt);
		rdp = dl_rdp_byte(opt);
		break;
	default:
		break;
	}
	out[0] = level;
	out[1] = rdp;
	out[2] = (uint8_t)wrp;
	out[3] = (uint8_t)(wrp >> 8);
	out[4] = bor;
	out[5] = user;
}

// READ_MEM: the whole range [addr, addr + len) inside one of the platform's windows (window(i, &w) for i = 0, 1, ...
// until it returns false); in a words window (peripheral registers) only whole aligned words. len 0 or no windows:
// refused
bool dl_mem_allowed(uint32_t addr, uint32_t len, bool (*window)(unsigned i, hal_mem_window_t *w));

// REBOOT refusal reasons (the reply's second byte when the status is DL_ST_REFUSED)
enum {
	DL_REBOOT_OK		= 0,
	DL_REBOOT_BAD_TOKEN	= 1,	// the token isn't the chip UID's low word
	DL_REBOOT_RDP		= 2,	// ROM DFU asked with RDP not level 0, or on a chip family without it
	DL_REBOOT_TX		= 3,	// transmitting (radio or hotspot)
	DL_REBOOT_NO_CHIP	= 4,	// the chip info could not be read
	DL_REBOOT_ARM_FAILED	= 5,	// the board or platform refuses the mode (hal_system_t.reboot_ok), or the ROM DFU
					// flag could not be stored
};

// The gate: mode is HAL_REBOOT_*, chip NULL when unreadable. Returns DL_REBOOT_*
uint8_t dl_reboot_check(uint8_t mode, uint32_t token, const hal_chip_t *chip, bool transmitting);

#endif
