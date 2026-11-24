// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS's data flash map (boards/cs7000p/store_cs7000p.c) under core/storage and core/settings as the
// board builds them (4-byte addresses, a region that must be claimed before its first write, the maker's bytes kept),
// on the chip model of tests/host/spiNorTest.c as the 32 MB part: the areas don't overlap and stay in the low half, the
// board sector is in the hole no codeplug address maps to and clear of the maker's ranges, a chip is foreign whether
// it is blank or holds another firmware's bytes and then sees no program and no erase at all, and after the claim
// nothing above 16 MB and nothing in the board sector is ever written. Then the maker's table and records
// (hal_flash_map_t.factory, docs/spec/settings-store.md 4.1) through the core's writers of sector 1, on an INVENTED
// table: a DTMF and an APRS edit, a CPS write of EEPROM part 1 from an image with other bytes there, "start a new
// codeplug", every one done with the rest of the sector written and the maker's bytes as they were, and no erase or
// program of a sector holding them outside the core's grant; the same with the window blank and zero; a write that
// differs only inside the table (nothing erased); a power cut at every erase and program of a rewrite of sector 1;
// and the first start after an earlier build left an entry for sector 1 with other bytes for the table, cut too.
#define SPI_NOR_TEST_LIB
#pragma GCC diagnostic ignored "-Wunused-function"	// the generic tests spiNorTest.c runs itself
#include "spiNorTest.c"
#include "store_cs7000p.h"
#include "codeplug/codeplug.h"
#include "cps/cps_openfw.h"
#include "debuglink/dlog.h"

// The board's defines (board.mk): -DSPI_NOR_ADDR4=1 -DSTORAGE_CLAIM_FIRST=1 -DSTORAGE_FACTORY_KEEP=1
#if SPI_NOR_ADDR4 != 1 || STORAGE_CLAIM_FIRST != 1 || STORAGE_FACTORY_KEEP != 1
#error "this test builds the driver and core/storage as the board does (board.mk)"
#endif

#if CONFIG_DEBUGLINK
void dlog(const char *fmt, ...) { (void)fmt; }
#endif

