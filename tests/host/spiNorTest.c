// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// drivers/flash/spi_nor against a model of a W25Q / GD25Q class chip at the SPI bus (the command set of the Winbond
// W25Q128JV and GigaDevice GD25Q128 data sheets: 03 read, 02 page program, 20 sector erase, 06 write enable,
// 05 / 35 / 15 status, 9F JEDEC ID, 48 security register read, 4B unique ID). The model keeps what the driver must get right:
// erase sets a 4 KB sector to 0xFF, programming only clears bits, a page program that runs past the end of its
// 256-byte page wraps to the start of the same page, program and erase need the write enable latch (and clear it),
// the chip stays busy for a while after each, addresses wrap at the chip's size (3-byte addresses reach 16 MB). It
// can fail the bus, refuse the write enable (write protected), stay busy forever and lose power in the middle of a
// program or an erase. Then core/storage and core/settings run on the driver as the boards wire it (16 MB and 1 MB
// parts), with power cuts at every program / erase of a settings save and a codeplug sector rewrite. Every command any
// test sent is checked at the end: none may erase or program a security register or write a status register (the
// factory calibration lives there). A board's own storage test includes this file with SPI_NOR_TEST_LIB defined and
// brings its own main (boards/<id>/tests).
//
// Built a second time with -DSPI_NOR_ADDR4=1 (spiNorAddr4Test), every test above runs again on the driver as a board
// with a part larger than 16 MB builds it, and the model is also such a part (Winbond W25Q256JV data sheet): the
// opcodes that always take four address bytes (13 read, 12 page program, 21 sector erase), the address mode (ADS)
// and the mode it powers up in (ADP), the extended address register (C5) that every command with four address bytes
// replaces and a command with three uses as the fourth, a suspended operation (SUS) and the software reset (66, 99)
// with its dead time after. None of that is reset with the MCU: the tests leave the chip as other firmware might
// and start the driver on it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flash/spi_nor/spi_nor.h"
#include "storage/storage.h"
#include "settings/settings.h"
#include "hal_time.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

static uint32_t delays_ms;
uint32_t hal_millis(void) { return 0; }

// ---- The chip model ----

static struct {
	uint8_t *mem;
	uint32_t size;		// the array (power of two); addresses wrap at it
	uint8_t id[3];
	uint8_t security[3][256];
	uint8_t uid[16];
	bool wel;		// write enable latch
	uint32_t busy;		// status polls left that still read busy
	uint32_t busy_polls;	// busy polls after a program / erase (UINT32_MAX: stuck)
	bool protect;		// write enable refused (WP / block protect)
	// The command in progress (chip select low)
	bool selected;
	uint32_t pos;		// bytes clocked in this command
	uint8_t cmd;
	uint32_t addr;
	uint8_t page[256];	// page program data, by offset in the page
	bool page_set[256];
	uint32_t data_len;	// page program bytes clocked in
	// Bookkeeping and faults
	uint32_t selects, deselects, outside;	// xfer while not selected
	uint32_t programs, erases;		// operations done
	uint32_t page_overruns;			// page programs longer than the room to the page end
	uint32_t violations;			// programmed a 1 over a 0
	uint32_t fail_xfer_at;			// fail the n-th xfer from now (0 = never)
	uint32_t ops_left;			// program / erase operations before the power goes (0 = not armed)
	bool cut;				// no power: every transfer fails
	uint32_t status_reads[3];
	// A part larger than 16 MB (capacity code 0x19 and up). It has no reset pin: all of this outlives a reset of the
	// MCU and goes only with the power or with the reset command
	bool addr4;		// has the opcodes with four address bytes, the extended address register, ADS and ADP
	bool ads;		// address mode now: 03 / 02 / 20 / 48 take four address bytes, 4B five dummy bytes
	bool adp;		// the mode it powers up and resets into (non-volatile; only other firmware sets it)
	uint8_t ext;		// extended address register: the fourth address byte of a command with three
	bool sus;		// an erase or program is suspended (other firmware's 75)
	bool reset_armed;	// 66 was the command before this one
	uint32_t dead_us;	// after a reset: no command is accepted for this long (tRST)
	bool deaf;		// the command in progress came in that time
	uint8_t alen;		// address bytes of the command in progress
	uint32_t resets;
	uint32_t reset_harm;	// resets that hit a busy or suspended chip (they can corrupt its data)
	uint32_t early;		// commands sent in the dead time
	uint32_t high_ops;	// programs and erases at 16 MB and above
	uint32_t seen[256];	// command bytes this chip saw
} chip;

// Every command byte seen, over all tests (chip_create doesn't clear it), and the two things no test may do
static uint32_t cmds_seen[256];
static uint32_t harm_total, early_total;

void hal_delay_ms(uint32_t ms)
{
	delays_ms += ms;
	chip.dead_us = 0;
}

void hal_delay_us(uint32_t us)
{
	chip.dead_us -= us < chip.dead_us ? us : chip.dead_us;
}

static void chip_create(uint32_t size, uint8_t mfr, uint8_t type, uint8_t cap)
{
	free(chip.mem);
	memset(&chip, 0, sizeof(chip));
	chip.mem = malloc(size);
	memset(chip.mem, 0xFF, size);
	chip.size = size;
	chip.id[0] = mfr;
	chip.id[1] = type;
	chip.id[2] = cap;
	chip.addr4 = cap >= 0x19 && cap <= 0x22;
	chip.busy_polls = 2;
	for (int r = 0; r < 3; r++) {
		for (int i = 0; i < 256; i++) {
			chip.security[r][i] = (uint8_t)(r * 0x40 + i);
		}
	}
}

static void chip_power_on(void)
{
	chip.cut = false;
	chip.ops_left = 0;
	chip.wel = false;
	chip.busy = 0;
	chip.selected = false;
	chip.ads = chip.adp;
	chip.ext = 0;
	chip.sus = false;
	chip.reset_armed = false;
	chip.dead_us = 0;
}

// Enable Reset then Reset: the power-up state without the power going
static void chip_reset(void)
{
	if (chip.busy != 0 || chip.sus) {
		chip.reset_harm++;
		harm_total++;
	}
	chip.resets++;
	chip.wel = false;
	chip.busy = 0;
	chip.sus = false;
	chip.ads = chip.adp;
	chip.ext = 0;
	chip.dead_us = 30;
}

static bool cmd4(uint8_t cmd)
{
	return cmd == 0x13 || cmd == 0x12 || cmd == 0x21;
}

static bool cmd_read(uint8_t cmd)
{
	return cmd == 0x03 || (chip.addr4 && cmd == 0x13);
}

static bool cmd_program(uint8_t cmd)
{
	return cmd == 0x02 || (chip.addr4 && cmd == 0x12);
}

static bool cmd_erase(uint8_t cmd)
{
	return cmd == 0x20 || (chip.addr4 && cmd == 0x21);
}

// One program or erase starts: false when the power goes in the middle of it
static bool chip_op_survives(void)
{
	if (chip.ops_left == 0) {
		return true;
	}
	if (--chip.ops_left == 0) {
		chip.cut = true;
		return false;
	}
	return true;
}

