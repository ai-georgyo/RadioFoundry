// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS reader of the maker's calibration table (makercal_cs7000p.c), on INVENTED tables only: no
// unit's values and no byte of any radio's backup. The maker's arithmetic on the spec's invented row (every vector of
// the blueprint, the product's and the sum's overflow), each integrity rule alone refusing the table, each use rule
// alone clearing one group and nothing else, the bytes nothing reads, the VCO switch windows and the extensions past
// the end points, the sub-audio and squelch settings, the trims' codings and the receive bound, the RSSI floor.
#include <stdio.h>
#include <string.h>
#include "makercal_cs7000p.h"
#include "rfcal_cs7000p.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define MHZ	1000000u
#define KHZ	1000u
#define SIZE	CS7000P_MAKER_SIZE

// --- An invented table ---

// The spec's invented TX row (calibration-cs7000p.md 3) and invented RX points, two either side of 479 MHz
static const uint32_t TX_PTS[8] = {
	400200000u, 429800000u, 430200000u, 445000000u, 458000000u, 472000000u, 492000000u, 511800000u,
};
static const uint32_t RX_PTS[8] = {
	400500000u, 420000000u, 440000000u, 460000000u, 478950000u, 479050000u, 495000000u, 511500000u,
};
static const uint8_t HIGH[8] = { 190, 194, 195, 210, 213, 215, 212, 209 };
static const uint8_t LOW[8] = { 100, 99, 99, 108, 115, 122, 126, 127 };
static const uint8_t TUNE[8] = { 120, 124, 128, 132, 136, 140, 150, 160 };

static void put32(uint8_t *t, unsigned off, uint32_t v)
{
	t[off] = (uint8_t)(v >> 24);
	t[off + 1] = (uint8_t)(v >> 16);
	t[off + 2] = (uint8_t)(v >> 8);
	t[off + 3] = (uint8_t)v;
}
static void put_points(uint8_t *t, unsigned off, const uint32_t p[8])
{
	for (unsigned i = 0; i < 8; i++) {
		put32(t, off + 4u * i, p[i]);
	}
}
static void put_row(uint8_t *t, unsigned off, const uint8_t y[8]) { memcpy(t + off, y, 8); }
static void fill_row(uint8_t *t, unsigned off, uint8_t v) { memset(t + off, v, 8); }
static void put16(uint8_t *t, unsigned off, unsigned i, uint16_t v)
{
	t[off + 2u * i] = (uint8_t)v;
	t[off + 2u * i + 1u] = (uint8_t)(v >> 8);
}
static void fill16(uint8_t *t, unsigned off, uint16_t v)
{
	for (unsigned i = 0; i < 8; i++) {
		put16(t, off, i, v);
	}
}

static void invented(uint8_t t[SIZE])
{
	memset(t, 0, SIZE);
	put_points(t, 0x000, TX_PTS);
	put_points(t, 0x020, RX_PTS);
	t[0x040] = 60;
	t[0x041] = 160;
	t[0x042] = 30;
	t[0x043] = 90;
	put_row(t, 0x044, TUNE);
	fill_row(t, 0x04C, 40);		// level 9 open
	fill_row(t, 0x054, 50);		// level 9 close
	fill_row(t, 0x05C, 120);	// level 1 open
	fill_row(t, 0x064, 130);	// level 1 close
	put_row(t, 0x06C, HIGH);
	put_row(t, 0x074, LOW);
	fill_row(t, 0x07C, 150);	// never read
	fill_row(t, 0x084, 40);
	fill_row(t, 0x08C, 52);
	fill_row(t, 0x094, 200);
	fill_row(t, 0x09C, 60);
	fill_row(t, 0x0A4, 70);
	fill_row(t, 0x0AC, 80);
	static const uint8_t dmr2[8] = { 90, 91, 70, 72, 74, 76, 78, 80 }, dmr1[8] = { 100, 101, 102, 103, 104, 105, 106, 107 };
	static const uint8_t fm2[8] = { 130, 131, 110, 112, 114, 116, 118, 120 }, fm1[8] = { 140, 139, 138, 137, 136, 135, 134, 133 };
	put_row(t, 0x0B4, dmr2);
	put_row(t, 0x0BC, dmr1);
	put_row(t, 0x0C4, fm2);
	put_row(t, 0x0CC, fm1);
	fill_row(t, 0x0D4, 0x11);	// never read
	t[0x0DC] = 0x7B;		// trim A: d = -5
	t[0x0DD] = 0xA0;
	t[0x0DE] = 0x33;		// never read
	t[0x0DF] = 0x80;
	t[0x0E8] = 1;			// never read
	t[0x0E9] = 2;
	t[0x0EA] = 3;
	fill_row(t, 0x0EC, 20);		// high power: advance, S, R, H, F (the spec's invented 12 / 19 / 300 / 310)
	fill_row(t, 0x0F4, 12);
	fill_row(t, 0x0FC, 19);
	fill16(t, 0x104, 300);
	fill16(t, 0x114, 310);
	fill_row(t, 0x124, 21);		// low power
	fill_row(t, 0x12C, 12);
	fill_row(t, 0x134, 20);
	fill16(t, 0x13C, 301);
	fill16(t, 0x14C, 311);
	fill_row(t, 0x15C, 61);
	fill_row(t, 0x164, 159);
	t[0x174] = 0x7D;		// trim B: d = -3
	t[0x1FF] = 0x5A;		// never read
}

static uint32_t crc32(const uint8_t *b, unsigned n)
{
	uint32_t c = 0xFFFFFFFFu;
	for (unsigned i = 0; i < n; i++) {
		c ^= b[i];
		for (int k = 0; k < 8; k++) {
			c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
		}
	}
	return ~c;
}

static bool parse(const uint8_t *t, cs7000p_maker_t *m) { return cs7000p_maker_parse(t, crc32(t, SIZE), 0, m); }

// --- The arithmetic ---

static uint8_t row_hz(const cs7000p_maker_t *m, unsigned row, uint32_t hz, uint8_t *flags)
{
	*flags = 0;
	return cs7000p_maker_row(m->tx_p10, m->row[row], hz, flags);
}

