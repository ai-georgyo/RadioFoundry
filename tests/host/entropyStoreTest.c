// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The boot count under the nonce counter (core/crypto/entropy_boot.c, docs/spec/crypto.md 8.3) on the real settings
// store (core/settings, core/storage) and a simulated flash, where entropyTest has a fake store: the start-up save,
// a worn ring that heals, the bump at the counter's last quarter through the settings task's save, a power cut at
// every point of the start-up save, a settings layout change both ways, and a foreign region claimed. Whether the
// store has a count is read from its stats there, so this is the test of that reading. Across all of it no nonce
// counter value is given twice, and a count that gave nonces is never read again at a later start-up. No hardware
// generator, and one credited stir a start-up (the first key-up's time, which entropy_nonce asks of a weak pool): the
// pool stays weak, so only the counter counts
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto/entropy.h"
#include "crypto/entropy_boot.h"
#include "settings/settings.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static unsigned checks, failed;

#define CHECK(c, ...)                                                                                                \
	do {                                                                                                         \
		checks++;                                                                                            \
		if (!(c)) {                                                                                          \
			failed++;                                                                                    \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                  \
			printf(__VA_ARGS__);                                                                         \
			printf("\n");                                                                                \
		}                                                                                                    \
	} while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x40000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

const board_t board = { .name = "test", .flash = &sim_flash, .power_count = 5 };

static uint32_t now_ms;

// The microsecond clock entropy_boot.c stirs with
uint32_t hal_micros(void)
{
	return now_ms * 1000u;
}

static bool chip_ok(hal_chip_t *c)
{
	memset(c, 0, sizeof *c);
	c->uid[0] = 0x5EEDu;
	return true;
}

static const hal_system_t sys_chip = { .chip = chip_ok };
#define SEEN_MAX 20000u
static uint32_t seen[SEEN_MAX];
static unsigned seen_n;
static uint32_t used_max;	// the highest boot count a nonce was given on
static bool any_used;

static uint32_t stored_count(void)
{
	settings_t s;
	settings_read(&s);
	return settings_boot_count(&s);
}

// A start-up: the store loaded from flash, then the boot count bumped and saved, with the power cut after cut_units
// units of that save (0: not cut). A count a nonce was given on before must never come again
static void power_on_cut(uint32_t cut_units)
{
	sim_flash_power_on();
	CHECK(storage_init(), "storage_init");
	settings_init();
	uint32_t was = stored_count();
	CHECK(!any_used || was >= used_max, "a used count went back: stored %u, used %u", was, used_max);
	sim_flash_cut_after(cut_units);
	entropy_boot_start(&sys_chip, now_ms);
	entropy_mix_u32(now_ms, 2);	// the first key-up's time (radio_m17.c)
}

static void power_on(void)
{
	power_on_cut(0);
}

// The radio task's tick and the settings task's poll, 100 ms apart, with a nonce drawn on each
static unsigned run(unsigned ticks)
{
	unsigned given = 0;
	uint8_t nonce[14];

	for (unsigned i = 0; i < ticks; i++) {
		now_ms += 100u;
		settings_poll(now_ms);
		entropy_boot_poll(now_ms);
		if (entropy_nonce(nonce, sizeof nonce)) {
			uint32_t c = (uint32_t)nonce[10] << 24 | (uint32_t)nonce[11] << 16 | (uint32_t)nonce[12] << 8 |
				     nonce[13];
			if (seen_n < SEEN_MAX) {
				seen[seen_n++] = c;
			}
			uint32_t boot = c >> 12;
			used_max = !any_used || boot > used_max ? boot : used_max;
			any_used = true;
			given++;
		}
	}
	return given;
}

static int cmpu32(const void *x, const void *y)
{
	uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
	return a < b ? -1 : a > b;
}

// Every record of ours in the ring (the ownership mark aside) relabeled to another settings layout, its CRC redone:
// the ring as a firmware with that layout left it, for an update or a downgrade that changed the layout
static void relayout(uint16_t layout)
{
	for (uint32_t a = map.settings_addr; a < map.settings_addr + map.settings_size; a += STORAGE_SLOT_SIZE) {
		uint8_t *r = &sim_flash_data[a];
		uint16_t f = (uint16_t)(r[8] | (r[9] << 8));
		if (storage_slot_check(r) != STORAGE_SLOT_VALID || (f & 0x0FFFu) == 0) {
			continue;
		}
		f = (uint16_t)((f & 0xF000u) | layout);
		r[8] = (uint8_t)f;
		r[9] = (uint8_t)(f >> 8);
		uint16_t n = (uint16_t)(r[10] | (r[11] << 8));
		uint32_t crc = storage_crc32(0, r, STORAGE_SLOT_HDR + n);
		memcpy(&r[STORAGE_SLOT_HDR + n], &crc, sizeof crc);	// the host is little endian like the radios
	}
}

// A start-up on a region another firmware's data fills (foreign until claimed)
static void power_on_foreign(void)
{
	sim_flash_power_on();
	CHECK(!storage_init() && storage_region() == STORAGE_REGION_FOREIGN, "foreign");
	settings_init();
	entropy_boot_start(&sys_chip, now_ms);
	entropy_mix_u32(now_ms, 2);
}

static unsigned duplicates(void)
{
	qsort(seen, seen_n, sizeof seen[0], cmpu32);
	unsigned dup = 0;
	for (unsigned i = 1; i < seen_n; i++) {
		dup += seen[i] == seen[i - 1];
	}
	return dup;
}

