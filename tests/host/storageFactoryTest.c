// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The factory ranges (hal_flash_map_t.factory, docs/spec/settings-store.md 4.1) as a board that keeps the maker's
// bytes builds them (-DSTORAGE_FACTORY_KEEP=1). core/storage on the simulated NOR flash, which counts erases and
// programs per sector and refuses those in a kept sector outside the grant (its own backstop): the list's checks at
// storage_init (a list that fails leaves the region none, with no flash operation at all), the helpers at the edges of
// a range, and the kept rewrite: every writer succeeds, the factory bytes stay the stored ones, the writer's bytes
// there are dropped and counted, a whole kept sector is never touched, a record in a kept sector takes the sector
// path; a power cut at every flash operation of a kept rewrite and of the replays that write; a read that goes wrong
// once; the held journal; the replay of every shape of J0 (this firmware's, another firmware's converted or left
// pending, stale headers, patch entries); a claim; the debug read inside a range; the staging and satellite areas.
// Then the STM32 data flash glue (platform/stm32/spi_flash) on a fake SPI NOR driver that records what reaches the
// chip: its backstop refuses every erase and program in a unit holding factory bytes but the one unit granted. Built a
// second time with -DSPI_NOR_ADDR4=1 (storageFactoryAddr4Test): also an address 16 MB up, which a slip to 3-byte
// addresses would take there. Every range and every byte here is invented.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "satellite/store.h"
#include "debuglink/dlog.h"
#include "flash/spi_nor/spi_nor.h"
#include "spi_flash_board.h"
#include "sim/sim_flash.h"

#if STORAGE_FACTORY_KEEP != 1
#error "this test is core/storage and the STM32 flash glue with -DSTORAGE_FACTORY_KEEP=1"
#endif

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

static unsigned log_lines;
static char log_last[96];

#if CONFIG_DEBUGLINK
void dlog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(log_last, sizeof(log_last), fmt, ap);
	va_end(ap);
	log_lines++;
}
#endif

#define FLASH_SIZE	0x40000u
#define RING		0x11000u
#define RING_SIZE	0x8000u
#define JOURNAL		0x19000u
#define J1		(JOURNAL + STORAGE_SECTOR)
#define S		STORAGE_SECTOR
// Three ranges, as a maker might leave them: one inside a sector (the rest of that sector is the codeplug's), two
// whole sectors, and one across a sector end
#define RANGE_A		0x1000u
#define RANGE_A_SIZE	0x200u
#define RANGE_B		0x3000u
#define RANGE_B_SIZE	(2u * S)
#define RANGE_C		0x6F00u
#define RANGE_C_SIZE	0x200u
#define FREE		0x2000u		// between A and B
#define FREE_HI		0x8000u		// after C

// Inside the open-firmware EEPROM image, as the maker's data may be; the areas of ours are elsewhere
#define BASE_MAP \
	.settings_addr = RING, .settings_size = RING_SIZE, .journal_addr = JOURNAL, \
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x30000u, \
	.calibration_addr = 0x10000u, .calibration_size = 0x200u

static const hal_flash_map_t plain = { BASE_MAP };
static const hal_flash_map_t with_factory = {
	BASE_MAP,
	.factory = { { RANGE_A, RANGE_A_SIZE }, { RANGE_B, RANGE_B_SIZE }, { RANGE_C, RANGE_C_SIZE } },
};
// Every sector holding factory bytes of with_factory
static const uint32_t kept_sectors[] = { 0x1000u, 0x3000u, 0x4000u, 0x6000u, 0x7000u };

const board_t board = { .name = "test", .flash = &sim_flash, .power_count = 5 };

static void fill(uint8_t *b, uint32_t len, uint8_t seed)
{
	for (uint32_t i = 0; i < len; i++) {
		b[i] = (uint8_t)(seed + i * 7u + (i >> 8));
	}
}

// A fresh chip with the map, old codeplug bytes everywhere below the ring (the maker's among them)
static void fresh(const hal_flash_map_t *m)
{
	sim_flash_create(FLASH_SIZE, m);
	fill(sim_flash_data, 0x10000u, 0x5A);
}

// The same chip read with another map (an earlier firmware's, or this one's)
static bool boot(const hal_flash_map_t *m)
{
	sim_flash.map = m;
	sim_flash_power_on();
	return storage_init();
}

static uint32_t touches(uint32_t addr)
{
	return sim_flash_erases(addr) + sim_flash_programs(addr);
}