static void test_vectors(void)
{
	uint8_t t[SIZE], f;
	cs7000p_maker_t m;
	invented(t);
	CHECK(parse(t, &m));
	const unsigned H = CS7000P_MK_POWER_HI, L = CS7000P_MK_POWER_LO;

	CHECK(row_hz(&m, H, 445000000u, &f) == 210 && f == 0);		// a point takes the segment ending at it
	CHECK(row_hz(&m, H, 437600000u, &f) == 203 && f == 0);		// t = 810: half rounds up
	CHECK(row_hz(&m, H, 496000000u, &f) == 212 && f == 0);		// t = 846: toward zero, not floor
	CHECK(row_hz(&m, H, 410000000u, &f) == 191 && f == 0);
	CHECK(row_hz(&m, H, 500000000u, &f) == 211 && f == 0);
	CHECK(row_hz(&m, H, 400000000u, &f) == 190 && f == 0);		// below the first point: segment 0 extended
	CHECK(row_hz(&m, L, 400000000u, &f) == 100 && f == 0);
	CHECK(row_hz(&m, H, 527000000u, &f) == 207 && f == 0);		// past the last: segment 6 extended
	CHECK(row_hz(&m, L, 527000000u, &f) == 128 && f == 0);
	CHECK(row_hz(&m, H, 429800000u, &f) == 194 && f == 0);		// f <= P[1]
	CHECK(row_hz(&m, H, 429800005u, &f) == 194 && f == 0);		// still P[1] in 10 Hz units
	CHECK(row_hz(&m, H, 429800010u, &f) == 194 && f == 0);		// segment 1
	CHECK(row_hz(&m, H, 430000000u, &f) == 195 && f == 0);		// the maker's mixed value, t = 778
	// The line itself at those t: 846 by truncation, 845 by floor; 810 / 4 = 202.5
	f = 0;
	CHECK(cs7000p_maker_line(49200000, 51180000, 212, 209, 49600000, &f) == 212 && f == 0);
	CHECK(cs7000p_maker_line(43020000, 44500000, 195, 210, 43760000, &f) == 203 && f == 0);

	// Clamps and the product's overflow, raw (the rows in FM_MOD2; the lookups below)
	static const uint8_t r1[8] = { 100, 120, 121, 150, 180, 220, 250, 254 };
	static const uint8_t r2[8] = { 1, 250, 250, 250, 250, 250, 250, 250 };
	static const uint8_t r3[8] = { 1, 254, 254, 254, 254, 254, 254, 254 };
	f = 0;
	CHECK(cs7000p_maker_row(m.tx_p10, r1, 527000000u, &f) == 255 && f == CS7000P_MK_CLAMP);	// t = 1028
	f = 0;
	CHECK(cs7000p_maker_row(m.tx_p10, r1, 512000000u, &f) == 254 && f == 0);		// t = 1016
	f = 0;
	CHECK(cs7000p_maker_row(m.tx_p10, r2, 400000000u, &f) == 0 && f == CS7000P_MK_CLAMP);		// t = -2
	f = 0;
	CHECK(cs7000p_maker_row(m.tx_p10, r2, 400100000u, &f) == 0 && f == 0);			// t = 1
	f = 0;
	CHECK(cs7000p_maker_row(m.tx_p10, r2, 400200000u, &f) == 1 && f == 0);			// t = 4
	f = 0;
	// 4 x 253 x 2,960,000 is past 32 bits; t = 1016 all the same
	CHECK(cs7000p_maker_row(m.tx_p10, r3, 429800000u, &f) == 254 && f == CS7000P_MK_OVERFLOW);
	f = 0;
	CHECK(cs7000p_maker_row(m.tx_p10, r3, 420000000u, &f) == 170 && f == 0);			// t = 680

	// The sum: points 10 Hz apart, the product 4 x 233 x 2,304,167 = 2,147,483,644 fits, 4 y0 + it does not
	static const uint32_t p10[8] = { 40000000, 40100000, 40200000, 40300000, 40400000, 40500000, 42000000, 42000001 };
	static const uint8_t ys[8] = { 10, 10, 10, 10, 10, 10, 10, 243 };
	f = 0;
	CHECK(cs7000p_maker_line(42000000, 42000001, 10, 243, 44304167, &f) == 255);
	CHECK(f == (CS7000P_MK_OVERFLOW | CS7000P_MK_CLAMP));
	f = 0;
	CHECK(cs7000p_maker_row(p10, ys, 443041670u, &f) == 255 && f == (CS7000P_MK_OVERFLOW | CS7000P_MK_CLAMP));
	f = 0;	// one 10 Hz step less: the sum fits, only the clamp
	CHECK(cs7000p_maker_row(p10, ys, 443041660u, &f) == 255 && f == CS7000P_MK_CLAMP);
	// The product at exactly INT32_MIN: it would fit, flagged anyway
	f = 0;
	(void)cs7000p_maker_line(0, 1, 128, 0, 4194304, &f);
	CHECK(f & CS7000P_MK_OVERFLOW);
	f = 0;
	(void)cs7000p_maker_line(0, 1, 128, 0, 4194303, &f);
	CHECK(!(f & CS7000P_MK_OVERFLOW));

	// Equal ends: the mean; settings in place of the frequency
	CHECK(cs7000p_maker_line(5, 5, 7, 10, 123, NULL) == 8);
	CHECK(cs7000p_maker_line(1, 9, 120, 40, 2, NULL) == 110);
	CHECK(cs7000p_maker_line(1, 9, 120, 40, 5, NULL) == 80);
	CHECK(cs7000p_maker_line(1, 9, 120, 40, 8, NULL) == 50);
	CHECK(cs7000p_maker_line(670, 2541, 40, 52, 885, NULL) == 41);
	CHECK(cs7000p_maker_line(670, 2541, 40, 52, 1000, NULL) == 42);
	CHECK(cs7000p_maker_line(670, 2541, 40, 52, 2503, NULL) == 52);
}

// --- The rules ---

static cs7000p_maker_t base;

// One table, one change: its state, groups, reason and offset, and check() agreeing
static void expect(const uint8_t *t, uint16_t floor12, uint8_t state, bool valid, uint16_t fields, uint8_t why,
		   uint16_t off, int line)
{
	cs7000p_maker_t m;
	bool ok = cs7000p_maker_parse(t, crc32(t, SIZE), floor12, &m);
	uint8_t cwhy = 0xEE;
	uint16_t coff = 0xEEEE;
	bool cok = cs7000p_maker_check(t, SIZE, floor12, &cwhy, &coff);
	if (ok != valid || m.valid != valid || m.state != state || m.fields != fields || m.why != why ||
	    m.why_off != off || cok != valid || cwhy != (valid ? CS7000P_MK_WHY_NONE : why) || coff != (valid ? 0 : off)) {
		printf("FAIL %s:%d: state %u valid %d fields 0x%04x why %s off 0x%03x (check %d %s 0x%03x)\n", __FILE__, line,
		       m.state, m.valid, m.fields, cs7000p_maker_why_name(m.why), m.why_off, cok,
		       cs7000p_maker_why_name(cwhy), coff);
		failures++;
	}
}
#define EXPECT(t, fl, st, va, fi, wh, of) expect(t, fl, st, va, fi, wh, of, __LINE__)
#define ALL	CS7000P_MK_F_ALL
#define BAD(t, group, why, off)	EXPECT(t, 0, CS7000P_MK_OK, false, ALL & ~(group), why, off)
#define USE(t, group, why, off)	EXPECT(t, 0, CS7000P_MK_OK, true, ALL & ~(group), why, off)

static void test_valid(void)
{
	uint8_t t[SIZE];
	invented(t);
	EXPECT(t, 0, CS7000P_MK_OK, true, ALL, CS7000P_MK_WHY_NONE, 0);
	CHECK(parse(t, &base));
	CHECK(base.crc == crc32(t, SIZE) && base.floor12 == 0);
	CHECK(base.tx_hz[0] == 400200000u && base.tx_p10[7] == 51180000u && base.rx_hz[5] == 479050000u);
	CHECK(base.row[CS7000P_MK_POWER_HI][3] == 210 && base.row16[CS7000P_MK_LO_OFF][7] == 311);
	CHECK(base.trim_a == 0x7B && base.trim_b == 0x7D && base.rssi_lo == 60 && base.rssi_hi == 160);
	CHECK(base.battery == 0xA0 && base.vox9 == 30 && base.vox0 == 90 && base.tone_in == 0x80);

	// No bytes, the wrong size
	cs7000p_maker_t m;
	CHECK(!cs7000p_maker_parse(NULL, 0, 0, &m) && m.state == CS7000P_MK_UNREAD && m.why == CS7000P_MK_WHY_UNREAD);
	CHECK(m.fields == 0 && !m.valid);
	uint8_t why;
	uint16_t off;
	CHECK(!cs7000p_maker_check(NULL, SIZE, 0, &why, &off) && why == CS7000P_MK_WHY_UNREAD);
	CHECK(!cs7000p_maker_check(t, SIZE - 1u, 0, &why, &off) && why == CS7000P_MK_WHY_SIZE);
	CHECK(cs7000p_maker_check(t, SIZE, 0, NULL, NULL));
}

