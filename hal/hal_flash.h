// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Data flash (external SPI NOR on most radios)
#ifndef HAL_FLASH_H
#define HAL_FLASH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// A board whose maker keeps data of its own inside the flash map (a factory calibration, records the maker's tools
// read) builds with -DSTORAGE_FACTORY_KEEP=1 (MCU_DEFS in its board.mk) and lists those bytes in
// hal_flash_map_t.factory (docs/spec/settings-store.md 4.1). Every other board builds without it: the helpers below
// are then constant false, and nothing of the rule is compiled into core/storage or a flash driver. A define and not
// only the list, so that the boards that keep nothing pay nothing for it
#ifndef STORAGE_FACTORY_KEEP
#define STORAGE_FACTORY_KEEP	0
#endif

// A byte range of the data flash
typedef struct {
	uint32_t addr;
	uint32_t size;
} hal_flash_range_t;

#define HAL_FLASH_FACTORY_MAX	4u

// Where the core keeps its data in the board's data flash. Every area is whole erase units. A size of 0 means the
// board has no such area (the feature then runs from RAM only, or is refused)
typedef struct {
	// The settings store (core/settings): a ring of at least 2 erase units, written record by record
	uint32_t settings_addr;
	uint32_t settings_size;
	// The sector journal (core/storage): 2 erase units, a copy of the sector being rewritten and its header, so a
	// power cut during a codeplug sector rewrite can be finished at the next boot
	uint32_t journal_addr;
	// The open-firmware codeplug (docs/spec/open-firmware-cps.md 7): the 64 KB "EEPROM" image and the 1 MB "flash"
	// image as physical addresses (the STM32 radios: 0 and 0x20000), and the end of the area an open-firmware CPS may
	// write
	uint32_t openfw_eeprom;
	uint32_t openfw_flash;
	uint32_t openfw_end;
	// The calibration window the open-firmware CPS reads and writes (open-firmware-cps.md 2.2), 0 size if none
	uint32_t calibration_addr;
	uint32_t calibration_size;
	// Reserved for the native codeplug (docs/spec/codeplug-native.md); never written by the open-firmware CPS
	uint32_t native_addr;
	uint32_t native_size;
	// Scratch for a whole new codeplug image before it replaces the stored one (the TFTP codeplug write,
	// docs/spec/tftp.md 4): outside every area above, never written by the open-firmware CPS. 0 size: no staged writes
	uint32_t staging_addr;
	uint32_t staging_size;
	// Satellite TLE data (docs/spec/satellite.md): separate from update staging so TFTP and CPS-fast never share it
	uint32_t satellite_addr;
	uint32_t satellite_size;
	// The maker's data on the flash (its factory calibration, its records): bytes no writer of ours changes, in any
	// storage state, and the debug link may read (docs/spec/settings-store.md 4.1). Byte ranges, ascending and apart,
	// whose erase units are clear of every area above but the open-firmware images; size 0 ends the list. Read only
	// with STORAGE_FACTORY_KEEP. The last field, so that no other field moves
	hal_flash_range_t factory[HAL_FLASH_FACTORY_MAX];
} hal_flash_map_t;

// Whether any byte of [addr, addr + len) lies in an erase unit (unit bytes, aligned) that holds a byte of a factory
// range: an erase there takes the maker's bytes with it, a program there may change them. False for a NULL map or len
// 0, and always false without STORAGE_FACTORY_KEEP. Every entry of size other than 0 counts, one past a size 0 entry
// too: storage_init() refuses such a list, and a driver's backstop must not depend on that
static inline bool hal_flash_factory_unit(const hal_flash_map_t *m, uint32_t unit, uint32_t addr, uint32_t len)
{
#if STORAGE_FACTORY_KEEP
	if (m == NULL || len == 0u) {
		return false;
	}
	// 32-bit divisions (no 64-bit division helper in a driver's path), 64-bit sums: a range may end at the top of the
	// address space
	const uint32_t u = unit != 0u ? unit : 1u;
	const uint64_t last = (uint64_t)addr + len - 1u;
	for (unsigned i = 0; i < HAL_FLASH_FACTORY_MAX; i++) {
		// The range widened to the erase units it touches, first and last byte
		const hal_flash_range_t *r = &m->factory[i];
		if (r->size == 0u) {
			continue;
		}
		const uint64_t r_last64 = (uint64_t)r->addr + r->size - 1u;
		const uint32_t r_last = r_last64 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)r_last64;
		const uint32_t first = r->addr - r->addr % u;
		const uint64_t end = (uint64_t)(r_last - r_last % u) + u - 1u;
		if (addr <= end && first <= last) {
			return true;
		}
	}
	return false;
