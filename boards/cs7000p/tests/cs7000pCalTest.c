// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS calibration (rfcal_cs7000p.c). As shipped every table is unset: no frequency, level or mode
// may transmit and no level has a drive. On a made-up table: the interpolation, the ends, and that a row with one
// point missing is no row. The RSSI and noise mappings never step back.
#include <stdio.h>
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

int main(void)
{
	test_shipped();
	test_interpolation();
	test_verdict();
	test_status();
	printf("# cs7000pCalTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