static void test_whole(void)
{
	uint8_t t[SIZE];
	memset(t, 0xFF, SIZE);
	EXPECT(t, 0, CS7000P_MK_BLANK, false, 0, CS7000P_MK_WHY_BLANK, 0);
	memset(t, 0x00, SIZE);
	EXPECT(t, 0, CS7000P_MK_ZERO, false, 0, CS7000P_MK_WHY_ZERO, 0);

	invented(t);
	put32(t, 0x00C, 0xFFFFFFFFu);
	EXPECT(t, 0, CS7000P_MK_POINTS, false, 0, CS7000P_MK_WHY_PT_UNSET, 0x00C);
	invented(t);
	put32(t, 0x03C, 0xFFFFFFFFu);
	EXPECT(t, 0, CS7000P_MK_POINTS, false, 0, CS7000P_MK_WHY_PT_UNSET, 0x03C);
	// The range's edges count as in it
	invented(t);
	put32(t, 0x03C, 527000000u);
	EXPECT(t, 0, CS7000P_MK_OK, true, ALL, CS7000P_MK_WHY_NONE, 0);
	put32(t, 0x03C, 527000001u);
	EXPECT(t, 0, CS7000P_MK_POINTS, false, 0, CS7000P_MK_WHY_PT_RANGE, 0x03C);
	invented(t);
	put32(t, 0x000, 400000000u);
	EXPECT(t, 0, CS7000P_MK_OK, true, ALL, CS7000P_MK_WHY_NONE, 0);
	put32(t, 0x000, 399999999u);
	EXPECT(t, 0, CS7000P_MK_POINTS, false, 0, CS7000P_MK_WHY_PT_RANGE, 0x000);
	// Strictly rising: an equal point and a falling one
	invented(t);
	put32(t, 0x014, TX_PTS[4]);
	EXPECT(t, 0, CS7000P_MK_POINTS, false, 0, CS7000P_MK_WHY_PT_ORDER, 0x014);
	invented(t);
	put32(t, 0x024, RX_PTS[0] - 1u);
	EXPECT(t, 0, CS7000P_MK_POINTS, false, 0, CS7000P_MK_WHY_PT_ORDER, 0x024);
}

// Integrity rules, one at a time: the table is not valid, and the group's bit is clear
static void test_integrity(void)
{
	uint8_t t[SIZE];

	// A point at 0xFF in a row the maker uses (before that row's own rule sees it)
	invented(t);
	t[0x047] = 0xFF;
	BAD(t, CS7000P_MK_F_TUNE, CS7000P_MK_WHY_ROW_UNSET, 0x047);
	invented(t);
	t[0x0A4 + 6] = 0xFF;	// the tone DAC's: used by the maker, in no group of the port's
	BAD(t, 0, CS7000P_MK_WHY_ROW_UNSET, 0x0AA);
	invented(t);
	t[0x08C] = 0xFF;
	BAD(t, CS7000P_MK_F_CTCSS, CS7000P_MK_WHY_ROW_UNSET, 0x08C);
	invented(t);
	put16(t, 0x104, 2, 0xFFFFu);
	BAD(t, CS7000P_MK_F_BURST_HI, CS7000P_MK_WHY_ROW_UNSET, 0x108);
	invented(t);
	t[0x12C + 7] = 0xFF;
	BAD(t, CS7000P_MK_F_BURST_LO, CS7000P_MK_WHY_ROW_UNSET, 0x133);
	// Rows the maker never reads may hold 0xFF
	invented(t);
	fill_row(t, 0x07C, 0xFF);
	fill_row(t, 0x0D4, 0xFF);
	fill_row(t, 0x15C, 0xFF);
	fill_row(t, 0x164, 0xFF);
	EXPECT(t, 0, CS7000P_MK_OK, true, ALL, CS7000P_MK_WHY_NONE, 0);

	// Codes outside 1-254
	invented(t);
	t[0x049] = 0;
	BAD(t, CS7000P_MK_F_TUNE, CS7000P_MK_WHY_CODE, 0x049);
	invented(t);
	t[0x074] = 0;
	BAD(t, CS7000P_MK_F_POWER, CS7000P_MK_WHY_CODE, 0x074);
	invented(t);
	t[0x0CC + 3] = 0;
	BAD(t, CS7000P_MK_F_FM_PAIR, CS7000P_MK_WHY_CODE, 0x0CF);
	invented(t);
	t[0x0B4 + 1] = 0;
	BAD(t, CS7000P_MK_F_DMR_PAIR, CS7000P_MK_WHY_CODE, 0x0B5);
	// 1 and 254 pass
	invented(t);
	t[0x0C4] = 1;
	t[0x0CC] = 254;
	EXPECT(t, 0, CS7000P_MK_OK, true, ALL, CS7000P_MK_WHY_NONE, 0);

	// Power: high above low at every point, every code over the floor
	invented(t);
	t[0x06C + 2] = LOW[2];
	BAD(t, CS7000P_MK_F_POWER, CS7000P_MK_WHY_POWER_ORDER, 0x06E);
	invented(t);
	EXPECT(t, 16u * 99u - 1u, CS7000P_MK_OK, true, ALL, CS7000P_MK_WHY_NONE, 0);
	EXPECT(t, 16u * 99u, CS7000P_MK_OK, false, ALL & ~CS7000P_MK_F_POWER, CS7000P_MK_WHY_POWER_FLOOR, 0x075);
	EXPECT(t, 16u * 100u, CS7000P_MK_OK, false, ALL & ~CS7000P_MK_F_POWER, CS7000P_MK_WHY_POWER_FLOOR, 0x074);

	// Trims: at most 64 steps from 128, either way
	static const uint8_t trim_ok[] = { 0x40, 0x80, 0xC0 }, trim_bad[] = { 0x3F, 0xC1, 0x00, 0xFF };
	for (unsigned i = 0; i < sizeof trim_ok; i++) {
		invented(t);
		t[0x0DC] = trim_ok[i];
		t[0x174] = trim_ok[i];
		EXPECT(t, 0, CS7000P_MK_OK, true, ALL, CS7000P_MK_WHY_NONE, 0);
	}
	for (unsigned i = 0; i < sizeof trim_bad; i++) {
		invented(t);
		t[0x0DC] = trim_bad[i];
		BAD(t, CS7000P_MK_F_TRIM_A, CS7000P_MK_WHY_TRIM, 0x0DC);
		invented(t);
		t[0x174] = trim_bad[i];
		BAD(t, CS7000P_MK_F_TRIM_B, CS7000P_MK_WHY_TRIM, 0x174);
	}

	// Squelch: open below close at both levels, level 1 above level 9
	invented(t);
	t[0x04C + 3] = 50;
	BAD(t, CS7000P_MK_F_SQUELCH, CS7000P_MK_WHY_SQ_HYST, 0x04F);
	invented(t);
	t[0x05C] = 130;
	BAD(t, CS7000P_MK_F_SQUELCH, CS7000P_MK_WHY_SQ_HYST, 0x05C);
	invented(t);
	t[0x05C + 4] = 40;
	BAD(t, CS7000P_MK_F_SQUELCH, CS7000P_MK_WHY_SQ_LEVEL, 0x060);
	invented(t);
	t[0x054 + 6] = 130;
	BAD(t, CS7000P_MK_F_SQUELCH, CS7000P_MK_WHY_SQ_LEVEL, 0x06A);

	// RSSI: the high level's code above the low one's
	invented(t);
	t[0x041] = 60;
	BAD(t, CS7000P_MK_F_RSSI, CS7000P_MK_WHY_RSSI_ORDER, 0x041);
	invented(t);
	t[0x041] = 59;
	BAD(t, CS7000P_MK_F_RSSI, CS7000P_MK_WHY_RSSI_ORDER, 0x041);

	// Bursts, at one point each: S below R, N = 2 (R - S) at least 5, H after R, F not before H
	invented(t);
	t[0x0F4 + 5] = 19;
	BAD(t, CS7000P_MK_F_BURST_HI, CS7000P_MK_WHY_BURST_START, 0x0F9);
	invented(t);
	t[0x0FC + 2] = 14;	// N = 4
	BAD(t, CS7000P_MK_F_BURST_HI, CS7000P_MK_WHY_BURST_RISE, 0x0FE);
	t[0x0FC + 2] = 15;	// N = 6
	EXPECT(t, 0, CS7000P_MK_OK, true, ALL, CS7000P_MK_WHY_NONE, 0);
	invented(t);
	fill16(t, 0x104, 19);	// all eight: the use rule stays quiet
	BAD(t, CS7000P_MK_F_BURST_HI, CS7000P_MK_WHY_BURST_HOLD, 0x104);
	invented(t);
	fill16(t, 0x114, 299);
	BAD(t, CS7000P_MK_F_BURST_HI, CS7000P_MK_WHY_BURST_OFF, 0x114);
	fill16(t, 0x114, 300);	// F = H passes
	EXPECT(t, 0, CS7000P_MK_OK, true, ALL, CS7000P_MK_WHY_NONE, 0);
	invented(t);
	t[0x12C] = 21;
	BAD(t, CS7000P_MK_F_BURST_LO, CS7000P_MK_WHY_BURST_START, 0x12C);
	invented(t);
	fill16(t, 0x14C, 300);
	BAD(t, CS7000P_MK_F_BURST_LO, CS7000P_MK_WHY_BURST_OFF, 0x14C);

	// Two failures: the first in the order the rules run is the one named
	invented(t);
	t[0x0DC] = 0x00;
	t[0x049] = 0;
	BAD(t, CS7000P_MK_F_TUNE | CS7000P_MK_F_TRIM_A, CS7000P_MK_WHY_CODE, 0x049);
}

