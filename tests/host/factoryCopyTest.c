// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The factory copy (core/storage/factory_copy.c, docs/spec/settings-store.md 4.2) on the simulated NOR flash, with
// core/storage built as a board that keeps factory bytes builds it (-DSTORAGE_FACTORY_KEEP=1): the record's layout and
// every state of its parse (a record for another range among them), the window's states, the source order row by row
// with an invented checker, the first-start write (only in a region of ours, only over a blank sector, once, a power
// cut at every flash operation of it, the shared buffer taken, a flash failure), a configuration that doesn't fit the
// map, and STORAGE op 3's reply (BUSY before and during the load). Every table and every byte here is invented.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "storage/factory_copy.h"
#include "support/sha256.h"
#include "debuglink/dlog.h"
#include "sim/sim_flash.h"

#if STORAGE_FACTORY_KEEP != 1 || !CONFIG_FACTORY_COPY
#error "this test is the factory copy over core/storage with -DSTORAGE_FACTORY_KEEP=1"
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
#define S		STORAGE_SECTOR
#define RING		0x11000u
#define JOURNAL		0x19000u
#define WIN		0x1000u		// factory range 0, inside a sector of the open-firmware image: the one copied
#define N		0x200u
#define MAKER		0x1E000u	// factory range 1: two whole sectors of the maker's records
#define REC		0x1B000u	// the board's record sector
#define RLEN		FACTORY_COPY_REC_LEN(N)

static const hal_flash_map_t map = {
	.settings_addr = RING, .settings_size = 0x8000u, .journal_addr = JOURNAL,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x30000u,
	.staging_addr = 0x30000u, .staging_size = 0x4000u,
	.factory = { { WIN, N }, { MAKER, 2u * S } },
};
// A list storage_init refuses (not ascending): the region none
static const hal_flash_map_t refused_list = {
	.settings_addr = RING, .settings_size = 0x8000u, .journal_addr = JOURNAL,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x30000u,
	.factory = { { WIN, N }, { 0x800u, 0x10u } },
};

// ---- The invented checker: bytes 0-1 0xC5 0x3A (why 1), the last byte the sum of the others (why 2) ----

static unsigned checks;
static bool busy_seen;		// factory_copy_info answered BUSY from inside the load
static bool probe;		// ask factory_copy_info from the checker

static uint8_t sum8(const uint8_t *b, uint32_t n)
{
	uint8_t s = 0;
	for (uint32_t i = 0; i < n; i++) {
		s = (uint8_t)(s + b[i]);
	}
	return s;
}

static bool check(const uint8_t *b, uint32_t n, uint8_t *why, uint16_t *off)
{
	checks++;
	if (probe) {
		uint8_t o[FACTORY_COPY_INFO_LEN];
		size_t len = 0;
		busy_seen = !factory_copy_info(board.factory_copy, o, &len);
	}
	if (b[0] != 0xC5 || b[1] != 0x3A) {
		*why = 1;
		*off = b[0] != 0xC5 ? 0 : 1;
		return false;
	}
	if (b[n - 1] != sum8(b, n - 1)) {
		*why = 2;
		*off = (uint16_t)(n - 1);
		return false;
	}
	return true;
}

static uint8_t table[N];
static factory_copy_cfg_t cfg = { .range = 0, .record_addr = REC, .check = check, .table = table };

const board_t board = { .name = "test", .flash = &sim_flash, .power_count = 5, .factory_copy = &cfg };

// A table the checker passes, its bytes from seed
static void good(uint8_t *b, uint8_t seed)
{
	for (uint32_t i = 0; i < N; i++) {
		b[i] = (uint8_t)(seed + i * 13u + (i >> 5));
	}
	b[0] = 0xC5;
	b[1] = 0x3A;
	b[N - 1] = sum8(b, N - 1);
}

static void put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The record as the spec lays it out, built here field by field (not by the code under test)
static void spec_record(uint8_t *r, uint32_t addr, const uint8_t *bytes, uint32_t n)
{
	sha256_t s;
	memset(r, 0xFF, FACTORY_COPY_REC_LEN(n));
	r[0] = 0x52;
	r[1] = 0x46;
	r[2] = 0x46;
	r[3] = 0x43;
	put16(r + 4, 1);
	put16(r + 6, n);
	put32(r + 8, addr);
	put32(r + 12, storage_crc32(0, bytes, n));
	sha256_init(&s);
	sha256_update(&s, bytes, n);
	sha256_final(&s, r + 16);
	memcpy(r + 64, bytes, n);
	put32(r + 64 + n, storage_crc32(0, r, 64 + n));
}

// The record's closing CRC again, after a test changed a field
static void reseal(uint8_t *r)
{
	put32(r + 64 + N, storage_crc32(0, r, 64 + N));
}

