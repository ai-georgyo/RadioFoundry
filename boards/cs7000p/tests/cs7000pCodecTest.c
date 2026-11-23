// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS codec route (vocoder_cs7000p.c, included here as the firmware builds it) against sector 7 as
// tools/codec_extract.py merges a synthetic codec for this board (--test-desc; the file is the argument). What must
// hold on the radio: the merge's descriptor and both of its windows pass the boot check's steps
// (platform/stm32/codec_desc.h, platform/stm32/hrc/vocoder.c matches()) with the board's own profile; the profile
// pins the placement and both CRC-32s, so nothing but the codec as the merge tool placed and blanked it is taken; a
// byte changed anywhere in a window, a blanked span and the fill between two windows included, fails the check; a
// descriptor that names flash outside sector 7 or over itself is refused before any byte is read.
// No codec here: the tool's synthetic codec is random bytes with the real layout, so its two CRC-32s are its own and
// not the pinned ones. A second argument "pinned" says the file is sector 7 of an image merged from the maker's file,
// outside the repository: the two CRC-32s must then be the pinned ones, as the boot check asks. Nothing of this has
// run on a radio.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../vocoder_cs7000p.c"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define SIGNATURE	0x4FF0E92Du	// the code's first word (vocoder.c CODEC_SIGNATURE)
// Sector 7: from the flash the board leaves to the codec to the end of the descriptor
#define SECTOR		(CODEC_DESC_ADDR + (uint32_t)sizeof(codec_desc_t) - CODEC_FLASH_LO)

// Where the merge puts the pieces of this build (tools/rfoundry/rfoundry/stockfw.py, the profile's "placement"; the
// windows and the spans that are not the codec's: docs/spec/codec-cs7000p.md, "Codec in the V9.00.96 file")
#define STOCK_CODE	0x080A4F24u		// where the maker's file holds the code's start (the page's S)
#define CODE_HOLE	(CODEC_CODE + 0x102Cu)	// the larger span between two code ranges, blanked
#define CODE_HOLE_LEN	25008u
#define ISLAND1		0x080F5CA0u		// the block's first window
#define ISLAND1_END	0x080F6514u		// then 4 bytes of fill up to island 2
#define ISLAND2		0x080F6518u
#define TABLE_HOLE	0x080F9950u		// a span inside a table window, blanked
#define TABLE_HOLE_LEN	4u

static uint8_t flash[0x20000];
static uint8_t sound[sizeof flash];		// the sector as the tool wrote it
static bool pinned;				// the sector holds the maker's codec: its CRC-32s are the profile's

static const stm32_codec_profile_t *prof(void)
{
	return stm32_codec_profiles.list[0];
}

static uint8_t *at(uint32_t addr)
{
	return flash + (addr - CODEC_FLASH_LO);
}

// The descriptor as the firmware reads it, copied out so that a case can change it
static codec_desc_t desc(void)
{
	codec_desc_t d;
	memcpy(&d, at(CODEC_DESC_ADDR), sizeof d);
	return d;
}

static void seal(codec_desc_t *d)
{
	d->crc = codec_desc_crc32((const uint8_t *)d, offsetof(codec_desc_t, crc), 0);
}

// The descriptor's own checks as the boot check makes them for this board's profile
static bool desc_ok(const codec_desc_t *d)
{
	const stm32_codec_profile_t *p = prof();
	return codec_desc_check(d, p->tag, p->flash_lo, CODEC_DESC_ADDR, p->code_len);
}

static bool bytes_ok(void)
{
	codec_desc_t d = desc();
	return desc_ok(&d) && codec_desc_check_bytes(&d, flash, prof()->flash_lo);
}

static bool erased(uint32_t from, uint32_t to)
{
	for (uint32_t a = from; a < to; a++) {
		if (*at(a) != 0xFF) {
			return false;
		}
	}
	return true;
}