// Use rules, one at a time: the table stays valid, only that group's bit is clear
static void test_use(void)
{
	uint8_t t[SIZE];

	invented(t);
	t[0x041] = 0xFF;
	USE(t, CS7000P_MK_F_RSSI, CS7000P_MK_WHY_RSSI_END, 0x041);
	invented(t);
	t[0x040] = 0;
	USE(t, CS7000P_MK_F_RSSI, CS7000P_MK_WHY_RSSI_END, 0x040);
	invented(t);
	t[0x084 + 2] = 0;
	USE(t, CS7000P_MK_F_CTCSS, CS7000P_MK_WHY_LEVEL_ZERO, 0x086);
	invented(t);
	t[0x094] = 0;
	USE(t, CS7000P_MK_F_DCS, CS7000P_MK_WHY_LEVEL_ZERO, 0x094);
	invented(t);
	put16(t, 0x104, 5, 301);
	USE(t, CS7000P_MK_F_BURST_HI, CS7000P_MK_WHY_BURST_U16, 0x10E);
	invented(t);
	put16(t, 0x14C, 7, 312);
	USE(t, CS7000P_MK_F_BURST_LO, CS7000P_MK_WHY_BURST_U16, 0x15A);
	invented(t);
	t[0x0DD] = 0x00;
	USE(t, CS7000P_MK_F_BATTERY, CS7000P_MK_WHY_BATTERY, 0x0DD);
	t[0x0DD] = 0xFF;
	USE(t, CS7000P_MK_F_BATTERY, CS7000P_MK_WHY_BATTERY, 0x0DD);

	// A use failure and an integrity failure: the integrity one is named, both bits clear
	invented(t);
	t[0x0DD] = 0x00;
	t[0x174] = 0x00;
	BAD(t, CS7000P_MK_F_BATTERY | CS7000P_MK_F_TRIM_B, CS7000P_MK_WHY_TRIM, 0x174);
}

// What nothing reads changes nothing but the CRC; what only the view shows changes no verdict and no lookup
static void test_unread_bytes(void)
{
	static const struct { uint16_t from, to; } never[] = {
		{ 0x07C, 0x083 }, { 0x0D4, 0x0DB }, { 0x0DE, 0x0DE }, { 0x0E0, 0x0EB }, { 0x16C, 0x173 },
		{ 0x175, 0x1FF },
	};
	uint8_t t[SIZE], orig[SIZE];
	cs7000p_maker_t m;
	invented(orig);
	for (unsigned r = 0; r < sizeof never / sizeof never[0]; r++) {
		for (unsigned b = never[r].from; b <= never[r].to; b++) {
			static const uint8_t vals[] = { 0x00, 0xFF, 0x5A };
			for (unsigned v = 0; v < sizeof vals; v++) {
				invented(t);
				t[b] = vals[v];
				uint32_t crc = crc32(t, SIZE);
				CHECK(cs7000p_maker_parse(t, crc, 0, &m));
				CHECK(m.crc == crc);
				CHECK(crc != base.crc || vals[v] == orig[b]);
				m.crc = base.crc;
				if (memcmp(&m, &base, sizeof m) != 0) {
					printf("FAIL %s:%d: byte 0x%03x = 0x%02x changed the parse\n", __FILE__, __LINE__, b,
					       vals[v]);
					failures++;
				}
			}
		}
	}

	// Shown at most: VOX, the tone input, the tone DAC's rows (any code but the unset one), the tuner's RSSI rows
	static const struct { uint16_t from, to; } shown[] = {
		{ 0x042, 0x043 }, { 0x0DF, 0x0DF }, { 0x09C, 0x0B3 }, { 0x15C, 0x16B },
	};
	for (unsigned r = 0; r < sizeof shown / sizeof shown[0]; r++) {
		for (unsigned b = shown[r].from; b <= shown[r].to; b++) {
			invented(t);
			t[b] = b >= 0x15C ? 0xFF : 0x01;
			CHECK(parse(t, &m));
			CHECK(m.state == base.state && m.fields == base.fields && m.why == base.why && m.valid);
			uint16_t d0 = 0, d1 = 0;
			uint8_t a0 = 0, a1 = 0, b0 = 0, b1 = 0;
			for (uint32_t hz = 400u * MHZ; hz <= 527u * MHZ; hz += 250u * KHZ) {
				CHECK(cs7000p_maker_rx_tune(&m, hz, &d0, NULL) == cs7000p_maker_rx_tune(&base, hz, &d1, NULL));
				CHECK(d0 == d1);
				CHECK(cs7000p_maker_drive(&m, hz, 1, &d0, NULL) == cs7000p_maker_drive(&base, hz, 1, &d1, NULL));
				CHECK(d0 == d1);
				CHECK(cs7000p_maker_pair(&m, hz, false, &a0, &b0, NULL) ==
				      cs7000p_maker_pair(&base, hz, false, &a1, &b1, NULL));
				CHECK(a0 == a1 && b0 == b1);
			}
		}
	}
}