// Erases plus programs of every kept sector so far
static uint32_t kept_touches(void)
{
	uint32_t n = 0;
	for (unsigned i = 0; i < sizeof(kept_sectors) / sizeof(kept_sectors[0]); i++) {
		n += touches(kept_sectors[i]);
	}
	return n;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

// A journal sector entry as a power cut after its header leaves it (settings-store.md 4): J1 the new content, J0 the
// header, the done byte pending
static void pending_sector(uint32_t target, const uint8_t *data)
{
	memset(&sim_flash_data[JOURNAL], 0xFF, 2u * S);
	memcpy(&sim_flash_data[J1], data, S);
	uint8_t *h = &sim_flash_data[JOURNAL];
	put32(&h[0], 0x4C4E4A4Fu);
	put32(&h[4], target);
	put32(&h[8], storage_crc32(0, data, S));
	put32(&h[12], storage_crc32(0, h, 12));
}

// A patch transaction as a cut right after its commit point leaves it: the header and the data in J0, no mark
static void pending_patch(uint32_t addr, const uint8_t *data, uint32_t len)
{
	memset(&sim_flash_data[JOURNAL], 0xFF, 2u * S);
	uint8_t *h = &sim_flash_data[JOURNAL];
	put32(&h[0], 0x324E4A4Fu);
	put32(&h[4], addr);
	put32(&h[8], len);
	put32(&h[12], storage_crc32(storage_crc32(0, h, 12), data, len));
	memcpy(&h[24], data, len);
}

// ---- The helpers ----

static void test_helpers(void)
{
	static const hal_flash_map_t m = {
		.factory = { { 0x1000u, 0x1000u }, { 0x5100u, 0x10u }, { 0xFFFFF000u, 0x1000u } },
	};
	CHECK(!hal_flash_factory_unit(NULL, S, 0x1000u, 1));
	CHECK(!hal_flash_factory_unit(&m, S, 0x1000u, 0));
	CHECK(!hal_flash_factory_unit(&m, S, 0x0FFFu, 1));
	CHECK(hal_flash_factory_unit(&m, S, 0x0FFFu, 2));		// the last byte before, and the first inside
	CHECK(hal_flash_factory_unit(&m, S, 0x1000u, 1));
	CHECK(hal_flash_factory_unit(&m, S, 0x1FFFu, 1));
	CHECK(!hal_flash_factory_unit(&m, S, 0x2000u, 1));
	CHECK(hal_flash_factory_unit(&m, S, 0x0000u, 0x3000u));	// a span around it
	CHECK(!hal_flash_factory_unit(&m, S, 0x2000u, 0x3000u));	// up to the unit below the next range
	// A range inside a unit: the whole unit holds factory bytes
	CHECK(hal_flash_factory_unit(&m, S, 0x5000u, 1));
	CHECK(hal_flash_factory_unit(&m, S, 0x5FFFu, 1));
	CHECK(!hal_flash_factory_unit(&m, S, 0x6000u, 1));
	CHECK(!hal_flash_factory_unit(&m, S, 0x4FFFu, 1));
	// The bytes themselves
	CHECK(!hal_flash_factory_bytes(&m, 0x50FFu, 1));
	CHECK(hal_flash_factory_bytes(&m, 0x50FFu, 2));
	CHECK(hal_flash_factory_bytes(&m, 0x5100u, 1));
	CHECK(hal_flash_factory_bytes(&m, 0x510Fu, 1));
	CHECK(!hal_flash_factory_bytes(&m, 0x5110u, 1));
	CHECK(!hal_flash_factory_bytes(&m, 0x5000u, 0x100u));
	CHECK(!hal_flash_factory_bytes(&m, 0x5110u, 0xEF0u));
	CHECK(hal_flash_factory_bytes(&m, 0x5000u, 0x1000u));
	CHECK(!hal_flash_factory_bytes(&m, 0x5100u, 0));
	CHECK(hal_flash_factory_unit(&m, 0, 0x5100u, 1) && !hal_flash_factory_unit(&m, 0, 0x5110u, 1));	// unit 0: bytes
	// A range at the top of the address space, and a span that reaches it
	CHECK(hal_flash_factory_unit(&m, S, 0xFFFFFFFFu, 1) && hal_flash_factory_bytes(&m, 0xFFFFFFFFu, 1));
	CHECK(!hal_flash_factory_unit(&m, S, 0xFFFFEFFFu, 1));
	CHECK(hal_flash_factory_bytes(&m, 0xFFFFEFFFu, 2));
	// An entry past one of size 0 still counts (storage_init refuses that list; a driver's backstop doesn't rely on it)
	static const hal_flash_map_t ended = { .factory = { { 0, 0 }, { 0x1000u, 0x10u } } };
	CHECK(hal_flash_factory_unit(&ended, S, 0x1FFFu, 1) && !hal_flash_factory_unit(&ended, S, 0, 1));
	CHECK(hal_flash_factory_bytes(&ended, 0x100Fu, 1) && !hal_flash_factory_bytes(&ended, 0x1010u, 1));
	CHECK(!hal_flash_factory_unit(&plain, S, 0, FLASH_SIZE) && !hal_flash_factory_bytes(&plain, 0, FLASH_SIZE));
}

// ---- The list's checks ----

static bool fake_replace(uint32_t addr, const void *data)
{
	return sim_flash.erase(addr) && sim_flash.write(addr, data, S);
}

// A list that fails: region none, not one read, program or erase, every write refused, one log line
static void bad_list(const hal_flash_map_t *m, unsigned line)
{
	uint8_t a[S];

	fresh(m);
	uint32_t u0 = sim_flash_units(), r0 = sim_flash_reads();
	unsigned l0 = log_lines;
	bool ok = !boot(m) && storage_region() == STORAGE_REGION_NONE && !storage_ready();
	fill(a, S, (uint8_t)line);
	ok = ok && !storage_sector_write(0x2000u, a) && !storage_write(0x2000u, a, 16);
	ok = ok && sim_flash_units() == u0 && sim_flash_reads() == r0;
#if CONFIG_DEBUGLINK
	ok = ok && log_lines == l0 + 1u && strstr(log_last, "factory list") != NULL;
#else
	(void)l0;
#endif
	if (!ok) {
		printf("FAIL %s:%u: the list was not refused as it should be\n", __FILE__, line);
		failures++;
	}
}

static bool good_list(const hal_flash_map_t *m)
{
	fresh(m);
	return boot(m) && storage_region() == STORAGE_REGION_OURS;
}

#define BAD(...) do { static const hal_flash_map_t m_ = { BASE_MAP, __VA_ARGS__ }; bad_list(&m_, __LINE__); } while (0)
#define GOOD(...) do { static const hal_flash_map_t m_ = { BASE_MAP, __VA_ARGS__ }; CHECK(good_list(&m_)); } while (0)

static void test_list_checks(void)
{
	// The order of the entries
	BAD(.factory = { { 0, 0 }, { 0x1000u, 0x10u } });				// an entry past the end
	BAD(.factory = { { 0x1000u, 0 } });						// an end with an address
	BAD(.factory = { { 0x1000u, 0x10u }, { 0, 0 }, { 0x2000u, 0x10u } });
	BAD(.factory = { { 0x2000u, 0x10u }, { 0x1000u, 0x10u } });			// descending
	BAD(.factory = { { 0x1000u, 0x20u }, { 0x1010u, 0x20u } });			// overlapping
	BAD(.factory = { { 0x1000u, 0x20u }, { 0x1020u, 0x20u } });			// adjacent: one range
	GOOD(.factory = { { 0x1000u, 0x20u }, { 0x1021u, 0x20u } });			// apart by a byte
	// Inside the chip
	BAD(.factory = { { FLASH_SIZE - 0x10u, 0x11u } });
	BAD(.factory = { { FLASH_SIZE, 1u } });
	BAD(.factory = { { 0xFFFFF000u, 0x2000u } });					// and no wrap
	GOOD(.factory = { { 0, 0x10u }, { FLASH_SIZE - 0x10u, 0x10u } });		// both ends
	// Every sector a range touches clear of our areas, a byte in the same sector as one of them too
	BAD(.factory = { { RING + RING_SIZE - 1u, 1u } });				// the settings ring
	BAD(.factory = { { JOURNAL + 2u * S - 1u, 2u } });				// the journal (J1)
	GOOD(.factory = { { JOURNAL + 2u * S, 0x10u } });
	BAD(.factory = { { 0x10800u, 0x10u } });					// the calibration window's sector
	BAD(.native_addr = 0x30000u, .native_size = 0x8000u, .factory = { { 0x37FF0u, 0x10u } });
	BAD(.staging_addr = 0x30000u, .staging_size = 0x8000u, .factory = { { 0x2FFF0u, 0x11u } });
	GOOD(.staging_addr = 0x30000u, .staging_size = 0x8000u, .factory = { { 0x2FFF0u, 0x10u } });
	BAD(.satellite_addr = 0x3C000u, .satellite_size = 0x3000u, .factory = { { 0x3E000u, 0x2000u } });
	GOOD(.satellite_addr = 0x3C000u, .satellite_size = 0x3000u, .factory = { { 0x3F000u, 0x1000u } });
	// What J0 can carry of a sector that holds kept bytes beside others: 4048 (STORAGE_SECTOR - 48)
	GOOD(.factory = { { 0x2000u, 4048u } });
	BAD(.factory = { { 0x2000u, 4049u } });
	BAD(.factory = { { 0x202Fu, 4049u } });
	GOOD(.factory = { { 0x2030u, 4049u } });					// 4048 here, 1 in the next
	GOOD(.factory = { { 0x2000u, S } });						// a whole sector: never written
	GOOD(.factory = { { 0x2000u, 3u * S } });
	BAD(.factory = { { 0x2000u, 2u * S + 4049u } });				// its last sector
	BAD(.factory = { { 0x1F00u, 0x100u + 2u * S + 4049u } });
	BAD(.factory = { { 0x2000u, 2001u }, { 0x2800u, 2048u } });			// two ranges in one sector
	GOOD(.factory = { { 0x2000u, 2000u }, { 0x2800u, 2048u } });
	// No copy-on-write flash underneath
	sim_flash.replace = fake_replace;
	static const hal_flash_map_t one = { BASE_MAP, .factory = { { 0x1000u, 0x10u } } };
	bad_list(&one, __LINE__);
	CHECK(good_list(&plain));		// without a list as ever
	sim_flash.replace = NULL;
	CHECK(good_list(&one));
	CHECK(good_list(&with_factory));
}

// ---- The kept rewrite ----

static storage_stats_t st(void)
{
	storage_stats_t s;
	storage_get_stats(&s);
	return s;
}

// The factory bytes of with_factory, and whether an address is one
static const hal_flash_range_t ranges[] = { { RANGE_A, RANGE_A_SIZE }, { RANGE_B, RANGE_B_SIZE }, { RANGE_C, RANGE_C_SIZE } };

static bool factory_byte(uint32_t a)
{
	for (unsigned i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		if (a >= ranges[i].addr && a - ranges[i].addr < ranges[i].size) {
			return true;
		}
	}
	return false;
}

// The chip as fresh() left it: what every factory byte must still hold
static uint8_t orig[0x10000u];
// The range inside a sector as fresh_kept() leaves it: the filled bytes (-1), or all 0xFF (a table already lost: a
// blank kept stream programs nothing) or all 0x00
static int kept_fill = -1;

static void fresh_kept(void)
{
	fresh(&with_factory);
	if (kept_fill >= 0) {
		memset(&sim_flash_data[RANGE_A], kept_fill, RANGE_A_SIZE);
	}
	memcpy(orig, sim_flash_data, sizeof(orig));
}

// Every factory byte as the maker left it (orig)
static bool kept_intact(void)
{
	for (unsigned i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		if (memcmp(&sim_flash_data[ranges[i].addr], &orig[ranges[i].addr], ranges[i].size) != 0) {
			return false;
		}
	}
	return true;
}

// The sector at base, outside its factory bytes, equals want
static bool rest_is(uint32_t base, const uint8_t *want)
{
	for (uint32_t i = 0; i < S; i++) {
		if (!factory_byte(base + i) && sim_flash_data[base + i] != want[i]) {
			return false;
		}
	}
	return true;
}

// The sector at base as a writer meant it with the kept bytes in place: want outside, kept bytes from orig
static void merged(uint32_t base, const uint8_t *want, uint8_t *out)
{
	for (uint32_t i = 0; i < S; i++) {
		out[i] = factory_byte(base + i) ? orig[base + i] : want[i];
	}
}

static bool grant_closed(void)
{
	return sim_flash_granted() == UINT32_MAX && sim_flash_ungranted() == 0;
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Erases and programs that reached a kept sector, each with the grant open on that very sector (the sim's backstop
// refuses the others, and core/storage's own wr()/er() before it)
static unsigned granted_ops, ungranted_ops;

static void grant_log(char kind, uint32_t addr, uint32_t len, const void *data, bool ok)
{
	(void)data;
	if (kind == 'r' || !ok || !hal_flash_factory_unit(sim_flash.map, S, addr, len != 0 ? len : 1u)) {
		return;
	}
	if (sim_flash_granted() == (addr & ~(S - 1u))) {
		granted_ops++;
	} else {
		ungranted_ops++;
	}
}

static void test_sector_write(void)
{
	static uint8_t a[S], b[S], want[S];

	fresh_kept();
	CHECK(boot(&with_factory) && storage_ready());
	storage_stats_t s0 = st();
	unsigned l0 = log_lines;
	// Other bytes everywhere, the range's among them: success, the rest written, the factory bytes the stored ones
	fill(a, S, 9);
	granted_ops = ungranted_ops = 0;
	sim_flash_set_log(grant_log);
	CHECK(storage_sector_write(RANGE_A, a));
	CHECK(rest_is(RANGE_A, a) && kept_intact());
	CHECK(st().factory_rewrites == s0.factory_rewrites + 1u && st().factory_dropped == s0.factory_dropped + 1u);
	CHECK(st().sector_writes == s0.sector_writes + 1u && st().factory_refused == s0.factory_refused);
	CHECK(granted_ops == 1u + S / 256u && ungranted_ops == 0);	// the target's erase and its 16 chunks
#if CONFIG_DEBUGLINK
	CHECK(log_lines == l0 + 1u && strcmp(log_last, "storage: kept 0x001000-0x0011ff, a write's bytes there dropped") == 0);
#endif
	// J0: the sector header, done, and the kept block of the 512 bytes after it
	const uint8_t *j = &sim_flash_data[JOURNAL];
	CHECK(get32(&j[0]) == 0x4C4E4A4Fu && get32(&j[4]) == RANGE_A && j[16] == 0);
	CHECK(get32(&j[32]) == 0x4B4E4A4Fu && get32(&j[36]) == RANGE_A && get32(&j[40]) == RANGE_A_SIZE);
	CHECK(memcmp(&j[48], &orig[RANGE_A], RANGE_A_SIZE) == 0 && j[48 + RANGE_A_SIZE] == 0xFF);
	merged(RANGE_A, a, want);
	CHECK(memcmp(&sim_flash_data[J1], want, S) == 0);			// J1: the merged sector
	// The same bytes again: a skip (the factory bytes still differ: dropped again, no second log line)
	uint32_t u0 = sim_flash_units();
	CHECK(storage_sector_write(RANGE_A, a) && sim_flash_units() == u0);
	CHECK(st().sector_skips == s0.sector_skips + 1u && st().factory_dropped == s0.factory_dropped + 2u);
	CHECK(log_lines == l0 + (CONFIG_DEBUGLINK ? 1u : 0u));
	// Only a factory byte differs: a skip too
	memcpy(b, &sim_flash_data[RANGE_A], S);
	b[0x10] ^= 0x80;
	CHECK(storage_sector_write(RANGE_A, b) && sim_flash_units() == u0 && st().factory_dropped == s0.factory_dropped + 3u);
	// A tool that sends the radio's own factory bytes and changes others: rewritten, nothing dropped
	b[0x10] ^= 0x80;
	b[0x800] ^= 0x55;
	CHECK(storage_sector_write(RANGE_A, b) && memcmp(&sim_flash_data[RANGE_A], b, S) == 0);
	CHECK(st().factory_dropped == s0.factory_dropped + 3u && st().factory_rewrites == s0.factory_rewrites + 2u);
	// A whole kept sector: success, nothing erased or programmed
	uint32_t t0 = touches(RANGE_B) + touches(RANGE_B + S);
	CHECK(storage_sector_write(RANGE_B, a) && storage_sector_write(RANGE_B + S, a));
	CHECK(touches(RANGE_B) + touches(RANGE_B + S) == t0 && st().factory_dropped == s0.factory_dropped + 5u);
	// Both sectors of the range across a sector end
	CHECK(storage_sector_write(0x6000u, a) && storage_sector_write(0x7000u, a));
	CHECK(rest_is(0x6000u, a) && rest_is(0x7000u, a) && kept_intact());
	// A free sector between kept ones as ever: a plain entry, no kept block
	CHECK(storage_sector_write(FREE, a) && memcmp(a, &sim_flash_data[FREE], S) == 0);
	CHECK(get32(&j[4]) == FREE && get32(&j[32]) == 0xFFFFFFFFu);
	CHECK(st().factory_refused == s0.factory_refused && grant_closed() && ungranted_ops == 0);
	sim_flash_set_log(NULL);
	// The next start: the region ours, nothing foreign; an earlier firmware's start (no list) also stays ours, its scan
	// counts the kept block after the done byte as one foreign slot
	CHECK(storage_sector_write(0x7000u, b));		// J0 left holding a kept entry
	CHECK(boot(&with_factory) && storage_region() == STORAGE_REGION_OURS && st().foreign_slots == 0);
	CHECK(boot(&plain) && storage_region() == STORAGE_REGION_OURS && st().foreign_slots == 1);
	CHECK(boot(&with_factory) && st().foreign_slots == 0 && kept_intact() && sim_flash_violations() == 0);
}

static void test_write(void)
{
	static uint8_t a[0x8000u], before[0x10000u];

	fresh_kept();
	CHECK(boot(&with_factory));
	storage_stats_t s0 = st();
	fill(a, sizeof(a), 3);
	granted_ops = ungranted_ops = 0;
	sim_flash_set_log(grant_log);
	// A DTMF-like record inside a kept sector, clear of its factory bytes: the sector path (a sector entry, no patch
	// entry), under the grant, the factory bytes stored, the rest of the sector as it was
	memcpy(before, sim_flash_data, sizeof(before));
	CHECK(storage_write(RANGE_A + 0x400u, a, 0x78u));
	CHECK(memcmp(&sim_flash_data[RANGE_A + 0x400u], a, 0x78u) == 0 && kept_intact());
	CHECK(memcmp(&sim_flash_data[RANGE_A + 0x200u], &before[RANGE_A + 0x200u], 0x200u) == 0);
	CHECK(memcmp(&sim_flash_data[RANGE_A + 0x478u], &before[RANGE_A + 0x478u], S - 0x478u) == 0);
	CHECK(get32(&sim_flash_data[JOURNAL]) == 0x4C4E4A4Fu && get32(&sim_flash_data[JOURNAL + 32]) == 0x4B4E4A4Fu);
	CHECK(st().factory_rewrites == s0.factory_rewrites + 1u && st().factory_dropped == s0.factory_dropped);
	CHECK(granted_ops == 1u + S / 256u);
	// A record reaching the factory bytes: written beside them, its bytes there dropped
	CHECK(storage_write(RANGE_A + RANGE_A_SIZE - 0x10u, a, 0x20u));
	CHECK(memcmp(&sim_flash_data[RANGE_A + RANGE_A_SIZE], a + 0x10u, 0x10u) == 0 && kept_intact());
	CHECK(st().factory_dropped == s0.factory_dropped + 1u);
	// A record across a free sector into a whole kept sector: the free part written, the kept one unchanged
	CHECK(storage_write(RANGE_B - 0x10u, a, 0x20u) && memcmp(&sim_flash_data[RANGE_B - 0x10u], a, 0x10u) == 0);
	CHECK(kept_intact());
	// An erase of a kept sector's range: the factory bytes stay, the rest erased
	CHECK(storage_write(RANGE_A, NULL, S) && kept_intact() && sim_flash_data[RANGE_A + RANGE_A_SIZE] == 0xFF);
	CHECK(sim_flash_data[RANGE_A + S - 1u] == 0xFF);
	// The sector path over every kind of sector, and "start a new codeplug" (an erase of it all)
	CHECK(storage_write(0, a, sizeof(a)) && kept_intact());
	for (uint32_t base = 0; base < sizeof(a); base += S) {
		CHECK(rest_is(base, a + base));
	}
	CHECK(storage_write(0, NULL, 0x10000u) && kept_intact() && sim_flash_data[FREE] == 0xFF && sim_flash_data[0] == 0xFF);
	// A free sector's record: still the patch transaction
	CHECK(storage_write(FREE_HI + 0x10u, a, 16) && get32(&sim_flash_data[JOURNAL]) == 0x324E4A4Fu);
	CHECK(memcmp(&sim_flash_data[FREE_HI + 0x10u], a, 16) == 0);
	CHECK(st().factory_refused == s0.factory_refused && ungranted_ops == 0 && grant_closed());
	CHECK(sim_flash_violations() == 0);
	sim_flash_set_log(NULL);
}

// ---- A power cut at every flash operation ----

// The erases and programs of one run, from a dry run: where each starts in units after the run's start, and its units
#define OPS_MAX		256u
static uint32_t op_start[OPS_MAX], op_units[OPS_MAX], ops, units0;

static void op_log(char kind, uint32_t addr, uint32_t len, const void *data, bool ok)
{
	(void)addr;
	(void)data;
	if (kind != 'r' && ok && ops < OPS_MAX) {
		uint32_t n = kind == 'e' ? 1u : len;
		op_start[ops] = sim_flash_units() - n - units0;
		op_units[ops++] = n;
	}
}

// Cut points: the first unit of every operation, the second, its middle and its last (n units go through, the next
// fails). 0 is no cut at all (sim_flash_cut_after), so a run's very first unit is never cut: nothing would be written
static uint32_t cut_points(uint32_t *pt, uint32_t max)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < ops; i++) {
		uint32_t c[4] = { op_start[i], op_start[i] + 1u, op_start[i] + op_units[i] / 2u, op_start[i] + op_units[i] - 1u };
		for (unsigned k = 0; k < 4; k++) {
			bool dup = c[k] == 0 || (k > 0 && c[k] >= op_start[i] + op_units[i]);
			for (uint32_t j = 0; !dup && j < n; j++) {
				dup = pt[j] == c[k];
			}
			if (!dup && n < max) {
				pt[n++] = c[k];
			}
		}
	}
	return n;
}

static void dry_run(void (*setup)(void), bool (*run)(void))
{
	setup();
	ops = 0;
	units0 = sim_flash_units();
	sim_flash_set_log(op_log);
	CHECK(run());
	sim_flash_set_log(NULL);
}

static bool blank_at(uint32_t addr, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		if (sim_flash_data[addr + i] != 0xFF) {
			return false;
		}
	}
	return true;
}

