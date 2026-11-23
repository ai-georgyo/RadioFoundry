// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The sector journal's flash operations, pinned (docs/spec/settings-store.md 4, 6). A scripted run of core/storage on
// the simulated NOR flash with a map that lists no factory ranges: sector writes and their skip, patches inside a
// sector and across a sector end, the sector path, an erase of a range, the native area's erase, a foreign region and
// its claim, storage_init on every shape of J0 and J1 the journal's format has, and a power cut at the start, just
// after the start, in the middle and at the end of every program and every erase of each of them, each followed by
// a start and one more write. Every flash operation (kind, address, length, its bytes, its result), every call's
// result, the stats and the flash after each step go into one 64-bit FNV-1a hash, compared with the golden value
// below.
//
// The golden value was produced once from the storage.c of commit 51dedf59:
//   git show 51dedf59:core/storage/storage.c > $TMPDIR/storage_before.c
//   make -C tests/host -B storageJournalLogTest JOURNAL_LOG_STORAGE=$TMPDIR/storage_before.c
//   tests/host/storageJournalLogTest		(prints the hash)
// The Makefile builds this test with today's storage.c. A change to the journal that moves any flash operation fails
// here; one that means to has to say so by changing the golden value, with the reason in its commit.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "debuglink/dlog.h"
#include "sim/sim_flash.h"

#define GOLDEN		0x74135635351b583cull

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
#if CONFIG_DEBUGLINK
void dlog(const char *fmt, ...) { (void)fmt; }
#endif

#define FLASH_SIZE	0x40000u
#define RING		0x11000u
#define RING_SIZE	0x8000u
#define JOURNAL		0x19000u
#define J1		(JOURNAL + STORAGE_SECTOR)
#define CODEPLUG	0x20000u
#define NATIVE		0x30000u
#define NATIVE_SIZE	0x4000u
#define S		STORAGE_SECTOR

static const hal_flash_map_t map = {
	.settings_addr = RING, .settings_size = RING_SIZE, .journal_addr = JOURNAL,
	.openfw_eeprom = 0, .openfw_flash = CODEPLUG, .openfw_end = NATIVE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
	.native_addr = NATIVE, .native_size = NATIVE_SIZE,
};

const board_t board = { .name = "test", .flash = &sim_flash, .power_count = 5 };

// ---- The hash ----

static uint64_t hash = 0xCBF29CE484222325ull;

static void mix(const void *p, uint32_t len)
{
	const uint8_t *b = p;
	for (uint32_t i = 0; i < len; i++) {
		hash = (hash ^ b[i]) * 0x100000001B3ull;
	}
}

static void mix32(uint32_t v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	mix(b, 4);
}

// Program and erase operations seen, with the units used before each (where a cut can go)
#define OPS_MAX		256u
static uint32_t op_start[OPS_MAX], op_len[OPS_MAX];
static uint32_t ops;
static uint32_t ops_total;

static void on_op(char kind, uint32_t addr, uint32_t len, const void *data, bool ok)
{
	mix(&kind, 1);
	mix32(addr);
	mix32(len);
	mix32(ok);
	if (data != NULL) {
		mix(data, len);
	}
	ops_total++;
	if (kind != 'r' && ops < OPS_MAX) {
		// The units this operation used are known after it: its start is where the one before it ended
		op_len[ops] = kind == 'e' ? 1u : len;
		ops++;
	}
}

static void result(char tag, bool ok)
{
	mix(&tag, 1);
	mix32(ok);
}

// What the run went through, so that a script that stopped reaching a path fails here, not only in the hash
// (the stats count from the start of the run)
static uint32_t seen_replays, seen_writes, seen_foreign, seen_cuts, cut_points;