// --- Lookups ---

static void test_windows(void)
{
	uint8_t t[SIZE], v = 0, why = 0;
	uint16_t dac = 0;
	cs7000p_maker_t m;
	const cs7000p_mk_row_t H = CS7000P_MK_POWER_HI;

	// Transmit: inside the 0.4 MHz switch segment, the end value on the frequency's VCO side
	CHECK(cs7000p_maker_tx_row(&base, H, 429900000u, &v, &why) && v == 194 && why == CS7000P_MK_LK_OK);
	CHECK(cs7000p_maker_tx_row(&base, H, 430000000u, &v, &why) && v == 194);
	CHECK(cs7000p_maker_tx_row(&base, H, 430000009u, &v, &why) && v == 194);	// bit 8 = 1 below 430.00001
	CHECK(cs7000p_maker_tx_row(&base, H, 430000010u, &v, &why) && v == 195);
	CHECK(cs7000p_maker_tx_row(&base, H, 430100000u, &v, &why) && v == 195);
	CHECK(cs7000p_maker_tx_row(&base, H, 429800000u, &v, &why) && v == 194);	// the points themselves
	CHECK(cs7000p_maker_tx_row(&base, H, 430200000u, &v, &why) && v == 195);
	CHECK(cs7000p_maker_tx_row(&base, H, 437600000u, &v, &why) && v == 203);	// elsewhere the maker's line
	CHECK(cs7000p_maker_drive(&base, 429900000u, 1, &dac, &why) && dac == 16u * 194u);
	CHECK(cs7000p_maker_drive(&base, 430100000u, 0, &dac, &why) && dac == 16u * 99u);
	// Past the ends: inside the transmit band only
	CHECK(cs7000p_maker_drive(&base, 400000000u, 1, &dac, &why) && dac == 16u * 190u);
	CHECK(cs7000p_maker_drive(&base, 512000000u, 1, &dac, &why) && dac == 16u * 209u);
	dac = 1234;
	CHECK(!cs7000p_maker_drive(&base, 512000001u, 1, &dac, &why) && why == CS7000P_MK_LK_BAND && dac == 1234);
	CHECK(!cs7000p_maker_drive(&base, 527000000u, 1, &dac, &why) && why == CS7000P_MK_LK_BAND);
	CHECK(!cs7000p_maker_drive(&base, 399999999u, 0, &dac, &why) && why == CS7000P_MK_LK_BAND);
	CHECK(!cs7000p_maker_drive(&base, 406050000u, 0, &dac, &why) && why == CS7000P_MK_LK_BAND);	// the gap
	CHECK(!cs7000p_maker_drive(&base, 433500000u, 2, &dac, &why) && why == CS7000P_MK_LK_ROW);

	// Receive: inside the 0.1 MHz switch segment at 479 MHz, the same side's end value
	CHECK(cs7000p_maker_rx_tune(&base, 479000000u, &dac, &why) && dac == 16u * TUNE[4] && why == CS7000P_MK_LK_OK);
	CHECK(cs7000p_maker_rx_tune(&base, 479000009u, &dac, &why) && dac == 16u * TUNE[4]);
	CHECK(cs7000p_maker_rx_tune(&base, 479000010u, &dac, &why) && dac == 16u * TUNE[5]);
	CHECK(cs7000p_maker_rx_tune(&base, 433500000u, &dac, &why) && dac == 16u * 127u);	// t = 506
	// Past the ends, extended then clamped into the DAC buffer's range
	CHECK(cs7000p_maker_rx_tune(&base, 527000000u, &dac, &why) && dac == 16u * 169u);	// t = 677
	CHECK(cs7000p_maker_rx_tune(&base, 400000000u, &dac, &why) && dac == 16u * 120u);
	// Never past the receive band's edges
	dac = 4321;
	CHECK(!cs7000p_maker_rx_tune(&base, 527000001u, &dac, &why) && why == CS7000P_MK_LK_BAND && dac == 4321);
	CHECK(!cs7000p_maker_rx_tune(&base, 399999999u, &dac, &why) && why == CS7000P_MK_LK_BAND);

	// A switch segment 16 MHz wide: transmit refused inside it, receive takes the same-side end
	invented(t);
	static const uint32_t tx_wide[8] = { 400200000u, 410000000u, 420000000u, 436000000u, 445000000u, 458000000u,
					     472000000u, 492000000u };
	static const uint32_t rx_wide[8] = { 400500000u, 420000000u, 440000000u, 460000000u, 470000000u, 486000000u,
					     500000000u, 511500000u };
	put_points(t, 0x000, tx_wide);
	put_points(t, 0x020, rx_wide);
	CHECK(parse(t, &m));
	CHECK(!cs7000p_maker_tx_row(&m, H, 425000000u, &v, &why) && why == CS7000P_MK_LK_WINDOW);
	CHECK(!cs7000p_maker_tx_row(&m, H, 431000000u, &v, &why) && why == CS7000P_MK_LK_WINDOW);
	CHECK(!cs7000p_maker_drive(&m, 430000000u, 0, &dac, &why) && why == CS7000P_MK_LK_WINDOW);
	CHECK(cs7000p_maker_tx_row(&m, H, 420000000u, &v, &why) && v == HIGH[2]);
	CHECK(cs7000p_maker_tx_row(&m, H, 436000000u, &v, &why) && v == HIGH[3]);
	CHECK(cs7000p_maker_rx_tune(&m, 475000000u, &dac, &why) && dac == 16u * TUNE[4]);
	CHECK(cs7000p_maker_rx_tune(&m, 482000000u, &dac, &why) && dac == 16u * TUNE[5]);

	// Every TX point above the switch: 420 MHz would extend across it
	invented(t);
	static const uint32_t tx_high[8] = { 431000000u, 440000000u, 450000000u, 460000000u, 470000000u, 480000000u,
					     490000000u, 511000000u };
	put_points(t, 0x000, tx_high);
	CHECK(parse(t, &m));
	CHECK(!cs7000p_maker_tx_row(&m, H, 420000000u, &v, &why) && why == CS7000P_MK_LK_SWITCH);
	CHECK(!cs7000p_maker_tx_row(&m, H, 430000000u, &v, &why) && why == CS7000P_MK_LK_SWITCH);
	CHECK(cs7000p_maker_tx_row(&m, H, 430000010u, &v, &why));
	CHECK(cs7000p_maker_tx_row(&m, H, 431500000u, &v, &why) && why == CS7000P_MK_LK_OK);
	CHECK(cs7000p_maker_tx_row(&m, H, 512000000u, &v, &why));
	// Every TX point below it: the same the other way
	invented(t);
	static const uint32_t tx_low[8] = { 400200000u, 404000000u, 408000000u, 412000000u, 416000000u, 420000000u,
					    425000000u, 429900000u };
	put_points(t, 0x000, tx_low);
	CHECK(parse(t, &m));
	CHECK(!cs7000p_maker_tx_row(&m, H, 433500000u, &v, &why) && why == CS7000P_MK_LK_SWITCH);
	CHECK(cs7000p_maker_tx_row(&m, H, 430000000u, &v, &why) && why == CS7000P_MK_LK_OK);
	CHECK(cs7000p_maker_tx_row(&m, H, 400000000u, &v, &why));
	// Every RX point above 479.00001 MHz: 470 MHz keeps today's tune
	invented(t);
	static const uint32_t rx_high[8] = { 480000000u, 485000000u, 490000000u, 495000000u, 500000000u, 505000000u,
					     510000000u, 520000000u };
	put_points(t, 0x020, rx_high);
	CHECK(parse(t, &m));
	dac = CS7000P_RX_TUNE_NOMINAL;
	CHECK(!cs7000p_maker_rx_tune(&m, 470000000u, &dac, &why) && why == CS7000P_MK_LK_SWITCH);
	CHECK(dac == CS7000P_RX_TUNE_NOMINAL);
	CHECK(cs7000p_maker_rx_tune(&m, 525000000u, &dac, &why) && why == CS7000P_MK_LK_OK);
	// Every RX point below it: 490 MHz keeps today's tune, 478 MHz is the table's
	invented(t);
	static const uint32_t rx_low[8] = { 400500000u, 410000000u, 420000000u, 430000000u, 440000000u, 450000000u,
					    465000000u, 478000000u };
	put_points(t, 0x020, rx_low);
	CHECK(parse(t, &m));
	dac = CS7000P_RX_TUNE_NOMINAL;
	CHECK(!cs7000p_maker_rx_tune(&m, 490000000u, &dac, &why) && why == CS7000P_MK_LK_SWITCH);
	CHECK(dac == CS7000P_RX_TUNE_NOMINAL);
	CHECK(cs7000p_maker_rx_tune(&m, 479000000u, &dac, &why) && why == CS7000P_MK_LK_OK);	// extended, low VCO
	CHECK(cs7000p_maker_rx_tune(&m, 478000000u, &dac, &why) && dac == 16u * TUNE[7]);

	// The switch segment the first one: below P[0] its line would carry the high VCO's P[1] into the low side
	invented(t);
	static const uint32_t tx_first[8] = { 429900000u, 430500000u, 440000000u, 450000000u, 460000000u, 470000000u,
					      490000000u, 511000000u };
	put_points(t, 0x000, tx_first);
	CHECK(parse(t, &m));
	CHECK(!cs7000p_maker_tx_row(&m, H, 429000000u, &v, &why) && why == CS7000P_MK_LK_SWITCH);
	CHECK(!cs7000p_maker_tx_row(&m, H, 429899990u, &v, &why) && why == CS7000P_MK_LK_SWITCH);
	CHECK(cs7000p_maker_tx_row(&m, H, 429900000u, &v, &why) && v == HIGH[0]);	// the point itself
	CHECK(cs7000p_maker_tx_row(&m, H, 430000000u, &v, &why) && v == HIGH[0]);	// inside, low side
	CHECK(cs7000p_maker_tx_row(&m, H, 430200000u, &v, &why) && v == HIGH[1]);	// inside, high side
	// The switch segment the last one: past P[7] its line would carry the low VCO's P[6] into the high side
	invented(t);
	static const uint32_t rx_last[8] = { 400500000u, 420000000u, 440000000u, 460000000u, 470000000u, 475000000u,
					     478950000u, 479050000u };
	put_points(t, 0x020, rx_last);
	CHECK(parse(t, &m));
	dac = CS7000P_RX_TUNE_NOMINAL;
	CHECK(!cs7000p_maker_rx_tune(&m, 479050010u, &dac, &why) && why == CS7000P_MK_LK_SWITCH);
	CHECK(!cs7000p_maker_rx_tune(&m, 500000000u, &dac, &why) && why == CS7000P_MK_LK_SWITCH);
	CHECK(dac == CS7000P_RX_TUNE_NOMINAL);
	CHECK(cs7000p_maker_rx_tune(&m, 479050000u, &dac, &why) && dac == 16u * TUNE[7]);
	CHECK(cs7000p_maker_rx_tune(&m, 479000000u, &dac, &why) && dac == 16u * TUNE[6]);
	CHECK(cs7000p_maker_rx_tune(&m, 400000000u, &dac, &why) && why == CS7000P_MK_LK_OK);	// the other end extends

	// Rows by direction
	CHECK(!cs7000p_maker_tx_row(&base, CS7000P_MK_TUNE, 433500000u, &v, &why) && why == CS7000P_MK_LK_ROW);
	CHECK(!cs7000p_maker_rx_row(&base, H, 433500000u, &v, &why) && why == CS7000P_MK_LK_ROW);
	CHECK(!cs7000p_maker_tx_row(&base, CS7000P_MK_ROWS, 433500000u, &v, &why) && why == CS7000P_MK_LK_ROW);
	CHECK(cs7000p_maker_rx_row(&base, CS7000P_MK_RSSI_HI_ROW, 433500000u, &v, &why) && v == 159);
}

