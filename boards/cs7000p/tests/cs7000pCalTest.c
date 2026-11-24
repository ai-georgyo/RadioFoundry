// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS calibration (rfcal_cs7000p.c). As shipped every table is unset: no frequency, level or mode
// may transmit and no level has a drive. On a made-up table: the interpolation, the ends, and that a row with one
// point missing is no row. The RSSI and noise mappings never step back. The receive getters with the maker's table
// (invented here): a refused one, or none, gives every value as without it; a valid one each value from its own field
// group, the tune row's lookup and trim A's receive bound falling back alone.
#include <stdio.h>
#include <string.h>
#include "rfcal_cs7000p.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define MHZ	1000000u

static void test_shipped(void)
{
	const cs7000p_cal_t *c = &cs7000p_cal_model;
	uint16_t d = 1234;
	uint8_t m2 = 12, m1 = 34;

	for (uint32_t hz = 380u * MHZ; hz <= 540u * MHZ; hz += 125000u) {
		for (uint8_t level = 0; level < 4; level++) {
			CHECK(!cs7000p_cal_tx_ok(c, hz, level, false) && !cs7000p_cal_tx_ok(c, hz, level, true));
			CHECK(!cs7000p_cal_drive(c, hz, level, &d));
		}
		CHECK(!cs7000p_cal_mod_pair(c, hz, false, &m2, &m1) && !cs7000p_cal_mod_pair(c, hz, true, &m2, &m1));
		// The front end still gets a tuning value: the nominal one
		CHECK(cs7000p_cal_rx_tune(c, hz) == CS7000P_RX_TUNE_NOMINAL);
	}
	CHECK(d == 1234 && m2 == 12 && m1 == 34);	// a refusal writes nothing
	CHECK(!cs7000p_cal_any_tx(c));
	CHECK(!c->ref_set && cs7000p_cal_ref_offset(c) == CS7000P_REF_OFFSET && CS7000P_REF_OFFSET == 0);
	CHECK(c->floor == 0);
	// Every point of every row, by name: nobody filled one in without the others
	for (unsigned i = 0; i < CS7000P_CAL_POINTS; i++) {
		for (unsigned l = 0; l < CS7000P_POWER_LEVELS; l++) {
			CHECK(c->drive[l][i] == CS7000P_CAL_UNSET);
		}
		CHECK(c->rx_tune[i] == CS7000P_CAL_UNSET);
		CHECK(c->fm_mod2[i] == CS7000P_CAL_UNSET && c->fm_mod1[i] == CS7000P_CAL_UNSET);
		CHECK(c->dmr_mod2[i] == CS7000P_CAL_UNSET && c->dmr_mod1[i] == CS7000P_CAL_UNSET);
	}
	CHECK(CS7000P_CAL_POINTS == 9u && CS7000P_CAL_FIRST_HZ == 400u * MHZ && CS7000P_CAL_STEP_HZ == 15u * MHZ);
	CHECK(CS7000P_RX_TUNE_NOMINAL >= CS7000P_TUNE_MIN && CS7000P_RX_TUNE_NOMINAL <= CS7000P_TUNE_MAX);
}