// A fresh chip: the window holds w (NULL: blank), the record sector r (NULL: blank), the maker's records in range 1
static void fresh(const uint8_t *w, const uint8_t *r)
{
	sim_flash_create(FLASH_SIZE, &map);
	memset(&sim_flash_data[MAKER], 0x6B, 2u * S);
	if (w != NULL) {
		memcpy(&sim_flash_data[WIN], w, N);
	}
	if (r != NULL) {
		memcpy(&sim_flash_data[REC], r, RLEN);
	}
	cfg.range = 0;
	cfg.record_addr = REC;
	cfg.check = check;
	cfg.table = table;
	log_lines = 0;
}

// Another radio's start on the same chip: storage_init, then the load as app.c runs it
static uint32_t load_units;	// erases and programmed bytes of the load
static factory_copy_state_t start(void)
{
	sim_flash.map = &map;
	sim_flash_power_on();
	storage_init();
	memset(table, 0x5A, sizeof(table));
	uint32_t u = sim_flash_units();
	factory_copy_load(&cfg);
	load_units = sim_flash_units() - u;
	factory_copy_state_t s;
	factory_copy_get(&s);
	return s;
}

static bool table_is(const uint8_t *b)
{
	return memcmp(table, b, N) == 0;
}

static bool table_blank(void)
{
	for (uint32_t i = 0; i < N; i++) {
		if (table[i] != 0xFF) {
			return false;
		}
	}
	return true;
}

static bool window_is(const uint8_t *b)
{
	return memcmp(&sim_flash_data[WIN], b, N) == 0;
}

// The record's whole sector as the first-start write leaves it: the record for b, then 0xFF
static bool record_sector_is(const uint8_t *b)
{
	static uint8_t want[S];
	memset(want, 0xFF, S);
	spec_record(want, WIN, b, N);
	return memcmp(&sim_flash_data[REC], want, S) == 0;
}

static bool sector_blank(uint32_t a)
{
	for (uint32_t i = 0; i < S; i++) {
		if (sim_flash_data[a + i] != 0xFF) {
			return false;
		}
	}
	return true;
}

// ---- STORAGE op 3 before any load ----

static void test_busy_before_load(void)
{
	uint8_t o[FACTORY_COPY_INFO_LEN];
	size_t len = 0;
	CHECK(!factory_copy_loaded());
	CHECK(!factory_copy_info(&cfg, o, &len));
	factory_copy_state_t s;
	memset(&s, 0x77, sizeof(s));
	factory_copy_get(&s);
	CHECK(!s.loaded && s.source == FACTORY_COPY_SRC_NONE);
	// A board without a factory copy: present 0, at any time
	CHECK(factory_copy_info(NULL, o, &len) && len == 1 && o[0] == 0);
}

// ---- The record ----

static void test_record_layout(void)
{
	static uint8_t b[N], want[RLEN], got[RLEN];
	good(b, 3);
	sim_flash_create(FLASH_SIZE, &map);
	spec_record(want, WIN, b, N);
	memset(got, 0x11, sizeof(got));
	factory_copy_record_build(got, WIN, b, N);
	CHECK(memcmp(got, want, RLEN) == 0);
	CHECK(get32(got) == FACTORY_COPY_MAGIC && got[48] == 0xFF && got[63] == 0xFF);
	// Built in place: the bytes already at offset 64
	memset(got, 0x22, sizeof(got));
	memcpy(got + 64, b, N);
	factory_copy_record_build(got, WIN, got + 64, N);
	CHECK(memcmp(got, want, RLEN) == 0);
}

static void test_record_parse(void)
{
	static uint8_t b[N], r[RLEN], other[N];
	fresh(NULL, NULL);
	good(b, 9);
	spec_record(r, WIN, b, N);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_OK);

	memset(r, 0xFF, RLEN);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_BLANK);
	memset(r, 0x00, RLEN);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_OTHER);
	// One byte of the magic
	spec_record(r, WIN, b, N);
	r[3] ^= 0x01;
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_OTHER);
	// A later version: newer, whatever follows; version 0: bad
	spec_record(r, WIN, b, N);
	put16(r + 4, 2);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_NEWER);
	put16(r + 6, 0x7777);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_NEWER);
	spec_record(r, WIN, b, N);
	put16(r + 4, 0);
	reseal(r);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_BAD);
	// Every field and check, each alone (the closing CRC sealed again where it isn't the field)
	spec_record(r, WIN, b, N);
	put16(r + 6, N - 1);
	reseal(r);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_BAD);
	spec_record(r, WIN, b, N);
	r[50] = 0x00;
	reseal(r);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_BAD);
	spec_record(r, WIN, b, N);
	r[64 + N] ^= 0x80;
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_BAD);
	spec_record(r, WIN, b, N);
	r[12] ^= 0x01;
	reseal(r);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_BAD);
	spec_record(r, WIN, b, N);
	r[16 + 31] ^= 0x01;
	reseal(r);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_BAD);
	// A byte of the table, with the data CRC and the closing CRC both sealed again: the sha256 still says no
	spec_record(r, WIN, b, N);
	r[64 + 100] ^= 0x01;
	put32(r + 12, storage_crc32(0, r + 64, N));
	reseal(r);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_BAD);
	// Every field right, but the bytes fail the board's check
	memcpy(other, b, N);
	other[N - 1] ^= 0x01;
	spec_record(r, WIN, other, N);
	unsigned c0 = checks;
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_BAD && checks == c0 + 1u);
	// A record for another range: another address, or another range's length
	spec_record(r, MAKER, b, N);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_BAD);
	spec_record(r, WIN + 0x10u, b, N);
	CHECK(factory_copy_record_parse(&cfg, r) == FACTORY_COPY_REC_BAD);
	static uint8_t big[FACTORY_COPY_REC_LEN(N + 0x10u)], bb[N + 0x10u];
	memset(bb, 0x44, sizeof(bb));
	memcpy(bb, b, N);
	spec_record(big, WIN, bb, N + 0x10u);
	CHECK(factory_copy_record_parse(&cfg, big) == FACTORY_COPY_REC_BAD);
}