// The clamp table: what transmit refuses and what receive uses
static void test_clamps(void)
{
	static const uint8_t r1[8] = { 100, 120, 121, 150, 180, 220, 250, 254 };
	static const uint8_t r2[8] = { 1, 250, 250, 250, 250, 250, 250, 250 };
	static const uint8_t r3[8] = { 1, 254, 254, 254, 254, 254, 254, 254 };
	uint8_t t[SIZE], v = 0, why = 0;
	uint16_t dac = 0;
	cs7000p_maker_t m;
	const cs7000p_mk_row_t P = CS7000P_MK_FM_MOD2;

	// The rows in a pair (transmit) and in the tune (receive), the RX points the TX ones
	invented(t);
	put_points(t, 0x020, TX_PTS);
	put_row(t, 0x0C4, r1);
	put_row(t, 0x044, r1);
	CHECK(parse(t, &m));
	CHECK(!cs7000p_maker_tx_row(&m, P, 527000000u, &v, &why) && why == CS7000P_MK_LK_BAND);
	CHECK(cs7000p_maker_tx_row(&m, P, 512000000u, &v, &why) && v == 254);
	CHECK(cs7000p_maker_rx_tune(&m, 527000000u, &dac, &why) && dac == CS7000P_TUNE_MAX);	// 255 x 16, clamped
	CHECK(cs7000p_maker_rx_row(&m, CS7000P_MK_TUNE, 527000000u, &v, &why) && v == 255);

	put_row(t, 0x0C4, r2);
	put_row(t, 0x044, r2);
	CHECK(parse(t, &m));
	CHECK(!cs7000p_maker_tx_row(&m, P, 400000000u, &v, &why) && why == CS7000P_MK_LK_MATH);	// clamped
	CHECK(!cs7000p_maker_tx_row(&m, P, 400100000u, &v, &why) && why == CS7000P_MK_LK_CODE);	// 0
	CHECK(cs7000p_maker_tx_row(&m, P, 400200000u, &v, &why) && v == 1);
	CHECK(cs7000p_maker_rx_row(&m, CS7000P_MK_TUNE, 400000000u, &v, &why) && v == 0);
	CHECK(cs7000p_maker_rx_tune(&m, 400000000u, &dac, &why) && dac == CS7000P_TUNE_MIN);

	put_row(t, 0x0C4, r3);
	put_row(t, 0x044, r3);
	CHECK(parse(t, &m));
	CHECK(!cs7000p_maker_tx_row(&m, P, 429800000u, &v, &why) && why == CS7000P_MK_LK_MATH);	// overflow
	CHECK(cs7000p_maker_tx_row(&m, P, 420000000u, &v, &why) && v == 170);
	dac = CS7000P_RX_TUNE_NOMINAL;
	CHECK(!cs7000p_maker_rx_tune(&m, 429800000u, &dac, &why) && why == CS7000P_MK_LK_MATH);
	CHECK(dac == CS7000P_RX_TUNE_NOMINAL);
	uint8_t a = 0, b = 0;
	CHECK(!cs7000p_maker_pair(&m, 429800000u, false, &a, &b, &why) && a == 0 && b == 0);
	CHECK(cs7000p_maker_pair(&m, 420000000u, false, &a, &b, &why) && a == 170 && b == 140);

	// A drive the line takes under the floor past the last point, the points themselves over it
	invented(t);
	static const uint32_t tx_pts[8] = { 400200000u, 429800000u, 430200000u, 445000000u, 458000000u, 472000000u,
					    500000000u, 501000000u };
	static const uint8_t low[8] = { 110, 105, 105, 108, 115, 122, 200, 101 };
	put_points(t, 0x000, tx_pts);
	put_row(t, 0x074, low);
	CHECK(cs7000p_maker_parse(t, 0, 1610, &m));
	CHECK(cs7000p_maker_drive(&m, 501000000u, 0, &dac, &why) && dac == 16u * 101u);
	dac = 77;
	CHECK(!cs7000p_maker_drive(&m, 501010000u, 0, &dac, &why) && why == CS7000P_MK_LK_FLOOR && dac == 77);
}

