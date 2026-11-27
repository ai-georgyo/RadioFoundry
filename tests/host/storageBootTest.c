// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The start-up order of core/app/app.c (storage_init, cps_fast_resume, tftp_codeplug_resume, settings_init) on a
// region that isn't ours: another firmware's bytes everywhere, the staging area included, so both resumes would take
// their erase branch. Their erases go to the flash directly, past storage.c's foreign-region refusal: the
// storage_ready() test in each is all that keeps the chip as it was (docs/spec/settings-store.md 5). Built twice:
// as is (foreign by its data) and with -DSTORAGE_CLAIM_FIRST=1 (also foreign while blank, until the claim).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cps/cps_fast.h"
#include "settings/settings.h"
#include "storage/storage.h"
#include "net/tftp_codeplug.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x400000u
#define STAGING		0x200000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x1C0000u,
	.native_addr = 0x1C0000u, .native_size = 0x40000u,
	.staging_addr = STAGING, .staging_size = 0x40000u,
};
static band_t bands[] = { { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 1, .power_count = 2, .flash = &sim_flash };

// A start on the chip as it is: foreign, nothing erased or programmed, both resumes refused
static void boot_untouched(const char *what)
{
	uint8_t *snap = malloc(FLASH_SIZE);
	memcpy(snap, sim_flash_data, FLASH_SIZE);
	sim_flash_power_on();
	uint32_t units = sim_flash_units();
	storage_init();
	CHECK(storage_region() == STORAGE_REGION_FOREIGN);
	CHECK(!cps_fast_resume());
	CHECK(!tftp_codeplug_resume());
	settings_init();
	CHECK(sim_flash_units() == units);
	CHECK(sim_flash_erases(STAGING) == 0);
	CHECK(memcmp(snap, sim_flash_data, FLASH_SIZE) == 0);
	CHECK(sim_flash_violations() == 0);
	printf("# %s: nothing written at start-up\n", what);
	free(snap);
}

// Another firmware's data everywhere; the staging area's first word is neither erased nor a record of ours
static void test_foreign_chip(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	for (uint32_t i = 0; i < FLASH_SIZE; i++) {
		sim_flash_data[i] = (uint8_t)(0x5Au + i * 13u + (i >> 7));	// the maker's data, layout unknown
	}
	boot_untouched("another firmware's data, staging area included");
	sim_flash_free();
}

#if STORAGE_CLAIM_FIRST
// Claim first: a blank settings region is not ours either, and the staging area may hold the maker's data
static void test_blank_ring_unclaimed(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	for (uint32_t i = 0; i < 0x40000u; i++) {
		sim_flash_data[STAGING + i] = (uint8_t)(0x21u + i * 7u);
	}
	boot_untouched("claim first, blank ring, data in the staging area");
	sim_flash_free();
}
#endif

int main(void)
{
	test_foreign_chip();
#if STORAGE_CLAIM_FIRST
	test_blank_ring_unclaimed();
#endif
	printf("# storageBootTest%s %s (%d failures)\n", STORAGE_CLAIM_FIRST ? " (claim first)" : "",
	       failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