// ---- The window and the record as the load reads them ----

static void test_window_states(void)
{
	static uint8_t b[N];
	factory_copy_state_t s;

	fresh(NULL, NULL);
	s = start();
	CHECK(s.loaded && s.win == FACTORY_COPY_WIN_BLANK && s.why == 1 && s.off == 0 && s.source == FACTORY_COPY_SRC_NONE);
	CHECK(s.win_crc == storage_crc32(0, &sim_flash_data[WIN], N) && s.rec == FACTORY_COPY_REC_BLANK && table_blank());
	CHECK(s.wrote == FACTORY_COPY_WROTE_NO && load_units == 0);

	memset(b, 0, N);
	fresh(b, NULL);
	s = start();
	CHECK(s.win == FACTORY_COPY_WIN_ZERO && s.why == 1 && s.source == FACTORY_COPY_SRC_NONE && table_blank());

	good(b, 1);
	b[N - 1] ^= 0x01;
	fresh(b, NULL);
	s = start();
	CHECK(s.win == FACTORY_COPY_WIN_REFUSED && s.why == 2 && s.off == N - 1 && s.win_crc == storage_crc32(0, b, N));
	CHECK(s.source == FACTORY_COPY_SRC_NONE && s.rec == FACTORY_COPY_REC_BLANK && load_units == 0 && table_blank());
	b[0] = 0x00;
	fresh(b, NULL);
	s = start();
	CHECK(s.win == FACTORY_COPY_WIN_REFUSED && s.why == 1 && s.off == 0);

	// Unread: a read that fails, two that differ (the first or the second read): never checked, CRC 0
	good(b, 1);
	fresh(b, NULL);
	sim_flash_bad_read(WIN + 0x40u, 1);
	unsigned c0 = checks;
	s = start();
	sim_flash_bad_read(0, 0);
	CHECK(s.win == FACTORY_COPY_WIN_UNREAD && s.why == FACTORY_COPY_WHY_READ && s.win_crc == 0 && checks == c0);
	CHECK(s.source == FACTORY_COPY_SRC_NONE && s.rec == FACTORY_COPY_REC_BLANK && load_units == 0 && table_blank());
	for (uint32_t nth = 1; nth <= 2; nth++) {
		fresh(b, NULL);
		sim_flash.map = &map;
		sim_flash_power_on();
		storage_init();
		sim_flash_misread(WIN + 0x123u, nth);
		factory_copy_load(&cfg);
		factory_copy_get(&s);
		CHECK(s.win == FACTORY_COPY_WIN_UNREAD && s.why == FACTORY_COPY_WHY_UNSTABLE && s.win_crc == 0);
		CHECK(s.source == FACTORY_COPY_SRC_NONE && sector_blank(REC) && table_blank());
	}
	sim_flash_misread(0, 0);
}