static void test_settings(void)
{
	uint8_t t[SIZE], o = 0, c = 0, l = 0, why = 0;
	cs7000p_maker_t m;
	const uint32_t f = 433500000u;

	CHECK(cs7000p_maker_squelch(&base, f, 2, &o, &c, &why) && o == 110 && c == 120);
	CHECK(cs7000p_maker_squelch(&base, f, 5, &o, &c, &why) && o == 80 && c == 90);
	CHECK(cs7000p_maker_squelch(&base, f, 8, &o, &c, &why) && o == 50 && c == 60);
	CHECK(cs7000p_maker_squelch(&base, f, 1, &o, &c, &why) && o == 120 && c == 130);
	CHECK(cs7000p_maker_squelch(&base, f, 9, &o, &c, &why) && o == 40 && c == 50);
	CHECK(cs7000p_maker_squelch(&base, f, 0, &o, &c, &why) && o == 255 && c == 255);
	o = c = 0;
	CHECK(cs7000p_maker_squelch(&base, f, 10, &o, &c, &why) && o == 255 && c == 255);

	CHECK(cs7000p_maker_ctcss(&base, f, 885, false, &l, &why) && l == 41);
	CHECK(cs7000p_maker_ctcss(&base, f, 1000, false, &l, &why) && l == 42);
	CHECK(cs7000p_maker_ctcss(&base, f, 2503, false, &l, &why) && l == 52);
	CHECK(cs7000p_maker_ctcss(&base, f, 885, true, &l, &why) && l == 82);
	CHECK(cs7000p_maker_dcs(&base, f, false, &l, &why) && l == 200);
	CHECK(cs7000p_maker_dcs(&base, f, true, &l, &why) && l == 254);	// doubled, at most 254
	// Transmit rules on the sub-audio rows: outside the band nothing
	CHECK(!cs7000p_maker_ctcss(&base, 520000000u, 885, false, &l, &why) && why == CS7000P_MK_LK_BAND);

	cs7000p_mk_burst_t b;
	memset(&b, 0, sizeof b);
	CHECK(cs7000p_maker_burst(&base, f, 1, &b, &why));
	CHECK(b.adv == 20 && b.start == 12 && b.rise == 19 && b.hold == 300 && b.off == 310);
	CHECK(cs7000p_maker_burst(&base, f, 0, &b, &why));
	CHECK(b.adv == 21 && b.start == 12 && b.rise == 20 && b.hold == 301 && b.off == 311);
	CHECK(!cs7000p_maker_burst(&base, f, 2, &b, &why) && why == CS7000P_MK_LK_ROW);

	// Every point passes the burst rules (R - S 7 and 4), the line past the last one does not
	invented(t);
	static const uint32_t pts[8] = { 400200000u, 429800000u, 430200000u, 445000000u, 458000000u, 465000000u,
					 472000000u, 492000000u };
	put_points(t, 0x000, pts);
	t[0x0F4 + 6] = 10;
	t[0x0F4 + 7] = 14;
	t[0x0FC + 6] = 17;
	t[0x0FC + 7] = 18;
	CHECK(parse(t, &m) && (m.fields & CS7000P_MK_F_BURST_HI));
	CHECK(cs7000p_maker_burst(&m, 492000000u, 1, &b, &why) && b.start == 14 && b.rise == 18);
	CHECK(cs7000p_maker_burst(&m, 495000000u, 1, &b, &why) && b.start == 15 && b.rise == 18);	// N = 6
	memset(&b, 0, sizeof b);
	CHECK(!cs7000p_maker_burst(&m, 500000000u, 1, &b, &why) && why == CS7000P_MK_LK_BURST);	// N = 4
	CHECK(!cs7000p_maker_burst(&m, 512000000u, 1, &b, &why) && why == CS7000P_MK_LK_BURST);	// S 18, R 19
	CHECK(b.start == 0 && b.rise == 0 && b.hold == 0);
	CHECK(cs7000p_maker_burst(&m, 512000000u, 0, &b, &why));	// the low level's rows are flat

	// A use rule clears what depends on its group and nothing else
	invented(t);
	t[0x084] = 0;
	CHECK(parse(t, &m));
	CHECK(!cs7000p_maker_ctcss(&m, f, 885, false, &l, &why) && why == CS7000P_MK_LK_GROUP);
	CHECK(cs7000p_maker_dcs(&m, f, false, &l, &why));
	invented(t);
	put16(t, 0x104, 3, 299);
	CHECK(parse(t, &m));
	CHECK(!cs7000p_maker_burst(&m, f, 1, &b, &why) && why == CS7000P_MK_LK_GROUP);
	CHECK(cs7000p_maker_burst(&m, f, 0, &b, &why));
}

