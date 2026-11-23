// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS data flash: the map the core gets and the board's own reserved sector
// (docs/spec/hardware-cs7000p.md).
//
// The chip is a 32 MB part that the maker's firmware has used. Its factory calibration table (0x001000-0x0011FF) and
// some of its records are located (docs/spec/calibration-cs7000p.md 2), its codeplug, fonts and voice data are not,
// and nothing in the map protects any of it yet. So the board builds with STORAGE_CLAIM_FIRST (board.mk): until the
// owner claims the region after a full backup, nothing in the map is erased or programmed, blank or not. The map
// itself stays in the low 16 MB; the upper half is read for the backup only.
#ifndef STORE_CS7000P_H
#define STORE_CS7000P_H

#include <stdint.h>
#include "hal_flash.h"

#define CS7000P_STORE_FULL_SIZE		0x2000000u	// the fitted part, all of which a backup reads
#define CS7000P_STORE_SECTOR_SIZE	0x1000u
// The board sector: in the hole between the core's journal (0x019000-0x01AFFF) and the open-firmware flash image
// (0x020000), which no CPS address maps to. Reserved for a later per-unit calibration record of the tree's own
// format; only the view reads it and nothing writes it yet, and a writer may touch it only when it is blank or
// already carries that format's magic. Not free: the maker's records are at 0x01E000 and 0x01E400 and its program
// erases the sector (docs/spec/calibration-cs7000p.md 2.4), so the record moves to a blank sector before anything
// writes it
#define CS7000P_STORE_CAL_ADDR		0x01E000u
#define CS7000P_STORE_CAL_SIZE		CS7000P_STORE_SECTOR_SIZE

extern const hal_flash_map_t cs7000p_flash_map;

#endif