static uint8_t cut_new[S], cut_old[S];
static uint32_t cut_target;

// This firmware's kept rewrite of a sector that holds factory bytes beside others, the journal blank (first)
// or holding a kept entry of another sector before
static void setup_first(void)
{
	fresh_kept();
	CHECK(boot(&with_factory));
	cut_target = RANGE_A;
	memcpy(cut_old, &sim_flash_data[cut_target], S);
	fill(cut_new, S, 77);
}

static void setup_second(void)
{
	static uint8_t x[S];
	fresh_kept();
	CHECK(boot(&with_factory));
	fill(x, S, 31);
	CHECK(storage_sector_write(0x7000u, x));
	cut_target = 0x6000u;
	memcpy(cut_old, &sim_flash_data[cut_target], S);
	fill(cut_new, S, 78);
}

static bool run_rewrite(void)
{
	return storage_sector_write(cut_target, cut_new);
}

static void test_cut_rewrite(void)
{
	static uint32_t pt[4u * OPS_MAX];
	void (*setups[2])(void) = { setup_first, setup_second };

	for (unsigned v = 0; v < 2; v++) {
		dry_run(setups[v], run_rewrite);
		uint32_t n = cut_points(pt, sizeof(pt) / sizeof(pt[0]));
		unsigned olds = 0, news = 0;
		CHECK(ops > 30u && n > 100u);
		for (uint32_t i = 0; i < n; i++) {
			setups[v]();
			uint32_t r0 = st().factory_refused;
			sim_flash_cut_after(pt[i]);
			bool done = run_rewrite();
			bool j0_blank = blank_at(JOURNAL, S), j1_blank = blank_at(J1, S);
			bool ok = !done && boot(&with_factory) && storage_region() == STORAGE_REGION_OURS;
			// J0 never counts a foreign slot; J1 does only as for any journaled write (written while J0 is blank)
			ok = ok && st().foreign_slots == (j0_blank && !j1_blank ? 1u : 0u);
			ok = ok && kept_intact() && grant_closed() && st().factory_refused == r0;
			bool is_old = rest_is(cut_target, cut_old), is_new = rest_is(cut_target, cut_new);
			ok = ok && (is_old || is_new);
			olds += is_old;
			news += is_new && !is_old;
			// The journal is usable at once: the write again, then another sector
			ok = ok && storage_sector_write(cut_target, cut_new) && rest_is(cut_target, cut_new);
			ok = ok && storage_sector_write(FREE, cut_old) && kept_intact() && sim_flash_violations() == 0;
			if (!ok) {
				printf("FAIL %s:%d: kept rewrite %u (fill %d) cut after %u units\n", __FILE__, __LINE__, v,
				       kept_fill, (unsigned)pt[i]);
				failures++;
			}
		}
		CHECK(olds > 0 && news > 0);
	}
}

