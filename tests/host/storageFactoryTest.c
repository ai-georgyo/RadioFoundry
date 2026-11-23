// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The factory ranges (hal_flash_map_t.factory, docs/spec/settings-store.md 4.1) as a board that keeps the maker's
// bytes builds them (-DSTORAGE_FACTORY_KEEP=1). core/storage on the simulated NOR flash that counts erases and
// programs per sector: the list's checks at storage_init (a list that fails leaves the region none, with no flash
// operation at all), the helpers at the edges of a range, the debug read inside a range, the staging and satellite
// areas, and no erase or program of a sector holding factory bytes by any writer (a write that would change one is
// refused whole, one that leaves it as it is succeeds; a journal entry left for one stays pending, refused where the
// core erases and programs). Then the STM32 data flash glue (platform/stm32/spi_flash) on a fake SPI NOR driver that
// records what reaches the chip: its backstop refuses every erase and program in a unit holding factory bytes but the
// one unit granted. Built a second time with -DSPI_NOR_ADDR4=1 (storageFactoryAddr4Test): also an address 16 MB up,
// which a slip to 3-byte addresses would take there. Every range and every byte here is invented.
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

static uint32_t refused(void)
{
	storage_stats_t s;
	storage_get_stats(&s);
	return s.factory_refused;
}