static void state(void)
{
	static storage_stats_t last;
	storage_stats_t s;
	storage_get_stats(&s);
	seen_replays += s.replays != last.replays;
	seen_writes += s.sector_writes != last.sector_writes;
	seen_foreign += storage_region() == STORAGE_REGION_FOREIGN;
	last = s;
	mix32(s.sector_writes);
	mix32(s.sector_skips);
	mix32(s.replays);
	mix32(s.errors);
	mix32(s.foreign_refusals);
	mix32(s.foreign_slots);
	mix32(s.claim_pending);
	mix32(storage_region());
	mix32(storage_ready());
	mix(sim_flash_data, FLASH_SIZE);
}

// ---- Flash contents ----

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void fill(uint8_t *b, uint32_t len, uint8_t seed)
{
	for (uint32_t i = 0; i < len; i++) {
		b[i] = (uint8_t)(seed + i * 7u + (i >> 8));
	}
}

// A codeplug area with old contents, the region as start-up leaves a blank one (ours, the mark written)
static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	fill(&sim_flash_data[CODEPLUG], 0x8000u, 0x31);
	static const uint8_t magic[4] = { 0x4F, 0x44, 0x52, 0x4E };
	memcpy(&sim_flash_data[NATIVE], magic, 4);
	fill(&sim_flash_data[NATIVE + S], 64, 0x77);
}

static bool boot(void)
{
	sim_flash_power_on();
	bool ok = storage_init();
	result('i', ok);
	state();
	return ok;
}

// A journal sector entry as a cut after its header leaves it
static void put_sector_entry(uint32_t target, const uint8_t *data, bool crc_ok, bool hdr_ok)
{
	memset(&sim_flash_data[JOURNAL], 0xFF, 2u * S);
	memcpy(&sim_flash_data[J1], data, S);
	uint8_t *h = &sim_flash_data[JOURNAL];
	put32(&h[0], 0x4C4E4A4Fu);
	put32(&h[4], target);
	put32(&h[8], storage_crc32(0, data, S) ^ (crc_ok ? 0u : 1u));
	put32(&h[12], storage_crc32(0, h, 12) ^ (hdr_ok ? 0u : 0x100u));
}

// A patch transaction in J0 (marks: the 5 mark bytes as given, 0xFF pending)
static void put_patch_entry(uint32_t addr, const uint8_t *data, uint32_t len, const uint8_t marks[5], bool crc_ok)
{
	memset(&sim_flash_data[JOURNAL], 0xFF, 2u * S);
	uint8_t *h = &sim_flash_data[JOURNAL];
	put32(&h[0], 0x324E4A4Fu);
	put32(&h[4], addr);
	put32(&h[8], len);
	put32(&h[12], storage_crc32(storage_crc32(0, h, 12), data, len) ^ (crc_ok ? 0u : 4u));
	memcpy(&h[16], marks, 5);
	memcpy(&h[24], data, len);
}

// ---- The operations ----

static uint8_t data_a[S], data_b[S], big[0x1800u];

static bool op_sector(void) { return storage_sector_write(CODEPLUG + S, data_a); }
static bool op_sector_same(void) { return storage_sector_write(CODEPLUG + 2u * S, &sim_flash_data[CODEPLUG + 2u * S]); }
static bool op_patch(void) { return storage_write(CODEPLUG + 2u * S + 0x100u, data_b, 64); }
static bool op_patch_across(void) { return storage_write(CODEPLUG + 3u * S - 0x10u, data_b + 64, 0x40u); }
static bool op_patch_max(void) { return storage_write(CODEPLUG + 4u * S + 24u, data_b, S - 24u); }
static bool op_sector_path(void) { return storage_write(CODEPLUG + 3u * S + 0x800u, big, sizeof(big)); }
static bool op_erase(void) { return storage_write(CODEPLUG + S, NULL, 2u * S); }
static bool op_native(void) { return storage_native_erase() == STORAGE_NATIVE_ERASED; }
static bool op_claim(void) { return storage_claim_request() && storage_claim(); }
static bool op_replay(void) { return storage_init(); }

typedef struct {
	char tag;
	void (*setup)(void);
	bool (*run)(void);
} step_t;

static void setup_ours(void)
{
	fresh();
	boot();
}

