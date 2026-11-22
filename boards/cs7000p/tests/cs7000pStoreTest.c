// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS's data flash map (boards/cs7000p/store_cs7000p.c) under core/storage and core/settings as the
// board builds them (4-byte addresses, a region that must be claimed before its first write), on the chip model of
// tests/host/spiNorTest.c as the 32 MB part: the areas don't overlap and stay in the low half, the board sector is
// in the hole no codeplug address maps to, a chip is foreign whether it is blank or holds another firmware's bytes
// and then sees no program and no erase at all, and after the claim nothing above 16 MB and nothing in the board
// sector is ever written.
#define SPI_NOR_TEST_LIB
#pragma GCC diagnostic ignored "-Wunused-function"	// the generic tests spiNorTest.c runs itself
#include "spiNorTest.c"
#include "store_cs7000p.h"

#if SPI_NOR_ADDR4 != 1 || STORAGE_CLAIM_FIRST != 1
#error "this test builds the driver and core/storage as the board does: -DSPI_NOR_ADDR4=1 -DSTORAGE_CLAIM_FIRST=1"
#endif

#define FULL		CS7000P_STORE_FULL_SIZE
#define LOW_HALF	0x1000000u			// what 3-byte addresses reach, and where the whole map is
#define HOLE		0x01B000u			// from the journal's end to the open-firmware flash image
#define CODEPLUG	0x020FF0u			// somewhere a codeplug write goes

static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return alen != 0 && blen != 0 && a < b + blen && b < a + alen;
}

static void test_map(void)
{
	const hal_flash_map_t *m = &cs7000p_flash_map;
	const uint32_t openfw_eeprom_len = 0x10000u;	// the 64 KB "EEPROM" image
	struct { uint32_t addr, len; } area[] = {
		{ m->settings_addr, m->settings_size }, { m->journal_addr, 2 * STORAGE_SECTOR },
		{ m->openfw_eeprom, openfw_eeprom_len }, { m->openfw_flash, m->openfw_end - m->openfw_flash },
		{ m->native_addr, m->native_size }, { m->staging_addr, m->staging_size },
		{ CS7000P_STORE_CAL_ADDR, CS7000P_STORE_CAL_SIZE },
	};
	const unsigned n = sizeof(area) / sizeof(area[0]);

	for (unsigned i = 0; i < n; i++) {
		CHECK(area[i].len != 0);
		CHECK(area[i].addr % STORAGE_SECTOR == 0 && area[i].len % STORAGE_SECTOR == 0);
		CHECK(area[i].addr + area[i].len <= LOW_HALF);		// nothing of ours above 16 MB
		for (unsigned j = i + 1; j < n; j++) {
			if (overlaps(area[i].addr, area[i].len, area[j].addr, area[j].len)) {
				printf("FAIL map: areas %u and %u overlap\n", i, j);
				failures++;
			}
		}
	}
	CHECK(FULL == (32u << 20) && CS7000P_STORE_SECTOR_SIZE == STORAGE_SECTOR && SPI_NOR_SECTOR_SIZE == STORAGE_SECTOR);
	CHECK(hal_flash_map_end(m, STORAGE_SECTOR) <= LOW_HALF);
	CHECK(hal_flash_map_for(m, NULL, FULL, STORAGE_SECTOR) == m);
	// The settings ring and the journal are one run, and the board sector is in the hole behind them
	CHECK(m->settings_size >= 2 * STORAGE_SECTOR && m->settings_addr + m->settings_size == m->journal_addr);
	CHECK(m->journal_addr + 2 * STORAGE_SECTOR == HOLE);
	CHECK(CS7000P_STORE_CAL_ADDR >= HOLE && CS7000P_STORE_CAL_ADDR + CS7000P_STORE_CAL_SIZE <= m->openfw_flash);
	// The codeplug tools' addresses: the open-firmware images where they are on the 16 MB radios
	CHECK(m->openfw_eeprom == 0 && m->openfw_flash == 0x20000u && m->openfw_end == m->native_addr);
	// No CPS calibration window: no calibration format is known for this radio
	CHECK(m->calibration_size == 0);
}

static void create(void)
{
	chip_create(FULL, 0xEF, 0x40, 0x19);		// W25Q256JV
	flash.map = &cs7000p_flash_map;
}

// Bytes that are neither erased nor ours, as another firmware leaves them
static void junk(uint32_t addr, uint32_t len, uint8_t seed)
{
	for (uint32_t i = 0; i < len; i++) {
		chip.mem[addr + i] = (uint8_t)(seed + i * 13u + (i >> 7) + (i >> 15));
	}
}