static void chip_execute(void)
{
	if (chip.deaf) {
		return;
	}
	if (chip.cmd == 0xC5 && chip.addr4 && chip.pos == 2 && chip.wel && chip.busy == 0) {
		chip.ext = (uint8_t)chip.addr;	// the latch stays set: the data sheet's list of what clears it has no C5
		return;
	}
	if (chip.sus) {
		return;		// (simpler than the chip: nothing is programmed or erased while an operation is suspended)
	}
	if (cmd_program(chip.cmd) && chip.pos >= 1u + chip.alen && chip.wel && chip.busy == 0) {
		uint32_t base = (chip.addr % chip.size) & ~0xFFu;
		chip.high_ops += base >= (16u << 20);
		bool whole = chip_op_survives();
		unsigned done = 0;
		for (unsigned i = 0; i < 256; i++) {
			if (!chip.page_set[i]) {
				continue;
			}
			if (!whole && done++ >= chip.data_len / 2) {
				break;		// torn: only part of the page programmed
			}
			if ((chip.page[i] & ~chip.mem[base + i]) != 0) {
				chip.violations++;
			}
			chip.mem[base + i] &= chip.page[i];
		}
		chip.programs++;
		chip.wel = false;
		chip.busy = chip.busy_polls;
	} else if (cmd_erase(chip.cmd) && chip.pos == 1u + chip.alen && chip.wel && chip.busy == 0) {
		uint32_t base = (chip.addr % chip.size) & ~0xFFFu;
		chip.high_ops += base >= (16u << 20);
		if (chip_op_survives()) {
			memset(&chip.mem[base], 0xFF, 4096);
		} else {
			memset(&chip.mem[base], 0xFF, 1024);	// torn: part erased, the rest scrambled
			for (uint32_t i = 1024; i < 4096; i++) {
				chip.mem[base + i] ^= (uint8_t)(i * 29u);
			}
		}
		chip.erases++;
		chip.wel = false;
		chip.busy = chip.busy_polls;
	}
}

static void bus_select(bool active)
{
	if (active) {
		chip.selects++;
		chip.selected = true;
		chip.pos = 0;
		chip.data_len = 0;
		memset(chip.page_set, 0, sizeof(chip.page_set));
		chip.deaf = chip.dead_us != 0;
		if (chip.deaf) {
			chip.early++;
			early_total++;
		}
	} else {
		chip.deselects++;
		if (chip.selected && !chip.cut) {
			chip_execute();
		}
		chip.selected = false;
	}
}

static uint8_t chip_byte(uint8_t in)
{
	uint32_t p = chip.pos++;
	if (p == 0) {
		cmds_seen[in]++;
		chip.seen[in]++;
	}
	if (chip.deaf) {
		return 0xFF;
	}
	if (p == 0) {
		bool armed = chip.reset_armed;
		chip.reset_armed = in == 0x66;
		chip.cmd = in;
		chip.addr = 0;
		chip.alen = chip.ads || (chip.addr4 && cmd4(in)) ? 4 : 3;
		if (in == 0x06 && !chip.protect && chip.busy == 0) {
			chip.wel = true;
		} else if (in == 0x04 && chip.busy == 0) {
			chip.wel = false;
		} else if (in == 0x99 && armed) {
			chip_reset();
		}
		return 0xFF;
	}
	if (cmd4(chip.cmd) && !chip.addr4) {
		return 0xFF;	// a part of 16 MB or less doesn't know them
	}
	switch (chip.cmd) {
	case 0x05:
	case 0x35:
	case 0x15: {
		int which = chip.cmd == 0x05 ? 0 : chip.cmd == 0x35 ? 1 : 2;
		chip.status_reads[which]++;
		if (which == 1) {
			return (uint8_t)(0x10 | (chip.sus ? 0x80 : 0));	// SUS is bit 7
		}
		if (which == 2) {
			return (uint8_t)(0x20 | (chip.adp ? 0x02 : 0) | (chip.ads ? 0x01 : 0));	// ADP bit 1, ADS bit 0
		}
		uint8_t st = (uint8_t)((chip.busy != 0 ? 0x01 : 0) | (chip.wel ? 0x02 : 0));
		if (chip.busy != 0 && chip.busy != UINT32_MAX) {
			chip.busy--;
		}
		return st;
	}
	case 0x9F:
		// (a busy chip answers its status and nothing else)
		return p <= 3 && chip.busy == 0 ? chip.id[p - 1] : 0xFF;
	case 0x4B: {
		uint32_t first = chip.ads ? 6 : 5;	// four dummy bytes, five in 4-byte address mode, then the ID
		return p >= first && p < first + sizeof(chip.uid) ? chip.uid[p - first] : 0xFF;
	}
	case 0xC5:
		if (p == 1) {
			chip.addr = in;
		}
		return 0xFF;
	case 0x03:
	case 0x02:
	case 0x20:
	case 0x48:
	case 0x13:
	case 0x12:
	case 0x21:
		if (p <= chip.alen) {
			chip.addr = (chip.addr << 8) | in;
			if (p == chip.alen && chip.alen == 4) {
				chip.ext = (uint8_t)(chip.addr >> 24);	// every command with four address bytes replaces it
			} else if (p == chip.alen && chip.cmd != 0x48) {
				chip.addr |= (uint32_t)chip.ext << 24;	// with three, it is the fourth
			}
			return 0xFF;
		}
		if (cmd_read(chip.cmd)) {
			return chip.busy != 0 ? 0xFF : chip.mem[chip.addr++ % chip.size];
		}
		if (chip.cmd == 0x48) {
			if (p == chip.alen + 1u) {
				return 0xFF;	// dummy
			}
			uint32_t a = chip.addr++;
			unsigned reg = (a >> 12) & 0xF;
			return reg >= 1 && reg <= 3 ? chip.security[reg - 1][a & 0xFF] : 0xFF;
		}
		if (cmd_program(chip.cmd)) {
			uint32_t off = (chip.addr + chip.data_len) & 0xFFu;	// wraps inside the page
			if (chip.data_len == 256u - (chip.addr & 0xFFu)) {
				chip.page_overruns++;
			}
			chip.page[off] = in;
			chip.page_set[off] = true;
			chip.data_len++;
		}
		return 0xFF;
	default:
		return 0xFF;
	}
}

static bool bus_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	if (chip.fail_xfer_at != 0 && --chip.fail_xfer_at == 0) {
		return false;
	}
	if (chip.cut) {
		return false;
	}
	if (!chip.selected) {
		chip.outside++;
		return false;
	}
	for (uint32_t i = 0; i < len; i++) {
		uint8_t out = chip_byte(tx != NULL ? tx[i] : 0xFF);
		if (rx != NULL) {
			rx[i] = out;
		}
	}
	return true;
}

static const spi_bus_t bus = { .select = bus_select, .xfer = bus_xfer };

// ---- The driver ----