static void test_record_states(void)
{
	static uint8_t b[N], r[RLEN];
	factory_copy_state_t s;
	uint8_t bad_b[N];

	good(b, 2);
	memcpy(bad_b, b, N);
	bad_b[5] ^= 0x10;		// fails the window's check

	// A record whose header is blank but whose sector isn't: other, at either end of the sector
	const uint32_t junk_at[] = { RLEN, RLEN + 1u, 0x800u, S - 1u };
	for (unsigned i = 0; i < sizeof(junk_at) / sizeof(junk_at[0]); i++) {
		fresh(bad_b, NULL);
		sim_flash_data[REC + junk_at[i]] = 0x00;
		s = start();
		CHECK(s.rec == FACTORY_COPY_REC_OTHER && s.rec_crc == 0 && s.source == FACTORY_COPY_SRC_NONE);
	}
	// Bad: the CRC of its bytes shown all the same
	spec_record(r, WIN, b, N);
	r[64 + N] ^= 0x01;
	fresh(bad_b, r);
	s = start();
	CHECK(s.rec == FACTORY_COPY_REC_BAD && s.rec_crc == storage_crc32(0, b, N) && s.source == FACTORY_COPY_SRC_NONE);
	// Newer
	spec_record(r, WIN, b, N);
	put16(r + 4, 9);
	fresh(bad_b, r);
	s = start();
	CHECK(s.rec == FACTORY_COPY_REC_NEWER && s.rec_crc == 0 && s.source == FACTORY_COPY_SRC_NONE);
	// Unread: a failed read, two reads that differ, the rest of the sector unreadable
	spec_record(r, WIN, b, N);
	fresh(bad_b, r);
	sim_flash_bad_read(REC + 0x80u, 1);
	s = start();
	sim_flash_bad_read(0, 0);
	CHECK(s.rec == FACTORY_COPY_REC_UNREAD && s.source == FACTORY_COPY_SRC_NONE && table_blank());
	fresh(bad_b, r);
	sim_flash.map = &map;
	sim_flash_power_on();
	storage_init();
	sim_flash_misread(REC + 0x90u, 2);
	factory_copy_load(&cfg);
	sim_flash_misread(0, 0);
	factory_copy_get(&s);
	CHECK(s.rec == FACTORY_COPY_REC_UNREAD && s.source == FACTORY_COPY_SRC_NONE);
	fresh(bad_b, NULL);
	sim_flash_bad_read(REC + 0xF00u, 1);
	s = start();
	sim_flash_bad_read(0, 0);
	CHECK(s.rec == FACTORY_COPY_REC_UNREAD);
}

// ---- The source order (settings-store.md 4.2), row by row ----

static void test_source_order(void)
{
	static uint8_t w[N], other[N], r[RLEN], refused[N];
	factory_copy_state_t s;

	good(w, 4);
	good(other, 40);
	memcpy(refused, w, N);
	refused[N - 1] ^= 0x01;

	// The window passes, the record blank: the window, and the record written now (below in detail)
	fresh(w, NULL);
	s = start();
	CHECK(s.source == FACTORY_COPY_SRC_WINDOW && s.win == FACTORY_COPY_WIN_OK && s.why == 0 && table_is(w));
	CHECK(s.wrote == FACTORY_COPY_WROTE_YES && s.rec == FACTORY_COPY_REC_OK && s.rec_crc == s.win_crc);

	// The window passes, the record ok with the same bytes: the window, nothing written
	spec_record(r, WIN, w, N);
	fresh(w, r);
	s = start();
	CHECK(s.source == FACTORY_COPY_SRC_WINDOW && s.rec == FACTORY_COPY_REC_OK && s.rec_crc == s.win_crc && table_is(w));
	CHECK(s.wrote == FACTORY_COPY_WROTE_NO && load_units == 0);

	// Both pass and differ: none, why conflict, both CRCs shown, nothing written, one log line
	spec_record(r, WIN, other, N);
	fresh(w, r);
	s = start();
	CHECK(s.source == FACTORY_COPY_SRC_NONE && s.win == FACTORY_COPY_WIN_OK && s.why == FACTORY_COPY_WHY_CONFLICT);
	CHECK(s.win_crc == storage_crc32(0, w, N) && s.rec_crc == storage_crc32(0, other, N) && s.rec == FACTORY_COPY_REC_OK);
	CHECK(table_blank() && load_units == 0 && window_is(w) && s.wrote == FACTORY_COPY_WROTE_NO);
	CHECK(log_lines == 1 && strstr(log_last, "differ") != NULL);

	// The window passes, the record bad, other or newer: the window, the record's state shown, nothing written
	for (unsigned k = 0; k < 3; k++) {
		spec_record(r, WIN, w, N);
		if (k == 0) {
			r[20] ^= 0x01;			// the sha256
			reseal(r);
		} else if (k == 1) {
			memset(r, 0x33, RLEN);
		} else {
			put16(r + 4, 2);
		}
		fresh(w, r);
		s = start();
		static const uint8_t want[] = { FACTORY_COPY_REC_BAD, FACTORY_COPY_REC_OTHER, FACTORY_COPY_REC_NEWER };
		CHECK(s.source == FACTORY_COPY_SRC_WINDOW && s.rec == want[k] && table_is(w));
		CHECK(s.wrote == FACTORY_COPY_WROTE_NO && load_units == 0 && memcmp(&sim_flash_data[REC], r, RLEN) == 0);
	}

	// The window refused, blank or unread, the record ok: the record (the window's refusal beside it); the window is
	// not written
	const uint8_t *wins[] = { refused, NULL, w };
	for (unsigned k = 0; k < 3; k++) {
		spec_record(r, WIN, other, N);
		fresh(wins[k], r);
		if (k == 2) {
			sim_flash_bad_read(WIN, 1);
		}
		s = start();
		sim_flash_bad_read(0, 0);
		CHECK(s.source == FACTORY_COPY_SRC_RECORD && s.rec == FACTORY_COPY_REC_OK && table_is(other));
		CHECK(s.win == (k == 0 ? FACTORY_COPY_WIN_REFUSED : (k == 1 ? FACTORY_COPY_WIN_BLANK : FACTORY_COPY_WIN_UNREAD)));
		CHECK(s.rec_crc == storage_crc32(0, other, N) && load_units == 0 && s.wrote == FACTORY_COPY_WROTE_NO);
		CHECK(k == 1 ? sector_blank(WIN) : window_is(wins[k]));
	}

	// The window refused, the record anything else: none, nothing written (a blank record sector included)
	for (unsigned k = 0; k < 4; k++) {
		spec_record(r, WIN, other, N);
		if (k == 1) {
			r[64] ^= 0x01;
		} else if (k == 2) {
			memset(r, 0x00, RLEN);
		} else if (k == 3) {
			put16(r + 4, 3);
		}
		fresh(refused, k == 0 ? NULL : r);
		s = start();
		static const uint8_t want[] = { FACTORY_COPY_REC_BLANK, FACTORY_COPY_REC_BAD, FACTORY_COPY_REC_OTHER,
						FACTORY_COPY_REC_NEWER };
		CHECK(s.source == FACTORY_COPY_SRC_NONE && s.rec == want[k] && s.why == 2 && table_blank());
		CHECK(load_units == 0 && s.wrote == FACTORY_COPY_WROTE_NO && window_is(refused));
	}
	// The window passes, the record unread: the window, nothing written
	spec_record(r, WIN, other, N);
	fresh(w, r);
	sim_flash_bad_read(REC, 1);
	s = start();
	sim_flash_bad_read(0, 0);
	CHECK(s.rec == FACTORY_COPY_REC_UNREAD && s.win == FACTORY_COPY_WIN_OK && s.source == FACTORY_COPY_SRC_WINDOW);
	CHECK(s.wrote == FACTORY_COPY_WROTE_NO && load_units == 0);
}