static void test_trims_rssi(void)
{
	uint8_t t[SIZE];
	cs7000p_maker_t m;
	int16_t d = 0;
	uint16_t bias = 0;

	// The codings of d
	CHECK(cs7000p_maker_trim_d(0x80) == 0 && cs7000p_maker_trim_d(0x7B) == -5 && cs7000p_maker_trim_d(0xFF) == 127);
	CHECK(cs7000p_maker_trim_d(0x00) == -128);
	CHECK(cs7000p_maker_bias_hold(-5) == 0x1FB && cs7000p_maker_bias_hold(0) == 0x200);
	CHECK(cs7000p_maker_reg04(-5) == 0xFB && cs7000p_maker_reg04(5) == 0x05 && cs7000p_maker_reg04(-128) == 0x80);
	CHECK(cs7000p_maker_bias_signed(-5) == 0x3FB);
	CHECK((cs7000p_maker_bias_signed(-5) & 0xFFu) == 0xFB && (cs7000p_maker_bias_signed(-5) >> 8) == 0x03);
	CHECK(cs7000p_maker_bias_signed(5) == 0x005 && cs7000p_maker_bias_signed(0) == 0);
	// The hold's low byte and the signed word's are the same byte (d mod 1024 and 0x200 + d)
	for (int16_t k = -64; k <= 64; k++) {
		CHECK((cs7000p_maker_bias_hold(k) & 0xFFu) == (cs7000p_maker_bias_signed(k) & 0xFFu));
		CHECK(cs7000p_maker_reg04(k) == (uint8_t)((uint8_t)(k + 128) ^ 0x80u));
	}

	CHECK(cs7000p_maker_trim(&base, false, &d) && d == -5);
	CHECK(cs7000p_maker_trim(&base, true, &d) && d == -3);
	CHECK(cs7000p_maker_rx_bias(&base, &bias) && bias == 0x1FB);

	// The receive bound: 32 steps either way
	static const struct { uint8_t v; bool rx; uint16_t bias; } rx[] = {
		{ 0xA0, true, 0x220 }, { 0xA1, false, 0 }, { 0x60, true, 0x1E0 }, { 0x5F, false, 0 }, { 0xC0, false, 0 },
	};
	for (unsigned i = 0; i < sizeof rx / sizeof rx[0]; i++) {
		invented(t);
		t[0x0DC] = rx[i].v;
		CHECK(parse(t, &m) && (m.fields & CS7000P_MK_F_TRIM_A));	// the group passes either way
		CHECK(cs7000p_maker_trim(&m, false, &d) && d == cs7000p_maker_trim_d(rx[i].v));
		bias = 0x200;
		CHECK(cs7000p_maker_rx_bias(&m, &bias) == rx[i].rx);
		CHECK(bias == (rx[i].rx ? rx[i].bias : 0x200));
	}

	// RSSI on the two points (60, 160): a floor below the low one
	int16_t dbm = 0;
	CHECK(cs7000p_maker_rssi_dbm(&base, 110u << 4, &dbm) && dbm == -95);
	CHECK(cs7000p_maker_rssi_dbm(&base, (110u << 4) + 15u, &dbm) && dbm == -95);
	CHECK(cs7000p_maker_rssi_dbm(&base, 59u << 4, &dbm) && dbm == -121);
	CHECK(cs7000p_maker_rssi_dbm(&base, 60u << 4, &dbm) && dbm == -120);
	CHECK(cs7000p_maker_rssi_dbm(&base, 160u << 4, &dbm) && dbm == -70);
	CHECK(cs7000p_maker_rssi_dbm(&base, 0, &dbm) && dbm == -150);
	CHECK(cs7000p_maker_rssi_dbm(&base, 4095, &dbm) && dbm == -23);
	CHECK(cs7000p_maker_rssi_dbm(&base, 0xFFFFFFFFu, &dbm) && dbm == -23);
	int16_t prev = -32768;
	for (uint32_t raw = 0; raw <= 4095; raw++) {
		CHECK(cs7000p_maker_rssi_dbm(&base, raw, &dbm) && dbm >= prev);
		prev = dbm;
	}
	invented(t);
	t[0x041] = 0xFF;
	CHECK(parse(t, &m));
	dbm = 7;
	CHECK(!cs7000p_maker_rssi_dbm(&m, 1760, &dbm) && dbm == 7);
}

// A table that is not valid gives nothing; a valid one whose group failed gives nothing of that group
static void test_refused(void)
{
	uint8_t t[SIZE], v = 0, why = 0, a = 0, b = 0;
	uint16_t dac = 0;
	int16_t d = 0, dbm = 0;
	cs7000p_maker_t m;

	const uint8_t *tables[4];
	uint8_t blank[SIZE], zero[SIZE], trim[SIZE], points[SIZE];
	memset(blank, 0xFF, SIZE);
	memset(zero, 0, SIZE);
	invented(trim);
	trim[0x0DC] = 0x00;	// an integrity failure in a group receive does not use
	invented(points);
	put32(points, 0x020, 0xFFFFFFFFu);
	tables[0] = blank;
	tables[1] = zero;
	tables[2] = trim;
	tables[3] = points;
	for (unsigned i = 0; i < 4; i++) {
		CHECK(!parse(tables[i], &m));
		for (uint32_t hz = 400u * MHZ; hz <= 527u * MHZ; hz += 1u * MHZ) {
			CHECK(!cs7000p_maker_rx_tune(&m, hz, &dac, &why) && why == CS7000P_MK_LK_TABLE);
			CHECK(!cs7000p_maker_drive(&m, hz, 0, &dac, &why) && why == CS7000P_MK_LK_TABLE);
			CHECK(!cs7000p_maker_drive(&m, hz, 1, &dac, &why));
			CHECK(!cs7000p_maker_pair(&m, hz, false, &a, &b, &why) && !cs7000p_maker_pair(&m, hz, true, &a, &b, &why));
			CHECK(!cs7000p_maker_squelch(&m, hz, 5, &a, &b, &why));
			CHECK(!cs7000p_maker_ctcss(&m, hz, 885, false, &a, &why) && !cs7000p_maker_dcs(&m, hz, false, &a, &why));
			CHECK(!cs7000p_maker_tx_row(&m, CS7000P_MK_TONE_DTMF, hz, &v, &why));
		}
		CHECK(!cs7000p_maker_trim(&m, false, &d) && !cs7000p_maker_trim(&m, true, &d));
		CHECK(!cs7000p_maker_rx_bias(&m, &dac) && !cs7000p_maker_rssi_dbm(&m, 1000, &dbm));
	}
	CHECK(!cs7000p_maker_rx_tune(NULL, 433500000u, &dac, &why) && why == CS7000P_MK_LK_TABLE);
	CHECK(!cs7000p_maker_parse(NULL, 0, 0, &m) && !cs7000p_maker_drive(&m, 433500000u, 0, &dac, &why));

	invented(t);
	t[0x0C4] = 0;	// FM pair refused: the table is not valid, so nothing at all
	CHECK(!parse(t, &m) && !cs7000p_maker_pair(&m, 433500000u, true, &a, &b, &why) && why == CS7000P_MK_LK_TABLE);
}

static void test_names(void)
{
	for (unsigned i = 0; i < CS7000P_MK_WHY_COUNT; i++) {
		CHECK(strcmp(cs7000p_maker_why_name((uint8_t)i), "?") != 0);
		for (unsigned k = 0; k < i; k++) {
			CHECK(strcmp(cs7000p_maker_why_name((uint8_t)i), cs7000p_maker_why_name((uint8_t)k)) != 0);
		}
	}
	for (unsigned i = 0; i < CS7000P_MK_LK_COUNT; i++) {
		CHECK(strcmp(cs7000p_maker_lookup_name((uint8_t)i), "?") != 0);
	}
	for (unsigned i = CS7000P_MK_UNREAD; i <= CS7000P_MK_POINTS; i++) {
		CHECK(strcmp(cs7000p_maker_state_name((uint8_t)i), "?") != 0);
	}
	CHECK(strcmp(cs7000p_maker_why_name(CS7000P_MK_WHY_COUNT), "?") == 0);
	CHECK(strcmp(cs7000p_maker_state_name(CS7000P_MK_OK), "ok") == 0);
	CHECK(strcmp(cs7000p_maker_lookup_name(CS7000P_MK_LK_COUNT), "?") == 0);
}

int main(void)
{
	test_vectors();
	test_valid();
	test_whole();
	test_integrity();
	test_use();
	test_unread_bytes();
	test_windows();
	test_clamps();
	test_settings();
	test_trims_rssi();
	test_refused();
	test_names();
	printf("# cs7000pMakerCalTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