static void test_probe(void)
{
	spi_nor_t nor = { .bus = &bus };
	static const struct {
		uint8_t mfr, type, cap;
		uint32_t size;
		bool ok;
		uint32_t size4;		// the size with SPI_NOR_ADDR4 where it is another, and then 4-byte addresses
	} ids[] = {
		{ 0xEF, 0x40, 0x18, 16u << 20, true, 0 },	// W25Q128JV: 16 MB
		{ 0xC8, 0x40, 0x18, 16u << 20, true, 0 },	// GD25Q128
		{ 0xEF, 0x40, 0x17, 8u << 20, true, 0 },	// W25Q64: 8 MB
		{ 0xEF, 0x40, 0x14, 1u << 20, true, 0 },	// W25Q80: 1 MB
		{ 0xEF, 0x40, 0x10, 64u << 10, true, 0 },	// W25X05 class: 64 KB
		{ 0xEF, 0x40, 0x0F, 0, true, 0 },		// capacity code not known: size 0
		{ 0x85, 0x20, 0x01, 0, true, 0 },
		// Larger than 3-byte addresses reach: the driver can only address the first 16 MB (above that a read
		// or a program would wrap to the bottom of the chip, where the codeplug and the calibration are). With
		// SPI_NOR_ADDR4 it addresses all of the part
		{ 0xEF, 0x40, 0x19, 16u << 20, true, 32u << 20 },	// W25Q256: 32 MB
		{ 0xEF, 0x40, 0x20, 16u << 20, true, 64u << 20 },	// W25Q512: 64 MB (Winbond's code after 0x19 is 0x20)
		{ 0xEF, 0x40, 0x21, 16u << 20, true, 128u << 20 },	// W25Q01: 128 MB
		{ 0xEF, 0x40, 0x22, 16u << 20, true, 256u << 20 },	// W25Q02: 256 MB
		{ 0xC8, 0x40, 0x1A, 16u << 20, true, 64u << 20 },	// other makers count on from 0x19: 2^n bytes
		{ 0xC8, 0x40, 0x1C, 16u << 20, true, 256u << 20 },
		{ 0xC8, 0x40, 0x1D, 16u << 20, true, 256u << 20 },	// past what the driver knows: used as the largest
		{ 0xEF, 0x40, 0x23, 0, true, 0 },				// no such code: size 0
		{ 0x00, 0x00, 0x00, 0, false, 0 },		// bus stuck low
		{ 0xFF, 0xFF, 0xFF, 0, false, 0 },		// nothing drives MISO
		{ 0x00, 0x00, 0x18, 0, false, 0 },
	};
	for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		chip_create(64u << 10, ids[i].mfr, ids[i].type, ids[i].cap);
		nor.size = 123;
		bool ok = spi_nor_probe(&nor);
		CHECK(ok == ids[i].ok);
		uint32_t want = ids[i].size;
#if SPI_NOR_ADDR4
		want = ids[i].size4 != 0 ? ids[i].size4 : want;
#endif
		if (ok != ids[i].ok || nor.size != want) {
			printf("FAIL probe %02X %02X %02X: ok %d size 0x%X (want %d 0x%X)\n", ids[i].mfr, ids[i].type,
			       ids[i].cap, ok, nor.size, ids[i].ok, want);
			failures++;
		}
		if (ok) {
			CHECK(nor.manufacturer == ids[i].mfr && nor.device == ((ids[i].type << 8) | ids[i].cap));
		} else {
			CHECK(nor.manufacturer == 0 && nor.device == 0 && nor.size == 0);
		}
#if SPI_NOR_ADDR4
		// Status 1 and 2, the reset (once, to a chip that is neither busy nor suspended, and waited out), the ID,
		// and status 3 for the address mode of a part that has one
		bool big = ok && ids[i].size4 != 0;
		CHECK(chip.selects == (big ? 6u : 5u) && chip.deselects == chip.selects && chip.outside == 0);
		CHECK(chip.resets == 1 && chip.reset_harm == 0 && chip.early == 0);
		CHECK(chip.status_reads[0] == 1 && chip.status_reads[1] == 1 && chip.status_reads[2] == (big ? 1u : 0u));
		CHECK(nor.addr_len == (big ? 4 : 3) && !nor.ads && !nor.unsettled);
#else
		CHECK(chip.selects == 1 && chip.deselects == 1 && chip.outside == 0);
#endif
	}
	// The bus fails: false, nothing filled in, chip select released
	chip_create(64u << 10, 0xEF, 0x40, 0x18);
	chip.fail_xfer_at = 1;
	CHECK(!spi_nor_probe(&nor) && nor.size == 0 && nor.manufacturer == 0);
	CHECK(chip.selects == chip.deselects && !chip.selected);
	chip.fail_xfer_at = 2;			// the command goes out, the ID doesn't come back
	CHECK(!spi_nor_probe(&nor) && nor.size == 0);
	CHECK(chip.selects == chip.deselects && !chip.selected);
#if SPI_NOR_ADDR4
	// The longer probe of a part with 4-byte addresses, the bus failing in each of its ten transfers
	chip_create(64u << 10, 0xEF, 0x40, 0x19);
	for (uint32_t k = 1; k <= 10; k++) {
		hal_delay_ms(1);			// (time passes: a reset before the failure is over)
		chip.fail_xfer_at = k;
		CHECK(!spi_nor_probe(&nor) && nor.size == 0 && nor.manufacturer == 0 && nor.device == 0 && !nor.unsettled);
		CHECK(chip.fail_xfer_at == 0 && chip.selects == chip.deselects && !chip.selected);
	}
	hal_delay_ms(1);
	chip.fail_xfer_at = 11;
	CHECK(spi_nor_probe(&nor) && nor.size == (32u << 20) && chip.fail_xfer_at == 1);
	chip.fail_xfer_at = 0;
	CHECK(chip.reset_harm == 0 && chip.early == 0);
#endif
}

static void test_read_program_erase(void)
{
	spi_nor_t nor = { .bus = &bus };
	uint8_t b[1024], r[1024];

	chip_create(1u << 20, 0xEF, 0x40, 0x14);
	CHECK(spi_nor_probe(&nor) && nor.size == (1u << 20));
	// Zero lengths: no bus traffic
	uint32_t s0 = chip.selects;
	CHECK(spi_nor_read(&nor, 0x100, r, 0) && spi_nor_program(&nor, 0x100, b, 0) &&
	      spi_nor_read_security(&nor, 0x1000, r, 0));
	CHECK(chip.selects == s0);

	// A program across three pages (16 + 256 + 28 bytes): split at the page ends, each page after a write enable
	for (unsigned i = 0; i < sizeof(b); i++) {
		b[i] = (uint8_t)(i * 7u + 3u);
	}
	uint32_t p0 = chip.programs;
	CHECK(spi_nor_program(&nor, 0x30F0, b, 300));
	CHECK(chip.programs == p0 + 3 && chip.page_overruns == 0 && chip.violations == 0);
	CHECK(memcmp(&chip.mem[0x30F0], b, 300) == 0);
	CHECK(chip.mem[0x30EF] == 0xFF && chip.mem[0x30F0 + 300] == 0xFF && chip.mem[0x3000] == 0xFF);
	CHECK(!chip.wel && chip.busy == 0);	// waited until done
	// Read it back, also across the pages and in odd lengths
	memset(r, 0, sizeof(r));
	CHECK(spi_nor_read(&nor, 0x30F0, r, 300) && memcmp(r, b, 300) == 0);
	CHECK(spi_nor_read(&nor, 0x30F1, r, 1) && r[0] == b[1]);
	// A whole page, page aligned: one program
	p0 = chip.programs;
	CHECK(spi_nor_program(&nor, 0x5000, b, 256) && chip.programs == p0 + 1);
	// Programming again only clears bits (the driver doesn't erase): the caller's business
	uint8_t z = 0x0F;
	CHECK(spi_nor_program(&nor, 0x5000, &z, 1) && chip.mem[0x5000] == (b[0] & 0x0F));
	chip.violations = 0;			// (that one was on purpose)

	// Sector erase: the sector holding addr, aligned down; the neighbors untouched
	memset(&chip.mem[0x6FFF], 0x00, 0x1002);
	CHECK(spi_nor_erase_sector(&nor, 0x7ABC));
	CHECK(chip.mem[0x6FFF] == 0x00 && chip.mem[0x7000] == 0xFF && chip.mem[0x7FFF] == 0xFF && chip.mem[0x8000] == 0x00);
	CHECK(!chip.wel && chip.selects == chip.deselects && chip.outside == 0);

	// Busy waits: the erase may take long (1 ms polls, up to the data sheet's 400 ms and some), a program not
	delays_ms = 0;
	chip.busy_polls = 300;
	CHECK(spi_nor_erase_sector(&nor, 0x9000) && delays_ms >= 299 && delays_ms <= 300);
	delays_ms = 0;
	chip.busy_polls = 30;			// a program that doesn't finish within its limit
	CHECK(!spi_nor_program(&nor, 0xA000, b, 4));
	CHECK(delays_ms > 3 && delays_ms < 30);
	chip.busy_polls = UINT32_MAX;		// stuck busy: the erase gives up instead of hanging
	chip.busy = 0;
	delays_ms = 0;
	CHECK(!spi_nor_erase_sector(&nor, 0xB000));
	CHECK(delays_ms >= 400 && delays_ms <= 1001);
	chip.busy = 0;
	chip.busy_polls = 2;

	// Write protected: the write enable latch doesn't set, nothing is sent
	chip.protect = true;
	p0 = chip.programs;
	uint32_t e0 = chip.erases;
	CHECK(!spi_nor_program(&nor, 0xC000, b, 16) && !spi_nor_erase_sector(&nor, 0x1000));
	CHECK(chip.programs == p0 && chip.erases == e0 && chip.mem[0xC000] == 0xFF);
	chip.protect = false;

	// A bus failure in each transfer of a program: false, chip select always released
	for (uint32_t k = 1; k <= 8; k++) {
		chip.fail_xfer_at = k;
		bool ok = spi_nor_program(&nor, 0xD0F0, b, 32);	// two pages
		CHECK(!ok || chip.fail_xfer_at != 0);
		CHECK(chip.selects == chip.deselects && !chip.selected);
		chip.fail_xfer_at = 0;
		chip.busy = 0;
		chip.wel = false;
	}
	for (uint32_t k = 1; k <= 4; k++) {
		chip.fail_xfer_at = k;
		CHECK(!spi_nor_erase_sector(&nor, 0xE000) || chip.fail_xfer_at != 0);
		chip.fail_xfer_at = 0;
		chip.busy = 0;
		chip.wel = false;
	}
	chip.fail_xfer_at = 2;			// the read's data phase
	CHECK(!spi_nor_read(&nor, 0, r, 16));
	chip.fail_xfer_at = 0;
	CHECK(chip.selects == chip.deselects && chip.outside == 0);

	// Status registers 1..3; anything else refused without bus traffic
	uint8_t st = 0xAA;
	CHECK(spi_nor_status(&nor, 1, &st) && st == 0x00);
	CHECK(spi_nor_status(&nor, 2, &st) && st == 0x10);
	CHECK(spi_nor_status(&nor, 3, &st) && st == 0x20);
	s0 = chip.selects;
	CHECK(!spi_nor_status(&nor, 0, &st) && !spi_nor_status(&nor, 4, &st) && chip.selects == s0);

	// Security registers (W25Q: reg << 12 | offset, a dummy byte after the address)
	CHECK(spi_nor_read_security(&nor, (2u << 12) | 0x10, r, 4));
	CHECK(r[0] == 0x50 && r[1] == 0x51 && r[3] == 0x53);
	CHECK(spi_nor_read_security(&nor, (3u << 12) | 0xFE, r, 2) && r[0] == 0x7E && r[1] == 0x7F);

	// Unique ID (0x4B, four dummy bytes, 16 bytes)
	uint8_t id[SPI_NOR_UID_LEN];
	for (unsigned i = 0; i < sizeof(chip.uid); i++) {
		chip.uid[i] = (uint8_t)(0xA0 + i);
	}
	CHECK(spi_nor_read_uid(&nor, id) && id[0] == 0xA0 && id[15] == 0xAF);
	CHECK(chip.violations == 0 && chip.page_overruns == 0);
}