// ---- The first-start write ----

static void test_first_write(void)
{
	static uint8_t w[N];
	factory_copy_state_t s;
	good(w, 5);

	// Ours, the window passing, the record blank: written once, through the journal, read back; the window untouched
	fresh(w, NULL);
	uint32_t win_prog = sim_flash_programs(WIN), win_er = sim_flash_erases(WIN);
	s = start();
	CHECK(s.wrote == FACTORY_COPY_WROTE_YES && s.rec == FACTORY_COPY_REC_OK && record_sector_is(w));
	CHECK(sim_flash_erases(REC) == 1 && sim_flash_programs(WIN) == win_prog && sim_flash_erases(WIN) == win_er);
	CHECK(window_is(w) && sim_flash_violations() == 0 && log_lines == 1 && strstr(log_last, "written") != NULL);
	// Once: the next start finds it ok and writes nothing
	s = start();
	CHECK(s.wrote == FACTORY_COPY_WROTE_NO && s.rec == FACTORY_COPY_REC_OK && s.source == FACTORY_COPY_SRC_WINDOW);
	CHECK(load_units == 0 && sim_flash_erases(REC) == 1);

	// Never while the region is foreign (another firmware's bytes in the ring), nor none (a refused list): not one
	// erase or program by the load
	fresh(w, NULL);
	memset(&sim_flash_data[RING], 0x12, 0x100u);
	s = start();
	CHECK(storage_region() == STORAGE_REGION_FOREIGN && s.source == FACTORY_COPY_SRC_WINDOW && table_is(w));
	CHECK(s.wrote == FACTORY_COPY_WROTE_NO && s.rec == FACTORY_COPY_REC_BLANK && load_units == 0 && sector_blank(REC));
	// Claimed: the next start writes it
	CHECK(storage_claim());
	s = start();
	CHECK(storage_region() == STORAGE_REGION_OURS && s.wrote == FACTORY_COPY_WROTE_YES && record_sector_is(w));

	fresh(w, NULL);
	sim_flash.map = &refused_list;
	sim_flash_power_on();
	CHECK(!storage_init() && storage_region() == STORAGE_REGION_NONE);
	uint32_t u = sim_flash_units();
	factory_copy_load(&cfg);
	factory_copy_get(&s);
	CHECK(s.source == FACTORY_COPY_SRC_WINDOW && s.wrote == FACTORY_COPY_WROTE_NO && sim_flash_units() == u && sector_blank(REC));

	// Never over a sector that isn't blank
	fresh(w, NULL);
	sim_flash_data[REC + 0xFFFu] = 0xFE;
	s = start();
	CHECK(s.rec == FACTORY_COPY_REC_OTHER && s.wrote == FACTORY_COPY_WROTE_NO && load_units == 0);
	CHECK(sim_flash_data[REC + 0xFFFu] == 0xFE && sim_flash_erases(REC) == 0);

	// The shared buffer taken (a CPS session): both unread, nothing written, nothing read
	fresh(w, NULL);
	sim_flash.map = &map;
	sim_flash_power_on();
	storage_init();
	uint8_t *held = storage_buffer_acquire();
	CHECK(held != NULL);
	uint32_t reads = sim_flash_reads();
	memset(table, 0x5A, sizeof(table));
	factory_copy_load(&cfg);
	factory_copy_get(&s);
	storage_buffer_release();
	CHECK(s.loaded && s.win == FACTORY_COPY_WIN_UNREAD && s.why == FACTORY_COPY_WHY_BUFFER && s.rec == FACTORY_COPY_REC_UNREAD);
	CHECK(s.source == FACTORY_COPY_SRC_NONE && table_blank() && sim_flash_reads() == reads && sector_blank(REC));
	// The next start, with the buffer free: written
	s = start();
	CHECK(s.wrote == FACTORY_COPY_WROTE_YES && record_sector_is(w));

	// A program that fails on the record's sector: failed, shown; nothing tried again at this start; the journal's
	// replay finishes it at the next start (its commit point was reached), and the load then finds it ok
	fresh(w, NULL);
	sim_flash_bad_range(REC, S);
	s = start();
	sim_flash_bad_range(0, 0);
	CHECK(s.wrote == FACTORY_COPY_WROTE_FAILED && s.source == FACTORY_COPY_SRC_WINDOW && table_is(w));
	CHECK(log_lines == 1 && strstr(log_last, "failed") != NULL);
	s = start();
	CHECK(s.rec == FACTORY_COPY_REC_OK && s.wrote == FACTORY_COPY_WROTE_NO && record_sector_is(w));
	// A journal that fails before its commit point (J1 worn): the record sector untouched, written at the next start
	fresh(w, NULL);
	sim_flash_bad_range(JOURNAL + S, S);
	s = start();
	sim_flash_bad_range(0, 0);
	CHECK(s.wrote == FACTORY_COPY_WROTE_FAILED && s.rec == FACTORY_COPY_REC_BLANK && sector_blank(REC));
	s = start();
	CHECK(s.wrote == FACTORY_COPY_WROTE_YES && record_sector_is(w));

	// A marginal read in the record's sector during the load, whatever it hits (the load's reads, the journal's
	// checks, the read back): never "written" unless the read back found the record whole; a write shown as failed
	// is finished or done again at the next start
	unsigned caught = 0;
	for (uint32_t nth = 1; nth <= 8; nth++) {
		fresh(w, NULL);
		sim_flash.map = &map;
		sim_flash_power_on();
		storage_init();
		sim_flash_misread(REC + 0x100u, nth);
		factory_copy_load(&cfg);
		sim_flash_misread(0, 0);
		factory_copy_get(&s);
		// The load's own two reads differing: the record unread, nothing written at this start
		bool ok = s.wrote == FACTORY_COPY_WROTE_FAILED ||
			  (s.wrote == FACTORY_COPY_WROTE_NO && s.rec == FACTORY_COPY_REC_UNREAD && sector_blank(REC)) ||
			  (s.wrote == FACTORY_COPY_WROTE_YES && s.rec == FACTORY_COPY_REC_OK && record_sector_is(w));
		caught += s.wrote == FACTORY_COPY_WROTE_FAILED && record_sector_is(w);
		s = start();
		ok = ok && s.rec == FACTORY_COPY_REC_OK && record_sector_is(w) && s.source == FACTORY_COPY_SRC_WINDOW;
		if (!ok) {
			printf("FAIL %s:%d: misread %u of the record's sector\n", __FILE__, __LINE__, (unsigned)nth);
			failures++;
		}
	}
	CHECK(caught > 0);		// the read back itself saw it: written right, shown as failed
}

