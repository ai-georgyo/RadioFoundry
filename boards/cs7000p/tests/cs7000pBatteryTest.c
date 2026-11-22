// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS's power rules (battery_cs7000p.h): the supply sense model at the points the schematic gives,
// the knob as the battery reading shows it, when the supply may be held (only on a knob read on, never without the
// ADC), and when a radio that is still running after a power-off restarts (only after the knob went off and came
// back).
#include <stdio.h>
#include "battery_cs7000p.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static bool near(uint32_t got, uint32_t want, uint32_t tol)
{
	return got + tol >= want && got <= want + tol;
}

static void test_model(void)
{
	// The model's three points: POW_DET 1.56 / 1.91 / 2.16 V at 6.0 / 7.4 / 8.4 V
	CHECK(near(cs7000p_battery_mv(1560), 6000, 30));
	CHECK(near(cs7000p_battery_mv(1910), 7400, 30));
	CHECK(near(cs7000p_battery_mv(2160), 8400, 30));
	// Not the plain divider's ratio (0.299), which would read 7.4 V as 6.4 V
	CHECK(cs7000p_battery_mv(1910) > 7000);
	// Knob off: POW_DET is at 0 V, and so is the reading (no negative figure wrapping round)
	CHECK(cs7000p_battery_mv(0) == 0 && cs7000p_battery_mv(1) == 0 && cs7000p_battery_mv(61) == 0);
	CHECK(cs7000p_battery_mv(62) > 0 && cs7000p_battery_mv(62) < 10);
	// Monotonic over everything the pin can show, and no overflow at the ADC's full scale or past it
	uint32_t last = 0;
	for (uint32_t mv = 0; mv <= 3700; mv++) {
		uint32_t b = cs7000p_battery_mv(mv);
		CHECK(b >= last && b < 16000);
		last = b;
	}
	CHECK(cs7000p_battery_mv(cs7000p_adc_to_mv(4095, 3700)) == cs7000p_battery_mv(3700));
	CHECK(cs7000p_battery_mv(0xFFFF) > cs7000p_battery_mv(3700));
	// Every battery the radio runs on reads as "knob on", and the off state reads far under the threshold
	CHECK(cs7000p_battery_mv(1560) >= PWRSW_OFF_MV && cs7000p_battery_mv(0) < PWRSW_OFF_MV);
	// The switch open is its own level: POW_DET 0.7 V, far over the open switch's 0 V and far under a closed one at
	// any battery the radio can run on (a quarter of it: over 1 V down to 4.0 V)
	CHECK(CS7000P_KNOB_OPEN_MV == cs7000p_battery_mv(CS7000P_KNOB_OPEN_POW_DET_MV));
	CHECK(CS7000P_KNOB_OPEN_POW_DET_MV == 700u && near(CS7000P_KNOB_OPEN_MV, 2559, 1));
	CHECK(cs7000p_battery_mv(1000) >= CS7000P_KNOB_OPEN_MV && cs7000p_battery_mv(400) < CS7000P_KNOB_OPEN_MV);
}

static void test_scales(void)
{
	CHECK(cs7000p_adc_to_mv(0, 3240) == 0 && cs7000p_adc_to_mv(4095, 3240) == 3240);
	CHECK(near(cs7000p_adc_to_mv(2048, 3240), 1620, 1) && cs7000p_adc_to_mv(4095, 3700) == 3700);
	CHECK(CS7000P_VDDA_NOMINAL_MV == 3240u);
	// The volume position: 0 at the bottom, 100 at the full-volume reading and above it, never past 100
	CHECK(cs7000p_volume_pct(0) == 0 && cs7000p_volume_pct(CS7000P_VOL_FULL_RAW) == 100);
	CHECK(cs7000p_volume_pct(4095) == 100 && cs7000p_volume_pct(0xFFFF) == 100);
	uint8_t last = 0;
	for (uint32_t raw = 0; raw <= 4095; raw++) {
		uint8_t p = cs7000p_volume_pct(raw);
		CHECK(p >= last && p <= 100);
		last = p;
	}
}