// ---- core/storage and core/settings on the driver, as a board wires them ----

static spi_nor_t board_nor = { .bus = &bus };
static bool board_ready;

static bool f_init(void)
{
	board_ready = spi_nor_probe(&board_nor) && board_nor.size != 0;
	return board_ready;
}
static bool in_range(uint32_t addr, uint32_t len)
{
	return board_ready && addr < board_nor.size && len <= board_nor.size - addr;
}
#if SPI_NOR_ADDR4
// As the platform glue (platform/stm32/spi_flash) ends a transaction on a part with 4-byte addresses: the driver's
// parked calls, which it makes. One that worked leaves the register 0 and the chip write disabled, whatever the
// transaction was
static bool parked(bool ok)
{
	CHECK(!ok || (chip.ext == 0 && !chip.wel));
	return ok;
}
static bool f_read(uint32_t addr, void *buf, uint32_t len)
{
	return in_range(addr, len) && parked(spi_nor_read_parked(&board_nor, addr, buf, len));
}
static bool f_write(uint32_t addr, const void *buf, uint32_t len)
{
	return in_range(addr, len) && parked(spi_nor_program_parked(&board_nor, addr, buf, len));
}
static bool f_erase(uint32_t addr)
{
	return in_range(addr, 1) && parked(spi_nor_erase_sector_parked(&board_nor, addr));
}
#else
static bool f_read(uint32_t addr, void *buf, uint32_t len)
{
	return in_range(addr, len) && spi_nor_read(&board_nor, addr, buf, len);
}
static bool f_write(uint32_t addr, const void *buf, uint32_t len)
{
	return in_range(addr, len) && spi_nor_program(&board_nor, addr, buf, len);
}
static bool f_erase(uint32_t addr)
{
	return in_range(addr, 1) && spi_nor_erase_sector(&board_nor, addr);
}
#endif

// The DM-1701's map (docs/spec/settings-store.md 1)
static const hal_flash_map_t map16 = {
	.settings_addr = 0x011000u, .settings_size = 0x8000u, .journal_addr = 0x019000u,
	.openfw_eeprom = 0, .openfw_flash = 0x020000u, .openfw_end = 0xE00000u,
	.calibration_addr = 0x010000u, .calibration_size = 0x200u,
	.native_addr = 0xE00000u, .native_size = 0x100000u,
	.staging_addr = 0xF00000u, .staging_size = 0x40000u,
};

static hal_flash_t flash = {
	.init = f_init, .erase_size = SPI_NOR_SECTOR_SIZE, .read = f_read, .write = f_write, .erase = f_erase,
	.map = &map16,
};

const board_t board = { .name = "spinor", .flash = &flash };

// A start that the chip's power did not go away for (a reset of the MCU): the chip is as it was left
static bool warm_boot(void)
{
	chip.selected = false;
	flash.size = flash.init() ? board_nor.size : 0;
	bool ok = storage_init();
	settings_init();
	return ok;
}

static bool boot(void)
{
	chip_power_on();
	return warm_boot();
}

static void fill(uint8_t *b, uint8_t seed)
{
	for (unsigned i = 0; i < STORAGE_SECTOR; i++) {
		b[i] = (uint8_t)(seed ^ (i * 11u) ^ (i >> 5));
	}
}

static uint8_t cal[0x200];

static void put_calibration(void)
{
	for (unsigned i = 0; i < sizeof(cal); i++) {
		cal[i] = (uint8_t)(i * 3u + 1u);
	}
	memcpy(&chip.mem[map16.calibration_addr], cal, sizeof(cal));
}

static bool calibration_intact(void)
{
	return memcmp(&chip.mem[map16.calibration_addr], cal, sizeof(cal)) == 0;
}

static void test_store_16mb(void)
{
	uint8_t a[STORAGE_SECTOR], b[STORAGE_SECTOR];
	settings_t s;

	chip_create(16u << 20, 0xEF, 0x40, 0x18);
	put_calibration();
	CHECK(boot() && flash.size == (16u << 20));
	// The top of the chip: a staging sector rewrite and a native area erase reach the last megabyte
	fill(a, 1);
	CHECK(storage_sector_write(0xFFF000u, a) && memcmp(&chip.mem[0xFFF000u], a, sizeof(a)) == 0);
	CHECK(chip.mem[0] == 0xFF && chip.mem[0xFFF] == 0xFF);	// no wrap to the bottom
	static const uint8_t magic[4] = { 0x4F, 0x44, 0x52, 0x4E };
	CHECK(f_write(map16.native_addr, magic, 4) && f_write(map16.native_addr + 0xFF000u, a, 64));
	CHECK(storage_native_erase() == STORAGE_NATIVE_ERASED);
	CHECK(chip.mem[map16.native_addr] == 0xFF && chip.mem[map16.native_addr + 0xFF000u] == 0xFF);
	CHECK(!storage_sector_write(16u << 20, a));		// past the end
	// Settings round trip through the driver
	settings_begin()->squelch = 6;
	settings_end(1);
	CHECK(settings_flush());
	CHECK(boot());
	settings_read(&s);
	CHECK(s.squelch == 6);
	// A codeplug range across a sector end
	fill(b, 9);
	CHECK(storage_write(0x20FF0u, b, 64) && memcmp(&chip.mem[0x20FF0u], b, 64) == 0);
	CHECK(calibration_intact() && chip.violations == 0 && chip.page_overruns == 0);
#if SPI_NOR_ADDR4
	// A 16 MB part under a driver built for larger ones: the commands it always got, and none it doesn't know
	CHECK(board_nor.addr_len == 3 && !board_nor.ads);
	CHECK(chip.seen[0x03] != 0 && chip.seen[0x02] != 0 && chip.seen[0x20] != 0);
	CHECK(chip.seen[0x13] == 0 && chip.seen[0x12] == 0 && chip.seen[0x21] == 0);
	CHECK(chip.seen[0xC5] == 0 && chip.seen[0x04] == 0);
#endif
}