// The journal held at start (settings-store.md 4.1): a kept rewrite of the window's sector failed past its commit
// point and its replay at this start fails too, so J0 and J1 hold the only whole copy of that sector. The first-start
// write is refused, the journal untouched, the record sector blank; the next start with a working flash finishes the
// entry, then writes the record
static void test_held_journal(void)
{
	static uint8_t w[N], sec[S], j[2u * S];
	factory_copy_state_t s;
	good(w, 11);

	fresh(w, NULL);
	sim_flash.map = &map;
	sim_flash_power_on();
	CHECK(storage_init() && storage_ready());
	memset(sec, 0x00, S);				// every byte outside the window changes
	sim_flash_bad_range(WIN + 0x800u, 0x100u);	// the target's program fails after its erase, past the window
	CHECK(!storage_sector_write(WIN, sec));
	CHECK(window_is(w));				// programmed before the worn part
	memcpy(j, &sim_flash_data[JOURNAL], sizeof(j));

	s = start();					// the worn part still there: the replay fails again
	CHECK(s.source == FACTORY_COPY_SRC_WINDOW && s.wrote == FACTORY_COPY_WROTE_FAILED && sector_blank(REC));
	CHECK(memcmp(&sim_flash_data[JOURNAL], j, sizeof(j)) == 0 && sim_flash_violations() == 0);

	sim_flash_bad_range(0, 0);
	s = start();
	CHECK(s.source == FACTORY_COPY_SRC_WINDOW && s.wrote == FACTORY_COPY_WROTE_YES && record_sector_is(w));
	CHECK(window_is(w) && sim_flash_data[WIN + 0x800u] == 0x00 && sim_flash_data[WIN + S - 1u] == 0x00);
}

