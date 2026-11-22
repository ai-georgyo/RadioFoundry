// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// core/storage built for a flash that must be claimed before its first write (-DSTORAGE_CLAIM_FIRST=1,
// docs/spec/settings-store.md 5), with core/settings on the simulated NOR flash: a region without a record of ours is
// foreign whether it is blank or not, and nothing on the chip is programmed or erased until the claim; the claim
// leaves a record, so every later start finds the region ours, through power cuts too.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "settings/settings.h"
#include "sim/sim_flash.h"

#if STORAGE_CLAIM_FIRST != 1
#error "this test is storage.c with -DSTORAGE_CLAIM_FIRST=1 (settingsTest covers the default)"
#endif

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x40000u
#define RING		0x11000u
#define RING_SIZE	0x8000u
#define JOURNAL		0x19000u
#define REGION_END	(JOURNAL + 2u * STORAGE_SECTOR)	// the guarded region is [RING, REGION_END)
#define CODEPLUG	0x3000u				// somewhere a codeplug write would go

static const hal_flash_map_t map = {
	.settings_addr = RING, .settings_size = RING_SIZE, .journal_addr = JOURNAL,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

const board_t board = { .name = "test", .flash = &sim_flash, .power_count = 5 };

static bool boot(void)
{
	sim_flash_power_on();
	bool ok = storage_init();
	settings_init();
	return ok;
}

static void fill(uint8_t *b, uint8_t seed)
{
	for (unsigned i = 0; i < STORAGE_SECTOR; i++) {
		b[i] = (uint8_t)(seed + i * 7u + (i >> 8));
	}
}

// Bytes that are neither erased nor ours, as another firmware leaves them
static void junk(uint32_t addr, uint32_t len, uint8_t seed)
{
	for (uint32_t i = 0; i < len; i++) {
		sim_flash_data[addr + i] = (uint8_t)(seed + i * 13u + (i >> 7));
	}
}

static unsigned valid_records(void)
{
	unsigned n = 0;
	for (uint32_t a = RING; a < RING + RING_SIZE; a += STORAGE_SLOT_SIZE) {
		n += storage_slot_check(&sim_flash_data[a]) == STORAGE_SLOT_VALID;
	}
	return n;
}

static uint32_t erases(uint32_t from, uint32_t to)
{
	uint32_t n = 0;
	for (uint32_t a = from; a < to; a += STORAGE_SECTOR) {
		n += sim_flash_erases(a);
	}
	return n;
}

// The chip outside the guarded region is what snap holds
static bool outside_same(const uint8_t *snap)
{
	return memcmp(snap, sim_flash_data, RING) == 0 &&
	       memcmp(snap + REGION_END, sim_flash_data + REGION_END, FLASH_SIZE - REGION_END) == 0;
}

static bool all_blank(void)
{
	for (uint32_t i = 0; i < FLASH_SIZE; i++) {
		if (sim_flash_data[i] != 0xFF) {
			return false;
		}
	}
	return true;
}

// Everything a start and the writers do on a region that isn't claimed: nothing. The chip must be what snap holds
static void refused_everywhere(const uint8_t *snap)
{
	uint8_t a[STORAGE_SECTOR];
	settings_stats_t st;
	storage_stats_t s0, s1;
	settings_t s;
	uint32_t u0 = sim_flash_units();

	CHECK(!boot());
	CHECK(storage_region() == STORAGE_REGION_FOREIGN && !storage_ready() && !storage_claim_pending());
	settings_get_stats(&st);
	CHECK(st.load == SETTINGS_FOREIGN);
	settings_begin()->squelch = 5;
	settings_end(100);
	settings_poll(5000);
	CHECK(!settings_flush());
	settings_get_stats(&st);
	settings_read(&s);
	CHECK(s.squelch == 5 && st.saves == 0);		// the radio runs on from RAM, nothing saved
	storage_get_stats(&s0);
	fill(a, 5);
	CHECK(!storage_sector_write(CODEPLUG, a));
	CHECK(!storage_write(CODEPLUG, a, 16));
	CHECK(!storage_write(CODEPLUG, NULL, STORAGE_SECTOR));	// an erase of a range
	CHECK(!storage_write(RING, a, 16));
	CHECK(storage_native_erase() == STORAGE_NATIVE_FAILED);
	storage_get_stats(&s1);
	CHECK(s1.foreign_refusals - s0.foreign_refusals == 4 && s1.sector_writes == s0.sector_writes);
	// Not one program, not one erase, on any sector; and the same at the next start (nothing is "repaired")
	CHECK(!boot() && storage_region() == STORAGE_REGION_FOREIGN);
	CHECK(sim_flash_units() == u0 && erases(0, FLASH_SIZE) == 0);
	CHECK(memcmp(snap, sim_flash_data, FLASH_SIZE) == 0);
	CHECK(sim_flash_violations() == 0);
}

// A blank chip: the default build marks it at the first start and writes from then on. Here it is foreign
static void test_blank(void)
{
	settings_stats_t st;
	storage_stats_t ss;
	settings_t s;
	uint8_t a[STORAGE_SECTOR];
	uint8_t *snap = malloc(FLASH_SIZE);

	sim_flash_create(FLASH_SIZE, &map);
	memcpy(snap, sim_flash_data, FLASH_SIZE);
	refused_everywhere(snap);
	CHECK(all_blank());
	storage_get_stats(&ss);
	CHECK(ss.foreign_slots == 0);		// nothing of anyone's is there: foreign all the same
	CHECK(valid_records() == 0);
	// The debug link's read window works on it (the host dumps before it claims)
	CHECK(storage_debug_read_allowed(RING, RING_SIZE) && storage_debug_read_allowed(JOURNAL, 2u * STORAGE_SECTOR));

	// The claim, as the housekeeping task runs it: erase, mark, then save what the radio runs with
	settings_begin()->squelch = 5;
	settings_end(200);
	CHECK(storage_claim_request() && storage_claim_pending());
	storage_get_stats(&ss);
	CHECK(ss.claim_pending);
	CHECK(all_blank());			// the request alone touches nothing
	settings_poll(6000);
	CHECK(storage_region() == STORAGE_REGION_OURS && storage_ready() && !storage_claim_pending());
	settings_get_stats(&st);
	CHECK(!st.dirty && st.saves == 1);
	CHECK(storage_slot_check(&sim_flash_data[RING]) == STORAGE_SLOT_VALID);	// the ownership mark
	CHECK(valid_records() > 1);		// and the settings after it
	CHECK(outside_same(snap) && erases(0, RING) == 0 && erases(REGION_END, FLASH_SIZE) == 0);
	CHECK(erases(RING, REGION_END) == (REGION_END - RING) / STORAGE_SECTOR);
	// Writes work now
	fill(a, 9);
	CHECK(storage_sector_write(CODEPLUG, a));
	CHECK(memcmp(&sim_flash_data[CODEPLUG], a, STORAGE_SECTOR) == 0);
	CHECK(storage_write(CODEPLUG + STORAGE_SECTOR - 8u, a, 16));
	// And the next start finds the region ours, with the settings
	CHECK(boot() && storage_region() == STORAGE_REGION_OURS && storage_ready());
	settings_get_stats(&st);
	settings_read(&s);
	CHECK(st.load == SETTINGS_LOADED && s.squelch == 5);
	CHECK(!storage_claim_request());	// ours: nothing to claim
	CHECK(sim_flash_violations() == 0);
	free(snap);
}

// storage_claim() alone (no settings save after it): its mark is the record that makes the next start ours
static void test_claim_leaves_a_record(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(!storage_init() && storage_region() == STORAGE_REGION_FOREIGN);
	CHECK(storage_claim_request());
	CHECK(storage_claim());
	CHECK(storage_region() == STORAGE_REGION_OURS && storage_ready() && !storage_claim_pending());
	CHECK(valid_records() == 1);
	CHECK(storage_slot_check(&sim_flash_data[RING]) == STORAGE_SLOT_VALID);	// the first slot
	CHECK(!storage_claim());		// not foreign any more
	uint32_t u0 = sim_flash_units();
	CHECK(storage_init() && storage_region() == STORAGE_REGION_OURS);
	CHECK(storage_init() && storage_region() == STORAGE_REGION_OURS);
	CHECK(sim_flash_units() == u0 && valid_records() == 1);	// no second mark
	CHECK(sim_flash_violations() == 0);
}

// The case the define exists for: the region happens to be blank, and another firmware's data lies around it
static void test_data_around_a_blank_region(void)
{
	uint8_t *snap = malloc(FLASH_SIZE);

	sim_flash_create(FLASH_SIZE, &map);
	junk(0, RING, 3);					// up to the region's first byte
	junk(REGION_END, FLASH_SIZE - REGION_END, 11);		// from its end to the chip's
	memcpy(snap, sim_flash_data, FLASH_SIZE);
	refused_everywhere(snap);
	CHECK(storage_claim_request());
	settings_poll(6000);
	CHECK(storage_region() == STORAGE_REGION_OURS && valid_records() > 1);
	CHECK(outside_same(snap));		// the claim takes the region, nothing else
	CHECK(boot() && storage_region() == STORAGE_REGION_OURS);
	CHECK(outside_same(snap));
	free(snap);
}

// Another firmware's bytes in the region itself: as in the default build
static void test_other_bytes_in_the_region(void)
{
	storage_stats_t ss;
	uint8_t *snap = malloc(FLASH_SIZE);

	sim_flash_create(FLASH_SIZE, &map);
	junk(RING + 2u * STORAGE_SECTOR, 3000, 7);
	junk(JOURNAL, 100, 9);
	junk(CODEPLUG, 2u * STORAGE_SECTOR, 1);
	memcpy(snap, sim_flash_data, FLASH_SIZE);
	refused_everywhere(snap);
	storage_get_stats(&ss);
	CHECK(ss.foreign_slots > 0);
	CHECK(storage_claim_request());
	settings_poll(6000);
	CHECK(storage_region() == STORAGE_REGION_OURS && storage_ready());
	storage_get_stats(&ss);
	CHECK(ss.foreign_slots == 0);
	CHECK(sim_flash_data[RING + 2u * STORAGE_SECTOR + 5u] == 0xFF && sim_flash_data[JOURNAL] == 0xFF);
	CHECK(outside_same(snap));
	CHECK(boot() && storage_region() == STORAGE_REGION_OURS);
	free(snap);
}

// What counts as a record of ours is what the default build counts: a settings record or a journal header
static void test_ours(void)
{
	uint8_t a[STORAGE_SECTOR];
	settings_t s;

	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(storage_claim_request());
	settings_poll(6000);
	settings_begin()->squelch = 6;
	settings_end(1);
	CHECK(settings_flush());
	// Junk beside our records (a torn erase in our own ring): ours, the junk is skipped
	junk(RING + 3u * STORAGE_SECTOR, 2000, 3);
	CHECK(boot() && storage_region() == STORAGE_REGION_OURS);
	settings_read(&s);
	CHECK(s.squelch == 6);
	// Our journal header alone, the ring blank: ours, and the start programs nothing (no mark is added)
	fill(a, 4);
	CHECK(storage_sector_write(CODEPLUG, a));
	memset(sim_flash_data + RING, 0xFF, RING_SIZE);
	uint32_t u0 = sim_flash_units();
	CHECK(boot() && storage_region() == STORAGE_REGION_OURS);
	CHECK(sim_flash_units() == u0 && valid_records() == 0);
	// A sector copy in J1 with J0 never written, nothing else: foreign
	sim_flash_create(FLASH_SIZE, &map);
	junk(JOURNAL + STORAGE_SECTOR, STORAGE_SECTOR, 1);
	CHECK(!boot() && storage_region() == STORAGE_REGION_FOREIGN);
}

// A power cut at every flash operation of the claim and of the save that follows it. The default build takes a
// region with a torn mark as its own; here the next start is ours from the operation that completes the mark on,
// and foreign before it: nothing is repaired at that start, and a second claim finishes the job. Nothing outside
// the region changes either way
static void test_cut_inside_the_claim(void)
{
	uint8_t *snap = malloc(FLASH_SIZE);
	uint32_t total = 0;
	int ours_seen = 0, foreign_seen = 0;

	// The claim's own operations: the erases, then the mark
	sim_flash_create(FLASH_SIZE, &map);
	junk(0, FLASH_SIZE, 1);
	CHECK(!boot() && storage_claim_request() && storage_claim());
	const uint32_t marked = sim_flash_units();

	for (uint32_t cut = 0; cut <= total; cut++) {
		sim_flash_create(FLASH_SIZE, &map);
		junk(0, FLASH_SIZE, (uint8_t)(cut + 1u));	// the region full of another firmware's bytes too
		memcpy(snap, sim_flash_data, FLASH_SIZE);
		CHECK(!boot());
		CHECK(storage_claim_request());
		sim_flash_cut_after(cut);			// 0: no cut, to learn how many operations there are
		settings_poll(6000);
		if (cut == 0) {
			total = sim_flash_units();
			CHECK(total > marked);			// the save after the claim is in the sweep
			continue;
		}
		sim_flash_power_on();
		uint32_t u0 = sim_flash_units();
		bool ok = boot();
		CHECK(ok == (cut >= marked));			// the mark alone covers the first save
		CHECK(ok == (valid_records() > 0));
		CHECK(storage_region() == (ok ? STORAGE_REGION_OURS : STORAGE_REGION_FOREIGN));
		CHECK(sim_flash_units() == u0);			// a start alone never writes here
		if (ok) {
			ours_seen++;
		} else {
			foreign_seen++;
			CHECK(storage_claim_request());
			settings_poll(6000);
			CHECK(storage_region() == STORAGE_REGION_OURS && valid_records() > 1);
			CHECK(boot() && storage_region() == STORAGE_REGION_OURS);
		}
		CHECK(outside_same(snap));
	}
	CHECK(ours_seen > 0 && foreign_seen > 0);
	free(snap);
}

// --- once claimed, the region stays ours ---
// With the define a region that loses its last record is foreign again, and the radio saves nothing until a host
// claims it a second time. So no power cut may ever leave it without one: checked over laps of the smallest settings
// ring there is (two sectors: each erase takes half of all records, the claim's mark first) and both kinds of
// journal rewrite

static const hal_flash_map_t map_small = {
	.settings_addr = RING, .settings_size = 2u * STORAGE_SECTOR, .journal_addr = JOURNAL,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

#define STEPS		40u
#define STEP_SECTOR	7u	// every 16th step from here: a codeplug sector through the journal
#define STEP_PATCH	12u	// every 16th step from here: a record across a sector end (the patch transaction)

static void step(unsigned k)
{
	uint8_t a[STORAGE_SECTOR];

	if (k % 16u == STEP_SECTOR) {
		fill(a, (uint8_t)k);
		storage_sector_write(CODEPLUG, a);
	} else if (k % 16u == STEP_PATCH) {
		memset(a, (int)(k & 0xFFu), 100);
		storage_write(CODEPLUG + 2u * STORAGE_SECTOR - 48u, a, 100);
	} else {
		settings_t *s = settings_begin();
		s->squelch = (uint8_t)(k % 10u);
		s->dual_b = (uint16_t)(1u + k % 2u);	// a second page with every other save
		settings_end(k);
		settings_flush();
	}
}

// A cut at the operations of step k, started from the chip as img holds it; returns the cuts tried
static unsigned cuts_in_step(unsigned k, const uint8_t *img)
{
	unsigned tried = 0;
	uint32_t n = 0;

	for (uint32_t cut = 0; cut <= n; cut += (n <= 1024u || cut < 40u || n - cut < 40u) ? 1u : 53u) {
		sim_flash_create(FLASH_SIZE, &map_small);
		memcpy(sim_flash_data, img, FLASH_SIZE);
		CHECK(boot());
		uint32_t u0 = sim_flash_units();
		sim_flash_cut_after(cut);		// 0: no cut, to learn how many operations the step has
		step(k);
		if (cut == 0) {
			n = sim_flash_units() - u0;
			continue;
		}
		bool ok = boot();
		if (!ok || storage_region() != STORAGE_REGION_OURS || valid_records() == 0) {
			printf("FAIL claimed region lost: step %u, cut after %u of %u operations\n", k, cut, n);
			failures++;
		}
		// And it is in use again: the next change is saved
		settings_begin()->squelch ^= 1u;
		settings_end(k);
		CHECK(settings_flush());
		tried++;
	}
	return tried;
}

static void test_claimed_stays_ours(void)
{
	uint8_t *img = malloc(FLASH_SIZE), *run = malloc(FLASH_SIZE);
	unsigned tried = 0, laps = 0;

	sim_flash_create(FLASH_SIZE, &map_small);
	junk(0, FLASH_SIZE, 5);
	CHECK(!boot());
	CHECK(storage_claim_request());
	settings_poll(6000);
	CHECK(storage_region() == STORAGE_REGION_OURS);
	for (unsigned k = 0; k < STEPS; k++) {
		// The cuts run on copies; the run itself goes on from here, uncut
		memcpy(img, sim_flash_data, FLASH_SIZE);
		uint32_t ring0 = erases(RING, RING + RING_SIZE), first0 = sim_flash_erases(RING);
		step(k);
		bool ring_erase = erases(RING, RING + RING_SIZE) != ring0;
		laps += sim_flash_erases(RING) != first0;	// the sector that got the claim's mark
		memcpy(run, sim_flash_data, FLASH_SIZE);
		// Every operation of the first saves and of each save that erases a ring sector; one rewrite of each
		// kind, sampled (cuts_in_step: every operation near its ends, every 53rd between)
		if (k < 2u || ring_erase || k == STEP_SECTOR || k == STEP_PATCH) {
			tried += cuts_in_step(k, img);
		}
		sim_flash_create(FLASH_SIZE, &map_small);
		memcpy(sim_flash_data, run, FLASH_SIZE);
		CHECK(boot());
	}
	CHECK(laps >= 2 && tried > 1000);
	printf("claimed stays ours: %u cut points over %u laps of the ring\n", tried, laps);
	free(img);
	free(run);
}

int main(void)
{
	test_blank();
	test_claim_leaves_a_record();
	test_data_around_a_blank_region();
	test_other_bytes_in_the_region();
	test_ours();
	test_cut_inside_the_claim();
	test_claimed_stays_ours();
	sim_flash_free();
	printf("# %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