// A 1 MB part with the 16 MB part's map: the areas past its end are refused, the low ones work, and nothing wraps
// onto the bottom of the chip
static void test_store_small_part(void)
{
	uint8_t a[STORAGE_SECTOR];

	chip_create(1u << 20, 0xEF, 0x40, 0x14);
	put_calibration();
	uint8_t low[64];
	memset(low, 0x5A, sizeof(low));
	memcpy(&chip.mem[0x0], low, sizeof(low));		// the open-firmware "EEPROM" image's first bytes
	CHECK(boot() && flash.size == (1u << 20));
	fill(a, 4);
	CHECK(!storage_sector_write(0xF00000u, a));		// the staging area isn't there
	CHECK(storage_native_erase() == STORAGE_NATIVE_FAILED);
	CHECK(!storage_write(0xFFFFF0u, a, 32));
	CHECK(storage_sector_write(0x21000u, a) && memcmp(&chip.mem[0x21000u], a, sizeof(a)) == 0);
	settings_begin()->squelch = 2;
	settings_end(1);
	CHECK(settings_flush());
	CHECK(memcmp(&chip.mem[0x0], low, sizeof(low)) == 0 && calibration_intact());
	CHECK(chip.violations == 0);
}

// Power cuts at every program and erase of a settings save and of a codeplug sector rewrite, with the page
// programs and erases torn as the chip tears them: after the next boot the old or the new content, whole; the
// calibration copy untouched every time
static void test_store_cuts(void)
{
	uint8_t old[STORAGE_SECTOR], neu[STORAGE_SECTOR];
	const uint32_t target = 0x40000u;
	settings_t s;

	fill(old, 0x11);
	fill(neu, 0xC3);
	// How many operations a rewrite takes
	chip_create(16u << 20, 0xEF, 0x40, 0x18);
	put_calibration();
	boot();
	storage_sector_write(target, old);
	uint32_t ops0 = chip.programs + chip.erases;
	storage_sector_write(target, neu);
	uint32_t total = chip.programs + chip.erases - ops0;
	uint8_t *snap = malloc(16u << 20);
	int old_seen = 0, new_seen = 0;
	for (uint32_t cut = 1; cut <= total; cut++) {
		chip_create(16u << 20, 0xEF, 0x40, 0x18);
		put_calibration();
		boot();
		storage_sector_write(target, old);
		chip.ops_left = cut;
		bool ok = storage_sector_write(target, neu);
		CHECK(!ok || !chip.cut);
		boot();
		if (memcmp(&chip.mem[target], neu, STORAGE_SECTOR) == 0) {
			new_seen++;
		} else if (memcmp(&chip.mem[target], old, STORAGE_SECTOR) == 0) {
			old_seen++;
		} else {
			printf("FAIL spi_nor journal: cut at operation %u of %u leaves a mixed sector\n", cut, total);
			failures++;
		}
		CHECK(calibration_intact());
	}
	CHECK(old_seen > 0 && new_seen > 0);

	// A settings save, at a sector change (the save that erases the next ring sector) and inside a sector
	for (int variant = 0; variant < 2; variant++) {
		uint32_t saves_before = variant == 0 ? 15 : 3;	// (slot 0 holds the ownership mark)
		uint32_t n_ops = 0;
		old_seen = new_seen = 0;
		// cut 0: no cut, counts the operations of the save; the last cut point is past them (no cut either)
		for (uint32_t cut = 0; cut == 0 || cut <= n_ops + 1; cut++) {
			chip_create(16u << 20, 0xEF, 0x40, 0x18);
			put_calibration();
			boot();
			for (uint32_t i = 1; i <= saves_before; i++) {
				settings_begin()->vfo_rx_hz = 430000000u + i;
				settings_end(i);
				settings_flush();
			}
			memcpy(snap, chip.mem, 16u << 20);
			ops0 = chip.programs + chip.erases;
			chip.ops_left = cut;
			settings_begin()->vfo_rx_hz = 145000000u;
			settings_end(100);
			settings_flush();
			if (cut == 0) {
				n_ops = chip.programs + chip.erases - ops0;
			}
			boot();
			settings_read(&s);
			if (s.vfo_rx_hz == 145000000u) {
				new_seen++;
			} else if (s.vfo_rx_hz == 430000000u + saves_before) {
				old_seen++;
			} else {
				printf("FAIL spi_nor settings: cut %u after %u saves loads %u\n", cut, saves_before, s.vfo_rx_hz);
				failures++;
			}
			CHECK(calibration_intact());
			// Nothing outside the ring changed
			CHECK(memcmp(snap, chip.mem, map16.settings_addr) == 0);
			CHECK(memcmp(snap + map16.journal_addr, chip.mem + map16.journal_addr,
				     (16u << 20) - map16.journal_addr) == 0);
		}
		CHECK(new_seen > 0 && old_seen > 0);
	}
	free(snap);
	CHECK(chip.violations == 0 && chip.page_overruns == 0);
}

#if SPI_NOR_ADDR4
// ---- A part larger than 16 MB ----

#define MB16	(16u << 20)
#define MB32	(32u << 20)

// A W25Q256JV: 32 MB
static void big_create(void)
{
	chip_create(MB32, 0xEF, 0x40, 0x19);
	for (unsigned i = 0; i < sizeof(chip.uid); i++) {
		chip.uid[i] = (uint8_t)(0xA0 + i);
	}
}

static bool all_bytes(uint32_t from, uint32_t to, uint8_t v)
{
	for (uint32_t a = from; a < to; a++) {
		if (chip.mem[a] != v) {
			return false;
		}
	}
	return true;
}

// The security registers and the unique ID as the model holds them (chip_create, big_create)
static bool ids_read_right(const spi_nor_t *nor)
{
	uint8_t r[4], id[SPI_NOR_UID_LEN];
	return spi_nor_read_security(nor, (2u << 12) | 0x10, r, 4) && r[0] == 0x50 && r[1] == 0x51 && r[3] == 0x53 &&
	       spi_nor_read_security(nor, (3u << 12) | 0xFE, r, 2) && r[0] == 0x7E && r[1] == 0x7F &&
	       spi_nor_read_uid(nor, id) && id[0] == 0xA0 && id[15] == 0xAF;
}