static void test_hold_rule(void)
{
	const uint32_t on[3] = { 7400, 7390, 7410 };

	CHECK(CS7000P_HOLD_READINGS == 3);
	// Three readings on: the knob is on, and only then is the supply held
	CHECK(cs7000p_knob_from_readings(true, on, 3) == CS7000P_KNOB_ON && cs7000p_knob_holds(CS7000P_KNOB_ON));
	// A flat pack is still "on", and so is one under the shared off threshold: only the switch open is "off"
	const uint32_t flat[3] = { 6000, 5990, 5500 };
	CHECK(cs7000p_knob_from_readings(true, flat, 3) == CS7000P_KNOB_ON);
	const uint32_t sagged[3] = { PWRSW_OFF_MV - 1, 5000, CS7000P_KNOB_OPEN_MV };
	CHECK(cs7000p_knob_from_readings(true, sagged, 3) == CS7000P_KNOB_ON);
	// Any one reading off, whichever: off, and no hold. This start is a reset under a supply that is going away
	for (unsigned i = 0; i < 3; i++) {
		uint32_t mv[3] = { 7400, 7400, 7400 };
		mv[i] = 0;
		CHECK(cs7000p_knob_from_readings(true, mv, 3) == CS7000P_KNOB_OFF);
		mv[i] = CS7000P_KNOB_OPEN_MV - 1;
		CHECK(cs7000p_knob_from_readings(true, mv, 3) == CS7000P_KNOB_OFF);
		mv[i] = CS7000P_KNOB_OPEN_MV;
		CHECK(cs7000p_knob_from_readings(true, mv, 3) == CS7000P_KNOB_ON);
	}
	CHECK(!cs7000p_knob_holds(CS7000P_KNOB_OFF));
	CHECK(cs7000p_knob_from_readings(true, on, 2) == CS7000P_KNOB_OFF);	// not on fewer readings
	// No ADC: no hold whatever the buffer says, and the radio does not switch itself off on a reading it has not got
	CHECK(cs7000p_knob_from_readings(false, on, 3) == CS7000P_KNOB_NOADC && !cs7000p_knob_holds(CS7000P_KNOB_NOADC));
	CHECK(cs7000p_knob_switch_on(CS7000P_KNOB_NOADC, true) && cs7000p_knob_switch_on(CS7000P_KNOB_NOADC, false));
	// Nothing holds before the knob was read
	CHECK(!cs7000p_knob_holds(CS7000P_KNOB_UNKNOWN) && cs7000p_knob_switch_on(CS7000P_KNOB_UNKNOWN, true));
	// Read on: the debounced reading decides. Read off at the start: the first poll says so
	CHECK(cs7000p_knob_switch_on(CS7000P_KNOB_ON, true) && !cs7000p_knob_switch_on(CS7000P_KNOB_ON, false));
	CHECK(!cs7000p_knob_switch_on(CS7000P_KNOB_OFF, true) && !cs7000p_knob_switch_on(CS7000P_KNOB_OFF, false));
}

// The debounce the board feeds (core/radio/power_switch.c) on this board's readings: a dip is not a switch-off, the
// knob is
static void test_switch_reading(void)
{
	pwrsw_t s = { 0 };
	uint32_t t = 1000;

	CHECK(pwrsw_on(&s, cs7000p_battery_mv(1910), t));
	CHECK(pwrsw_on(&s, cs7000p_battery_mv(0), t += 100));		// one low reading
	CHECK(pwrsw_on(&s, cs7000p_battery_mv(1910), t += 100));
	CHECK(pwrsw_on(&s, cs7000p_battery_mv(0), t += 100) && pwrsw_on(&s, cs7000p_battery_mv(0), t += 100));
	CHECK(!pwrsw_on(&s, cs7000p_battery_mv(0), t += 100));		// the third in a row: off

	// What the board feeds it: the switch open as 0, anything else over its threshold, so a pack that sags under a
	// transmission, however long and however far, is not a switch-off
	CHECK(cs7000p_knob_reading(0) == 0 && cs7000p_knob_reading(CS7000P_KNOB_OPEN_MV - 1u) == 0);
	CHECK(cs7000p_knob_reading(CS7000P_KNOB_OPEN_MV) == PWRSW_OFF_MV);
	CHECK(cs7000p_knob_reading(PWRSW_OFF_MV - 1u) == PWRSW_OFF_MV && cs7000p_knob_reading(7400) == 7400);
	s = (pwrsw_t){ 0 };
	for (int i = 0; i < 100; i++) {
		CHECK(pwrsw_on(&s, cs7000p_knob_reading(i & 1 ? 5300u : CS7000P_KNOB_OPEN_MV), t += 100));
	}
	CHECK(pwrsw_on(&s, cs7000p_knob_reading(CS7000P_KNOB_OPEN_MV - 1u), t += 100));
	CHECK(pwrsw_on(&s, cs7000p_knob_reading(0), t += 100) && !pwrsw_on(&s, cs7000p_knob_reading(0), t += 100));
}

