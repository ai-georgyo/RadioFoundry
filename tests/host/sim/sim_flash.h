// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A simulated SPI NOR flash for host tests, as a hal_flash_t: programming only clears bits (old & new), erase sets a
// 4 KB sector to 0xFF, and a power cut can be armed after a number of "units" (one programmed byte or one erase):
// the operation in progress is torn (the bytes after the cut are not programmed; a torn erase leaves the sector
// half old, half erased), and every operation fails until sim_flash_power_on().
#ifndef SIM_FLASH_H
#define SIM_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_flash.h"

#define SIM_FLASH_SECTOR	4096u

extern hal_flash_t sim_flash;
extern uint8_t *sim_flash_data;

// A fresh flash of size bytes, all erased, with the given map (NULL: none)
void sim_flash_create(uint32_t size, const hal_flash_map_t *map);
void sim_flash_free(void);
// Arm a power cut after units more units; 0 disarms
void sim_flash_cut_after(uint32_t units);
bool sim_flash_is_cut(void);
void sim_flash_power_on(void);
// Units used so far (programmed bytes + erases), and erases per sector so far
uint32_t sim_flash_units(void);
uint32_t sim_flash_erases(uint32_t addr);
// Program calls that reached a sector so far (one per call per sector it touches, torn ones included)
uint32_t sim_flash_programs(uint32_t addr);
// Read calls so far, for a test of how much a reader reads
uint32_t sim_flash_reads(void);
// A worn range: programs there fail (nothing programmed); erases still work. len 0 clears it
void sim_flash_bad_range(uint32_t addr, uint32_t len);
// An unreadable range: reads that touch it fail. len 0 clears it
void sim_flash_bad_read(uint32_t addr, uint32_t len);
// A marginal read: the nth read from now (1 = the next) that covers addr returns the byte there with bit 0 flipped,
// once. nth 0 clears it
void sim_flash_misread(uint32_t addr, uint32_t nth);
// The backstop of a driver that keeps factory bytes (hal_flash_t.factory_grant, built with STORAGE_FACTORY_KEEP): an
// erase or a program in an erase unit holding factory bytes of the map, outside the unit granted, fails with nothing
// changed and is counted here. The grant in force (UINT32_MAX: none); a power cut closes it
uint32_t sim_flash_granted(void);
uint32_t sim_flash_ungranted(void);
// An operation log, for a test that pins the exact flash operations of a run: fn (NULL: none) is called after every
// read, program and erase with its kind ('r', 'w', 'e'), address, length (0 for an erase), the bytes read or asked to
// be programmed (NULL for an erase and a failed read) and its result. Kept across sim_flash_create()
typedef void (*sim_flash_log_fn)(char kind, uint32_t addr, uint32_t len, const void *data, bool ok);
void sim_flash_set_log(sim_flash_log_fn fn);
// Count an operation that programs a 1 where the flash holds a 0 (a missing erase), as an error
uint32_t sim_flash_violations(void);

#endif