#else
	(void)m;
	(void)unit;
	(void)addr;
	(void)len;
	return false;
#endif
}

// Whether any byte of [addr, addr + len) is a byte of a factory range itself. The same rules as above
static inline bool hal_flash_factory_bytes(const hal_flash_map_t *m, uint32_t addr, uint32_t len)
{
	return hal_flash_factory_unit(m, 1u, addr, len);
}

// The end of the highest area of a map whose flash erases in units of erase_size (an area of size 0 is none): a chip
// smaller than this can't hold the map, and a driver then offers none (an address past the chip fails, it never
// wraps to a lower one)
static inline uint32_t hal_flash_map_end(const hal_flash_map_t *m, uint32_t erase_size)
{
	if (m == NULL) {
		return 0;
	}
	const uint32_t ends[] = {
		m->settings_size != 0u ? m->settings_addr + m->settings_size : 0u,
		m->journal_addr + 2u * erase_size,
		m->openfw_eeprom + 0x10000u,
		m->openfw_end,
		m->calibration_size != 0u ? m->calibration_addr + m->calibration_size : 0u,
		m->native_size != 0u ? m->native_addr + m->native_size : 0u,
		m->staging_size != 0u ? m->staging_addr + m->staging_size : 0u,
		m->satellite_size != 0u ? m->satellite_addr + m->satellite_size : 0u,
	};
	uint32_t end = 0;
	for (unsigned i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
		end = ends[i] > end ? ends[i] : end;
	}
	return end;
}

// The map a chip of size bytes gets: the board's map when the chip holds it, else its map for a smaller part when that
// fits, else none (nothing is stored). Either may be NULL
static inline const hal_flash_map_t *hal_flash_map_for(const hal_flash_map_t *map, const hal_flash_map_t *small,
						       uint32_t size, uint32_t erase_size)
{
	if (map != NULL && hal_flash_map_end(map, erase_size) <= size) {
		return map;
	}
	if (small != NULL && hal_flash_map_end(small, erase_size) <= size) {
		return small;
	}
	return NULL;
}

typedef struct {
	bool (*init)(void);
	uint32_t size;		// bytes, filled in by init on boards that probe it
	uint32_t erase_size;	// smallest erase unit
	bool (*read)(uint32_t addr, void *buf, uint32_t len);
	// Program already erased bytes; any length and alignment
	bool (*write)(uint32_t addr, const void *buf, uint32_t len);
	bool (*erase)(uint32_t addr);	// one erase_size unit
	// Optional: replace one erase unit with new content of any bits in a single power-cut safe step (the old unit or
	// the new one survives, never a mix). Flash that is copy on write underneath (NAND behind a sector layer) offers
	// it; core/storage then rewrites a codeplug sector with it directly, with no journal and no erase first. NULL: the
	// journaled erase and program
	bool (*replace)(uint32_t addr, const void *data);
	const hal_flash_map_t *map;	// NULL: no areas for the core (nothing is stored)
	// Optional (NULL: not known): the chip's ID as init read it, manufacturer << 16 | device (SPI NOR: memory type << 8
	// | capacity code; SPI NAND: the device byte), 0 when nothing answered. No bus transfer: the support info reads it
	uint32_t (*jedec)(void);
	// Optional, a driver with a backstop under STORAGE_FACTORY_KEEP: the driver refuses every erase and program in an
	// erase unit holding factory bytes (hal_flash_factory_unit), nothing sent to the chip, except in the one unit
	// core/storage opened here (its address) for a rewrite that carries those bytes over; UINT32_MAX, or an address
	// that doesn't start a unit, closes it. NULL: no backstop
	void (*factory_grant)(uint32_t unit);
} hal_flash_t;

#endif