// ---- A read that goes wrong once ----

static uint32_t mr_addr, mr_reads, mr_merge, mr_second, mr_first;
static bool mr_j1_seen, mr_hdr_seen;

static void misread_log(char kind, uint32_t addr, uint32_t len, const void *data, bool ok)
{
	(void)data;
	(void)ok;
	if (kind == 'r' && mr_addr >= addr && mr_addr - addr < len) {
		mr_reads++;
		mr_first = mr_first != 0 ? mr_first : mr_reads;
		mr_merge = !mr_j1_seen ? mr_reads : mr_merge;
		mr_second = !mr_hdr_seen ? mr_reads : mr_second;
	}
	if (kind == 'w' && addr == J1) {
		mr_j1_seen = true;
	}
	if (kind == 'w' && addr == JOURNAL && len == 16u) {
		mr_hdr_seen = true;
	}
}

static void test_misread(void)
{
	static uint8_t a[S];

	// Which read of a factory byte is which: the compare (step 0), the merge into J1, the second read before the
	// header (the last before the commit point)
	mr_addr = RANGE_A + 0x10u;
	fresh_kept();
	CHECK(boot(&with_factory));
	fill(a, S, 5);
	mr_reads = mr_merge = mr_second = mr_first = 0;
	mr_j1_seen = mr_hdr_seen = false;
	sim_flash_set_log(misread_log);
	CHECK(storage_sector_write(RANGE_A, a) && kept_intact());
	sim_flash_set_log(NULL);
	CHECK(mr_first == 1u && mr_merge == 2u && mr_second == 3u);
	// A misread in the merge (J1 and the kept block carry it) or in the second read: stopped before the commit
	// point, the target untouched, nothing to replay, the write works the next time
	uint32_t which[2] = { mr_merge, mr_second };
	for (unsigned k = 0; k < 2; k++) {
		fresh_kept();
		CHECK(boot(&with_factory));
		uint32_t r0 = st().factory_refused, t0 = touches(RANGE_A);
		sim_flash_misread(mr_addr, which[k]);
		CHECK(!storage_sector_write(RANGE_A, a));
		CHECK(st().factory_refused == r0 + 1u && touches(RANGE_A) == t0 && blank_at(JOURNAL, 32));
		CHECK(get32(&sim_flash_data[JOURNAL + 32]) == 0xFFFFFFFFu);	// no valid kept block either
		CHECK(boot(&with_factory) && st().foreign_slots == 0 && touches(RANGE_A) == t0 && kept_intact());
		CHECK(storage_sector_write(RANGE_A, a) && rest_is(RANGE_A, a) && kept_intact() && grant_closed());
	}
	// A misread in the compare only counts a drop: the bytes written are the flash's
	fresh_kept();
	CHECK(boot(&with_factory));
	uint32_t d0 = st().factory_dropped;
	memcpy(a, &sim_flash_data[RANGE_A], S);
	a[0x800] ^= 1;
	sim_flash_misread(mr_addr, 1);
	CHECK(storage_sector_write(RANGE_A, a) && st().factory_dropped == d0 + 1u && kept_intact() && rest_is(RANGE_A, a));
}

// ---- The held journal ----

