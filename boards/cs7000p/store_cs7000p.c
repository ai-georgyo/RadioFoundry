// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS data flash layout (store_cs7000p.h, docs/spec/hardware-cs7000p.md). Host-clean, and the addresses
// are number literals on purpose: tools/boards.py reads this initializer as text for the flashing tools and the web
// site, and tests/cs7000pStoreTest.c runs the storage layer on it.
#include "store_cs7000p.h"

// The layout the STM32 radios with a 16 MB data flash use, in the low half of this 32 MB part, so every codeplug
// tool's addresses hold: the open-firmware codeplug where the CPS tools expect it, settings and journal at
// 0x011000-0x01AFFF, the native codeplug and the staging area above the 14 MB mark. 0x01B000-0x01FFFF is the hole
// no CPS address maps to; the board sector is in it (CS7000P_STORE_CAL_ADDR, 0x01B000), and the maker's records at
// its top. Nothing is mapped above 16 MB.
// No calibration window: that window is a copy an open-firmware CPS may write (docs/spec/cps-protocol.md 2.3), so the
// maker's factory table at 0x001000 (docs/spec/calibration-cs7000p.md) is not one. It is a factory range instead,
// kept through every rewrite of its sector (docs/spec/settings-store.md 4.1), and the same ranges are the board's
// [[flash.protect]] data entries (board.toml; tools/boards.py check compares them)
const hal_flash_map_t cs7000p_flash_map = {
	.settings_addr = 0x011000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x019000u,	// 2 sectors
	.openfw_eeprom = 0x000000u,
	.openfw_flash = 0x020000u,
	.openfw_end = 0xE00000u,
	.calibration_addr = 0,
	.calibration_size = 0,
	.native_addr = 0xE00000u,
	.native_size = 0x100000u,
	.staging_addr = 0xF00000u,	// 256 KB of the megabyte above the native codeplug (docs/spec/tftp.md 4)
	.staging_size = 0x040000u,
	.factory = {
		// The maker's factory table (calibration-cs7000p.md 2.1): 512 bytes of sector 1, beside the open-firmware
		// DTMF settings (0x1400) and APRS configurations (0x1588), which stay writable
		{ 0x001000u, 0x200u },
		// The maker's records at 0x01E000, 0x01E400 and 0x01F000 (calibration-cs7000p.md 2.4): two whole sectors
		{ 0x01E000u, 0x2000u },
	},
};
