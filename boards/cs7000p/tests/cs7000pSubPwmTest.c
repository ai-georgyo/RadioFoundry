// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS sub-audio PWM's duty rule (subpwm_cs7000p.h): with no tone the duty is exactly half the
// period, a sample moves it both ways in proportion to the depth, and at no sample and no depth does it reach 0 or
// the whole period, where the pin would stop toggling.
#include <stdio.h>
#include "subpwm_cs7000p.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define MID	(CS7000P_SUBPWM_FULL / 2u)

static void test_idle(void)
{
	// 512 / 1024: board.c starts the timer with these two, and the generator's mid scale lands on the same duty
	CHECK(CS7000P_SUBPWM_PERIOD == 1024u && CS7000P_SUBPWM_IDLE == 512u);
	CHECK(2u * CS7000P_SUBPWM_IDLE == CS7000P_SUBPWM_PERIOD);
	for (unsigned depth = 0; depth <= 255; depth++) {
		CHECK(cs7000p_subpwm_duty((uint16_t)MID, (uint8_t)depth) == CS7000P_SUBPWM_IDLE);
	}
	// Depth 0: no tone at all, whatever the sample
	for (uint32_t s = 0; s <= 0xFFFFu; s += 7u) {
		CHECK(cs7000p_subpwm_duty((uint16_t)s, 0) == CS7000P_SUBPWM_IDLE);
	}
	CHECK(CS7000P_CTCSS_DEPTH >= 1 && CS7000P_CTCSS_DEPTH <= 100);
	CHECK(CS7000P_DCS_INVERT == 0 && CS7000P_DCS_RX_INVERT == 0);
}

static void test_follows(void)
{
	// The full swing at the build's depth, both ways and the same size
	const uint8_t depth = (uint8_t)(CS7000P_CTCSS_DEPTH);
	const uint32_t swing = (CS7000P_SUBPWM_IDLE * depth + 50u) / 100u;	// to the nearest count
	CHECK(cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, depth) == CS7000P_SUBPWM_IDLE + swing);
	CHECK(cs7000p_subpwm_duty(0, depth) == CS7000P_SUBPWM_IDLE - swing);
	// Half a swing at half the sample
	CHECK(cs7000p_subpwm_duty((uint16_t)(MID + MID / 2u), 50) == CS7000P_SUBPWM_IDLE + 128u);
	CHECK(cs7000p_subpwm_duty((uint16_t)(MID - MID / 2u), 50) == CS7000P_SUBPWM_IDLE - 128u);
	CHECK(cs7000p_subpwm_duty((uint16_t)(MID + 100u), 100) == CS7000P_SUBPWM_IDLE + 100u);
	CHECK(cs7000p_subpwm_duty((uint16_t)(MID - 100u), 100) == CS7000P_SUBPWM_IDLE - 100u);
	for (unsigned d = 1; d <= 100; d++) {
		uint16_t last = cs7000p_subpwm_duty(0, (uint8_t)d);
		for (uint32_t s = 0; s <= CS7000P_SUBPWM_FULL; s++) {
			uint16_t duty = cs7000p_subpwm_duty((uint16_t)s, (uint8_t)d);
			CHECK(duty >= last);	// up with the sample, never back
			last = duty;
			// The same distance from idle on both sides (but for the clamp at the two ends)
			uint16_t mirror = cs7000p_subpwm_duty((uint16_t)(CS7000P_SUBPWM_FULL - s), (uint8_t)d);
			int up = (int)duty - (int)CS7000P_SUBPWM_IDLE, down = (int)CS7000P_SUBPWM_IDLE - (int)mirror;
			CHECK(up == down || (d == 100 && (s == 0 || s == CS7000P_SUBPWM_FULL)));
			// And on the sample's side of it
			CHECK(s < MID ? duty <= CS7000P_SUBPWM_IDLE : duty >= CS7000P_SUBPWM_IDLE);
		}
		// A deeper setting swings further
		CHECK(cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, (uint8_t)d) >=
		      cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, (uint8_t)(d - 1u)));
	}
}

static void test_never_constant(void)
{
	// Every sample a 16-bit generator could hand over, at every depth a byte holds
	for (unsigned depth = 0; depth <= 255; depth++) {
		for (uint32_t s = 0; s <= 0xFFFFu; s++) {
			uint16_t duty = cs7000p_subpwm_duty((uint16_t)s, (uint8_t)depth);
			if (duty < 1u || duty > CS7000P_SUBPWM_PERIOD - 1u) {
				CHECK(duty >= 1u && duty <= CS7000P_SUBPWM_PERIOD - 1u);
				return;
			}
		}
	}
	// The clamp is what does it at full depth: one count inside each end
	CHECK(cs7000p_subpwm_duty(0, 100) == 1u);
	CHECK(cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, 100) == CS7000P_SUBPWM_PERIOD - 1u);
	CHECK(cs7000p_subpwm_duty(0xFFFFu, 255) == CS7000P_SUBPWM_PERIOD - 1u);
	// A depth over 100 counts as 100, a sample over the scale as full scale
	CHECK(cs7000p_subpwm_duty((uint16_t)(MID + 10u), 200) == cs7000p_subpwm_duty((uint16_t)(MID + 10u), 100));
	CHECK(cs7000p_subpwm_duty(2000, 30) == cs7000p_subpwm_duty((uint16_t)CS7000P_SUBPWM_FULL, 30));
}

int main(void)
{
	test_idle();
	test_follows();
	test_never_constant();
	printf("# cs7000pSubPwmTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