static void test_held(void)
{
	static uint8_t a[S], b[S], journal[2u * S];

	fresh_kept();
	CHECK(boot(&with_factory));
	fill(a, S, 21);
	fill(b, S, 22);
	// The target's programs fail after its erase: past the commit point, its factory bytes now only in J0 and J1
	sim_flash_bad_range(RANGE_A, S);
	storage_stats_t s0 = st();
	CHECK(!storage_sector_write(RANGE_A, a));
	CHECK(blank_at(RANGE_A, S) && sim_flash_data[JOURNAL + 16] == 0xFF && grant_closed());
	memcpy(journal, &sim_flash_data[JOURNAL], sizeof(journal));
	// Every write is refused now, whatever sector, the patch path and the sector path alike, a skip too (a held
	// entry can be that sector's: its replay would undo a write skipped against it); the journal as it was
	CHECK(!storage_sector_write(FREE, b));
	CHECK(!storage_write(FREE + 0x10u, b, 16));
	CHECK(!storage_write(FREE_HI, b, 2u * S));
	CHECK(!storage_sector_write(0x7000u, b));
	CHECK(!storage_sector_write(FREE_HI + S, &sim_flash_data[FREE_HI + S]));
	CHECK(memcmp(journal, &sim_flash_data[JOURNAL], sizeof(journal)) == 0);
	CHECK(st().factory_refused == s0.factory_refused + 5u && st().errors > s0.errors + 5u);
	// The failure lifted: the next write finishes the entry first, then is done itself
	sim_flash_bad_range(0, 0);
	CHECK(storage_sector_write(FREE, b) && memcmp(&sim_flash_data[FREE], b, S) == 0);
	CHECK(rest_is(RANGE_A, a) && kept_intact() && st().factory_replays == s0.factory_replays + 1u);
	CHECK(storage_write(FREE_HI + 0x10u, a, 16) && grant_closed());
	// The held entry's own sector written as an erase would leave it (0xFF beside the factory bytes, which an erased
	// target reads as already): the entry finished first, then the write; never a skip against the erased target
	fresh_kept();
	CHECK(boot(&with_factory));
	sim_flash_bad_range(RANGE_A, S);
	CHECK(!storage_sector_write(RANGE_A, a));
	sim_flash_bad_range(0, 0);
	memset(b, 0xFF, S);
	CHECK(storage_sector_write(RANGE_A, b) && kept_intact() && blank_at(RANGE_A + RANGE_A_SIZE, S - RANGE_A_SIZE));
	fill(b, S, 22);
	CHECK(boot(&with_factory) && kept_intact() && blank_at(RANGE_A + RANGE_A_SIZE, S - RANGE_A_SIZE));
	// Held across a power cut: the start finishes it, as any cut
	fresh_kept();
	CHECK(boot(&with_factory));
	sim_flash_bad_range(RANGE_A, S);
	CHECK(!storage_sector_write(RANGE_A, a) && !storage_write(FREE + 0x10u, b, 16));
	sim_flash_bad_range(0, 0);
	CHECK(boot(&with_factory) && rest_is(RANGE_A, a) && kept_intact());
	CHECK(storage_write(FREE + 0x10u, b, 16));
	// A held patch entry is not replayed into the shared buffer while someone holds it (a CPS session's sector)
	fresh_kept();
	CHECK(boot(&with_factory));
	sim_flash_bad_range(RANGE_A, S);
	CHECK(!storage_sector_write(RANGE_A, a));
	sim_flash_bad_range(0, 0);
	uint8_t *buf = storage_buffer_acquire();
	CHECK(buf != NULL);
	if (buf != NULL) {
		memcpy(buf, b, S);
		CHECK(storage_sector_write(FREE, buf) && memcmp(&sim_flash_data[FREE], b, S) == 0);	// a sector entry: replayed
		storage_buffer_release();
	}
	CHECK(rest_is(RANGE_A, a) && kept_intact());
	// An earlier firmware's patch entry in a kept sector whose replay at the start fails after the target's erase:
	// held, and not replayed into the shared buffer while someone holds it; then finished by the next write
	uint8_t p[0x20];
	fill(p, sizeof(p), 23);
	fresh_kept();
	pending_patch(RANGE_A + 0x400u, p, sizeof(p));
	sim_flash_bad_range(RANGE_A, S);
	CHECK(boot(&with_factory) && blank_at(RANGE_A, S) && sim_flash_data[JOURNAL + 20] == 0xFF);
	sim_flash_bad_range(0, 0);
	memcpy(journal, &sim_flash_data[JOURNAL], sizeof(journal));
	buf = storage_buffer_acquire();
	CHECK(buf != NULL);
	if (buf != NULL) {
		memcpy(buf, b, S);
		CHECK(!storage_sector_write(FREE, buf) && memcmp(journal, &sim_flash_data[JOURNAL], sizeof(journal)) == 0);
		storage_buffer_release();
	}
	CHECK(storage_sector_write(FREE, b) && memcmp(&sim_flash_data[FREE], b, S) == 0);
	CHECK(memcmp(&sim_flash_data[RANGE_A + 0x400u], p, sizeof(p)) == 0 && kept_intact() && grant_closed());
	CHECK(sim_flash_violations() == 0);
}

// ---- The replay at a start: every shape of J0 ----

// A kept block for target in J0 as this firmware writes it: the stream from src (the target's bytes, say), n bytes
static void put_block(uint32_t target, const uint8_t *stream, uint32_t n)
{
	uint8_t *k = &sim_flash_data[JOURNAL + 32];
	put32(&k[0], 0x4B4E4A4Fu);
	put32(&k[4], target);
	put32(&k[8], n);
	memcpy(&k[16], stream, n);
	put32(&k[12], storage_crc32(storage_crc32(0, k, 12), stream, n));
}

// This firmware's entry pending with the target erased: the target's programs failed after its erase, then the power
static void pending_ours(uint32_t target, const uint8_t *data)
{
	CHECK(boot(&with_factory));
	sim_flash_bad_range(target, S);
	CHECK(!storage_sector_write(target, data));
	sim_flash_bad_range(0, 0);
}