// The profile by itself: one build, everything pinned, a placement the descriptor's checks accept
static void test_profile(void)
{
	const stm32_codec_profile_t *p = prof();
	CHECK(stm32_codec_profiles.count == 1 && sizeof profiles / sizeof profiles[0] == 1);
	CHECK(SECTOR == sizeof flash && p->flash_lo == CODEC_FLASH_LO);
	// Pinned: the boot check then takes the descriptor's address, CRC-32 and table windows only when they are these
	CHECK(p->code != 0 && p->code_crc != 0 && p->tables != NULL && p->windows == 1);
	CHECK(p->windows <= CODEC_DESC_WINDOWS);
	if (p->tables == NULL || p->windows != 1) {
		return;			// the rest reads the one block
	}
	// The code keeps its stock address modulo 16 at the sector's start, so it is word aligned; the block follows it,
	// and both end before the descriptor
	CHECK(p->code == p->flash_lo + (STOCK_CODE & 15u) && (p->code & 3u) == 0);
	CHECK(p->tables[0].addr >= p->code + p->code_len);
	CHECK(p->tables[0].addr + p->tables[0].len <= CODEC_DESC_ADDR);
	// Every moved window keeps its stock address modulo 16, so a multiple of 4: the block starts on one
	CHECK((p->tables[0].addr & 3u) == 0);
	const uint16_t entries[] = { p->at.encode, p->at.decode, p->at.enc_init, p->at.fec_init, p->at.dec_init,
		p->at.enc_params, p->at.enc_set, p->at.dec_set };
	for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++) {
		CHECK(entries[i] != 0 && (entries[i] & 1u) == 0 && entries[i] < p->code_len);	// code, in the window
		CHECK(p->code + entries[i] < CODE_HOLE);	// in the first code range, which ends where that span starts
		for (size_t k = 0; k < i; k++) {
			CHECK(entries[k] != entries[i]);
		}
	}
	// The eight entries the host calls, as the page lists them ("Entry points and calling convention")
	CHECK(p->at.encode == 0x14C && p->at.enc_set == 0x1E0 && p->at.enc_init == 0x294 && p->at.fec_init == 0x4FC);
	CHECK(p->at.decode == 0x604 && p->at.dec_set == 0x6AC && p->at.dec_init == 0x6F0 && p->at.enc_params == 0x854);
	// A descriptor with exactly the pinned values passes the descriptor's own checks
	codec_desc_t d;
	memset(&d, 0, sizeof d);
	d.magic = CODEC_DESC_MAGIC;
	d.version = CODEC_DESC_VERSION;
	d.tag = p->tag;
	d.code.addr = p->code;
	d.code.len = p->code_len;
	d.code.crc = p->code_crc;
	d.windows = p->windows;
	d.table[0] = p->tables[0];
	seal(&d);
	CHECK(desc_ok(&d));
	CHECK(!codec_desc_check(&d, p->tag + 1u, p->flash_lo, CODEC_DESC_ADDR, p->code_len));	// another build's tag
}