#define FULL		CS7000P_STORE_FULL_SIZE
#define LOW_HALF	0x1000000u			// what 3-byte addresses reach, and where the whole map is
#define HOLE		0x01B000u			// from the journal's end to the open-firmware flash image
#define CODEPLUG	0x020FF0u			// somewhere a codeplug write goes
#define TABLE		CS7000P_STORE_TABLE_ADDR	// the maker's factory table, in sector 1
#define TABLE_SIZE	CS7000P_STORE_TABLE_SIZE
#define SECTOR1		0x001000u
#define MAKER		CS7000P_STORE_MAKER_ADDR	// the maker's records: two whole sectors
#define MAKER_SIZE	CS7000P_STORE_MAKER_SIZE
#define DTMF		0x001400u			// the open-firmware DTMF settings (open-firmware-cps.md 7.3.9)
#define DTMF_SIZE	0x78u
#define APRS		0x001588u			// the APRS configurations (7.3.8), 8 x 64 bytes
#define APRS_SIZE	0x200u

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
	// No CPS calibration window: that window is one a CPS may write, and the maker's table is not one
	CHECK(m->calibration_size == 0);

	// The factory list: the table (512 bytes of sector 1) and the maker's records (two whole sectors in the hole),
	// nothing else
	CHECK(m->factory[0].addr == 0x001000u && m->factory[0].size == 0x200u);
	CHECK(m->factory[1].addr == 0x01E000u && m->factory[1].size == 0x2000u);
	CHECK(TABLE == m->factory[0].addr && TABLE_SIZE == m->factory[0].size);
	CHECK(MAKER == m->factory[1].addr && MAKER_SIZE == m->factory[1].size);
	for (unsigned i = 2; i < HAL_FLASH_FACTORY_MAX; i++) {
		CHECK(m->factory[i].addr == 0 && m->factory[i].size == 0);
	}
	// The table inside the EEPROM image's sector 1; the DTMF settings and APRS configurations in the same sector,
	// outside the table: they stay writable
	CHECK(TABLE >= m->openfw_eeprom && TABLE + TABLE_SIZE <= m->openfw_eeprom + openfw_eeprom_len);
	CHECK(TABLE >= SECTOR1 && TABLE + TABLE_SIZE <= SECTOR1 + STORAGE_SECTOR && SECTOR1 % STORAGE_SECTOR == 0);
	CHECK(hal_flash_factory_unit(m, STORAGE_SECTOR, DTMF, DTMF_SIZE) && !hal_flash_factory_bytes(m, DTMF, DTMF_SIZE));
	CHECK(hal_flash_factory_unit(m, STORAGE_SECTOR, APRS, APRS_SIZE) && !hal_flash_factory_bytes(m, APRS, APRS_SIZE));
	CHECK(DTMF + DTMF_SIZE <= APRS && APRS + APRS_SIZE <= SECTOR1 + STORAGE_SECTOR);
	CHECK(hal_flash_factory_bytes(m, TABLE, 1) && hal_flash_factory_bytes(m, TABLE + TABLE_SIZE - 1u, 1));
	CHECK(!hal_flash_factory_bytes(m, TABLE - 1u, 1) && !hal_flash_factory_bytes(m, TABLE + TABLE_SIZE, 1));
	// Sector 0 (the maker's device information beside the open-firmware callsign and DMR ID) is not kept
	CHECK(!hal_flash_factory_unit(m, STORAGE_SECTOR, 0, STORAGE_SECTOR));
	// The records: sector aligned, in the hole between the board sector and the open-firmware flash image
	CHECK(MAKER % STORAGE_SECTOR == 0 && MAKER_SIZE % STORAGE_SECTOR == 0 && MAKER >= HOLE &&
	      MAKER + MAKER_SIZE == m->openfw_flash);
	// The board sector (the copy's record) clear of every range, and so of every unit holding factory bytes
	CHECK(CS7000P_STORE_CAL_ADDR == 0x01B000u && CS7000P_STORE_CAL_ADDR + CS7000P_STORE_CAL_SIZE <= MAKER);
	CHECK(!hal_flash_factory_unit(m, STORAGE_SECTOR, CS7000P_STORE_CAL_ADDR, CS7000P_STORE_CAL_SIZE));
	// Every unit a range touches clear of the core's own areas (the settings ring, the journal, native, staging)
	for (unsigned i = 0; i < n; i++) {
		if (area[i].addr != m->openfw_eeprom && area[i].addr != m->openfw_flash) {
			CHECK(!hal_flash_factory_unit(m, STORAGE_SECTOR, area[i].addr, area[i].len));
		}
	}
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
	// The hole, the board sector and the maker's records in it: written by nothing of ours (the records are kept, and
	// nothing writes the board sector yet)
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

// ---- The maker's table and records through the core's writers ----

// A backstop as the STM32 glue keeps one (platform/stm32/spi_flash): an erase or a program in an erase unit holding
// factory bytes is refused unless core/storage granted that unit. Counted, and every erase or program of a whole kept
// sector too
static uint32_t granted_unit = UINT32_MAX;
static uint32_t kept_ops, ungranted_ops, maker_ops;

static void guard_grant(uint32_t unit)
{
	granted_unit = unit;
}

static bool guarded(uint32_t addr, uint32_t len)
{
	const hal_flash_map_t *m = &cs7000p_flash_map;
	if (overlaps(addr, len, MAKER, MAKER_SIZE)) {
		maker_ops++;
	}
	if (!hal_flash_factory_unit(m, STORAGE_SECTOR, addr, len)) {
		return true;
	}
	if (granted_unit == UINT32_MAX || addr < granted_unit || addr - granted_unit >= STORAGE_SECTOR ||
	    len > STORAGE_SECTOR - (addr - granted_unit)) {
		ungranted_ops++;
		return false;
	}
	kept_ops++;
	return true;
}

static bool g_write(uint32_t addr, const void *buf, uint32_t len)
{
	return guarded(addr, len) && f_write(addr, buf, len);
}

static bool g_erase(uint32_t addr)
{
	return guarded(addr, STORAGE_SECTOR) && f_erase(addr);
}