static void test_replay(void)
{
	static uint8_t a[S], b[S], want[S], before[S];
	uint8_t p[0x20];
	fill(a, S, 41);
	fill(b, S, 42);
	fill(p, sizeof(p), 55);

	// This firmware's entry (a header and a valid kept block): the target := J1 with the block over it
	fresh_kept();
	pending_ours(RANGE_A, a);
	storage_stats_t s0 = st();
	CHECK(blank_at(RANGE_A, S));
	CHECK(boot(&with_factory) && rest_is(RANGE_A, a) && kept_intact() && sim_flash_data[JOURNAL + 16] == 0);
	CHECK(st().factory_replays == s0.factory_replays + 1u && grant_closed());
	// ... with a read of J1 that goes wrong once: read again rather than given up as a stale header (the target is
	// erased, its factory bytes in J0 alone)
	fresh_kept();
	pending_ours(RANGE_A, a);
	sim_flash_misread(J1 + 0x10u, 1);
	CHECK(boot(&with_factory) && rest_is(RANGE_A, a) && kept_intact() && sim_flash_data[JOURNAL + 16] == 0);
	// An earlier firmware's verbatim replay of it (no list: J1 as it is) keeps the bytes too: J1 is merged
	fresh_kept();
	pending_ours(0x6000u, a);
	CHECK(boot(&plain) && storage_region() == STORAGE_REGION_OURS && st().foreign_slots == 1);
	CHECK(rest_is(0x6000u, a) && kept_intact());

	// Another firmware's entry (no kept block), the target intact: converted from the target, then finished. The
	// entry's bytes for the range (J1's) never land
	fresh_kept();
	pending_sector(RANGE_A, a);
	s0 = st();
	unsigned l0 = log_lines;
	CHECK(boot(&with_factory) && rest_is(RANGE_A, a) && kept_intact());
	CHECK(get32(&sim_flash_data[JOURNAL + 32]) == 0x4B4E4A4Fu && sim_flash_data[JOURNAL + 16] == 0);
	CHECK(st().factory_replays == s0.factory_replays + 1u && st().factory_refused == s0.factory_refused);
	CHECK(log_lines == l0 && grant_closed());
	// The same with the target torn by that firmware (its first half erased): what it left is what is kept
	fresh_kept();
	pending_sector(RANGE_A, a);
	memset(&sim_flash_data[RANGE_A], 0xFF, S / 2u);
	CHECK(boot(&with_factory) && rest_is(RANGE_A, a) && blank_at(RANGE_A, RANGE_A_SIZE));
	// A conversion cut (a torn kept block beside the header): left pending, nothing written, at every start
	fresh_kept();
	pending_sector(RANGE_A, a);
	memcpy(&sim_flash_data[JOURNAL + 48], &orig[RANGE_A], 100);
	memcpy(before, &sim_flash_data[RANGE_A], S);
	s0 = st();
	l0 = log_lines;
	uint32_t t0 = touches(RANGE_A);
	CHECK(boot(&with_factory) && boot(&with_factory) && st().foreign_slots == 0);
	CHECK(st().factory_refused == s0.factory_refused + 2u && touches(RANGE_A) == t0);
	CHECK(memcmp(before, &sim_flash_data[RANGE_A], S) == 0 && sim_flash_data[JOURNAL + 16] == 0xFF);
#if CONFIG_DEBUGLINK
	CHECK(log_lines == l0 + 2u && strstr(log_last, "left pending") != NULL);
#endif
	// ... and not held: the next journaled write replaces the entry
	CHECK(storage_sector_write(FREE, b) && storage_sector_write(RANGE_A, b) && rest_is(RANGE_A, b) && kept_intact());
	// A valid block for another target, or of another length than the map's: pending
	for (unsigned k = 0; k < 2; k++) {
		fresh_kept();
		pending_sector(RANGE_A, a);
		if (k == 0) {
			put_block(0x7000u, &orig[0x7000u], 0x100u);
		} else {
			put_block(RANGE_A, &orig[RANGE_A], RANGE_A_SIZE - 1u);
		}
		s0 = st();
		t0 = touches(RANGE_A);
		CHECK(boot(&with_factory) && st().factory_refused == s0.factory_refused + 1u && touches(RANGE_A) == t0);
		CHECK(sim_flash_data[JOURNAL + 16] == 0xFF && kept_intact());
	}
	// A whole kept sector: the done byte only, nothing of J1 lands
	fresh_kept();
	pending_sector(RANGE_B + S, a);
	s0 = st();
	t0 = touches(RANGE_B + S);
	CHECK(boot(&with_factory) && touches(RANGE_B + S) == t0 && kept_intact() && sim_flash_data[JOURNAL + 16] == 0);
	CHECK(st().factory_replays == s0.factory_replays + 1u);
	// A stale header (a cut J0 erase can leave one) beside a new J1: no conversion, nothing copied, done
	fresh_kept();
	pending_sector(RANGE_A, a);
	memcpy(&sim_flash_data[J1], b, S);
	memcpy(before, &sim_flash_data[RANGE_A], S);
	CHECK(boot(&with_factory) && memcmp(before, &sim_flash_data[RANGE_A], S) == 0);
	CHECK(sim_flash_data[JOURNAL + 16] == 0 && blank_at(JOURNAL + 32, 48));
	// ... also with a stale kept block of this firmware's beside it
	fresh_kept();
	CHECK(boot(&with_factory) && storage_sector_write(RANGE_A, a));
	sim_flash_data[JOURNAL + 16] = 0xFF;
	memcpy(&sim_flash_data[J1], b, S);
	memcpy(before, &sim_flash_data[RANGE_A], S);
	CHECK(boot(&with_factory) && memcmp(before, &sim_flash_data[RANGE_A], S) == 0 && sim_flash_data[JOURNAL + 16] == 0);

	// An earlier firmware's patch entry in a kept sector, clear of its factory bytes: replayed under the grant
	fresh_kept();
	pending_patch(RANGE_A + 0x400u, p, sizeof(p));
	s0 = st();
	granted_ops = ungranted_ops = 0;
	sim_flash_set_log(grant_log);
	CHECK(boot(&with_factory) && memcmp(&sim_flash_data[RANGE_A + 0x400u], p, sizeof(p)) == 0 && kept_intact());
	sim_flash_set_log(NULL);
	CHECK(sim_flash_data[JOURNAL + 20] == 0 && granted_ops > 0 && ungranted_ops == 0 && grant_closed());
	CHECK(st().factory_replays == s0.factory_replays + 1u);
	// ... staged already (J1 the sector merged with it, by that firmware): copied from J1, the bytes as stored
	fresh_kept();
	pending_patch(RANGE_A + 0x400u, p, sizeof(p));
	memcpy(want, &orig[RANGE_A], S);
	memcpy(&want[0x400], p, sizeof(p));
	memcpy(&sim_flash_data[J1], want, S);
	sim_flash_data[JOURNAL + 16] = 0;					// the first sector staged
	memset(&sim_flash_data[RANGE_A + 0x800u], 0xFF, 0x800u);		// and its erase was cut
	CHECK(boot(&with_factory) && memcmp(&sim_flash_data[RANGE_A], want, S) == 0 && kept_intact());
	// One reaching the factory bytes: pending, nothing written
	fresh_kept();
	pending_patch(RANGE_A + RANGE_A_SIZE - 0x10u, p, sizeof(p));
	s0 = st();
	t0 = touches(RANGE_A);
	CHECK(boot(&with_factory) && st().factory_refused == s0.factory_refused + 1u && touches(RANGE_A) == t0);
	CHECK(sim_flash_data[JOURNAL + 20] == 0xFF && kept_intact());
	// One in a free sector as ever
	fresh_kept();
	pending_patch(FREE_HI + 0x40u, p, sizeof(p));
	CHECK(boot(&with_factory) && memcmp(p, &sim_flash_data[FREE_HI + 0x40u], sizeof(p)) == 0);
	CHECK(sim_flash_data[JOURNAL + 20] == 0 && kept_intact() && sim_flash_violations() == 0);
}

// A start whose replay can't read the entry of a kept sector whose target is erased: J1, the kept block's header, its
// stream, J0's header, a patch entry's data. Never given up: the session's writes are refused (the first erase would
// take the only copies of the factory bytes), and a clean start, or the held journal once the reads work again,
// finishes it. A misread of the block once is read again
static void test_unread(void)
{
	static uint8_t a[S], b[S], journal[2u * S];
	static const struct { uint32_t addr, len; } bad[] = {
		{ J1, S }, { JOURNAL + 32u, 16u }, { JOURNAL + 48u + 0x100u, 16u }, { JOURNAL, 16u },
	};
	fill(a, S, 61);
	fill(b, S, 62);

	for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
		for (unsigned later = 0; later < 2; later++) {
			fresh_kept();
			pending_ours(RANGE_A, a);
			sim_flash_bad_read(bad[k].addr, bad[k].len);
			bool ok = boot(&with_factory) && storage_region() == STORAGE_REGION_OURS && blank_at(RANGE_A, S);
			memcpy(journal, &sim_flash_data[JOURNAL], sizeof(journal));
			if (later == 1 && k != 3) {
				// The reads work again in the same session: the next write finishes the entry first
				sim_flash_bad_read(0, 0);
				ok = ok && storage_sector_write(FREE, b) && rest_is(RANGE_A, a) && kept_intact();
				ok = ok && memcmp(&sim_flash_data[FREE], b, S) == 0;
			} else {
				ok = ok && !storage_sector_write(FREE, b) && !storage_write(FREE + 0x10u, b, 16);
				ok = ok && !storage_sector_write(0x7000u, b) && !storage_write(FREE_HI, b, 2u * S);
				ok = ok && memcmp(journal, &sim_flash_data[JOURNAL], sizeof(journal)) == 0;
				sim_flash_bad_read(0, 0);
				// J0's header unread holds until the next start, the others are tried again by each write
				ok = ok && (k == 3 ? !storage_sector_write(FREE, b) : true);
				ok = ok && boot(&with_factory) && rest_is(RANGE_A, a) && kept_intact();
				ok = ok && storage_sector_write(FREE, b);
			}
			ok = ok && grant_closed() && sim_flash_violations() == 0;
			if (!ok) {
				printf("FAIL %s:%d: unread 0x%x (later %u, fill %d)\n", __FILE__, __LINE__, (unsigned)bad[k].addr,
				       later, kept_fill);
				failures++;
			}
		}
	}
	// The kept block misread once by the replay (its CRC fails): read again, not taken as torn. The start's scan reads
	// that byte three times before (J0's blank check, the kept area's, the block's CRC)
	fresh_kept();
	pending_ours(RANGE_A, a);
	sim_flash_misread(JOURNAL + 48u + 0x10u, 4);
	CHECK(boot(&with_factory) && rest_is(RANGE_A, a) && kept_intact() && sim_flash_data[JOURNAL + 16] == 0);
	// An earlier firmware's patch entry, its sector staged and the target erased, its data unread at the start
	uint8_t p[0x20];
	fill(p, sizeof(p), 63);
	fresh_kept();
	pending_patch(RANGE_A + 0x400u, p, sizeof(p));
	sim_flash_bad_range(RANGE_A, S);
	CHECK(boot(&with_factory) && blank_at(RANGE_A, S));
	sim_flash_bad_range(0, 0);
	sim_flash_bad_read(JOURNAL + 24u, sizeof(p));
	CHECK(boot(&with_factory) && blank_at(RANGE_A, S) && !storage_sector_write(FREE, b));
	sim_flash_bad_read(0, 0);
	CHECK(boot(&with_factory) && memcmp(&sim_flash_data[RANGE_A + 0x400u], p, sizeof(p)) == 0 && kept_intact());
	CHECK(storage_sector_write(FREE, b) && grant_closed() && sim_flash_violations() == 0);
	// A sector holding no factory bytes (the factory copy's record, say), erased by a cut past its commit point, J1
	// unread at the start: not given up either
	fresh_kept();
	CHECK(boot(&with_factory));
	pending_sector(FREE_HI, a);
	memset(&sim_flash_data[FREE_HI], 0xFF, S);
	sim_flash_bad_read(J1 + 0x800u, 16u);
	CHECK(boot(&with_factory) && blank_at(FREE_HI, S) && sim_flash_data[JOURNAL + 16] == 0xFF);