static uint32_t errors(void)
{
	storage_stats_t s;
	storage_get_stats(&s);
	return s.errors;
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

// ---- The writers ----

static void test_sector_write(void)
{
	uint8_t a[S], keep[S];

	fresh(&with_factory);
	CHECK(boot(&with_factory) && storage_ready());
	uint32_t t0 = kept_touches(), j0 = touches(JOURNAL) + touches(J1), r0 = refused();
	// The bytes it holds: skipped, success
	memcpy(keep, &sim_flash_data[RANGE_A], S);
	CHECK(storage_sector_write(RANGE_A, keep));
	CHECK(storage_sector_write(RANGE_B + S, &sim_flash_data[RANGE_B + S]));
	// Other bytes, also with the range's own as they are: refused, nothing erased, the journal not touched
	memcpy(a, keep, S);
	a[0x400] ^= 1;
	CHECK(!storage_sector_write(RANGE_A, a));
	memcpy(a, keep, S);
	a[0x10] ^= 0x80;					// only inside the range
	CHECK(!storage_sector_write(RANGE_A, a));
	fill(a, S, 9);
	CHECK(!storage_sector_write(RANGE_B, a));
	CHECK(!storage_sector_write(0x7000u, a));		// the second sector of the range across a sector end
	CHECK(refused() - r0 == 4);
	CHECK(kept_touches() == t0 && touches(JOURNAL) + touches(J1) == j0);
	CHECK(memcmp(keep, &sim_flash_data[RANGE_A], S) == 0);
	// A free sector between two kept ones is written as ever
	CHECK(storage_sector_write(FREE, a) && memcmp(a, &sim_flash_data[FREE], S) == 0);
	CHECK(storage_sector_write(FREE_HI, a) && memcmp(a, &sim_flash_data[FREE_HI], S) == 0);
	CHECK(kept_touches() == t0 && sim_flash_violations() == 0);
}

static void test_write(void)
{
	static uint8_t a[3u * S], before[0x9000u];

	fresh(&with_factory);
	CHECK(boot(&with_factory));
	memcpy(before, sim_flash_data, sizeof(before));
	uint32_t t0 = kept_touches(), j0 = touches(JOURNAL) + touches(J1), r0 = refused();
	fill(a, sizeof(a), 3);
	// A record in a kept sector outside the range's bytes, in them, across into a free sector: refused whole, before
	// J0 or the free sector is touched (no rewrite that carries the maker's bytes over exists yet)
	CHECK(!storage_write(RANGE_A + 0x400u, a, 16));
	CHECK(!storage_write(RANGE_A + 0x100u, a, 16));
	CHECK(!storage_write(FREE - 0x10u, a, 0x20u));
	CHECK(!storage_write(RANGE_B - 0x10u, a, 0x20u));
	CHECK(!storage_write(RANGE_A + 0x400u, NULL, 16));			// an erase of a range
	CHECK(!storage_write(0, a, sizeof(a)));					// the sector path
	CHECK(!storage_write(0, NULL, sizeof(a)));
	CHECK(refused() - r0 == 7);
	CHECK(kept_touches() == t0 && touches(JOURNAL) + touches(J1) == j0 && touches(FREE) == 0 && touches(0) == 0);
	CHECK(memcmp(before, sim_flash_data, sizeof(before)) == 0);
	// What the flash holds already: success, nothing written
	CHECK(storage_write(RANGE_A + 0x100u, &before[RANGE_A + 0x100u], 16));
	CHECK(storage_write(FREE - 0x10u, &before[FREE - 0x10u], 0x20u));
	CHECK(storage_write(0, before, 3u * S));
	memset(&sim_flash_data[RANGE_A + 0x400u], 0xFF, 16);
	CHECK(storage_write(RANGE_A + 0x400u, NULL, 16));
	CHECK(refused() - r0 == 7 && touches(JOURNAL) + touches(J1) == j0);
	// Free sectors: the patch path and the sector path as ever
	CHECK(storage_write(FREE + 0x10u, a, 16) && memcmp(&sim_flash_data[FREE + 0x10u], a, 16) == 0);
	CHECK(storage_write(FREE_HI + S - 0x10u, a, 0x20u) && memcmp(&sim_flash_data[FREE_HI + S - 0x10u], a, 0x20u) == 0);
	CHECK(storage_write(FREE_HI, a, 2u * S) && memcmp(&sim_flash_data[FREE_HI], a, 2u * S) == 0);
	CHECK(storage_write(FREE_HI, NULL, 2u * S) && sim_flash_data[FREE_HI + S + 5u] == 0xFF);
	CHECK(kept_touches() == t0 && sim_flash_violations() == 0);
}

// Journal entries that a firmware without the list (an earlier one) left pending for a kept sector: nothing reaches
// the sector, the refusal is where the core erases and programs, the entry stays pending
static void test_replay(void)
{
	uint8_t a[S], keep[S];

	fresh(&with_factory);
	memcpy(keep, &sim_flash_data[RANGE_A], S);
	fill(a, S, 41);
	pending_sector(RANGE_A, a);
	uint32_t t0 = kept_touches(), r0 = refused(), e0 = errors();
	CHECK(boot(&with_factory));
	CHECK(boot(&with_factory));
	CHECK(refused() - r0 == 2 && errors() - e0 == 2);
	CHECK(sim_flash_data[JOURNAL + 16] == 0xFF);		// the done byte: still pending
	CHECK(kept_touches() == t0 && memcmp(keep, &sim_flash_data[RANGE_A], S) == 0);
	// The same entry under the earlier firmware's map is finished: it was a real pending one
	CHECK(boot(&plain) && memcmp(a, &sim_flash_data[RANGE_A], S) == 0);

	// A patch entry across a free and a kept sector: the kept one is never erased
	fresh(&with_factory);
	uint8_t p[0x20];
	fill(p, sizeof(p), 55);
	memcpy(keep, &sim_flash_data[RANGE_B], S);
	pending_patch(RANGE_B - 0x10u, p, sizeof(p));
	t0 = kept_touches();
	r0 = refused();
	CHECK(boot(&with_factory));
	CHECK(refused() - r0 == 1 && sim_flash_data[JOURNAL + 20] == 0xFF);	// "all done": still pending
	CHECK(kept_touches() == t0 && memcmp(keep, &sim_flash_data[RANGE_B], S) == 0);
	CHECK(boot(&plain) && memcmp(p, &sim_flash_data[RANGE_B - 0x10u], sizeof(p)) == 0);

	// A power cut inside those replays, then another start: the kept sectors never erased or
	// programmed, their bytes as they were, the region ours. The sector entry writes nothing at all (no unit to cut);
	// the patch entry finishes its free sector and stages the kept one in J1 before the refusal
	for (unsigned kind = 0; kind < 2; kind++) {
		unsigned cuts = 0;
		// Every unit near each sector's worth of units (where one operation ends and the next begins), every 31st
		// between
		for (uint32_t n = 1; n < 20000u; n += (n % S < 8u || n % S > S - 8u) ? 1u : 31u) {
			fresh(&with_factory);
			uint32_t target = kind == 0 ? RANGE_A : RANGE_B - 0x10u, kept_at = kind == 0 ? RANGE_A : RANGE_B;
			memcpy(keep, &sim_flash_data[kept_at], S);
			if (kind == 0) {
				pending_sector(target, a);
			} else {
				pending_patch(target, p, sizeof(p));
			}
			t0 = kept_touches();
			sim_flash.map = &with_factory;
			sim_flash_power_on();
			sim_flash_cut_after(n);
			(void)storage_init();
			bool cut = sim_flash_is_cut();
			bool ok = boot(&with_factory) && storage_region() == STORAGE_REGION_OURS;
			ok = ok && kept_touches() == t0 && memcmp(keep, &sim_flash_data[kept_at], S) == 0;
			ok = ok && sim_flash_violations() == 0;
			if (!ok) {
				printf("FAIL %s:%d: replay %u cut after %u units\n", __FILE__, __LINE__, kind, (unsigned)n);
				failures++;
			}
			if (!cut) {
				break;
			}
			cuts++;
		}
		CHECK(kind == 0 ? cuts == 0 : cuts > 400u);
	}

	// A patch entry in a free sector is finished as ever
	fresh(&with_factory);
	pending_patch(FREE_HI + 0x40u, p, sizeof(p));
	t0 = kept_touches();
	CHECK(boot(&with_factory) && memcmp(p, &sim_flash_data[FREE_HI + 0x40u], sizeof(p)) == 0);
	CHECK(sim_flash_data[JOURNAL + 20] == 0 && kept_touches() == t0 && sim_flash_violations() == 0);
}

// A region of another firmware's, then claimed: the claim and the writes after it never reach a kept sector
static void test_claim(void)
{
	uint8_t a[S];

	fresh(&with_factory);
	memset(&sim_flash_data[RING + 0x300u], 0x42, 64);	// another firmware's bytes in the ring
	fill(a, S, 17);
	uint32_t t0 = kept_touches();
	CHECK(!boot(&with_factory) && storage_region() == STORAGE_REGION_FOREIGN);
	CHECK(!storage_write(RANGE_A, &sim_flash_data[RANGE_A], 16) && !storage_sector_write(FREE, a));
	CHECK(storage_debug_read_allowed(RANGE_A, RANGE_A_SIZE));
	CHECK(storage_claim_request() && storage_claim() && storage_ready());
	CHECK(boot(&with_factory) && storage_region() == STORAGE_REGION_OURS);
	CHECK(storage_sector_write(FREE, a) && !storage_sector_write(RANGE_A, a));
	CHECK(kept_touches() == t0 && sim_flash_violations() == 0);
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
	test_replay();
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