// A power cut at the first, second, middle and last unit of every erase and program of the first-start write, then a
// start: the record whole and ok (finished by the journal's replay, or written by that start), never anything else;
// the window untouched, the maker's records too
static void test_cut_write(void)
{
	static uint8_t w[N];
	static uint32_t pt[2048];
	factory_copy_state_t s;
	good(w, 6);

	fresh(w, NULL);
	sim_flash.map = &map;
	sim_flash_power_on();
	storage_init();
	sim_flash_ops_begin();
	factory_copy_load(&cfg);
	uint32_t n = sim_flash_cut_points(pt, sizeof(pt) / sizeof(pt[0]));
	factory_copy_get(&s);
	// Seven operations: J1's erase and program, J0's erase and header, the sector's erase and program, the done byte
	CHECK(s.wrote == FACTORY_COPY_WROTE_YES && n >= 14u);
	unsigned replayed = 0, rewritten = 0;
	for (uint32_t i = 0; i < n; i++) {
		fresh(w, NULL);
		sim_flash.map = &map;
		sim_flash_power_on();
		storage_init();
		sim_flash_cut_after(pt[i]);
		factory_copy_load(&cfg);
		factory_copy_get(&s);
		bool ok = s.wrote == FACTORY_COPY_WROTE_FAILED && s.source == FACTORY_COPY_SRC_WINDOW;
		s = start();
		ok = ok && storage_region() == STORAGE_REGION_OURS && s.rec == FACTORY_COPY_REC_OK;
		ok = ok && s.source == FACTORY_COPY_SRC_WINDOW && table_is(w) && record_sector_is(w) && window_is(w);
		ok = ok && sim_flash_data[MAKER] == 0x6B && sim_flash_data[MAKER + 2u * S - 1u] == 0x6B && sim_flash_violations() == 0;
		replayed += s.wrote == FACTORY_COPY_WROTE_NO;
		rewritten += s.wrote == FACTORY_COPY_WROTE_YES;
		// And a third start writes nothing
		s = start();
		ok = ok && s.wrote == FACTORY_COPY_WROTE_NO && load_units == 0;
		if (!ok) {
			printf("FAIL %s:%d: first-start write cut after %u units\n", __FILE__, __LINE__, (unsigned)pt[i]);
			failures++;
		}
	}
	// Both ends of the commit point were cut: some starts finished the entry, others wrote the record themselves
	CHECK(replayed > 0 && rewritten > 0);
}

// ---- A configuration that doesn't fit the map: not read, nothing written ----

static void test_config(void)
{
	static uint8_t w[N];
	factory_copy_state_t s;
	good(w, 7);

	const uint32_t recs[] = { REC + 0x100u, RING, JOURNAL + S, 0x2000u, 0x20000u, 0x30000u, WIN, MAKER + S, FLASH_SIZE };
	for (unsigned i = 0; i < sizeof(recs) / sizeof(recs[0]) + 4u; i++) {
		fresh(w, NULL);
		if (i < sizeof(recs) / sizeof(recs[0])) {
			cfg.record_addr = recs[i];
		} else if (i == sizeof(recs) / sizeof(recs[0])) {
			cfg.range = 1;			// larger than FACTORY_COPY_MAX
		} else if (i == sizeof(recs) / sizeof(recs[0]) + 1u) {
			cfg.range = 2;			// size 0: the end of the list
		} else if (i == sizeof(recs) / sizeof(recs[0]) + 2u) {
			cfg.range = HAL_FLASH_FACTORY_MAX;
		} else {
			cfg.check = NULL;
		}
		sim_flash.map = &map;
		sim_flash_power_on();
		storage_init();
		uint32_t reads = sim_flash_reads(), u = sim_flash_units();
		log_lines = 0;
		factory_copy_load(&cfg);
		factory_copy_get(&s);
		bool ok = s.loaded && s.win == FACTORY_COPY_WIN_UNREAD && s.why == FACTORY_COPY_WHY_CONFIG;
		ok = ok && s.rec == FACTORY_COPY_REC_UNREAD && s.source == FACTORY_COPY_SRC_NONE && log_lines == 1;
		ok = ok && sim_flash_reads() == reads && sim_flash_units() == u;
		if (!ok) {
			printf("FAIL %s:%d: configuration %u taken\n", __FILE__, __LINE__, i);
			failures++;
		}
	}
	// No table: refused before anything, the table pointer never written through
	fresh(w, NULL);
	cfg.table = NULL;
	s = start();
	CHECK(s.why == FACTORY_COPY_WHY_CONFIG && s.source == FACTORY_COPY_SRC_NONE && sector_blank(REC));
	fresh(NULL, NULL);
}