#if CONFIG_DEBUGLINK
	CHECK(strstr(log_last, "refused until the next start") != NULL);
#endif
	CHECK(!storage_sector_write(FREE, b) && !storage_write(FREE + 0x10u, b, 16) && blank_at(FREE_HI, S));
	sim_flash_bad_read(0, 0);
	CHECK(boot(&with_factory) && memcmp(&sim_flash_data[FREE_HI], a, S) == 0 && storage_sector_write(FREE, b));
}

// A power cut at every flash operation of the replays that write: this firmware's entry, a conversion, a patch entry
static unsigned replay_kind;
static uint8_t rp_data[S];

static void setup_replay(void)
{
	fresh_kept();
	if (replay_kind == 0) {
		pending_ours(RANGE_A, rp_data);
	} else if (replay_kind == 1) {
		pending_sector(RANGE_A, rp_data);
	} else {
		pending_patch(RANGE_A + 0x400u, rp_data, 0x40u);
	}
	sim_flash.map = &with_factory;
	sim_flash_power_on();
}

static bool run_replay(void)
{
	return storage_init();
}

static void test_cut_replay(void)
{
	static uint32_t pt[4u * OPS_MAX];
	static uint8_t want[S];

	fill(rp_data, S, 99);
	for (replay_kind = 0; replay_kind < 3; replay_kind++) {
		dry_run(setup_replay, run_replay);
		uint32_t n = cut_points(pt, sizeof(pt) / sizeof(pt[0]));
		CHECK(n > 10u);
		if (replay_kind == 2) {
			memcpy(want, &orig[RANGE_A], S);
			memcpy(&want[0x400], rp_data, 0x40u);
		}
		unsigned finished = 0;
		for (uint32_t i = 0; i < n; i++) {
			setup_replay();
			sim_flash_cut_after(pt[i]);
			(void)storage_init();
			bool ok = boot(&with_factory) && storage_region() == STORAGE_REGION_OURS && kept_intact() && grant_closed();
			bool fin = replay_kind == 2 ? memcmp(&sim_flash_data[RANGE_A], want, S) == 0 : rest_is(RANGE_A, rp_data);
			// A cut conversion leaves the entry pending, the target as it was; everything else is finished
			bool pend = replay_kind == 1 && memcmp(&sim_flash_data[RANGE_A], &orig[RANGE_A], S) == 0 &&
				    sim_flash_data[JOURNAL + 16] == 0xFF;
			ok = ok && (fin || pend) && sim_flash_violations() == 0;
			finished += fin;
			if (!ok) {
				printf("FAIL %s:%d: replay %u (fill %d) cut after %u units\n", __FILE__, __LINE__, replay_kind,
				       kept_fill, (unsigned)pt[i]);
				failures++;
			}
		}
		CHECK(finished > 0);
	}
}

// A region of another firmware's, then claimed: the claim never reaches a kept sector, the writes after it keep them
static void test_claim(void)
{
	uint8_t a[S];

	fresh_kept();
	memset(&sim_flash_data[RING + 0x300u], 0x42, 64);	// another firmware's bytes in the ring
	fill(a, S, 17);
	uint32_t t0 = kept_touches();
	CHECK(!boot(&with_factory) && storage_region() == STORAGE_REGION_FOREIGN);
	CHECK(!storage_write(RANGE_A, &sim_flash_data[RANGE_A], 16) && !storage_sector_write(FREE, a));
	CHECK(!storage_sector_write(RANGE_A, a));
	CHECK(storage_debug_read_allowed(RANGE_A, RANGE_A_SIZE));
	CHECK(storage_claim_request() && storage_claim() && storage_ready());
	CHECK(kept_touches() == t0);
	CHECK(boot(&with_factory) && storage_region() == STORAGE_REGION_OURS);
	CHECK(storage_sector_write(FREE, a) && storage_sector_write(RANGE_A, a) && rest_is(RANGE_A, a) && kept_intact());
	CHECK(grant_closed() && sim_flash_violations() == 0);
}

// ---- Readers and the other areas ----

static void test_debug_read(void)
{
	fresh(&with_factory);
	CHECK(boot(&with_factory));
	CHECK(storage_debug_read_allowed(RANGE_A, RANGE_A_SIZE));
	CHECK(storage_debug_read_allowed(RANGE_A + RANGE_A_SIZE - 1u, 1));
	CHECK(!storage_debug_read_allowed(RANGE_A + RANGE_A_SIZE - 1u, 2));	// past its end
	CHECK(!storage_debug_read_allowed(RANGE_A - 1u, 2));
	CHECK(!storage_debug_read_allowed(RANGE_A + 0x400u, 16));		// the same sector, outside the range
	CHECK(storage_debug_read_allowed(RANGE_B, RANGE_B_SIZE));
	CHECK(storage_debug_read_allowed(RANGE_B + S - 0x10u, 0x20u));		// across its two sectors
	CHECK(storage_debug_read_allowed(RANGE_C + 0xF0u, 0x20u));		// across a sector end
	CHECK(!storage_debug_read_allowed(RANGE_A, RANGE_B + RANGE_B_SIZE - RANGE_A));	// two ranges and between
	CHECK(!storage_debug_read_allowed(FREE, 16));
	CHECK(!storage_debug_read_allowed(RANGE_A, 0));
	CHECK(!storage_debug_read_allowed(0xFFFFFFF0u, 0x20u));
	CHECK(storage_debug_read_allowed(RING, 16) && storage_debug_read_allowed(0x10000u, 0x200u));
	// A list storage_init refused: no range is readable
	static const hal_flash_map_t bad = { BASE_MAP, .factory = { { RANGE_A, RANGE_A_SIZE }, { RANGE_A, 1u } } };
	fresh(&bad);
	CHECK(!boot(&bad) && !storage_debug_read_allowed(RANGE_A, 16) && storage_debug_read_allowed(RING, 16));
	// Without factory ranges the window is as it was
	fresh(&plain);
	CHECK(boot(&plain));
	CHECK(!storage_debug_read_allowed(RANGE_A, 16) && storage_debug_read_allowed(JOURNAL, 2u * S));
}

static void test_staging(void)
{
	// The open-firmware flash image ends below the staging area
#define STAGED_MAP \
	.settings_addr = RING, .settings_size = RING_SIZE, .journal_addr = JOURNAL, \
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x30000u, \
	.staging_addr = 0x30000u, .staging_size = 0x8000u
	static const hal_flash_map_t staged = { STAGED_MAP };
	static const hal_flash_map_t over = { STAGED_MAP, .factory = { { 0x37FFFu, 1u } } };
	static const hal_flash_map_t beside = { STAGED_MAP, .factory = { { 0x38000u, 1u } } };
	sim_flash_create(FLASH_SIZE, &staged);
	CHECK(storage_staging_size() == 0x8000u);
	// storage_init refuses such a list; the staging writers ask again on their own
	sim_flash.map = &over;
	CHECK(storage_staging_size() == 0);
	sim_flash.map = &beside;
	CHECK(storage_staging_size() == 0x8000u);
}

static void test_satellite(void)
{
#define SAT_MAP \
	.settings_addr = RING, .settings_size = RING_SIZE, .journal_addr = JOURNAL, \
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x30000u, \
	.satellite_addr = 0x30000u, .satellite_size = 0x3000u
	static const hal_flash_map_t sat = { SAT_MAP, .factory = { { 0x33000u, 1u } } };
	static const hal_flash_map_t over = { SAT_MAP, .factory = { { 0x32FFFu, 1u } } };
	fresh(&sat);
	CHECK(boot(&sat) && satellite_store_available());
	// storage_init refuses such a list; the satellite store asks again on its own
	sim_flash.map = &over;
	CHECK(!satellite_store_available());
	sim_flash.map = &sat;
	CHECK(satellite_store_available());
}

// ---- The STM32 glue's backstop, on a fake SPI NOR driver ----

#define CHIP_SIZE	(SPI_NOR_ADDR4 ? (32u << 20) : (16u << 20))
static unsigned chip_programs, chip_erases;
static uint32_t chip_last;