int main(void)
{
	sim_flash_create(FLASH_SIZE, &map);

	// A blank store: the first start-up counts 1, saved at once
	power_on();
	CHECK(entropy_boot_count() == 1u && entropy_boot_saved() && stored_count() == 1u, "first start-up: %u",
	      entropy_boot_count());
	CHECK(run(10) == 10u, "nonces on a saved count");
	power_on();
	CHECK(entropy_boot_count() == 2u && entropy_boot_saved(), "second start-up: %u", entropy_boot_count());

	// The bump: at draw 3,072 the next count goes into the settings, the settings task saves it after its delay,
	// and the counter moves on to it without running out
	unsigned given = run(5000);
	CHECK(given == 5000u && entropy_boot_count() == 3u && entropy_boot_saved(), "bumped: %u given, count %u", given,
	      entropy_boot_count());
	power_on();
	CHECK(entropy_boot_count() == 4u, "after a bump the next start-up is past it: %u", entropy_boot_count());

	// A worn ring: the start-up save fails, the count is unsaved and the weak pool gives nothing; once the flash
	// takes records again the settings task's retry stores it and the nonces come
	sim_flash_bad_range(map.settings_addr, map.settings_size);
	power_on();
	CHECK(entropy_boot_count() == 5u && !entropy_boot_saved(), "worn: unsaved");
	CHECK(run(30) == 0u, "worn: refused");
	sim_flash_bad_range(0, 0);
	given = run(40);	// past the store's save delay
	CHECK(entropy_boot_saved() && given > 0u && stored_count() == 5u, "healed: %u given", given);

	// A power cut at every point of the start-up save: a cut save gives nothing, and the next start-up reads a count
	// past every one that gave a nonce
	unsigned cuts = 0;
	for (uint32_t u = 1; u < 2000u; u++) {
		power_on_cut(u);
		bool cut = sim_flash_is_cut();
		sim_flash_cut_after(0);
		cuts += cut;
		CHECK(!cut || !entropy_boot_saved(), "cut after %u units: said saved", u);
		unsigned g = run(3);
		CHECK(cut ? g == 0u : g == 3u, "cut after %u units: %u given", u, g);
		if (!cut) {
			break;		// the whole save fitted in u units: so it does in more
		}
	}
	CHECK(cuts > 400u, "a save is a few hundred units: %u cut", cuts);
	power_on();
	CHECK(entropy_boot_saved() && run(3) == 3u, "after the cuts");

	// A settings layout change (an update, or a downgrade, that changed SETTINGS_LAYOUT): the store loads the
	// defaults, but the boot count comes over, so the next start-up is past every count used (power_on checks it)
	uint32_t before = entropy_boot_count();
	relayout(SETTINGS_LAYOUT + 1u);
	power_on();
	settings_stats_t st;
	settings_get_stats(&st);
	CHECK(st.load == SETTINGS_DEFAULTS && entropy_boot_count() == before + 1u && entropy_boot_saved(),
	      "layout change: load %d, count %u after %u", (int)st.load, entropy_boot_count(), before);
	CHECK(run(3) == 3u, "layout change: nonces");
	// and back to this layout: a downgrade's records carry it the same way
	before = entropy_boot_count();
	given = run(4000);	// past the bump: the stored count moves on while the records are of the other layout
	CHECK(given == 4000u && entropy_boot_count() == before + 1u, "bump before the downgrade: %u",
	      entropy_boot_count());
	relayout(SETTINGS_LAYOUT);
	power_on();
	CHECK(entropy_boot_count() == before + 2u && entropy_boot_saved(), "back: count %u", entropy_boot_count());
	CHECK(run(3) == 3u, "back: nonces");

	unsigned dup = duplicates();
	CHECK(dup == 0u && seen_n < SEEN_MAX, "%u counters twice in %u", dup, seen_n);
	unsigned seen_main = seen_n;

	// Another radio, its ring full of another firmware's data: foreign, so every start-up runs on count 1 unsaved
	// and the weak pool gives nothing. The claim erases the ring and saves what the radio runs with, the count
	// included; the counter goes on from it and the next start-up is past it
	sim_flash_create(FLASH_SIZE, &map);
	for (uint32_t i = 0; i < map.settings_size; i++) {
		sim_flash_data[map.settings_addr + i] = (uint8_t)(i * 13u + 7u);
	}
	seen_n = 0;
	any_used = false;
	for (unsigned k = 0; k < 3; k++) {
		power_on_foreign();
		CHECK(entropy_boot_count() == 1u && !entropy_boot_saved(), "foreign start-up %u", k);
		CHECK(run(30) == 0u, "foreign: refused");
	}
	CHECK(storage_claim_request(), "claim");
	given = run(30);	// the housekeeping's poll claims and saves, the radio core's tick sees it stored
	CHECK(storage_region() == STORAGE_REGION_OURS && entropy_boot_saved() && stored_count() == 1u && given > 0u,
	      "claimed: %u given, stored %u", given, stored_count());
	power_on();
	CHECK(entropy_boot_count() == 2u && entropy_boot_saved() && run(3) == 3u, "after the claim: %u",
	      entropy_boot_count());
	dup = duplicates();
	CHECK(dup == 0u, "claim: %u counters twice in %u", dup, seen_n);

	sim_flash_free();
	printf("entropyStoreTest: %u checks, %u failed (%u nonces, last count %u)\n", checks, failed,
	       seen_main + seen_n, entropy_boot_count());
	return failed ? 1 : 0;
}