// The tool's sector against the profile
static void test_sector(void)
{
	const stm32_codec_profile_t *p = prof();
	codec_desc_t d = desc();
	CHECK(desc_ok(&d));
	CHECK(d.code.addr == p->code && d.code.len == p->code_len && d.windows == p->windows);
	for (uint32_t i = 0; i < p->windows && i < CODEC_DESC_WINDOWS; i++) {
		CHECK(d.table[i].addr == p->tables[i].addr && d.table[i].len == p->tables[i].len);
	}
	for (uint32_t i = p->windows; i < CODEC_DESC_WINDOWS; i++) {
		CHECK(d.table[i].addr == 0 && d.table[i].len == 0 && d.table[i].crc == 0);	// unused: zero
	}
	CHECK(codec_desc_check_bytes(&d, flash, p->flash_lo));
	uint32_t first;
	memcpy(&first, at(d.code.addr), sizeof first);
	CHECK(first == SIGNATURE);
	if (pinned) {
		// The maker's codec as merged: the descriptor carries exactly what the profile pins (vocoder.c matches())
		CHECK(d.code.crc == p->code_crc);
		CHECK(memcmp(d.table, p->tables, p->windows * sizeof(codec_desc_window_t)) == 0);
	} else {
		// The synthetic codec's CRC-32s are not the real codec's, so the boot check refuses it by the pins
		CHECK(d.code.crc != p->code_crc && d.table[0].crc != p->tables[0].crc);
	}

	// What the merge blanks is 0xFF, and so is what it places nothing in
	CHECK(erased(CODE_HOLE, CODE_HOLE + CODE_HOLE_LEN));
	CHECK(erased(TABLE_HOLE, TABLE_HOLE + TABLE_HOLE_LEN));
	CHECK(erased(ISLAND1_END, ISLAND2));				// the fill between two windows
	CHECK(erased(CODEC_FLASH_LO, p->code));				// before the code
	CHECK(erased(p->code + p->code_len, p->tables[0].addr));	// between the code and the block
	CHECK(erased(p->tables[0].addr + p->tables[0].len, CODEC_DESC_ADDR));	// the free flash after the block
	CHECK(!erased(CODE_HOLE - 16u, CODE_HOLE) && !erased(CODE_HOLE + CODE_HOLE_LEN, CODE_HOLE + CODE_HOLE_LEN + 16u));
}

// One byte changed in turn: every placed byte is under one of the two CRC-32s
static void test_changed_bytes(void)
{
	const stm32_codec_profile_t *p = prof();
	const uint32_t block_end = p->tables[0].addr + p->tables[0].len;
	enum { BIT = -1 };		// one bit of the byte changed, whatever it holds
	const struct {
		uint32_t addr;
		int to;			// the new value, written over 0xFF; or BIT
	} cases[] = {
		{ p->code, BIT },				// the code's first byte
		{ p->code + p->at.encode, BIT },		// an entry
		{ p->code + p->code_len - 3u, BIT },		// near the code's end
		{ CODE_HOLE, 0x00 },				// a blanked span of the code: its first, a middle...
		{ CODE_HOLE + CODE_HOLE_LEN / 2u, 0x00 },
		{ CODE_HOLE + CODE_HOLE_LEN - 1u, 0xFE },	// ...and its last byte
		{ ISLAND1, BIT },				// island 1, the block's first byte
		{ ISLAND1_END - 1u, BIT },
		{ ISLAND1_END, 0x00 },				// the fill between island 1 and island 2
		{ ISLAND2 - 1u, 0x7F },
		{ ISLAND2, BIT },
		{ TABLE_HOLE, 0x00 },				// a blanked span of a table window
		{ TABLE_HOLE + TABLE_HOLE_LEN - 1u, 0x00 },
		{ block_end - 1u, BIT },			// the last table window's last byte
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		memcpy(flash, sound, sizeof flash);
		uint8_t *b = at(cases[i].addr);
		CHECK(cases[i].to == BIT || *b == 0xFF);	// a blanked or fill byte is 0xFF in the tool's sector
		*b = cases[i].to == BIT ? (uint8_t)(*b ^ 1u) : (uint8_t)cases[i].to;
		if (bytes_ok()) {
			printf("FAIL %s: a changed byte at 0x%08X passes\n", __FILE__, (unsigned)cases[i].addr);
			failures++;
		}
	}
	// Outside the two windows nothing is placed and nothing is checked: the four bytes between the code and the
	// block, and the free flash after the block
	const uint32_t outside[] = { p->flash_lo, p->code - 1u, p->code + p->code_len, p->tables[0].addr - 1u, block_end,
		CODEC_DESC_ADDR - 1u };
	for (size_t i = 0; i < sizeof outside / sizeof outside[0]; i++) {
		memcpy(flash, sound, sizeof flash);
		*at(outside[i]) = 0x00;
		CHECK(bytes_ok());
	}
	// The descriptor's own bytes are under its own CRC-32
	for (uint32_t i = 0; i < sizeof(codec_desc_t); i++) {
		memcpy(flash, sound, sizeof flash);
		*at(CODEC_DESC_ADDR + i) ^= 0x10;
		if (bytes_ok()) {
			printf("FAIL %s: a changed descriptor byte %u passes\n", __FILE__, (unsigned)i);
			failures++;
		}
	}
	memcpy(flash, sound, sizeof flash);
	CHECK(bytes_ok());
}