static void setup_foreign(void)
{
	fresh();
	memset(&sim_flash_data[RING + 0x300u], 0x42, 40);
	boot();
}

static void setup_replay_sector(void)
{
	fresh();
	boot();
	put_sector_entry(CODEPLUG + S, data_a, true, true);
}

static void setup_replay_patch(void)
{
	static const uint8_t pending[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	fresh();
	boot();
	put_patch_entry(CODEPLUG + 3u * S - 0x10u, data_b, 0x40u, pending, true);
}

static void setup_replay_staged(void)
{
	// The first sector staged (its merged copy in J1), not rewritten
	static const uint8_t staged[5] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
	fresh();
	boot();
	put_patch_entry(CODEPLUG + 3u * S - 0x10u, data_b, 0x40u, staged, true);
	memcpy(&sim_flash_data[J1], &sim_flash_data[CODEPLUG + 2u * S], S);
	memcpy(&sim_flash_data[J1 + S - 0x10u], data_b, 0x10u);
}

static const step_t steps[] = {
	{ 'a', setup_ours, op_sector },
	{ 'b', setup_ours, op_sector_same },
	{ 'c', setup_ours, op_patch },
	{ 'd', setup_ours, op_patch_across },
	{ 'm', setup_ours, op_patch_max },
	{ 'e', setup_ours, op_sector_path },
	{ 'f', setup_ours, op_erase },
	{ 'g', setup_ours, op_native },
	{ 'h', setup_foreign, op_claim },
	{ 'r', setup_replay_sector, op_replay },
	{ 'p', setup_replay_patch, op_replay },
	{ 's', setup_replay_staged, op_replay },
};

// After a step (cut or not): a start, then one more write, which reuses the journal
static void after(void)
{
	boot();
	result('z', storage_sector_write(CODEPLUG + 5u * S, data_b));
	state();
}

// One step uncut, recording where its programs and erases are; then a cut at the start, one unit in, the middle and
// the last unit of each, every time from the same setup
static void run_step(const step_t *st)
{
	st->setup();
	ops = 0;
	result(st->tag, st->run());
	uint32_t n = ops;
	state();
	after();
	// The units before each operation: replay the uncut step's units (a write's bytes, one per erase)
	uint32_t at = 0;
	for (uint32_t k = 0; k < n; k++) {
		op_start[k] = at;
		at += op_len[k];
	}
	for (uint32_t k = 0; k < n && k < OPS_MAX; k++) {
		uint32_t cuts[4] = { op_start[k], op_start[k] + 1u, op_start[k] + op_len[k] / 2u, op_start[k] + op_len[k] - 1u };
		for (unsigned c = 0; c < 4; c++) {
			// A cut point equal to one before it is the same run; one past the operation is the next one's
			bool dup = cuts[c] >= op_start[k] + op_len[k] && op_len[k] != 0;
			for (unsigned d = 0; d < c; d++) {
				dup = dup || cuts[d] == cuts[c];
			}
			if (dup || cuts[c] == 0) {
				continue;	// 0 disarms the simulated cut: "before the step" is no step
			}
			st->setup();
			mix32(k);
			mix32(c);
			sim_flash_cut_after(cuts[c]);
			result(st->tag, st->run());
			result('!', sim_flash_is_cut());
			seen_cuts += sim_flash_is_cut();
			cut_points++;
			state();
			after();
		}
	}
}

// storage_init on each shape of J0 and J1 the format has, from a region that holds the mark
static void run_shapes(void)
{
	static const uint8_t none[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	static const uint8_t first_done[5] = { 0x00, 0x00, 0xFF, 0xFF, 0xFF };
	static const uint8_t all_done[5] = { 0x00, 0x00, 0x00, 0x00, 0x00 };

	for (unsigned shape = 0; shape < 15; shape++) {
		fresh();
		boot();
		mix32(shape);
		switch (shape) {
		case 0:		// J0 and J1 blank
			memset(&sim_flash_data[JOURNAL], 0xFF, 2u * S);
			break;
		case 1:		// J0 blank, J1 a copy (a cut between J1 and the header)
			memset(&sim_flash_data[JOURNAL], 0xFF, S);
			memcpy(&sim_flash_data[J1], data_a, S);
			break;
		case 2:		// a sector entry, the target not rewritten yet
			put_sector_entry(CODEPLUG + S, data_a, true, true);
			break;
		case 3:		// a sector entry, the target already equal
			put_sector_entry(CODEPLUG + S, &sim_flash_data[CODEPLUG + S], true, true);
			break;
		case 4:		// a sector entry whose copy doesn't match its CRC
			put_sector_entry(CODEPLUG + S, data_a, false, true);
			break;
		case 5:		// a torn header
			put_sector_entry(CODEPLUG + S, data_a, true, false);
			break;
		case 6:		// a finished sector entry
			put_sector_entry(CODEPLUG + S, data_a, true, true);
			sim_flash_data[JOURNAL + 16] = 0;
			break;
		case 7:		// a sector entry aimed at the journal
			put_sector_entry(JOURNAL, data_a, true, true);
			break;
		case 8:		// a sector entry and other bytes after its done byte
			put_sector_entry(CODEPLUG + S, data_a, true, true);
			memset(&sim_flash_data[JOURNAL + 0x40u], 0x12, 8);
			break;
		case 9:		// a patch entry, nothing staged
			put_patch_entry(CODEPLUG + 2u * S + 0x40u, data_b, 0x80u, none, true);
			break;
		case 10:	// a patch entry across a sector end, the first sector done
			put_patch_entry(CODEPLUG + 3u * S - 0x10u, data_b, 0x40u, first_done, true);
			break;
		case 11:	// a patch entry, all done
			put_patch_entry(CODEPLUG + 3u * S - 0x10u, data_b, 0x40u, all_done, true);
			break;
		case 12:	// a torn patch header
			put_patch_entry(CODEPLUG + 3u * S - 0x10u, data_b, 0x40u, none, false);
			break;
		case 13:	// junk in J0, and in the ring beside the mark
			memset(&sim_flash_data[JOURNAL], 0x5A, 32);
			memset(&sim_flash_data[RING + 0x800u], 0x33, 16);
			break;
		case 14:	// a patch entry aimed past the chip
			put_patch_entry(FLASH_SIZE - 0x10u, data_b, 0x20u, none, true);
			break;
		}
		after();
		// The same shape on a region with no record of ours: J0 or J1 decide whether it is foreign
		memset(&sim_flash_data[RING], 0xFF, RING_SIZE);
		after();
	}
}

int main(void)
{
	fill(data_a, S, 0x5D);
	fill(data_b, S, 0xA2);
	fill(big, sizeof(big), 0x19);
	sim_flash_set_log(on_op);
	for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		run_step(&steps[i]);
	}
	run_shapes();
	// A write refused in a foreign region, and the debug read window
	setup_foreign();
	result('x', storage_sector_write(CODEPLUG, data_a));
	result('y', storage_write(CODEPLUG, data_a, 16));
	for (uint32_t a = 0; a < FLASH_SIZE; a += 0x800u) {
		result('q', storage_debug_read_allowed(a, 0x200u));
	}
	result('q', storage_debug_read_allowed(map.calibration_addr, map.calibration_size));
	result('q', storage_debug_read_allowed(0xFFFFFF00u, 0x200u));
	state();
	sim_flash_set_log(NULL);
	sim_flash_free();

	printf("# storageJournalLogTest: hash 0x%016llx over %u flash operations; %u cuts, %u replays, %u writes, %u foreign\n",
	       (unsigned long long)hash, ops_total, seen_cuts, seen_replays, seen_writes, seen_foreign);
	// Every cut point is inside an operation of its step, so each one cut
	CHECK(seen_cuts == cut_points && seen_cuts > 250);
	CHECK(seen_replays > 150 && seen_writes > 400 && seen_foreign > 20);
	CHECK(hash == GOLDEN);
	printf("# storageJournalLogTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