bool spi_nor_probe(spi_nor_t *nor)
{
	nor->manufacturer = 0xEF;
	nor->device = 0x4019;
	nor->size = CHIP_SIZE;
#if SPI_NOR_ADDR4
	nor->addr_len = 4;
	nor->unsettled = false;
#endif
	return true;
}
static bool chip_program(uint32_t addr)
{
	chip_programs++;
	chip_last = addr;
	return true;
}
static bool chip_erase(uint32_t addr)
{
	chip_erases++;
	chip_last = addr;
	return true;
}
bool spi_nor_read(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len)
{
	memset(buf, 0xFF, len);
	return true;
}
bool spi_nor_program(const spi_nor_t *nor, uint32_t addr, const void *buf, uint32_t len) { return chip_program(addr); }
bool spi_nor_erase_sector(const spi_nor_t *nor, uint32_t addr) { return chip_erase(addr); }
bool spi_nor_read_security(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len) { return false; }
#if SPI_NOR_ADDR4
bool spi_nor_park(const spi_nor_t *nor) { return true; }
bool spi_nor_read_parked(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len)
{
	return spi_nor_read(nor, addr, buf, len);
}
bool spi_nor_program_parked(const spi_nor_t *nor, uint32_t addr, const void *buf, uint32_t len)
{
	return chip_program(addr);
}
bool spi_nor_erase_sector_parked(const spi_nor_t *nor, uint32_t addr) { return chip_erase(addr); }
#endif

void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool open_drain) {}
void stm32_pin_af(GPIO_TypeDef *port, uint16_t pins, uint8_t af, bool open_drain, uint32_t pull) {}
void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool level) {}
void stm32_spi_master_init(SPI_TypeDef *spi, bool cpol, bool cpha, uint8_t br) {}
bool stm32_spi_xfer(SPI_TypeDef *spi, const uint8_t *tx, uint8_t *rx, uint32_t len) { return false; }

// Whether an erase or a program reached the chip, or was refused with nothing sent
static bool erases_at(uint32_t addr)
{
	unsigned n = chip_erases;
	return stm32_spi_flash.erase(addr) && chip_erases == n + 1u && chip_last == addr;
}
static bool erase_refused(uint32_t addr)
{
	unsigned n = chip_erases;
	uint32_t r = stm32_spi_flash_factory_refusals();
	return !stm32_spi_flash.erase(addr) && chip_erases == n && stm32_spi_flash_factory_refusals() == r + 1u;
}
static bool programs_at(uint32_t addr, uint32_t len)
{
	static uint8_t b[2u * S];
	unsigned n = chip_programs;
	return stm32_spi_flash.write(addr, b, len) && chip_programs == n + 1u && chip_last == addr;
}
static bool program_refused(uint32_t addr, uint32_t len)
{
	static uint8_t b[2u * S];
	unsigned n = chip_programs;
	uint32_t r = stm32_spi_flash_factory_refusals();
	return !stm32_spi_flash.write(addr, b, len) && chip_programs == n &&
	       stm32_spi_flash_factory_refusals() == r + 1u;
}

static void test_backstop(void)
{
	static SPI_TypeDef spi;
	static GPIO_TypeDef port;
	// The small map keeps a range of its own: where the maker's bytes are doesn't depend on the map in use
	static const hal_flash_map_t small = { BASE_MAP, .factory = { { 0x9100u, 0x10u } } };
	static const stm32_spi_flash_cfg_t cfg = {
		.spi = &spi, .bus_port = &port, .cs_port = &port, .device = 0x4019, .device_mask = 0xFFFF,
		.map = &with_factory, .small_map = &small,
	};
	CHECK(stm32_spi_flash_init(&cfg) && stm32_spi_flash.map == &with_factory);
	CHECK(stm32_spi_flash.factory_grant != NULL && stm32_spi_flash_factory_refusals() == 0);
	unsigned l0 = log_lines;
	// No grant: every unit holding factory bytes, of either map, whatever bytes of it the operation reaches
	CHECK(erase_refused(RANGE_A) && erase_refused(RANGE_A + 0xFFFu) && erase_refused(RANGE_B + S));
	CHECK(erase_refused(0x7000u) && erase_refused(0x9000u));
	CHECK(program_refused(RANGE_A + 0x400u, 16) && program_refused(RANGE_A, RANGE_A_SIZE));
	CHECK(program_refused(FREE - 0x10u, 0x20u) && program_refused(RANGE_C - 0xF00u, 1));
	CHECK(program_refused(0x9FFFu, 1));
#if CONFIG_DEBUGLINK
	CHECK(log_lines == l0 + 1u && strstr(log_last, "factory") != NULL);	// the first one only
#else
	(void)l0;
#endif
	// Other units as ever
	CHECK(erases_at(FREE) && erases_at(0x5000u) && erases_at(0x8000u) && erases_at(0));
	CHECK(programs_at(FREE, S) && programs_at(0x8FF0u, 0x10u) && programs_at(0xA000u, 16));
	// A grant opens that one unit, and nothing beside it
	stm32_spi_flash.factory_grant(RANGE_A);
	CHECK(erases_at(RANGE_A) && programs_at(RANGE_A, S) && programs_at(RANGE_A + 0x400u, 16));
	CHECK(programs_at(RANGE_A + S - 1u, 1));
	CHECK(program_refused(RANGE_A + S - 0x10u, 0x20u));		// on into the next unit
	CHECK(program_refused(RANGE_A - 0x10u, 0x20u));
	CHECK(erase_refused(RANGE_B) && erase_refused(0x7000u) && program_refused(0x9100u, 1));
	stm32_spi_flash.factory_grant(0x6000u);
	CHECK(erases_at(0x6000u) && erase_refused(0x7000u) && erase_refused(RANGE_A));
	// A grant off a unit's start opens nothing, and closes the one before it
	stm32_spi_flash.factory_grant(RANGE_A + 0x10u);
	CHECK(erase_refused(RANGE_A) && program_refused(RANGE_A + 0x10u, 1) && erase_refused(0x6000u));
	// Closed again
	stm32_spi_flash.factory_grant(UINT32_MAX);
	CHECK(erase_refused(RANGE_A) && erase_refused(0x6000u) && program_refused(RANGE_A, 1));
	CHECK(erases_at(FREE));
#if SPI_NOR_ADDR4
	// 16 MB up: what a slip to 3-byte addresses would reach is the maker's bytes, granted or not
	CHECK(erase_refused(0x1000000u + RANGE_A) && program_refused(0x1000000u + RANGE_B + 0x10u, 16));
	CHECK(program_refused(0xFFFFF0u, 0x1020u));			// from below 16 MB up across it to the alias
	stm32_spi_flash.factory_grant(RANGE_A);
	CHECK(erase_refused(0x1000000u + RANGE_A) && programs_at(RANGE_A, 16));
	// A grant of the alias itself opens it at its own address only, which still lands on the maker's bytes
	stm32_spi_flash.factory_grant(0x1000000u + RANGE_A);
	CHECK(erase_refused(0x1000000u + RANGE_A) && program_refused(0x1000000u + RANGE_A + 0x10u, 16));
	stm32_spi_flash.factory_grant(UINT32_MAX);
	// Above 16 MB elsewhere, and below it up to the alias: as ever
	CHECK(erases_at(0x1000000u + FREE) && programs_at(0x1000000u + 0x8000u, S) && programs_at(0xFFFFF0u, 0x10u));
	CHECK(erases_at(CHIP_SIZE - S) && !stm32_spi_flash.erase(CHIP_SIZE));
#else
	// A part of 16 MB: no alias to test, the top as ever
	CHECK(erases_at(CHIP_SIZE - S) && !stm32_spi_flash.erase(CHIP_SIZE));
#endif
}

int main(void)
{
	test_helpers();
	test_list_checks();
	test_sector_write();
	test_write();
	// The cuts again with the range inside a sector blank (a table an earlier build lost) and all zeros
	static const int fills[] = { -1, 0xFF, 0x00 };
	for (unsigned f = 0; f < sizeof(fills) / sizeof(fills[0]); f++) {
		kept_fill = fills[f];
		test_cut_rewrite();
		test_cut_replay();
	}
	kept_fill = -1;
	test_misread();
	test_held();
	test_replay();
	test_unread();
	test_claim();
	test_debug_read();
	test_staging();
	test_satellite();
	test_backstop();
	sim_flash_free();
	printf("# %s %s (%d failures)\n", SPI_NOR_ADDR4 ? "storageFactoryAddr4Test" : "storageFactoryTest",
	       failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