// A watch: the readings 10 ms apart from since_ms on; when it asks for the restart, or -1
static int watch_run(cs7000p_offwatch_t *w, const uint16_t *mv, unsigned n, uint32_t since_ms)
{
	for (unsigned i = 0; i < n; i++) {
		if (cs7000p_offwatch_restart(w, mv[i], since_ms + 10u * i)) {
			return (int)i;
		}
	}
	return -1;
}

static void test_off_watch(void)
{
	enum { N = 200 };
	uint16_t mv[N];
	cs7000p_offwatch_t w;

	// A power-off the radio asked for, the knob on throughout: it stays off, however long
	for (unsigned i = 0; i < N; i++) {
		mv[i] = 7400;
	}
	cs7000p_offwatch_start(&w, false);
	CHECK(watch_run(&w, mv, N, 0) == -1 && watch_run(&w, mv, N, 100000) == -1);
	// The knob off and staying off (the supply would be gone; a debugger's is not): no restart
	for (unsigned i = 0; i < N; i++) {
		mv[i] = 0;
	}
	cs7000p_offwatch_start(&w, true);
	CHECK(watch_run(&w, mv, N, 0) == -1);
	// Off, then on again after 0.8 s: the restart on the third reading on
	for (unsigned i = 80; i < N; i++) {
		mv[i] = 7400;
	}
	cs7000p_offwatch_start(&w, false);
	CHECK(watch_run(&w, mv, N, 0) == 82);
	// The knob was read off before the power-off and is on again by now: a restart, but not inside the time the
	// supply is given to go away
	for (unsigned i = 0; i < N; i++) {
		mv[i] = 7400;
	}
	cs7000p_offwatch_start(&w, true);
	CHECK(watch_run(&w, mv, N, 0) == (int)(CS7000P_OFF_SETTLE_MS / 10u));
	cs7000p_offwatch_start(&w, true);
	CHECK(watch_run(&w, mv, 2, CS7000P_OFF_SETTLE_MS + 1000u) == -1);	// two readings on are not enough
	CHECK(cs7000p_offwatch_restart(&w, 7400, CS7000P_OFF_SETTLE_MS + 1020u));
	// A contact that bounces: the count starts again at every low reading
	cs7000p_offwatch_start(&w, false);
	static const uint16_t bounce[] = { 7400, 0, 7400, 7400, 0, 7400, 0, 7400, 7400, 7400 };
	CHECK(watch_run(&w, bounce, sizeof(bounce) / sizeof(bounce[0]), 1000) == 9);
	// Just under the open threshold counts as off, at it as on
	cs7000p_offwatch_start(&w, false);
	static const uint16_t edge[] = { CS7000P_KNOB_OPEN_MV - 1, CS7000P_KNOB_OPEN_MV, CS7000P_KNOB_OPEN_MV,
					 CS7000P_KNOB_OPEN_MV };
	CHECK(watch_run(&w, edge, 4, 1000) == 3);
	// A pack that sags and recovers with the knob on (a critical battery's power-off, or any other): not the knob,
	// so no restart, however long it goes on
	for (unsigned i = 0; i < N; i++) {
		mv[i] = (uint16_t)(i % 7u < 3u ? 5000u : i % 7u < 5u ? PWRSW_OFF_MV - 1u : 6200u);
	}
	cs7000p_offwatch_start(&w, false);
	CHECK(watch_run(&w, mv, N, 0) == -1 && watch_run(&w, mv, N, 100000) == -1 && !w.seen_off);
}

int main(void)
{
	test_model();
	test_scales();
	test_hold_rule();
	test_switch_reading();
	test_off_watch();
	printf("# cs7000pBatteryTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