static void guard_on(void)
{
	flash.write = g_write;
	flash.erase = g_erase;
	flash.factory_grant = guard_grant;
	granted_unit = UINT32_MAX;
	kept_ops = ungranted_ops = maker_ops = 0;
}

static void guard_off(void)
{
	flash.write = f_write;
	flash.erase = f_erase;
	flash.factory_grant = NULL;
}

// The CPS host side, as tests/host/cpsTest.c drives the shim (qdmr's open-firmware driver): requests in one go,
// replies collected
static uint8_t cps_out[64];
static size_t cps_out_len;
static uint32_t cps_now = 1000;

static void io_write(const uint8_t *buf, size_t len)
{
	if (cps_out_len + len <= sizeof(cps_out)) {
		memcpy(cps_out + cps_out_len, buf, len);
	}
	cps_out_len += len;
}
static void io_reboot(bool save) { (void)save; }
static const cps_io_t cps_io = { .write = io_write, .reboot = io_reboot };

static void cps_request(const uint8_t *buf, size_t len)
{
	cps_out_len = 0;
	cps_feed(buf, len, cps_now);
	for (int i = 0; i < CPS_STALL_MS / 5 + 3 && cps_out_len == 0; i++) {
		cps_now += 5;
		cps_feed(NULL, 0, cps_now);
	}
	cps_now += 1;
}

static bool cps_control(uint8_t sub, uint8_t arg)
{
	uint8_t r[3] = { 'C', sub, arg };
	cps_request(r, sub == 6 ? 3 : 2);
	return cps_out_len == 1 && cps_out[0] == '-';
}

static bool cps_x(const uint8_t *r, size_t len, uint8_t sub)
{
	cps_request(r, len);
	return cps_out_len == 2 && cps_out[0] == 'X' && cps_out[1] == sub;
}