// A start and every writer the core has, on a chip that is not claimed: the region is foreign and the chip sees no
// program and no erase, at this start and at the next
static void refused_everywhere(const uint8_t *snap)
{
	uint8_t a[STORAGE_SECTOR];
	settings_stats_t st;
	settings_t s;

	for (int start = 0; start < 2; start++) {
		CHECK(!(start == 0 ? boot() : warm_boot()));
		CHECK(flash.size == FULL && board_nor.addr_len == 4);
		CHECK(storage_region() == STORAGE_REGION_FOREIGN && !storage_ready() && !storage_claim_pending());
		settings_get_stats(&st);
		CHECK(st.load == SETTINGS_FOREIGN);
		settings_begin()->squelch = 5;
		settings_end(100);
		settings_poll(5000);
		CHECK(!settings_flush());
		settings_read(&s);
		CHECK(s.squelch == 5);		// the radio runs on from RAM
		fill(a, 5);
		CHECK(!storage_sector_write(CODEPLUG & ~(STORAGE_SECTOR - 1u), a));
		CHECK(!storage_write(CODEPLUG, a, 64));
		CHECK(!storage_write(CODEPLUG, NULL, STORAGE_SECTOR));		// an erase of a range
		CHECK(!storage_write(cs7000p_flash_map.settings_addr, a, 16));
		CHECK(!storage_write(cs7000p_flash_map.staging_addr, a, 16));
		CHECK(storage_native_erase() == STORAGE_NATIVE_FAILED);
	}
	CHECK(chip.programs == 0 && chip.erases == 0 && chip.high_ops == 0);
	// Not one command that programs or erases reached the chip, with either address length, and no write enable
	CHECK(chip.seen[0x12] == 0 && chip.seen[0x21] == 0 && chip.seen[0x02] == 0 && chip.seen[0x20] == 0);
	CHECK(chip.seen[0x06] == 0 && chip.ext == 0 && !chip.wel);
	CHECK(memcmp(snap, chip.mem, FULL) == 0);
	CHECK(chip.violations == 0 && chip.page_overruns == 0);
}

// The backup the owner takes before the claim reads the whole part, the top half too, and leaves the chip as found
static void backup_reads(const uint8_t *snap)
{
	uint8_t r[0x300];

	for (uint32_t addr = 0; addr < FULL; addr += FULL / 64u + 0x155u) {
		uint32_t len = FULL - addr < sizeof(r) ? FULL - addr : (uint32_t)sizeof(r);
		CHECK(f_read(addr, r, len) && memcmp(r, snap + addr, len) == 0);
	}
	CHECK(f_read(FULL - sizeof(r), r, sizeof(r)) && memcmp(r, snap + FULL - sizeof(r), sizeof(r)) == 0);
	CHECK(storage_debug_read_allowed(cs7000p_flash_map.settings_addr, cs7000p_flash_map.settings_size));
	CHECK(chip.programs == 0 && chip.erases == 0 && chip.ext == 0 && memcmp(snap, chip.mem, FULL) == 0);
}

