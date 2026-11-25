// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS sub-audio PWM's duty rule (subpwm_cs7000p.h): with no tone the duty is exactly half the
// period, a sample moves it both ways in proportion to the depth in per mille of half the period, and at no sample and
// no depth does it reach 0 or the whole period, where the pin would stop toggling. Then the maker's sub-audio level L
// as a depth, (4000 L + 525) / 1050, at every level. host.mk also builds this file with the old option's name
// (CS7000P_CTCSS_DEPTH, a percent) and requires the build to fail on its #error.
#include <stdio.h>
#include "subpwm_cs7000p.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define MID	(CS7000P_SUBPWM_FULL / 2u)
#define FULL_PM	CS7000P_SUBPWM_DEPTH_FULL

static void test_idle(void)
{
	// 512 / 1024: board.c starts the timer with these two, and the generator's mid scale lands on the same duty
	CHECK(CS7000P_SUBPWM_PERIOD == 1024u && CS7000P_SUBPWM_IDLE == 512u);
	CHECK(2u * CS7000P_SUBPWM_IDLE == CS7000P_SUBPWM_PERIOD);
	CHECK(FULL_PM == 1000u);
	for (uint32_t depth = 0; depth <= 0xFFFFu; depth++) {
		CHECK(cs7000p_subpwm_duty((uint16_t)MID, (uint16_t)depth) == CS7000P_SUBPWM_IDLE);
	}
	// Depth 0: no tone at all, whatever the sample
	for (uint32_t s = 0; s <= 0xFFFFu; s += 7u) {
		CHECK(cs7000p_subpwm_duty((uint16_t)s, 0) == CS7000P_SUBPWM_IDLE);
	}
	// The build's depth: the old 30 % as per mille
	CHECK(CS7000P_CTCSS_DEPTH_PM == 300);
	CHECK(CS7000P_DCS_INVERT == 0 && CS7000P_DCS_RX_INVERT == 0);
}

static void test_follows(void)
{
	// The full swing at the build's depth, both ways and the same size: 512 x 300 / 1000 = 153.6, so 154
	const uint16_t depth = (uint16_t)(CS7000P_CTCSS_DEPTH_PM);
	const uint32_t swing = (CS7000P_SUBPWM_IDLE * depth + FULL_PM / 2u) / FULL_PM;	// to the nearest count
	CHECK(swing == 154u);
	CHECK(cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, depth) == CS7000P_SUBPWM_IDLE + swing);
	CHECK(cs7000p_subpwm_duty(0, depth) == CS7000P_SUBPWM_IDLE - swing);
	// Half a swing at half the sample; one per mille is half a count at full scale
	CHECK(cs7000p_subpwm_duty((uint16_t)(MID + MID / 2u), 500) == CS7000P_SUBPWM_IDLE + 128u);
	CHECK(cs7000p_subpwm_duty((uint16_t)(MID - MID / 2u), 500) == CS7000P_SUBPWM_IDLE - 128u);
	CHECK(cs7000p_subpwm_duty((uint16_t)(MID + 100u), 1000) == CS7000P_SUBPWM_IDLE + 100u);
	CHECK(cs7000p_subpwm_duty((uint16_t)(MID - 100u), 1000) == CS7000P_SUBPWM_IDLE - 100u);
	CHECK(cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, 1) == CS7000P_SUBPWM_IDLE + 1u);	// 0.512, rounded
	CHECK(cs7000p_subpwm_duty(0, 1) == CS7000P_SUBPWM_IDLE - 1u);
	// The maker's levels' depths, pinned: L = 40 (152) and L = 254 (968)
	CHECK(cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, 152) == CS7000P_SUBPWM_IDLE + 78u);	// 77.8
	CHECK(cs7000p_subpwm_duty(0, 152) == CS7000P_SUBPWM_IDLE - 78u);
	CHECK(cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, 968) == CS7000P_SUBPWM_IDLE + 496u);	// 495.6
	CHECK(cs7000p_subpwm_duty(0, 968) == CS7000P_SUBPWM_IDLE - 496u);
	for (uint32_t d = 1; d <= FULL_PM; d++) {
		uint16_t last = cs7000p_subpwm_duty(0, (uint16_t)d);
		for (uint32_t s = 0; s <= CS7000P_SUBPWM_FULL; s++) {
			uint16_t duty = cs7000p_subpwm_duty((uint16_t)s, (uint16_t)d);
			CHECK(duty >= last);	// up with the sample, never back
			last = duty;
			// The same distance from idle on both sides (but for the clamp at the two ends)
			uint16_t mirror = cs7000p_subpwm_duty((uint16_t)(CS7000P_SUBPWM_FULL - s), (uint16_t)d);
			int up = (int)duty - (int)CS7000P_SUBPWM_IDLE, down = (int)CS7000P_SUBPWM_IDLE - (int)mirror;
			CHECK(up == down || (d == FULL_PM && (s == 0 || s == CS7000P_SUBPWM_FULL)));
			// And on the sample's side of it
			CHECK(s < MID ? duty <= CS7000P_SUBPWM_IDLE : duty >= CS7000P_SUBPWM_IDLE);
		}
		// The swing at full scale is the depth's share of half the period, to the nearest count (but for the clamp)
		const uint32_t want = (CS7000P_SUBPWM_IDLE * d + FULL_PM / 2u) / FULL_PM;
		CHECK(cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, (uint16_t)d) ==
		      (d == FULL_PM ? CS7000P_SUBPWM_PERIOD - 1u : CS7000P_SUBPWM_IDLE + want));
		// A deeper setting swings further
		CHECK(cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, (uint16_t)d) >=
		      cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, (uint16_t)(d - 1u)));
	}
}

