// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS data flash: the map the core gets and the board's own reserved sector
// (docs/spec/hardware-cs7000p.md).
//
// The chip is a 32 MB part that the maker's firmware has used. Its factory calibration table (0x001000-0x0011FF) and
// some of its records are located (docs/spec/calibration-cs7000p.md 2); its codeplug, fonts and voice data are not.
// The map keeps the located ones (hal_flash_map_t.factory, board.mk STORAGE_FACTORY_KEEP): every rewrite of sector 1
// carries the table's 512 bytes over from the flash and takes the rest (the open-firmware DTMF settings and APRS
// configurations) from the writer, and the two sectors of the maker's records, 0x01E000-0x01FFFF, are never erased or
// programmed (docs/spec/settings-store.md 4.1). The maker's device information in sector 0 is not kept: it shares
// that sector with the open-firmware callsign and DMR ID (hardware-cs7000p.md 10.3). The board builds with
// STORAGE_CLAIM_FIRST (board.mk) too: until the owner claims the region after a full backup, nothing in the map is
// erased or programmed, blank or not. The map itself stays in the low 16 MB; the upper half is read for the backup
// only.
#ifndef STORE_CS7000P_H
#define STORE_CS7000P_H

#include <stdint.h>
#include "hal_flash.h"

#define CS7000P_STORE_FULL_SIZE		0x2000000u	// the fitted part, all of which a backup reads
#define CS7000P_STORE_SECTOR_SIZE	0x1000u
// The maker's factory table (calibration-cs7000p.md 2.1), the first factory range of the map
#define CS7000P_STORE_TABLE_ADDR	0x001000u
#define CS7000P_STORE_TABLE_SIZE	0x200u
// The maker's records (calibration-cs7000p.md 2.4): two whole sectors the map keeps, never written
#define CS7000P_STORE_MAKER_ADDR	0x01E000u
#define CS7000P_STORE_MAKER_SIZE	0x2000u
// The board sector: in the hole between the core's journal (0x019000-0x01AFFF) and the maker's records (0x01E000),
// which no CPS address maps to, blank on the unit seen, and no constant-address flash call of the maker's program
// falls in it (calibration-cs7000p.md 2.4). It is to hold the tree's own verified copy of the maker's table (a record
// with the magic RFFC, hardware-cs7000p.md 10.2); nothing writes it yet, and only the view reads it. The map does not
// keep it: it is the tree's own, and no CPS address or editor reaches it.
// A later record of values the tree measures itself (RFC1, hardware-cs7000p.md 13.3) gets the next sector, 0x01C000
#define CS7000P_STORE_CAL_ADDR		0x01B000u
#define CS7000P_STORE_CAL_SIZE		CS7000P_STORE_SECTOR_SIZE

extern const hal_flash_map_t cs7000p_flash_map;

#endif