// A descriptor that names flash outside sector 7, or the descriptor's own bytes: refused before any byte is read
static void test_refused(void)
{
	const stm32_codec_profile_t *p = prof();
	codec_desc_t d = desc();
	d.code.addr = p->flash_lo - 4u;			// the code starts in sector 6, the application's
	seal(&d);
	CHECK(!desc_ok(&d));
	d = desc();
	d.code.addr = 0x08000000u;			// the code at the flash base
	seal(&d);
	CHECK(!desc_ok(&d));
	d = desc();
	d.code.addr = CODEC_DESC_ADDR - p->code_len;	// ends where the descriptor starts: the last place allowed
	seal(&d);
	CHECK(desc_ok(&d));
	d.code.addr += 4u;				// reaches the descriptor
	seal(&d);
	CHECK(!desc_ok(&d));
	d = desc();
	d.code.addr = p->code + 2u;			// not word aligned
	seal(&d);
	CHECK(!desc_ok(&d));
	d = desc();
	d.code.len = p->code_len - 4u;			// shorter than the build's code
	seal(&d);
	CHECK(!desc_ok(&d));
	d = desc();
	d.table[0].addr = p->flash_lo - 1u;		// the block starts below sector 7
	seal(&d);
	CHECK(!desc_ok(&d));
	d = desc();
	d.table[0].len = CODEC_DESC_ADDR - d.table[0].addr;	// ends where the descriptor starts: allowed
	seal(&d);
	CHECK(desc_ok(&d));
	d.table[0].len += 1u;				// reaches the descriptor
	seal(&d);
	CHECK(!desc_ok(&d));
	d = desc();
	d.table[0].addr = 0x08100000u;			// bank 2, which no tool writes
	seal(&d);
	CHECK(!desc_ok(&d));
	d = desc();
	d.table[0].len = 0xFFFFFFF0u;			// wraps
	seal(&d);
	CHECK(!desc_ok(&d));
	d = desc();
	d.windows = 2;					// a second window, zero: below sector 7
	seal(&d);
	CHECK(!desc_ok(&d));
	d = desc();
	d.tag ^= 1u;					// another build
	seal(&d);
	CHECK(!desc_ok(&d));
	d = desc();
	d.tag ^= 1u;					// a changed descriptor, its CRC-32 not made again
	CHECK(!desc_ok(&d));
	// An erased sector: the radio before any merge
	memset(flash, 0xFF, sizeof flash);
	CHECK(!bytes_ok());
	memcpy(flash, sound, sizeof flash);
}

int main(int argc, char **argv)
{
	test_profile();
	if (failures) {		// the cases below rest on the profile's placement
		printf("# cs7000pCodecTest FAILED (%d failures)\n", failures);
		return 1;
	}
	if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "pinned") != 0)) {
		printf("usage: cs7000pCodecTest SECTOR [pinned] (SECTOR: tools/codec_extract.py --test-desc SECTOR --board "
		       "cs7000p; with \"pinned\": the last 128 KB of an image merged from the maker's file)\n");
		return 1;
	}
	pinned = argc == 3;
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		printf("FAIL %s: can't open %s\n", __FILE__, argv[1]);
		return 1;
	}
	size_t n = fread(sound, 1, sizeof sound, f);
	int more = fgetc(f);
	fclose(f);
	// The tool writes from the end of the application's region (boards/cs7000p/memory.ld) to the end of bank 1: one
	// sector, so the region ends where the codec's flash starts
	CHECK(n == sizeof sound && more == EOF);
	if (n == sizeof sound) {
		memcpy(flash, sound, sizeof flash);
		test_sector();
		test_changed_bytes();
		test_refused();
	}
	printf("# cs7000pCodecTest%s %s (%d failures)\n", pinned ? " pinned" : "", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