// Reads, programs and erases land where asked in both halves. The same offset 16 MB apart tells a command that lost
// its fourth address byte, or took it from the extended address register, from one that didn't
static void addr4_array(const spi_nor_t *nor)
{
	uint8_t b[600], r[600];
	const uint32_t lo = 0x0030F0u, hi = MB16 + 0x0030F0u;

	for (unsigned i = 0; i < sizeof(b); i++) {
		b[i] = (uint8_t)(i * 13u + 5u);
	}
	uint32_t p0 = chip.programs;
	CHECK(spi_nor_program(nor, hi, b, 300) && chip.programs == p0 + 3);	// 16 + 256 + 28 bytes, as below 16 MB
	CHECK(memcmp(&chip.mem[hi], b, 300) == 0 && all_bytes(0x3000u, 0x5000u, 0xFF));
	CHECK(chip.mem[hi - 1] == 0xFF && chip.mem[hi + 300] == 0xFF);
	CHECK(chip.ext == 0x01);		// left where the address put it: ending the transaction is the caller's
	CHECK(spi_nor_program(nor, lo, b + 300, 300));
	CHECK(memcmp(&chip.mem[lo], b + 300, 300) == 0 && memcmp(&chip.mem[hi], b, 300) == 0);
	CHECK(chip.ext == 0x00);
	memset(r, 0, sizeof(r));
	CHECK(spi_nor_read(nor, hi, r, 300) && memcmp(r, b, 300) == 0);
	CHECK(spi_nor_read(nor, lo, r, 300) && memcmp(r, b + 300, 300) == 0);
	CHECK(spi_nor_read(nor, hi + 1, r, 1) && r[0] == b[1]);
	// Across the 16 MB line (a page ends there) and up to the last byte of the chip
	CHECK(spi_nor_program(nor, MB16 - 16, b, 48) && memcmp(&chip.mem[MB16 - 16], b, 48) == 0);
	CHECK(spi_nor_read(nor, MB16 - 16, r, 48) && memcmp(r, b, 48) == 0);
	CHECK(spi_nor_program(nor, MB32 - 4, b, 4) && memcmp(&chip.mem[MB32 - 4], b, 4) == 0);
	CHECK(spi_nor_read(nor, MB32 - 4, r, 4) && memcmp(r, b, 4) == 0);
	CHECK(all_bytes(0, 0x1000u, 0xFF));	// nothing wrapped to the bottom
	// Erase: the sector asked for, in the half asked for
	memset(&chip.mem[0x6FFFu], 0x00, 0x1002);
	memset(&chip.mem[MB16 + 0x6FFFu], 0x00, 0x1002);
	CHECK(spi_nor_erase_sector(nor, MB16 + 0x7ABCu));
	CHECK(all_bytes(MB16 + 0x7000u, MB16 + 0x8000u, 0xFF) && chip.mem[MB16 + 0x6FFFu] == 0x00 &&
	      chip.mem[MB16 + 0x8000u] == 0x00 && all_bytes(0x6FFFu, 0x8001u, 0x00));
	CHECK(spi_nor_erase_sector(nor, 0x7ABCu));
	CHECK(all_bytes(0x7000u, 0x8000u, 0xFF) && chip.mem[0x6FFFu] == 0x00 && chip.mem[0x8000u] == 0x00);
	CHECK(!chip.wel && chip.busy == 0);
	// Only the opcodes that take four address bytes whatever the mode: none that depends on it or changes it
	CHECK(chip.seen[0x13] != 0 && chip.seen[0x12] != 0 && chip.seen[0x21] != 0);
	CHECK(chip.seen[0x03] == 0 && chip.seen[0x02] == 0 && chip.seen[0x20] == 0);
	CHECK(chip.seen[0xB7] == 0 && chip.seen[0xE9] == 0);
	CHECK(chip.violations == 0 && chip.page_overruns == 0 && chip.selects == chip.deselects && chip.outside == 0);
}

static void test_addr4_array(void)
{
	spi_nor_t nor = { .bus = &bus };
	uint8_t b[32];

	big_create();
	CHECK(spi_nor_probe(&nor) && nor.size == MB32 && nor.addr_len == 4 && !nor.ads && !nor.unsettled);
	addr4_array(&nor);
	CHECK(ids_read_right(&nor));
	// The limits and the failures of the 3-byte commands hold for these: an erase that never ends, a refused write
	// enable, the bus failing in each transfer
	memset(b, 0x5A, sizeof(b));
	chip.busy_polls = UINT32_MAX;
	delays_ms = 0;
	CHECK(!spi_nor_erase_sector(&nor, MB16 + 0xB000u) && delays_ms >= 400 && delays_ms <= 1001);
	chip.busy = 0;
	chip.busy_polls = 2;
	chip.wel = false;
	chip.protect = true;
	uint32_t ops = chip.programs + chip.erases;
	CHECK(!spi_nor_program(&nor, MB16 + 0xC000u, b, 16) && !spi_nor_erase_sector(&nor, MB16 + 0x1000u));
	CHECK(chip.programs + chip.erases == ops && chip.mem[MB16 + 0xC000u] == 0xFF);
	chip.protect = false;
	for (uint32_t k = 1; k <= 8; k++) {
		chip.fail_xfer_at = k;
		bool ok = spi_nor_program(&nor, MB16 + 0xD0F0u, b, 32);	// two pages
		CHECK(!ok || chip.fail_xfer_at != 0);
		CHECK(chip.selects == chip.deselects && !chip.selected);
		chip.fail_xfer_at = 0;
		chip.busy = 0;
		chip.wel = false;
	}
	chip.fail_xfer_at = 2;
	CHECK(!spi_nor_read(&nor, MB16, b, 16));
	chip.fail_xfer_at = 0;
}

// The chip as other firmware left it when the MCU was reset. The probe's reset returns it to its power-up state, and
// what the driver then reads is what is there
static void test_addr4_left_state(void)
{
	spi_nor_t nor = { .bus = &bus };
	uint8_t r[4];

	big_create();
	chip.mem[0x1234u] = 0x11;
	chip.mem[MB16 + 0x1234u] = 0x22;
	// In 4-byte address mode (0xB7), the register from its last address, the write enable latch set
	chip.ads = true;
	chip.ext = 0x01;
	chip.wel = true;
	CHECK(spi_nor_probe(&nor) && nor.size == MB32 && nor.addr_len == 4 && !nor.ads && !nor.unsettled);
	CHECK(!chip.ads && chip.ext == 0 && !chip.wel && chip.resets == 1 && chip.reset_harm == 0 && chip.early == 0);
	CHECK(spi_nor_read(&nor, 0x1234u, r, 1) && r[0] == 0x11);
	CHECK(spi_nor_read(&nor, MB16 + 0x1234u, r, 1) && r[0] == 0x22);
	CHECK(ids_read_right(&nor));		// (in the mode it was left in these would have taken another form)
	// In 3-byte mode with the register set (it read the top half with 0xC5 and 0x03)
	chip.ext = 0x01;
	CHECK(spi_nor_probe(&nor) && chip.ext == 0 && chip.resets == 2);
	CHECK(spi_nor_read(&nor, 0x1234u, r, 1) && r[0] == 0x11);
	CHECK(chip.reset_harm == 0 && chip.early == 0);
}