// ---- STORAGE op 3 FACTORY_INFO ----

static void test_info(void)
{
	static uint8_t w[N], other[N], r[RLEN];
	uint8_t o[FACTORY_COPY_INFO_LEN + 8];
	size_t len = 0;
	factory_copy_state_t s;

	// BUSY while the load runs (asked from inside it), then the reply
	good(w, 8);
	fresh(w, NULL);
	probe = true;
	busy_seen = false;
	s = start();
	probe = false;
	CHECK(busy_seen && s.wrote == FACTORY_COPY_WROTE_YES);
	memset(o, 0xEE, sizeof(o));
	CHECK(factory_copy_info(&cfg, o, &len) && len == FACTORY_COPY_INFO_LEN && o[FACTORY_COPY_INFO_LEN] == 0xEE);
	CHECK(o[0] == 1 && get32(o + 1) == WIN && o[5] == (N & 0xFF) && o[6] == (N >> 8));
	CHECK(o[7] == FACTORY_COPY_SRC_WINDOW && o[8] == FACTORY_COPY_WIN_OK && o[9] == 0 && o[10] == 0 && o[11] == 0);
	CHECK(get32(o + 12) == storage_crc32(0, w, N) && o[16] == FACTORY_COPY_REC_OK && get32(o + 17) == storage_crc32(0, w, N));
	CHECK(o[21] == 0xFF && o[22] == 0 && o[23] == 0 && o[24] == FACTORY_COPY_WROTE_YES);

	// A refused window and a record of another table: the record in use, the window's reason and offset
	good(other, 41);
	memcpy(w, other, N);
	w[N - 1] ^= 0x01;
	spec_record(r, WIN, other, N);
	fresh(w, r);
	s = start();
	CHECK(factory_copy_info(&cfg, o, &len) && len == FACTORY_COPY_INFO_LEN);
	CHECK(o[7] == FACTORY_COPY_SRC_RECORD && o[8] == FACTORY_COPY_WIN_REFUSED && o[9] == 2);
	CHECK(o[10] == ((N - 1) & 0xFF) && o[11] == ((N - 1) >> 8) && get32(o + 12) == storage_crc32(0, w, N));
	CHECK(o[16] == FACTORY_COPY_REC_OK && get32(o + 17) == storage_crc32(0, other, N) && o[24] == FACTORY_COPY_WROTE_NO);

	// The conflict, and an unread window
	good(w, 9);
	fresh(w, r);
	s = start();
	CHECK(factory_copy_info(&cfg, o, &len) && o[7] == FACTORY_COPY_SRC_NONE && o[8] == FACTORY_COPY_WIN_OK);
	CHECK(o[9] == FACTORY_COPY_WHY_CONFLICT);
	fresh(w, NULL);
	sim_flash_bad_read(WIN, 1);
	s = start();
	sim_flash_bad_read(0, 0);
	CHECK(factory_copy_info(&cfg, o, &len) && o[8] == FACTORY_COPY_WIN_UNREAD && o[9] == FACTORY_COPY_WHY_READ);
	CHECK(get32(o + 12) == 0 && o[16] == FACTORY_COPY_REC_BLANK && get32(o + 17) == 0);

	// The names the view shows
	CHECK(strcmp(factory_copy_src_name(FACTORY_COPY_SRC_RECORD), "record") == 0);
	CHECK(strcmp(factory_copy_win_name(FACTORY_COPY_WIN_REFUSED), "refused") == 0);
	CHECK(strcmp(factory_copy_rec_name(FACTORY_COPY_REC_NEWER), "newer") == 0);
	CHECK(strcmp(factory_copy_why_name(FACTORY_COPY_WHY_CONFLICT), "conflict") == 0);
	CHECK(strcmp(factory_copy_why_name(FACTORY_COPY_WHY_CONFIG), "config") == 0);
	CHECK(strcmp(factory_copy_why_name(0), "none") == 0 && factory_copy_why_name(2) == NULL);
	CHECK(factory_copy_why_name(0xFF) == NULL && strcmp(factory_copy_rec_name(0x40), "?") == 0);
}

int main(void)
{
	test_busy_before_load();
	test_record_layout();
	test_record_parse();
	test_window_states();
	test_record_states();
	test_source_order();
	test_first_write();
	test_held_journal();
	test_cut_write();
	test_config();
	test_info();
	sim_flash_free();
	printf("# factoryCopyTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