// A CPS write of [from, to) of the EEPROM image from img as qdmr sends it: per 4 KB sector prepare, 32-byte data
// blocks with every byte of the image in the range (the table's too), commit
static bool cps_write(const uint8_t *img, uint32_t from, uint32_t to)
{
	bool ok = cps_control(0, 0) && cps_control(1, 0) && cps_control(3, 0) && cps_control(6, 4) && cps_control(6, 2);
	for (uint32_t a = from; ok && a < to;) {
		uint32_t sector = a / STORAGE_SECTOR, send = (sector + 1u) * STORAGE_SECTOR;
		uint8_t p[5] = { 'X', 1, (uint8_t)(sector >> 16), (uint8_t)(sector >> 8), (uint8_t)sector };
		ok = cps_x(p, sizeof(p), 1);
		for (; ok && a < to && a < send; a += 32u) {
			uint32_t n = to - a < 32u ? to - a : 32u;
			uint8_t d[8 + 32] = { 'X', 2, (uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a, 0,
					      (uint8_t)n };
			memcpy(d + 8, img + a, n);
			ok = cps_x(d, 8u + n, 2);
		}
		uint8_t c[2] = { 'X', 3 };
		ok = ok && cps_x(c, sizeof(c), 3);
	}
	return ok && cps_control(5, 0);
}

// EEPROM part 1 (0x0080-0x6060, open-firmware-cps.md 7.1)
static bool cps_write_part1(const uint8_t *img)
{
	return cps_write(img, 0x0080u, 0x6060u);
}

// An INVENTED table and records: no unit's bytes. Not blank, not zero, every byte position different. The window
// is also tried blank (as the first radio's is now: an earlier build replaced its table) and zero: the kept rewrite
// keeps whatever the window holds, valid or not; only the restore path (not here) changes it
static uint8_t inv_table[TABLE_SIZE], blank_table[TABLE_SIZE], zero_table[TABLE_SIZE];
static uint8_t inv_maker[MAKER_SIZE];
static const uint8_t *table = inv_table;	// what the window holds in the test running

static void invent(void)
{
	for (uint32_t i = 0; i < TABLE_SIZE; i++) {
		inv_table[i] = (uint8_t)(0x5Au + i * 7u + (i >> 6));
	}
	memset(blank_table, 0xFF, sizeof(blank_table));
	memset(zero_table, 0x00, sizeof(zero_table));
	for (uint32_t i = 0; i < MAKER_SIZE; i++) {
		inv_maker[i] = (uint8_t)(0xA5u ^ (i * 3u) ^ (i >> 8));
	}
}

static bool maker_intact(void)
{
	return memcmp(&chip.mem[TABLE], table, TABLE_SIZE) == 0 && memcmp(&chip.mem[MAKER], inv_maker, MAKER_SIZE) == 0;
}

// Sector 1 outside the table equals want's (a whole image's bytes at the same addresses)
static bool sector1_rest_is(const uint8_t *want)
{
	return memcmp(&chip.mem[SECTOR1], want + SECTOR1, TABLE - SECTOR1) == 0 &&
	       memcmp(&chip.mem[TABLE + TABLE_SIZE], want + TABLE + TABLE_SIZE,
		      SECTOR1 + STORAGE_SECTOR - (TABLE + TABLE_SIZE)) == 0;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// The last journaled write was the kept rewrite of sector 1: a sector entry (OJNL) for 0x1000 with the kept block
// (OJNK, the table's 512 bytes) in J0, done; not a patch transaction (OJN2)
static bool last_was_kept_rewrite(void)
{
	const uint8_t *j0 = &chip.mem[cs7000p_flash_map.journal_addr];
	return le32(j0) == 0x4C4E4A4Fu && le32(j0 + 4) == SECTOR1 && le32(j0 + 32) == 0x4B4E4A4Fu &&
	       le32(j0 + 36) == SECTOR1 && le32(j0 + 40) == TABLE_SIZE && j0[16] != 0xFFu &&
	       memcmp(j0 + 48, table, TABLE_SIZE) == 0;
}

static storage_stats_t stats_now(void)
{
	storage_stats_t s;
	storage_get_stats(&s);
	return s;
}

static void start_codeplug(bool cold)
{
	(void)(cold ? boot() : warm_boot());
	codeplug_init();
	cps_init(&cps_io);
}

// A chip the maker's firmware used: its bytes everywhere, the window (invented, blank or zero) and the invented
// records where the maker keeps them. The factory list passes storage_init's checks (the region is foreign, not none,
// and the debug link may read each range); after the claim, every writer of sector 1 the core has works, writes the
// rest of the sector and leaves the maker's bytes as they were
static void test_kept(const uint8_t *window)
{
	const hal_flash_map_t *m = &cs7000p_flash_map;
	uint8_t *snap = malloc(FULL);
	uint8_t *img = malloc(0x10000u);
	uint8_t rec[0x200];
	storage_stats_t s0, s1;

	invent();
	table = window;
	create();
	junk(0, FULL, 23);
	memcpy(&chip.mem[TABLE], table, TABLE_SIZE);
	memcpy(&chip.mem[MAKER], inv_maker, MAKER_SIZE);
	memcpy(snap, chip.mem, FULL);
	guard_on();

	// The list is accepted: the region is foreign (claim first), not none, and the ranges are readable for a backup
	// compare, each whole, nothing across a range's edge
	start_codeplug(true);
	CHECK(storage_region() == STORAGE_REGION_FOREIGN);
	CHECK(storage_debug_read_allowed(TABLE, TABLE_SIZE) && storage_debug_read_allowed(MAKER, MAKER_SIZE));
	CHECK(!storage_debug_read_allowed(TABLE - 1u, 2) && !storage_debug_read_allowed(TABLE + TABLE_SIZE - 1u, 2));
	CHECK(!storage_debug_read_allowed(SECTOR1, STORAGE_SECTOR) && !storage_debug_read_allowed(DTMF, DTMF_SIZE));
	CHECK(f_read(TABLE, rec, TABLE_SIZE) && memcmp(rec, table, TABLE_SIZE) == 0);
	CHECK(memcmp(snap, chip.mem, FULL) == 0 && chip.programs == 0 && chip.erases == 0);

	// The claim, then the codeplug: another firmware's bytes, so "start a new codeplug" first
	CHECK(storage_claim_request());
	settings_poll(6000);
	CHECK(storage_region() == STORAGE_REGION_OURS && storage_ready());
	CHECK(codeplug_init() == CODEPLUG_BUILTIN && codeplug_foreign());
	s0 = stats_now();
	CHECK(codeplug_start_new() && !codeplug_foreign() && codeplug_source() == CODEPLUG_OPENFW);
	CHECK(maker_intact() && sector1_rest_is(snap));		// it clears nothing in sector 1
	s1 = stats_now();
	CHECK(s1.factory_refused == s0.factory_refused && s1.errors == s0.errors);

	// A DTMF edit (a 120-byte record, the editors' storage_write): the sector path, so a kept rewrite of sector 1
	// rather than a patch transaction; the record written, the rest of the sector as it was, the table kept
	memcpy(img, chip.mem, 0x10000u);
	for (uint32_t i = 0; i < DTMF_SIZE; i++) {
		rec[i] = (uint8_t)(0x31u + i);
	}
	s0 = stats_now();
	CHECK(storage_write(DTMF, rec, DTMF_SIZE));
	s1 = stats_now();
	memcpy(img + DTMF, rec, DTMF_SIZE);
	CHECK(maker_intact() && sector1_rest_is(img) && last_was_kept_rewrite());
	CHECK(s1.factory_rewrites == s0.factory_rewrites + 1u && s1.factory_dropped == s0.factory_dropped);
	CHECK(s1.factory_refused == s0.factory_refused && s1.errors == s0.errors);

	// An APRS edit: a configuration as a CPS writes it (open-firmware-cps.md 7.3.8), then its path and SSID changed
	// by the radio's editor: each a kept rewrite of sector 1
	memset(rec, 0, 0x40u);
	memcpy(rec, "APRS1", 5);
	rec[0x08] = 9;
	memcpy(&rec[0x0F], "WIDE1 \x01WIDE2 \x01", 14);
	memset(&rec[0x1F], 0xFF, 24);
	memcpy(&rec[0x1F], "Mobile", 6);
	rec[0x3E] = 0x53;
	rec[0x3F] = 0x41;
	s0 = stats_now();
	CHECK(storage_write(APRS, rec, 0x40u));
	memcpy(img + APRS, rec, 0x40u);
	CHECK(maker_intact() && sector1_rest_is(img) && last_was_kept_rewrite());
#if CONFIG_APRS_AFSK
	codeplug_reload();
	codeplug_aprs_t cfg;
	CHECK(codeplug_set_aprs_path(1, "WIDE2-2", "") && last_was_kept_rewrite());
	CHECK(memcmp(&chip.mem[APRS + 0x0Fu], "WIDE2 \x02      \x00", 14) == 0);
	CHECK(codeplug_set_aprs_ssid(1, 12) && chip.mem[APRS + 0x08u] == 12 && last_was_kept_rewrite());
	CHECK(codeplug_aprs(1, &cfg) && cfg.ssid == 12 && strcmp(cfg.path[0], "WIDE2-2") == 0 &&
	      strcmp(cfg.comment, "Mobile") == 0);
	memcpy(img + APRS, &chip.mem[APRS], 0x40u);
	CHECK(maker_intact() && sector1_rest_is(img));
#endif
	s1 = stats_now();
	CHECK(s1.factory_rewrites >= s0.factory_rewrites + 1u && s1.factory_dropped == s0.factory_dropped);
	CHECK(s1.factory_refused == s0.factory_refused && s1.errors == s0.errors);

	// A CPS write of EEPROM part 1 from another radio's image: other bytes everywhere, the table's range included.
	// Acknowledged; every byte of part 1 is the image's but the table's, which stays; sector 0 (the maker's device
	// information, not kept) is the image's too
	for (uint32_t i = 0; i < 0x10000u; i++) {
		img[i] = (uint8_t)(0x0Du + i * 29u + (i >> 9));
	}
	s0 = stats_now();
	CHECK(cps_write_part1(img));
	s1 = stats_now();
	CHECK(maker_intact() && sector1_rest_is(img));
	bool part1 = true;
	for (uint32_t a = 0x0080u; a < 0x6060u; a++) {
		part1 = part1 && ((a >= TABLE && a < TABLE + TABLE_SIZE) || chip.mem[a] == img[a]);
	}
	CHECK(part1 && memcmp(&chip.mem[0x0080u], img + 0x0080u, 0x0220u - 0x0080u) == 0);
	CHECK(s1.factory_rewrites == s0.factory_rewrites + 1u && s1.factory_dropped == s0.factory_dropped + 1u);
	CHECK(s1.factory_refused == s0.factory_refused && s1.errors == s0.errors);
	cps_stats_t cs;
	cps_get_stats(&cs);
	CHECK(!cs.pending);

	// "Start a new codeplug" over that image (another firmware's codeplug again, or the radio's own: it works either
	// way), then an erase of the whole EEPROM image: the table and records stay
	codeplug_init();
	CHECK(codeplug_start_new() && !codeplug_foreign());
	CHECK(storage_write(m->openfw_eeprom, NULL, 0x10000u) && maker_intact());
	CHECK(chip.mem[TABLE + TABLE_SIZE] == 0xFFu && chip.mem[DTMF] == 0xFFu && chip.mem[APRS + APRS_SIZE - 1u] == 0xFFu);

	// The next starts, cold and warm: ours, nothing pending, the maker's bytes as they were
	for (int start = 0; start < 2; start++) {
		start_codeplug(start == 0);
		CHECK(storage_region() == STORAGE_REGION_OURS && storage_ready() && maker_intact());
		CHECK(stats_now().factory_refused == 0 && stats_now().foreign_slots == 0);
	}
	// Every erase and program of sector 1 was inside the core's grant; the records' sectors and the board sector
	// saw none; nothing above 16 MB
	CHECK(ungranted_ops == 0 && kept_ops > 0 && maker_ops == 0);
	CHECK(memcmp(snap + HOLE, chip.mem + HOLE, m->openfw_flash - HOLE) == 0);
	CHECK(chip.high_ops == 0 && memcmp(snap + LOW_HALF, chip.mem + LOW_HALF, FULL - LOW_HALF) == 0);
	CHECK(chip.violations == 0 && chip.page_overruns == 0 && chip.ext == 0 && !chip.wel);
	guard_off();
	free(img);
	free(snap);
	printf("# the maker's table and records kept through start new, DTMF, APRS, a CPS part 1 write, an erase\n");
}

// A claimed chip the maker's firmware used (its bytes in the low megabyte and the hole, the invented table and
// records), the open-firmware codeplug started new, the backstop on: where the tests below begin
static void kept_chip(void)
{
	table = inv_table;
	create();
	junk(0, 0x100000u, 29);
	memcpy(&chip.mem[TABLE], table, TABLE_SIZE);
	memcpy(&chip.mem[MAKER], inv_maker, MAKER_SIZE);
	guard_on();
	start_codeplug(true);
	CHECK(storage_claim_request());
	settings_poll(6000);
	CHECK(storage_ready());
	codeplug_init();
	CHECK(codeplug_start_new());
}

static uint32_t flash_ops(void)
{
	return chip.programs + chip.erases;
}

// A write that differs from the flash only inside the table: it succeeds, the bytes are dropped (counted), and the
// sector is neither erased nor programmed. Through the CPS shim (a tool sending the table's range only, or a sector
// with the radio's own bytes but there), storage_sector_write and a record write into the range
static void test_kept_only_inside(void)
{
	uint8_t *img = malloc(0x10000u);
	uint8_t sec[STORAGE_SECTOR];

	kept_chip();
	memcpy(img, chip.mem, 0x10000u);
	for (uint32_t i = 0; i < TABLE_SIZE; i++) {
		img[TABLE + i] = (uint8_t)~table[i];
	}
	storage_stats_t s0 = stats_now();
	uint32_t ops0 = flash_ops();
	CHECK(cps_write(img, TABLE, TABLE + TABLE_SIZE));		// only the range
	CHECK(cps_write(img, SECTOR1, SECTOR1 + STORAGE_SECTOR));	// the whole sector, the radio's bytes but there
	memcpy(sec, img + SECTOR1, sizeof(sec));
	CHECK(storage_sector_write(SECTOR1, sec));
	CHECK(storage_write(TABLE + 0x40u, img + TABLE + 0x40u, 0x80u));
	storage_stats_t s1 = stats_now();
	CHECK(flash_ops() == ops0 && maker_intact() && memcmp(&chip.mem[SECTOR1], img + SECTOR1, TABLE - SECTOR1) == 0);
	CHECK(s1.factory_dropped == s0.factory_dropped + 4u && s1.factory_rewrites == s0.factory_rewrites);
	CHECK(s1.factory_refused == s0.factory_refused && s1.errors == s0.errors);
	// And a write to the records' sectors: success, nothing written
	memset(sec, 0x00, sizeof(sec));
	CHECK(storage_sector_write(MAKER, sec) && storage_write(MAKER + STORAGE_SECTOR + 0x10u, sec, 0x40u));
	CHECK(storage_write(MAKER, NULL, MAKER_SIZE));
	CHECK(flash_ops() == ops0 && maker_intact() && ungranted_ops == 0 && maker_ops == 0);
	guard_off();
	free(img);
}

// One writer of sector 1, as a power cut finds it: before the cut, old is sector 1; the write would leave neu there
static uint8_t cut_old[STORAGE_SECTOR], cut_neu[STORAGE_SECTOR], cut_src[STORAGE_SECTOR];

static bool cut_dtmf(void)
{
	return storage_write(DTMF, cut_src + (DTMF - SECTOR1), DTMF_SIZE);
}

// A CPS-like whole-sector write from another image: its bytes for the table differ and are dropped
static bool cut_sector(void)
{
	return storage_sector_write(SECTOR1, cut_src);
}

static bool sector1_is(const uint8_t *want)
{
	return memcmp(&chip.mem[SECTOR1], want, STORAGE_SECTOR) == 0;
}

// A power cut at every erase and program of a kept rewrite of sector 1 (each torn as the chip model tears it), then a
// start with the power gone: the region ours, the table and records as they were, the rest of the sector all old or
// all new, nothing pending, and the write again works
static void test_kept_cuts(void)
{
	bool (*const writers[])(void) = { cut_dtmf, cut_sector };

	for (unsigned w = 0; w < 2; w++) {
		uint32_t total = 0;
		unsigned olds = 0, news = 0;
		// cut 0: no cut, counts the operations; the last cut point is past them
		for (uint32_t cut = 0; cut == 0 || cut <= total + 1u; cut++) {
			kept_chip();
			memcpy(cut_old, &chip.mem[SECTOR1], STORAGE_SECTOR);
			for (uint32_t i = 0; i < STORAGE_SECTOR; i++) {
				cut_src[i] = (uint8_t)(0x3Cu + i * 17u + (i >> 4));
			}
			// The sector the write leaves: the writer's bytes outside the table (the DTMF record only, for the edit)
			memcpy(cut_neu, w == 0 ? cut_old : cut_src, STORAGE_SECTOR);
			memcpy(cut_neu + (DTMF - SECTOR1), cut_src + (DTMF - SECTOR1), DTMF_SIZE);
			memcpy(cut_neu + (TABLE - SECTOR1), table, TABLE_SIZE);
			uint32_t ops0 = flash_ops();
			storage_stats_t s0 = stats_now();
			chip.ops_left = cut;
			bool done = writers[w]();
			if (cut == 0) {
				total = flash_ops() - ops0;
				CHECK(done && sector1_is(cut_neu) && total > 30u);
			}
			// (the counters run on across starts in this test: only their changes count)
			start_codeplug(true);
			storage_stats_t s1 = stats_now();
			bool is_old = sector1_is(cut_old), is_new = sector1_is(cut_neu);
			bool ok = storage_region() == STORAGE_REGION_OURS && storage_ready() && maker_intact() &&
				  (is_old || is_new) && s1.factory_refused == s0.factory_refused;
			olds += is_old && cut != 0;
			news += is_new && cut != 0;
			ok = ok && writers[w]() && sector1_is(cut_neu) && maker_intact();
			storage_stats_t s2 = stats_now();
			ok = ok && s2.errors == s1.errors && s2.factory_refused == s1.factory_refused;
			ok = ok && ungranted_ops == 0 && maker_ops == 0 && chip.high_ops == 0;
			if (!ok) {
				printf("FAIL %s:%d: writer %u of sector 1 cut at operation %u of %u\n", __FILE__, __LINE__, w,
				       (unsigned)cut, (unsigned)total);
				failures++;
			}
		}
		CHECK(olds > 0 && news > 0);
	}
	guard_off();
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

// An entry an earlier build of the port left for sector 1 (settings-store.md 4: J1 the new sector, J0 the header,
// the done byte pending), as a power cut right after its commit point leaves it, so the target is still intact.
// The earlier build kept nothing: its J1 holds the CPS image's bytes for the table too
static void earlier_entry(const uint8_t *j1)
{
	const uint32_t j = cs7000p_flash_map.journal_addr;
	uint8_t h[16];

	put32(&h[0], 0x4C4E4A4Fu);
	put32(&h[4], SECTOR1);
	put32(&h[8], storage_crc32(0, j1, STORAGE_SECTOR));
	put32(&h[12], storage_crc32(0, h, 12));
	memset(&chip.mem[j], 0xFF, 2u * STORAGE_SECTOR);
	memcpy(&chip.mem[j + STORAGE_SECTOR], j1, STORAGE_SECTOR);
	memcpy(&chip.mem[j], h, sizeof(h));
}

// The first start of this build on a radio where the earlier one was cut in the middle of a CPS write of sector 1: the
// replay finishes it with the stored table carried over (the image's bytes for it never land), also when this start
// is itself cut at any of its erases and programs
static void test_kept_replay(void)
{
	uint32_t total = 0;

	for (uint32_t cut = 0; cut == 0 || cut <= total + 1u; cut++) {
		kept_chip();
		memcpy(cut_old, &chip.mem[SECTOR1], STORAGE_SECTOR);
		for (uint32_t i = 0; i < STORAGE_SECTOR; i++) {
			cut_src[i] = (uint8_t)(0x71u + i * 5u + (i >> 3));
		}
		memcpy(cut_neu, cut_src, STORAGE_SECTOR);
		memcpy(cut_neu + (TABLE - SECTOR1), table, TABLE_SIZE);
		earlier_entry(cut_src);
		uint32_t ops0 = flash_ops();
		storage_stats_t s0 = stats_now();
		chip_power_on();
		chip.ops_left = cut;		// after the power is on: the cut is in this start's replay
		start_codeplug(false);
		storage_stats_t s1 = stats_now();
		if (cut == 0) {
			total = flash_ops() - ops0;
			CHECK(sector1_is(cut_neu) && s1.factory_replays == s0.factory_replays + 1u &&
			      s1.factory_refused == s0.factory_refused && total > 3u);
		}
		// The power back: whatever the cut left, the table is the stored one and the sector is old or the image's
		start_codeplug(true);
		storage_stats_t s2 = stats_now();
		bool ok = storage_region() == STORAGE_REGION_OURS && storage_ready() && maker_intact() &&
			  (sector1_is(cut_old) || sector1_is(cut_neu));
		// A cut inside the conversion (the kept block written from the target) leaves the entry pending at the next
		// start, the target untouched; the next journaled write replaces it
		ok = ok && (s2.factory_refused == s1.factory_refused || sector1_is(cut_old));
		ok = ok && cut_dtmf() && maker_intact() && ungranted_ops == 0 && maker_ops == 0;
		if (!ok) {
			printf("FAIL %s:%d: an earlier build's entry for sector 1, this start cut at operation %u of %u\n",
			       __FILE__, __LINE__, (unsigned)cut, (unsigned)total);
			failures++;
		}
	}
	guard_off();
}

int main(void)
{
	test_probe();
	test_map();
	test_blank_chip();
	test_used_chip();
	test_blank_region_in_a_used_chip();
	test_suspended_chip();
	test_kept(inv_table);
	test_kept(blank_table);
	test_kept(zero_table);
	test_kept_only_inside();
	test_kept_cuts();
	test_kept_replay();
	test_commands_sent();
	free(chip.mem);
	flash.map = &map16;
	printf("# cs7000pStoreTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