// A table as a measured radio would have it: 400, 415, ... 520 MHz
static const cs7000p_cal_t synth = {
	.drive = {
		{ 1000, 1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800 },
		{ 3000, 2900, 2800, 2700, 2600, 2500, 2400, 2300, 2200 },
	},
	.floor = 300,
	.rx_tune = { 100, 600, 1100, 1600, 2100, 2600, 3100, 3600, 4000 },
	.fm_mod2 = { 10, 20, 30, 40, 50, 60, 70, 80, 90 },
	.fm_mod1 = { 255, 240, 225, 210, 195, 180, 165, 150, 135 },
	.dmr_mod2 = { 1, 2, 3, 4, 5, 6, 7, 8, 9 },
	.dmr_mod1 = { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	.ref_set = true,
	.ref_offset = -7,
};

static void test_interpolation(void)
{
	uint16_t d = 0;
	uint8_t m2 = 0, m1 = 0;

	// On a point, halfway, a third of the way, falling and rising
	CHECK(cs7000p_cal_drive(&synth, 400u * MHZ, 0, &d) && d == 1000);
	CHECK(cs7000p_cal_drive(&synth, 415u * MHZ, 0, &d) && d == 1100);
	CHECK(cs7000p_cal_drive(&synth, 407500000u, 0, &d) && d == 1050);
	CHECK(cs7000p_cal_drive(&synth, 405u * MHZ, 0, &d) && d == 1033);
	CHECK(cs7000p_cal_drive(&synth, 407500000u, 1, &d) && d == 2950);
	CHECK(cs7000p_cal_drive(&synth, 410u * MHZ, 1, &d) && d == 2933);
	CHECK(cs7000p_cal_drive(&synth, 505u * MHZ, 0, &d) && d == 1700);
	CHECK(cs7000p_cal_drive(&synth, 512u * MHZ, 0, &d) && d == 1747);	// 1700 + 100 x 7 / 15
	// The ends: flat below the first point and from the last one on
	CHECK(cs7000p_cal_drive(&synth, 399u * MHZ, 0, &d) && d == 1000);
	CHECK(cs7000p_cal_drive(&synth, 0, 1, &d) && d == 3000);
	CHECK(cs7000p_cal_drive(&synth, 520u * MHZ, 0, &d) && d == 1800);
	CHECK(cs7000p_cal_drive(&synth, 527u * MHZ, 1, &d) && d == 2200);
	CHECK(cs7000p_cal_drive(&synth, 0xFFFFFFFFu, 0, &d) && d == 1800);
	// Between two points the value stays between them, all the way up the band
	uint16_t last = 0;
	for (uint32_t hz = 400u * MHZ; hz <= 520u * MHZ; hz += 12500u) {
		CHECK(cs7000p_cal_drive(&synth, hz, 0, &d) && d >= last && d >= 1000 && d <= 1800);
		last = d;
	}
	// No such level
	d = 77;
	CHECK(!cs7000p_cal_drive(&synth, 440u * MHZ, CS7000P_POWER_LEVELS, &d) && d == 77);
	CHECK(!cs7000p_cal_drive(&synth, 440u * MHZ, 255, &d));

	// 247.5: a half goes away from the row's start
	CHECK(cs7000p_cal_mod_pair(&synth, 407500000u, false, &m2, &m1) && m2 == 15 && m1 == 247);
	CHECK(cs7000p_cal_mod_pair(&synth, 430u * MHZ, true, &m2, &m1) && m2 == 3 && m1 == 0);
	CHECK(cs7000p_cal_ref_offset(&synth) == -7);
	// The tuning value, kept inside the DAC buffer's range at both ends
	CHECK(cs7000p_cal_rx_tune(&synth, 400u * MHZ) == CS7000P_TUNE_MIN);
	CHECK(cs7000p_cal_rx_tune(&synth, 430u * MHZ) == 1100 && cs7000p_cal_rx_tune(&synth, 437500000u) == 1350);
	CHECK(cs7000p_cal_rx_tune(&synth, 520u * MHZ) == CS7000P_TUNE_MAX);
	CHECK(cs7000p_cal_tune_clamp(0) == CS7000P_TUNE_MIN && cs7000p_cal_tune_clamp(4095) == CS7000P_TUNE_MAX &&
	      cs7000p_cal_tune_clamp(2048) == 2048 && cs7000p_cal_tune_clamp(0xFFFFFFFFu) == CS7000P_TUNE_MAX);
}

static void test_verdict(void)
{
	CHECK(cs7000p_cal_any_tx(&synth));
	for (uint32_t hz = 400u * MHZ; hz <= 512u * MHZ; hz += 500000u) {
		CHECK(cs7000p_cal_tx_ok(&synth, hz, 0, false) && cs7000p_cal_tx_ok(&synth, hz, 1, false));
		CHECK(cs7000p_cal_tx_ok(&synth, hz, 0, true) && cs7000p_cal_tx_ok(&synth, hz, 1, true));
		CHECK(!cs7000p_cal_tx_ok(&synth, hz, 2, false));
	}
	// One point of a level's row unset: that level is refused at every frequency, the other level is not
	for (unsigned i = 0; i < CS7000P_CAL_POINTS; i++) {
		cs7000p_cal_t c = synth;
		uint16_t d;
		c.drive[1][i] = CS7000P_CAL_UNSET;
		for (uint32_t hz = 400u * MHZ; hz <= 520u * MHZ; hz += 5u * MHZ) {
			CHECK(!cs7000p_cal_tx_ok(&c, hz, 1, false) && !cs7000p_cal_tx_ok(&c, hz, 1, true));
			CHECK(!cs7000p_cal_drive(&c, hz, 1, &d));
			CHECK(cs7000p_cal_tx_ok(&c, hz, 0, false));
		}
		// The same for a modulation row: that mode is refused, the other is not
		c = synth;
		c.fm_mod1[i] = CS7000P_CAL_UNSET;
		CHECK(!cs7000p_cal_tx_ok(&c, 440u * MHZ, 0, false) && cs7000p_cal_tx_ok(&c, 440u * MHZ, 0, true));
		c = synth;
		c.dmr_mod2[i] = CS7000P_CAL_UNSET;
		CHECK(cs7000p_cal_tx_ok(&c, 440u * MHZ, 0, false) && !cs7000p_cal_tx_ok(&c, 440u * MHZ, 0, true));
		// And a tuning row with a hole falls back to the nominal value as a whole
		c = synth;
		c.rx_tune[i] = CS7000P_CAL_UNSET;
		CHECK(cs7000p_cal_rx_tune(&c, 430u * MHZ) == CS7000P_RX_TUNE_NOMINAL);
	}
	cs7000p_cal_t c = synth;
	// Without the reference offset nothing transmits, though every row is there
	c.ref_set = false;
	CHECK(!cs7000p_cal_tx_ok(&c, 440u * MHZ, 0, false) && !cs7000p_cal_tx_ok(&c, 440u * MHZ, 1, true));
	CHECK(!cs7000p_cal_any_tx(&c) && cs7000p_cal_ref_offset(&c) == CS7000P_REF_OFFSET);
	// A value no DAC or register takes makes the row unset
	c = synth;
	c.drive[0][4] = CS7000P_DAC_MAX + 1u;
	CHECK(!cs7000p_cal_tx_ok(&c, 440u * MHZ, 0, false));
	c.drive[0][4] = CS7000P_DAC_MAX;
	CHECK(cs7000p_cal_tx_ok(&c, 440u * MHZ, 0, false));
	c = synth;
	c.fm_mod2[0] = 256;
	CHECK(!cs7000p_cal_tx_ok(&c, 440u * MHZ, 0, false));
	// A drive at or under the floor, or of 0, keys nothing and is no calibration
	c = synth;
	c.floor = 1700;
	CHECK(cs7000p_cal_tx_ok(&c, 520u * MHZ, 0, false) && !cs7000p_cal_tx_ok(&c, 505u * MHZ, 0, false));
	CHECK(!cs7000p_cal_tx_ok(&c, 400u * MHZ, 0, false) && cs7000p_cal_tx_ok(&c, 400u * MHZ, 1, false));
	c = synth;
	c.floor = 0;
	for (unsigned i = 0; i < CS7000P_CAL_POINTS; i++) {
		c.drive[0][i] = 0;
	}
	CHECK(!cs7000p_cal_tx_ok(&c, 440u * MHZ, 0, false) && cs7000p_cal_tx_ok(&c, 440u * MHZ, 1, false));
}

static void test_status(void)
{
	// The data sheet's two points, and a straight line through them
	CHECK(cs7000p_rssi_dbm(300) == -100 && cs7000p_rssi_dbm(2000) == -30);
	CHECK(cs7000p_rssi_dbm(1150) == -65);
	CHECK(cs7000p_rssi_dbm(0) < -100 && cs7000p_rssi_dbm(0) >= -113);
	int16_t last = cs7000p_rssi_dbm(0);
	for (uint32_t mv = 0; mv <= 4000; mv++) {
		int16_t v = cs7000p_rssi_dbm(mv);
		CHECK(v >= last && v <= last + 1);	// never back, never a jump
		last = v;
	}
	CHECK(cs7000p_rssi_dbm(0xFFFFFFFFu) == last && last < 30);
	// Noise: larger = more noise, the whole 12-bit reading onto 0..255, and "can't hear" while not receiving
	uint8_t n = 0;
	for (uint32_t raw = 0; raw <= 4095; raw++) {
		CHECK(cs7000p_noise(raw, true) >= n);
		n = cs7000p_noise(raw, true);
		CHECK(cs7000p_noise(raw, false) == 255);
	}
	CHECK(cs7000p_noise(0, true) == 0 && cs7000p_noise(4095, true) == 255 && cs7000p_noise(1600, true) == 100);
	CHECK(cs7000p_noise(0xFFFFFFFFu, true) == 255);
}

// --- Receive, the maker's table first (cs7000p_cal_*_m) ---

// An invented maker's table that passes every rule (calibration-cs7000p.md 3's layout, cs7000pMakerCalTest's values)
static void put32be(uint8_t *t, unsigned off, uint32_t v)
{
	t[off] = (uint8_t)(v >> 24);
	t[off + 1] = (uint8_t)(v >> 16);
	t[off + 2] = (uint8_t)(v >> 8);
	t[off + 3] = (uint8_t)v;
}

static void fill16(uint8_t *t, unsigned off, uint16_t v)
{
	for (unsigned i = 0; i < 8; i++) {
		t[off + 2u * i] = (uint8_t)v;
		t[off + 2u * i + 1u] = (uint8_t)(v >> 8);
	}
}

static void invented(uint8_t t[CS7000P_MAKER_SIZE], const uint32_t rx[8])
{
	static const uint32_t tx[8] = { 400200000u, 429800000u, 430200000u, 445000000u, 458000000u, 472000000u,
					492000000u, 511800000u };
	static const uint8_t high[8] = { 190, 194, 195, 210, 213, 215, 212, 209 };
	static const uint8_t low[8] = { 100, 99, 99, 108, 115, 122, 126, 127 };
	static const uint8_t tune[8] = { 120, 124, 128, 132, 136, 140, 150, 160 };
	memset(t, 0, CS7000P_MAKER_SIZE);
	for (unsigned i = 0; i < 8; i++) {
		put32be(t, 0x000u + 4u * i, tx[i]);
		put32be(t, 0x020u + 4u * i, rx[i]);
	}
	t[0x040] = 60;
	t[0x041] = 160;
	memcpy(t + 0x044, tune, 8);
	memset(t + 0x04C, 40, 8);
	memset(t + 0x054, 50, 8);
	memset(t + 0x05C, 120, 8);
	memset(t + 0x064, 130, 8);
	memcpy(t + 0x06C, high, 8);
	memcpy(t + 0x074, low, 8);
	memset(t + 0x084, 40, 8);
	memset(t + 0x08C, 52, 8);
	memset(t + 0x094, 200, 8);
	memset(t + 0x0B4, 80, 8);
	memset(t + 0x0BC, 100, 8);
	memset(t + 0x0C4, 120, 8);
	memset(t + 0x0CC, 135, 8);
	t[0x0DC] = 0x7B;	// trim A: d -5
	t[0x0DD] = 0xA0;
	memset(t + 0x0EC, 20, 8);
	memset(t + 0x0F4, 12, 8);
	memset(t + 0x0FC, 19, 8);
	memset(t + 0x124, 21, 8);
	memset(t + 0x12C, 12, 8);
	memset(t + 0x134, 20, 8);
	fill16(t, 0x104, 300);
	fill16(t, 0x114, 310);
	fill16(t, 0x13C, 301);
	fill16(t, 0x14C, 311);
	memset(t + 0x15C, 61, 8);
	memset(t + 0x164, 159, 8);
	t[0x174] = 0x7D;
}

static const uint32_t rx_pts[8] = { 400500000u, 420000000u, 440000000u, 460000000u, 478950000u, 479050000u,
				    495000000u, 511500000u };

static bool parsed(const uint8_t *t, cs7000p_maker_t *m)
{
	return cs7000p_maker_parse(t, 0, 0, m);
}

// No table, or one refused: every receive value is the one without the maker's table, at every frequency and reading
static void check_as_without(const cs7000p_cal_t *c, const cs7000p_maker_t *m)
{
	uint8_t src = 9, why = 9;
	for (uint32_t hz = 395u * MHZ; hz <= 530u * MHZ; hz += 250000u) {
		CHECK(cs7000p_cal_rx_tune_m(c, m, hz, &src) == cs7000p_cal_rx_tune(c, hz) && src == CS7000P_SRC_DEFAULT);
	}
	CHECK(cs7000p_cal_rx_ref_m(c, m, &src, &why) == cs7000p_cal_ref_offset(c));
	CHECK(src == CS7000P_SRC_DEFAULT && why == CS7000P_RXREF_WHY_TABLE);
	for (uint32_t mv = 0; mv <= 3400u; mv += 10u) {
		CHECK(cs7000p_cal_rssi_m(m, mv, mv, &src) == cs7000p_rssi_dbm(mv) && src == CS7000P_SRC_DEFAULT);
	}
	CHECK(cs7000p_cal_rx_tune_m(c, m, 446u * MHZ, NULL) == cs7000p_cal_rx_tune(c, 446u * MHZ));	// src may be NULL
}

static void test_maker_rx(void)
{
	static uint8_t t[CS7000P_MAKER_SIZE];
	cs7000p_maker_t m;
	uint8_t src = 9, why = 9;

	// None, blank, zero, a damaged table: as without one, on the shipped model and on a filled one
	check_as_without(&cs7000p_cal_model, NULL);
	check_as_without(&synth, NULL);
	memset(t, 0xFF, sizeof(t));
	CHECK(!parsed(t, &m));
	check_as_without(&cs7000p_cal_model, &m);
	memset(t, 0, sizeof(t));
	CHECK(!parsed(t, &m));
	check_as_without(&synth, &m);
	invented(t, rx_pts);
	t[0x046] = 0;		// a tune code of 0
	CHECK(!parsed(t, &m));
	check_as_without(&cs7000p_cal_model, &m);
	invented(t, rx_pts);
	t[0x0DC] = 0xC1;	// trim A past the reader's 64 steps: the whole table refused, its tune too
	CHECK(!parsed(t, &m));
	check_as_without(&synth, &m);

	// Valid: each value the table's, over the model's own rows
	invented(t, rx_pts);
	CHECK(parsed(t, &m) && m.fields == CS7000P_MK_F_ALL);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 433500000u, &src) == 16u * 127u && src == CS7000P_SRC_MAKER);	// t 506
	CHECK(cs7000p_cal_rx_tune_m(&cs7000p_cal_model, &m, 400u * MHZ, &src) == 16u * 120u && src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_cal_rx_ref_m(&synth, &m, &src, &why) == -5 && src == CS7000P_SRC_MAKER &&
	      why == CS7000P_RXREF_WHY_NONE);
	CHECK(cs7000p_cal_rssi_m(&m, 110u << 4, 500u, &src) == -95 && src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_cal_rssi_m(&m, 59u << 4, 500u, &src) == -121);	// a floor below the low point
	// Outside the receive band no tune is looked up: today's value there
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 399u * MHZ, &src) == cs7000p_cal_rx_tune(&synth, 399u * MHZ) &&
	      src == CS7000P_SRC_DEFAULT);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 527000001u, &src) == cs7000p_cal_rx_tune(&synth, 527000001u) &&
	      src == CS7000P_SRC_DEFAULT);
	// At a point, and on both sides of the receive VCO switch (points 478.95 and 479.05 MHz, codes 136 and 140): the
	// end value on the frequency's side up to 479.00001 MHz and past it, never the line that mixes the two VCOs
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 460u * MHZ, &src) == 16u * 132u && src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 478950000u, &src) == 16u * 136u && src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 479000000u, &src) == 16u * 136u && src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 479000009u, &src) == 16u * 136u && src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 479000010u, &src) == 16u * 140u && src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 479050000u, &src) == 16u * 140u && src == CS7000P_SRC_MAKER);
	// Past the last point the last segment extended (495 / 511.5 MHz, 150 / 160): t 660 at 520 MHz, 677 at 527 MHz
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 520u * MHZ, &src) == 16u * 165u && src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 527u * MHZ, &src) == 16u * 169u && src == CS7000P_SRC_MAKER);
	// The RSSI at its two points, over the top of the 12-bit reading and at 0
	CHECK(cs7000p_cal_rssi_m(&m, 60u << 4, 500u, &src) == -120 && cs7000p_cal_rssi_m(&m, 160u << 4, 500u, &src) == -70);
	CHECK(cs7000p_cal_rssi_m(&m, 0xFFFFu, 500u, &src) == cs7000p_cal_rssi_m(&m, 4095u, 500u, NULL));
	CHECK(cs7000p_cal_rssi_m(&m, 4095u, 500u, &src) == -23 && cs7000p_cal_rssi_m(&m, 0, 500u, &src) == -150);
	// The extension held at the DAC buffer's range: 0 below the first point gives 250, 255 past the last 3850. A
	// product the maker's 32 bits would wrap gives today's value
	invented(t, rx_pts);
	t[0x044] = 1;
	t[0x045] = 254;
	t[0x04A] = 200;
	t[0x04B] = 254;
	CHECK(parsed(t, &m));
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 400u * MHZ, &src) == CS7000P_TUNE_MIN && src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 527u * MHZ, &src) == CS7000P_TUNE_MAX && src == CS7000P_SRC_MAKER);
	t[0x04A] = 10;		// 4 x 244 x 3,200,000 (10 Hz units) at 527 MHz: over INT32_MAX
	CHECK(parsed(t, &m));
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 527u * MHZ, &src) == cs7000p_cal_rx_tune(&synth, 527u * MHZ) &&
	      src == CS7000P_SRC_DEFAULT);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 500u * MHZ, &src) != 0u && src == CS7000P_SRC_MAKER);
	invented(t, rx_pts);
	CHECK(parsed(t, &m));

	// Trim A at the receive bound and one step past it (both inside the reader's 64): used, then today's offset
	static const uint8_t trims[4] = { 0xA0, 0x60, 0xA1, 0x5F };
	for (unsigned i = 0; i < 4; i++) {
		invented(t, rx_pts);
		t[0x0DC] = trims[i];
		CHECK(parsed(t, &m));
		const int8_t r = cs7000p_cal_rx_ref_m(&synth, &m, &src, &why);
		if (i < 2u) {
			CHECK(r == (i == 0u ? 32 : -32) && src == CS7000P_SRC_MAKER && why == CS7000P_RXREF_WHY_NONE);
		} else {
			CHECK(r == -7 && src == CS7000P_SRC_DEFAULT && why == CS7000P_RXREF_WHY_BOUND);	// the model's
			CHECK(cs7000p_cal_rx_ref_m(&cs7000p_cal_model, &m, NULL, NULL) == CS7000P_REF_OFFSET);
		}
		// The bound is the bias's alone: the tune and the RSSI stay the table's
		CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 433500000u, &src) == 16u * 127u && src == CS7000P_SRC_MAKER);
		CHECK(cs7000p_cal_rssi_m(&m, 110u << 4, 500u, &src) == -95);
	}
	// A group the reader dropped: that value alone falls back. The RSSI by a use rule (a point at 0x00)
	invented(t, rx_pts);
	t[0x040] = 0;
	CHECK(parsed(t, &m) && m.fields == (CS7000P_MK_F_ALL & ~CS7000P_MK_F_RSSI));
	CHECK(cs7000p_cal_rssi_m(&m, 110u << 4, 500u, &src) == cs7000p_rssi_dbm(500u) && src == CS7000P_SRC_DEFAULT);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 433500000u, &src) == 16u * 127u && src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_cal_rx_ref_m(&synth, &m, &src, &why) == -5 && src == CS7000P_SRC_MAKER);
	// The tune's and trim A's groups, cleared as a refusal would (no rule of the reader drops them alone today)
	invented(t, rx_pts);
	CHECK(parsed(t, &m));
	m.fields &= (uint16_t)~CS7000P_MK_F_TUNE;
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 433500000u, &src) == cs7000p_cal_rx_tune(&synth, 433500000u) &&
	      src == CS7000P_SRC_DEFAULT);
	CHECK(cs7000p_cal_rx_ref_m(&synth, &m, &src, &why) == -5 && cs7000p_cal_rssi_m(&m, 110u << 4, 500u, NULL) == -95);
	m.fields = (uint16_t)(CS7000P_MK_F_ALL & ~CS7000P_MK_F_TRIM_A);
	CHECK(cs7000p_cal_rx_ref_m(&synth, &m, &src, &why) == -7 && src == CS7000P_SRC_DEFAULT &&
	      why == CS7000P_RXREF_WHY_GROUP);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 433500000u, &src) == 16u * 127u && src == CS7000P_SRC_MAKER);

	// One lookup refused, not the table: every RX point above the receive switch, so 470 MHz would extend across it.
	// Today's value there, the table's above the switch
	static const uint32_t rx_high[8] = { 480000000u, 485000000u, 490000000u, 495000000u, 500000000u, 505000000u,
					     510000000u, 520000000u };
	invented(t, rx_high);
	CHECK(parsed(t, &m));
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 470u * MHZ, &src) == cs7000p_cal_rx_tune(&synth, 470u * MHZ) &&
	      src == CS7000P_SRC_DEFAULT);
	CHECK(cs7000p_cal_rx_tune_m(&synth, &m, 485u * MHZ, &src) == 16u * 124u && src == CS7000P_SRC_MAKER);
}

int main(void)
{
	test_shipped();
	test_interpolation();
	test_verdict();
	test_status();
	test_maker_rx();
	printf("# cs7000pCalTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