static void test_addr4_park(void)
{
	spi_nor_t nor = { .bus = &bus };
	uint8_t b[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, r[8];

	big_create();
	CHECK(spi_nor_probe(&nor) && nor.addr_len == 4);
	CHECK(spi_nor_program(&nor, MB16 + 0x100u, b, 8) && chip.ext == 0x01);
	uint32_t s0 = chip.selects;
	CHECK(spi_nor_park(&nor) && chip.ext == 0 && !chip.wel);
	CHECK(chip.selects == s0 + 4 && chip.seen[0xC5] == 1 && chip.seen[0x04] == 1);	// 06, 05, C5 00, 04
	CHECK(spi_nor_read(&nor, MB16 + 0x100u, r, 8) && memcmp(r, b, 8) == 0 && chip.ext == 0x01);
	CHECK(spi_nor_park(&nor) && chip.ext == 0 && !chip.wel);
	CHECK(spi_nor_read(&nor, MB16 + 0x100u, r, 8) && memcmp(r, b, 8) == 0);	// (the park moved nothing)
	// The write enable refused: false, the register as it was
	chip.protect = true;
	CHECK(!spi_nor_park(&nor) && chip.ext == 0x01 && !chip.wel);
	chip.protect = false;
	// The bus failing in each of its five transfers: false, chip select released; then it works again
	for (uint32_t k = 1; k <= 5; k++) {
		chip.ext = 0x01;
		chip.fail_xfer_at = k;
		CHECK(!spi_nor_park(&nor) && chip.fail_xfer_at == 0);
		CHECK(chip.selects == chip.deselects && !chip.selected);
		chip.wel = false;
	}
	CHECK(spi_nor_park(&nor) && chip.ext == 0 && !chip.wel);
	// The parked calls the platform glue makes: a program and an erase park always, a failed one too (the bus fails
	// at the program's first transfer here, and the park after it works); a read only when it reached 16 MB
	uint32_t parks = chip.seen[0xC5];
	CHECK(spi_nor_program_parked(&nor, MB16 + 0x200u, b, 8) && chip.ext == 0 && !chip.wel);
	CHECK(spi_nor_program_parked(&nor, 0x200u, b, 8) && chip.seen[0xC5] == parks + 2);
	chip.ext = 0x01;
	chip.fail_xfer_at = 1;
	CHECK(!spi_nor_program_parked(&nor, MB16 + 0x300u, b, 8) && chip.ext == 0 && !chip.wel);
	CHECK(chip.seen[0xC5] == parks + 3);
	chip.ext = 0x01;
	chip.fail_xfer_at = 1;
	CHECK(!spi_nor_erase_sector_parked(&nor, MB16 + 0x1000u) && chip.ext == 0 && chip.seen[0xC5] == parks + 4);
	CHECK(spi_nor_erase_sector_parked(&nor, MB16 + 0x1000u) && chip.ext == 0 && chip.seen[0xC5] == parks + 5);
	CHECK(spi_nor_read_parked(&nor, MB16 - 8u, r, 8) && chip.seen[0xC5] == parks + 5);	// to the byte below
	CHECK(spi_nor_read_parked(&nor, MB16 - 7u, r, 8) && chip.ext == 0 && chip.seen[0xC5] == parks + 6);
	CHECK(spi_nor_read_parked(&nor, MB16 + 0x100u, r, 8) && memcmp(r, b, 8) == 0 && chip.ext == 0);
	CHECK(spi_nor_read_parked(&nor, MB16, r, 0) && chip.seen[0xC5] == parks + 7);	// nothing read: no park
	// A read that worked but whose park did not (the write enable refused): false, the register still set. Below
	// 16 MB nothing is parked, so the same refusal does not matter there
	chip.protect = true;
	CHECK(!spi_nor_read_parked(&nor, MB16 + 0x100u, r, 8) && memcmp(r, b, 8) == 0 && chip.ext == 0x01);
	CHECK(spi_nor_read_parked(&nor, 0x100u, r, 8) && chip.ext == 0);
	chip.protect = false;
	CHECK(chip.selects == chip.deselects && !chip.selected);
	// Never the commands that would change the address mode or a status register
	CHECK(chip.seen[0xB7] == 0 && chip.seen[0xE9] == 0);
	CHECK(chip.seen[0x01] == 0 && chip.seen[0x31] == 0 && chip.seen[0x11] == 0);
	// A part with 3-byte addresses has no such register: nothing is sent
	chip_create(MB16, 0xEF, 0x40, 0x18);
	CHECK(spi_nor_probe(&nor) && nor.addr_len == 3 && nor.size == MB16);
	s0 = chip.selects;
	CHECK(spi_nor_park(&nor) && chip.selects == s0);
}

// A chip that is busy or has a suspended operation when the driver starts: the MCU was reset in the middle of other
// firmware's erase or program, and the chip was not
static void test_addr4_probe_unsettled(void)
{
	spi_nor_t nor = { .bus = &bus };
	uint8_t a[STORAGE_SECTOR];

	// Busy: waited for, then reset
	big_create();
	chip.busy = 300;
	delays_ms = 0;
	CHECK(spi_nor_probe(&nor) && nor.size == MB32 && nor.addr_len == 4 && !nor.unsettled);
	CHECK(delays_ms >= 299 && delays_ms <= 300 && chip.resets == 1 && chip.reset_harm == 0 && chip.early == 0);
	// Busy past the longest erase: no reset, nothing but its status asked of it, not used
	big_create();
	chip.busy = UINT32_MAX;
	delays_ms = 0;
	CHECK(!spi_nor_probe(&nor) && nor.unsettled && nor.size == 0 && nor.manufacturer == 0 && nor.device == 0);
	CHECK(delays_ms >= 400 && delays_ms <= 1001 && chip.resets == 0);
	CHECK(chip.seen[0x66] == 0 && chip.seen[0x99] == 0 && chip.seen[0x9F] == 0 && chip.selects == chip.deselects);
	// A suspended operation: no reset and no wait; the chip is known by its ID and has no size
	big_create();
	chip.sus = true;
	delays_ms = 0;
	CHECK(spi_nor_probe(&nor) && nor.unsettled && nor.size == 0 && nor.manufacturer == 0xEF && nor.device == 0x4019);
	CHECK(chip.sus && chip.resets == 0 && chip.seen[0x66] == 0 && chip.seen[0x99] == 0 && delays_ms == 0);
	// As a board wires it: the flash did not start, and nothing reaches the chip
	fill(a, 7);
	CHECK(!warm_boot() && flash.size == 0 && !storage_ready());
	CHECK(!f_read(0x21000u, a, 16) && !f_write(0x21000u, a, 16) && !f_erase(0x21000u));
	CHECK(!storage_sector_write(0x21000u, a));
	CHECK(chip.sus && chip.resets == 0 && chip.programs == 0 && chip.erases == 0 && chip.seen[0x06] == 0);
	CHECK(all_bytes(0, 0x30000u, 0xFF));
	// A power cycle ends the suspended state: the chip starts
	CHECK(boot() && flash.size == MB32 && !board_nor.unsettled && storage_ready());
	CHECK(chip.reset_harm == 0 && chip.early == 0);
}

// A part other firmware set to power up in 4-byte address mode (ADP, non-volatile): used as any other. The array
// commands are the same; the security register and unique ID reads take the forms of that mode. The mode stays
static void test_addr4_adp(void)
{
	spi_nor_t nor = { .bus = &bus };
	uint8_t a[STORAGE_SECTOR];
	settings_t s;

	big_create();
	chip.adp = true;
	chip_power_on();
	CHECK(chip.ads);
	CHECK(spi_nor_probe(&nor) && nor.size == MB32 && nor.addr_len == 4 && nor.ads && !nor.unsettled);
	CHECK(chip.ads && chip.resets == 1 && chip.reset_harm == 0 && chip.early == 0);
	CHECK(ids_read_right(&nor));
	addr4_array(&nor);
	CHECK(chip.ext == 0x00 && spi_nor_read(&nor, MB32 - 1, a, 1) && chip.ext == 0x01);
	CHECK(spi_nor_park(&nor) && chip.ext == 0 && !chip.wel);
	// It entered 3-byte mode since power-up (0xE9): the reset puts it back, and the driver follows the chip
	chip.ads = false;
	CHECK(spi_nor_probe(&nor) && nor.ads && chip.ads && ids_read_right(&nor));
	// As a board wires it
	big_create();
	chip.adp = true;
	put_calibration();
	CHECK(boot() && flash.size == MB32 && board_nor.ads);
	settings_begin()->squelch = 3;
	settings_end(1);
	CHECK(settings_flush());
	fill(a, 5);
	CHECK(storage_sector_write(0xFFF000u, a) && memcmp(&chip.mem[0xFFF000u], a, sizeof(a)) == 0);
	CHECK(boot());
	settings_read(&s);
	CHECK(s.squelch == 3 && calibration_intact() && chip.high_ops == 0);
	CHECK(chip.adp && chip.ads && chip.seen[0xB7] == 0 && chip.seen[0xE9] == 0);
	CHECK(chip.seen[0x01] == 0 && chip.seen[0x31] == 0 && chip.seen[0x11] == 0);
	CHECK(chip.violations == 0 && chip.page_overruns == 0 && chip.reset_harm == 0 && chip.early == 0);
}

// core/storage and core/settings on a 32 MB part with the 16 MB layout in its low half, started on a chip other
// firmware left in 4-byte mode at an address in the top half. The whole chip reads back (the backup); the core writes
// only the low half; whatever was read or written, the register is 0 afterwards (f_read, f_write and f_erase check it
// after every transaction, here and in every test above)
static void test_addr4_store(void)
{
	uint8_t a[STORAGE_SECTOR], b[STORAGE_SECTOR], r[0x300];
	settings_t s;

	big_create();
	put_calibration();
	for (uint32_t i = MB16; i < MB32; i++) {
		chip.mem[i] = (uint8_t)(i ^ (i >> 9) ^ (i >> 17));	// the top half: someone else's
	}
	uint8_t *top = malloc(MB16);
	memcpy(top, &chip.mem[MB16], MB16);
	chip.ads = true;
	chip.ext = 0x01;
	CHECK(warm_boot() && flash.size == MB32 && board_nor.addr_len == 4 && !board_nor.ads);
	CHECK(chip.resets == 1 && chip.ext == 0);
	settings_begin()->squelch = 6;
	settings_end(1);
	CHECK(settings_flush() && chip.ext == 0 && !chip.wel);
	fill(a, 1);
	CHECK(storage_sector_write(0xFFF000u, a) && memcmp(&chip.mem[0xFFF000u], a, sizeof(a)) == 0);
	fill(b, 9);
	CHECK(storage_write(0x20FF0u, b, 64) && memcmp(&chip.mem[0x20FF0u], b, 64) == 0);
	static const uint8_t magic[4] = { 0x4F, 0x44, 0x52, 0x4E };
	CHECK(f_write(map16.native_addr, magic, 4) && storage_native_erase() == STORAGE_NATIVE_ERASED);
	CHECK(chip.high_ops == 0 && memcmp(top, &chip.mem[MB16], MB16) == 0 && chip.ext == 0);
	// The backup's reads: below, across and above the 16 MB line, to the last byte. Only a read that reached past
	// the line is followed by a park
	uint32_t parks = chip.seen[0xC5];
	CHECK(f_read(0x20FF0u, r, 64) && memcmp(r, b, 64) == 0);
	CHECK(f_read(MB16 - sizeof(r), r, sizeof(r)) && memcmp(r, &chip.mem[MB16 - sizeof(r)], sizeof(r)) == 0);
	CHECK(chip.seen[0xC5] == parks);
	for (uint32_t addr = MB16 - 0x280u; addr < MB16 + 0x1000u; addr += sizeof(r)) {
		memset(r, 0, sizeof(r));
		CHECK(f_read(addr, r, sizeof(r)) && memcmp(r, &chip.mem[addr], sizeof(r)) == 0);
		CHECK(chip.ext == 0 && !chip.wel && chip.seen[0xC5] == ++parks);
	}
	CHECK(f_read(MB32 - sizeof(r), r, sizeof(r)) && memcmp(r, top + MB16 - sizeof(r), sizeof(r)) == 0 && chip.ext == 0);
	CHECK(!f_read(MB32 - sizeof(r) + 1, r, sizeof(r)) && !f_read(MB32, r, 1));	// past the end
	// A write and an erase in the top half through the same wiring (nothing in the core's map is there)
	CHECK(f_erase(MB16 + 0x5000u) && all_bytes(MB16 + 0x5000u, MB16 + 0x6000u, 0xFF) && chip.ext == 0);
	CHECK(f_write(MB16 + 0x5010u, a, 300) && memcmp(&chip.mem[MB16 + 0x5010u], a, 300) == 0 && chip.ext == 0);
	CHECK(chip.high_ops == 3 && all_bytes(0x5000u, 0x6000u, 0xFF));
	CHECK(!f_write(MB32 - 8, a, 16) && !f_erase(MB32));
	// A failed transaction is parked too: a refused program leaves nothing behind
	chip.protect = true;
	chip.ext = 0x01;
	CHECK(!f_write(MB16 + 0x9000u, a, 16) && chip.ext == 0x01);	// (this park is refused as well)
	chip.protect = false;
	CHECK(f_read(MB16 + 0x9000u, r, 16) && chip.ext == 0);
	// The next start, without the power having gone
	CHECK(warm_boot() && chip.resets == 2);
	settings_read(&s);
	CHECK(s.squelch == 6 && memcmp(&chip.mem[0xFFF000u], a, sizeof(a)) == 0 && calibration_intact());
	CHECK(chip.seen[0x03] == 0 && chip.seen[0x02] == 0 && chip.seen[0x20] == 0);
	CHECK(chip.violations == 0 && chip.page_overruns == 0 && chip.reset_harm == 0 && chip.early == 0);
	free(top);
}
#endif

// Over every test: only the commands the driver is meant to send. None may erase (0x44) or program (0x42) a security
// register, write a status register (0x01, 0x31, 0x11: the security register lock bits), or erase more than a sector.
// 0x4B reads the unique ID
static void test_commands_sent(void)
{
#if SPI_NOR_ADDR4
	// And for a part with 4-byte addresses: the three array commands with four address bytes, the reset, the extended
	// address register write and the write disable after it. Not the address mode (0xB7, 0xE9), and still no
	// status register write (the non-volatile ADP bit is in register 3)
	static const uint8_t allowed[] = { 0x03, 0x02, 0x20, 0x06, 0x05, 0x35, 0x15, 0x9F, 0x48, 0x4B,
					   0x13, 0x12, 0x21, 0x66, 0x99, 0xC5, 0x04 };
	CHECK(harm_total == 0 && early_total == 0);	// no reset into a busy or suspended chip, none not waited out
#else
	static const uint8_t allowed[] = { 0x03, 0x02, 0x20, 0x06, 0x05, 0x35, 0x15, 0x9F, 0x48, 0x4B };
#endif
	for (unsigned c = 0; c < 256; c++) {
		bool ok = false;
		for (unsigned i = 0; i < sizeof allowed; i++) {
			ok |= allowed[i] == c;
		}
		if (!ok && cmds_seen[c] != 0) {
			printf("FAIL spi_nor: command 0x%02X sent %u times\n", c, cmds_seen[c]);
			failures++;
		}
	}
#ifndef SPI_NOR_TEST_LIB
	CHECK(cmds_seen[0x48] != 0 && cmds_seen[0x20] != 0);	// the tests did read security registers and erase
#endif
}

#ifndef SPI_NOR_TEST_LIB
int main(void)
{
	test_probe();
	test_read_program_erase();
	test_store_16mb();
	test_store_small_part();
	test_store_cuts();
#if SPI_NOR_ADDR4
	test_addr4_array();
	test_addr4_left_state();
	test_addr4_park();
	test_addr4_probe_unsettled();
	test_addr4_adp();
	test_addr4_store();
	CHECK(cmds_seen[0x13] != 0 && cmds_seen[0x12] != 0 && cmds_seen[0x21] != 0 && cmds_seen[0xC5] != 0);
#endif
	test_commands_sent();
	free(chip.mem);
	printf("# %s %s (%d failures)\n", SPI_NOR_ADDR4 ? "spiNorAddr4Test" : "spiNorTest", failures ? "FAILED" : "PASSED",
	       failures);
	return failures ? 1 : 0;
}
#endif
