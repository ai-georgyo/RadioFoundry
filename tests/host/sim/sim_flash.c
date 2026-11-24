// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A simulated SPI NOR flash (sim_flash.h)
#include <stdlib.h>
#include <string.h>
#include "sim_flash.h"

uint8_t *sim_flash_data;
static uint32_t *erase_count;
static uint32_t *program_count;
static uint32_t budget;		// units left before the cut, 0 = not armed
static bool armed, cut;
static uint32_t units, violations, reads;
static uint32_t bad_addr, bad_len;
static uint32_t unread_addr, unread_len;
static uint32_t misread_addr, misread_left;	// the misread_left-th read covering misread_addr flips a bit there
static uint32_t grant = UINT32_MAX, ungranted;
static uint32_t fail_left, op_calls;	// the fail_left-th erase or program call from now fails
static sim_flash_log_fn log_fn;
// The erases and programs of a recorded run (sim_flash_ops_begin): where each starts, in units after the start, and its
// units
#define OPS_MAX		1024u
static bool rec_on, rec_over;
static uint32_t rec_units0, rec_ops, op_start[OPS_MAX], op_units[OPS_MAX];

static bool sim_init(void) { return true; }

static bool in_range(uint32_t addr, uint32_t len)
{
	return addr < sim_flash.size && len <= sim_flash.size - addr;
}

// One unit used; false when the power goes now
static bool spend(void)
{
	if (cut) {
		return false;
	}
	if (armed) {
		if (budget == 0) {
			cut = true;
			return false;
		}
		budget--;
	}
	units++;
	return true;
}

static bool read_op(uint32_t addr, void *buf, uint32_t len)
{
	reads++;
	if (cut || !in_range(addr, len)) {
		return false;
	}
	if (unread_len != 0 && addr < unread_addr + unread_len && addr + len > unread_addr) {
		return false;
	}
	memcpy(buf, &sim_flash_data[addr], len);
	if (misread_left != 0 && misread_addr >= addr && misread_addr - addr < len && --misread_left == 0) {
		((uint8_t *)buf)[misread_addr - addr] ^= 1u;
	}
	return true;
}

// The backstop of a driver that keeps factory bytes (hal_flash_t.factory_grant): with STORAGE_FACTORY_KEEP, an erase
// or a program in a unit holding factory bytes of the map, outside the granted unit, is refused and counted
static bool refused(uint32_t addr, uint32_t len)
{
	bool granted = grant != UINT32_MAX && addr >= grant && addr - grant < SIM_FLASH_SECTOR &&
		       len <= SIM_FLASH_SECTOR - (addr - grant);
	if (!granted && hal_flash_factory_unit(sim_flash.map, SIM_FLASH_SECTOR, addr, len)) {
		ungranted++;
		return true;
	}
	return false;
}

static bool write_op(uint32_t addr, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;
	if (cut || !in_range(addr, len) || refused(addr, len)) {
		return false;
	}
	if (bad_len != 0 && addr < bad_addr + bad_len && addr + len > bad_addr) {
		return false;
	}
	for (uint32_t i = 0; i < len; i++) {
		if (i == 0 || ((addr + i) % SIM_FLASH_SECTOR) == 0) {
			program_count[(addr + i) / SIM_FLASH_SECTOR]++;
		}
		if (!spend()) {
			return false;
		}
		if ((p[i] & ~sim_flash_data[addr + i]) != 0) {
			violations++;
		}
		sim_flash_data[addr + i] &= p[i];
	}
	return true;
}

static bool erase_op(uint32_t addr)
{
	uint32_t base = addr & ~(SIM_FLASH_SECTOR - 1u);
	if (cut || !in_range(base, SIM_FLASH_SECTOR) || refused(base, 1)) {
		return false;
	}
	if (!spend()) {
		// Torn erase: the first half erased, the rest scrambled
		memset(&sim_flash_data[base], 0xFF, SIM_FLASH_SECTOR / 2);
		for (uint32_t i = SIM_FLASH_SECTOR / 2; i < SIM_FLASH_SECTOR; i++) {
			sim_flash_data[base + i] |= (uint8_t)(i * 37u);
		}
		return false;
	}
	memset(&sim_flash_data[base], 0xFF, SIM_FLASH_SECTOR);
	erase_count[base / SIM_FLASH_SECTOR]++;
	return true;
}

static bool sim_read(uint32_t addr, void *buf, uint32_t len)
{
	bool ok = read_op(addr, buf, len);
	if (log_fn != NULL) {
		log_fn('r', addr, len, ok ? buf : NULL, ok);
	}
	return ok;
}