// The claim, then the core at work: only the region is taken, and afterwards nothing above 16 MB and nothing in the
// board sector's hole is written
static void claim_and_use(uint8_t *snap)
{
	const hal_flash_map_t *m = &cs7000p_flash_map;
	const uint32_t region_end = m->journal_addr + 2 * STORAGE_SECTOR;
	uint8_t a[STORAGE_SECTOR];
	settings_t s;

	CHECK(storage_claim_request() && storage_claim_pending());
	CHECK(chip.programs == 0 && chip.erases == 0);		// the request alone touches nothing
	settings_begin()->squelch = 6;
	settings_end(200);
	settings_poll(6000);				// the housekeeping task: erase, mark, then the settings
	CHECK(storage_region() == STORAGE_REGION_OURS && storage_ready() && !storage_claim_pending());
	CHECK(storage_slot_check(&chip.mem[m->settings_addr]) == STORAGE_SLOT_VALID);	// the ownership mark
	// The claim took the ring and the journal, and nothing else on the chip
	CHECK(memcmp(snap, chip.mem, m->settings_addr) == 0);
	CHECK(memcmp(snap + region_end, chip.mem + region_end, FULL - region_end) == 0);
	CHECK(chip.high_ops == 0 && chip.ext == 0 && !chip.wel);
	// Writes work now: a codeplug sector, a write across a sector end, the native area, the staging area
	fill(a, 9);
	CHECK(storage_sector_write(0x021000u, a) && memcmp(&chip.mem[0x021000u], a, STORAGE_SECTOR) == 0);
	CHECK(storage_write(CODEPLUG, a, 64) && memcmp(&chip.mem[CODEPLUG], a, 64) == 0);
	CHECK(storage_sector_write(m->native_addr + m->native_size - STORAGE_SECTOR, a));
	CHECK(storage_sector_write(m->staging_addr, a));
	settings_begin()->squelch = 7;
	settings_end(7000);
	CHECK(settings_flush());
	// The next start, with the power gone and without: ours, with the settings
	for (int start = 0; start < 2; start++) {
		CHECK(start == 0 ? boot() : warm_boot());
		CHECK(storage_region() == STORAGE_REGION_OURS && storage_ready() && !storage_claim_request());
		settings_read(&s);
		CHECK(s.squelch == 7);
	}
	// Nothing above 16 MB was programmed or erased, and the top half is byte for byte what it was
	CHECK(chip.high_ops == 0 && memcmp(snap + LOW_HALF, chip.mem + LOW_HALF, FULL - LOW_HALF) == 0);
	// The hole, the board sector in it: written by nothing
	CHECK(memcmp(snap + HOLE, chip.mem + HOLE, m->openfw_flash - HOLE) == 0);
	// Only the commands that always take four address bytes; the address mode and the status registers untouched
	CHECK(chip.seen[0x02] == 0 && chip.seen[0x20] == 0 && chip.seen[0x03] == 0 && chip.seen[0x12] != 0 &&
	      chip.seen[0x21] != 0);
	CHECK(chip.seen[0xB7] == 0 && chip.seen[0xE9] == 0 && chip.seen[0x01] == 0 && chip.seen[0x31] == 0 &&
	      chip.seen[0x11] == 0);
	CHECK(chip.ext == 0 && !chip.wel && !chip.ads && chip.violations == 0 && chip.page_overruns == 0);
	CHECK(chip.reset_harm == 0 && chip.early == 0);
}

// A blank chip: the default build would mark it at the first start and write from then on. Here it is foreign
static void test_blank_chip(void)
{
	uint8_t *snap = malloc(FULL);
	storage_stats_t ss;

	create();
	memcpy(snap, chip.mem, FULL);
	refused_everywhere(snap);
	storage_get_stats(&ss);
	CHECK(ss.foreign_slots == 0);		// nothing of anyone's is there: foreign all the same
	backup_reads(snap);
	claim_and_use(snap);
	free(snap);
}

// A chip another firmware has used, with its bytes in the ring too, left in 4-byte mode at an address in the top half
static void test_used_chip(void)
{
	uint8_t *snap = malloc(FULL);

	create();
	junk(0, FULL, 3);
	memcpy(snap, chip.mem, FULL);
	chip.ads = true;
	chip.ext = 0x01;
	CHECK(!warm_boot() && chip.resets == 1 && chip.ext == 0 && !chip.ads);	// the probe's reset: 3-byte mode, register 0
	refused_everywhere(snap);
	backup_reads(snap);
	claim_and_use(snap);
	free(snap);
}

// The case the claim-first build exists for: the ring and journal happen to be blank and the other firmware's data is
// everywhere else, the board sector's hole included
static void test_blank_region_in_a_used_chip(void)
{
	const hal_flash_map_t *m = &cs7000p_flash_map;
	uint8_t *snap = malloc(FULL);

	create();
	junk(0, m->settings_addr, 5);
	junk(HOLE, FULL - HOLE, 11);
	memcpy(snap, chip.mem, FULL);
	refused_everywhere(snap);
	claim_and_use(snap);
	free(snap);
}

// A chip the other firmware left with a suspended operation is not used at all until a power cycle: no size, no
// map in use, nothing written
static void test_suspended_chip(void)
{
	uint8_t *snap = malloc(FULL);

	create();
	junk(0, FULL, 7);
	memcpy(snap, chip.mem, FULL);
	chip.sus = true;
	CHECK(!warm_boot() && flash.size == 0 && chip.resets == 0);
	CHECK(!storage_ready() && !storage_claim_request());
	CHECK(chip.programs == 0 && chip.erases == 0 && memcmp(snap, chip.mem, FULL) == 0);
	chip.sus = false;		// the power cycle
	refused_everywhere(snap);
	free(snap);
}

int main(void)
{
	test_probe();
	test_map();
	test_blank_chip();
	test_used_chip();
	test_blank_region_in_a_used_chip();
	test_suspended_chip();
	test_commands_sent();
	free(chip.mem);
	flash.map = &map16;
	printf("# cs7000pStoreTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