static void test_never_constant(void)
{
	// Every sample a 16-bit generator could hand over, at every depth up to full and a few past it
	static const uint32_t past[] = { 1001u, 1024u, 2000u, 0x7FFFu, 0xFFFFu };
	for (uint32_t i = 0; i <= FULL_PM + sizeof(past) / sizeof(past[0]); i++) {
		const uint32_t depth = i <= FULL_PM ? i : past[i - FULL_PM - 1u];
		for (uint32_t s = 0; s <= 0xFFFFu; s++) {
			uint16_t duty = cs7000p_subpwm_duty((uint16_t)s, (uint16_t)depth);
			if (duty < 1u || duty > CS7000P_SUBPWM_PERIOD - 1u) {
				CHECK(duty >= 1u && duty <= CS7000P_SUBPWM_PERIOD - 1u);
				return;
			}
		}
	}
	// The clamp is what does it at full depth: one count inside each end
	CHECK(cs7000p_subpwm_duty(0, 1000) == 1u);
	CHECK(cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, 1000) == CS7000P_SUBPWM_PERIOD - 1u);
	CHECK(cs7000p_subpwm_duty(0xFFFFu, 0xFFFFu) == CS7000P_SUBPWM_PERIOD - 1u);
	// A depth over full counts as full, a sample over the scale as full scale
	CHECK(cs7000p_subpwm_duty((uint16_t)(MID + 10u), 2000) == cs7000p_subpwm_duty((uint16_t)(MID + 10u), 1000));
	CHECK(cs7000p_subpwm_duty((uint16_t)(MID - 10u), 1001) == cs7000p_subpwm_duty((uint16_t)(MID - 10u), 1000));
	CHECK(cs7000p_subpwm_duty(2000, 300) == cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, 300));
}

// The maker's level as a depth: 4 L counts of its 1050-count half period, in per mille to the nearest
static void test_level_rule(void)
{
	// calibration-cs7000p.md 5.5's numbers: L = 40 a narrow channel's, 254 a 25 kHz one's doubled to the cap
	CHECK(cs7000p_subpwm_depth_pm(0) == 0u);
	CHECK(cs7000p_subpwm_depth_pm(1) == 4u);		// 3.81
	CHECK(cs7000p_subpwm_depth_pm(40) == 152u);		// 152.38
	CHECK(cs7000p_subpwm_depth_pm(41) == 156u);		// 156.19
	CHECK(cs7000p_subpwm_depth_pm(82) == 312u);		// 312.38
	CHECK(cs7000p_subpwm_depth_pm(200) == 762u);		// 761.90
	CHECK(cs7000p_subpwm_depth_pm(254) == 968u);		// 967.62
	CHECK(cs7000p_subpwm_depth_pm(255) == 971u);		// 971.43: no level the reader gives, still under full
	uint16_t last = 0;
	for (uint32_t l = 0; l <= 255u; l++) {
		const uint16_t d = cs7000p_subpwm_depth_pm((uint8_t)l);
		// The nearest per mille: |1050 d - 4000 L| at most half of 1050
		const int32_t err = (int32_t)d * 1050 - (int32_t)l * 4000;
		CHECK(err >= -525 && err <= 525);
		CHECK(d >= last && d < FULL_PM);	// rising, and never the clamp's full depth
		CHECK(l == 0u || d > last);		// every step of L is a step of the depth (3.8 per mille each)
		last = d;
		// The swing it gives at full scale: 4 L / 1050 of half the period, within a count
		const int32_t swing = (int32_t)cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, d) -
				      (int32_t)CS7000P_SUBPWM_IDLE;
		const int32_t exact_x1050 = (int32_t)l * 4 * (int32_t)CS7000P_SUBPWM_IDLE;
		CHECK(swing * 1050 - exact_x1050 <= 1050 && exact_x1050 - swing * 1050 <= 1050);
	}
}

int main(void)
{
	test_idle();
	test_follows();
	test_never_constant();
	test_level_rule();
	printf("# cs7000pSubPwmTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