static void record(uint32_t n)
{
	if (rec_on && n != 0) {
		if (rec_ops == OPS_MAX) {
			rec_over = true;
			return;
		}
		op_start[rec_ops] = units - n - rec_units0;
		op_units[rec_ops++] = n;
	}
}

// The failing operation's turn (sim_flash_fail_op): true for the one that fails
static bool fails_now(void)
{
	op_calls++;
	return fail_left != 0 && --fail_left == 0;
}

static bool sim_write(uint32_t addr, const void *buf, uint32_t len)
{
	bool ok = !fails_now() && write_op(addr, buf, len);
	if (ok) {
		record(len);
	}
	if (log_fn != NULL) {
		log_fn('w', addr, len, buf, ok);
	}
	return ok;
}

static bool sim_erase(uint32_t addr)
{
	bool ok = !fails_now() && erase_op(addr);
	if (ok) {
		record(1);
	}
	if (log_fn != NULL) {
		log_fn('e', addr, 0, NULL, ok);
	}
	return ok;
}

static void sim_grant(uint32_t unit)
{
	grant = unit % SIM_FLASH_SECTOR == 0 ? unit : UINT32_MAX;
}

hal_flash_t sim_flash = {
	.init = sim_init, .size = 0, .erase_size = SIM_FLASH_SECTOR,
	.read = sim_read, .write = sim_write, .erase = sim_erase, .map = NULL,
	.factory_grant = sim_grant,
};

void sim_flash_create(uint32_t size, const hal_flash_map_t *map)
{
	sim_flash_free();
	sim_flash_data = malloc(size);
	erase_count = calloc(size / SIM_FLASH_SECTOR + 1u, sizeof(uint32_t));
	program_count = calloc(size / SIM_FLASH_SECTOR + 1u, sizeof(uint32_t));
	memset(sim_flash_data, 0xFF, size);
	sim_flash.size = size;
	sim_flash.map = map;
	armed = cut = false;
	units = violations = 0;
	bad_len = 0;
	unread_len = 0;
	misread_left = 0;
	fail_left = 0;
	grant = UINT32_MAX;
	ungranted = 0;
}

void sim_flash_free(void)
{
	free(sim_flash_data);
	free(erase_count);
	free(program_count);
	sim_flash_data = NULL;
	erase_count = NULL;
	program_count = NULL;
}

void sim_flash_cut_after(uint32_t n)
{
	armed = n != 0;
	budget = n;
}

bool sim_flash_is_cut(void) { return cut; }

void sim_flash_power_on(void)
{
	cut = false;
	armed = false;
	grant = UINT32_MAX;	// the driver's RAM
}

uint32_t sim_flash_units(void) { return units; }
uint32_t sim_flash_reads(void) { return reads; }
uint32_t sim_flash_erases(uint32_t addr) { return erase_count[addr / SIM_FLASH_SECTOR]; }
uint32_t sim_flash_programs(uint32_t addr) { return program_count[addr / SIM_FLASH_SECTOR]; }
uint32_t sim_flash_violations(void) { return violations; }

void sim_flash_bad_range(uint32_t addr, uint32_t len)
{
	bad_addr = addr;
	bad_len = len;
}

void sim_flash_bad_read(uint32_t addr, uint32_t len)
{
	unread_addr = addr;
	unread_len = len;
}

void sim_flash_fail_op(uint32_t nth)
{
	fail_left = nth;
}

uint32_t sim_flash_op_calls(void) { return op_calls; }

void sim_flash_misread(uint32_t addr, uint32_t nth)
{
	misread_addr = addr;
	misread_left = nth;
}

uint32_t sim_flash_granted(void) { return grant; }
uint32_t sim_flash_ungranted(void) { return ungranted; }

void sim_flash_set_log(sim_flash_log_fn fn)
{
	log_fn = fn;
}

void sim_flash_ops_begin(void)
{
	rec_on = true;
	rec_over = false;
	rec_ops = 0;
	rec_units0 = units;
}

uint32_t sim_flash_cut_points(uint32_t *pt, uint32_t max)
{
	uint32_t n = 0;
	rec_on = false;
	for (uint32_t i = 0; i < rec_ops; i++) {
		uint32_t c[4] = { op_start[i], op_start[i] + 1u, op_start[i] + op_units[i] / 2u,
				  op_start[i] + op_units[i] - 1u };
		for (unsigned k = 0; k < 4; k++) {
			// 0 would disarm the cut; a later point past the operation's end is the next one's
			bool dup = c[k] == 0 || (k > 0 && c[k] >= op_start[i] + op_units[i]);
			for (uint32_t j = 0; !dup && j < n; j++) {
				dup = pt[j] == c[k];
			}
			if (!dup) {
				if (n == max) {
					return 0;
				}
				pt[n++] = c[k];
			}
		}
	}
	return rec_over ? 0 : n;
}
