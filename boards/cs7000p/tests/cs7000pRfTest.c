// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS frequency plan (rfplan_cs7000p.c) on the real synthesizer and IF receiver drivers: the bands
// and the transmit gap, the reference divider and the divide ratio across the band on both sides of the first LO,
// the reference trim, and the IF receiver's words for both channel widths.
#include <stdio.h>
#include "rfplan_cs7000p.h"
#include "rf/sky72310/sky72310.h"
#include "radio/ref_trim.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t absdiff(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

static void test_bands(void)
{
	// Receive: 400 to 527 MHz, the edges in
	CHECK(!cs7000p_rx_band(399999999u) && cs7000p_rx_band(400000000u));
	CHECK(cs7000p_rx_band(527000000u) && !cs7000p_rx_band(527000001u));
	CHECK(!cs7000p_rx_band(0) && !cs7000p_rx_band(145000000u) && !cs7000p_rx_band(0xFFFFFFFFu));
	// Transmit: the grant's two rows, and nothing over 512 MHz
	CHECK(!cs7000p_tx_band(399999999u) && cs7000p_tx_band(400000000u));
	CHECK(cs7000p_tx_band(512000000u) && !cs7000p_tx_band(512000001u) && !cs7000p_tx_band(527000000u));
	// The distress beacon band between the rows is receive only, and so is the half channel (12.5 kHz) on each side
	// of it: no carrier whose 25 kHz channel reaches into 406.0-406.1 MHz
	CHECK(CS7000P_TX_GAP_LO_HZ == 405987500u && CS7000P_TX_GAP_HI_HZ == 406112500u);
	CHECK(cs7000p_tx_band(405987500u) && !cs7000p_tx_band(405987501u));
	CHECK(!cs7000p_tx_band(406000000u) && !cs7000p_tx_band(406100000u));
	CHECK(!cs7000p_tx_band(406025000u) && !cs7000p_tx_band(406050000u) && !cs7000p_tx_band(406099999u));
	CHECK(!cs7000p_tx_band(406112499u) && cs7000p_tx_band(406112500u));
	for (uint32_t hz = 405987501u; hz < 406112500u; hz += 997u) {
		CHECK(!cs7000p_tx_band(hz) && cs7000p_rx_band(hz));
	}
	for (uint32_t hz = 400000000u; hz <= 512000000u; hz += 12500u) {
		CHECK(cs7000p_tx_band(hz) == (hz + 12500u <= 406000000u || hz >= 406100000u + 12500u));
	}
	// Whatever transmits is also received
	for (uint32_t hz = 390000000u; hz <= 540000000u; hz += 12500u) {
		CHECK(!cs7000p_tx_band(hz) || cs7000p_rx_band(hz));
	}
	// The plan refuses what the bands refuse, and leaves its result alone
	cs7000p_pll_plan_t p = { .lo_hz = 1, .vco_hz = 2, .ref_div = 3 };
	CHECK(!cs7000p_pll_plan(399999999u, false, false, 0, &p) && !cs7000p_pll_plan(527000001u, false, false, 0, &p));
	CHECK(!cs7000p_pll_plan(406050000u, true, false, 0, &p) && !cs7000p_pll_plan(512000001u, true, false, 0, &p));
	CHECK(p.lo_hz == 1 && p.vco_hz == 2 && p.ref_div == 3);
	CHECK(cs7000p_pll_plan(406050000u, false, false, 0, &p));	// receive in the gap
}

// One plan against the part: the divider, the ratio in its range, and the frequency the words give
static void check_plan(uint32_t hz, bool tx, bool lo_high, uint32_t want_lo)
{
	cs7000p_pll_plan_t p;
	sky72310_ratio_t r;
	bool ok = cs7000p_pll_plan(hz, tx, lo_high, 0, &p);
	CHECK(ok);
	if (!ok) {
		return;
	}
	CHECK(p.lo_hz == want_lo && p.vco_hz == want_lo);
	// 2 everywhere: 1 is out of the part's range here, and a larger one only slows the loop
	CHECK(p.ref_div == 2);
	CHECK(!sky72310_ratio(CS7000P_F_REF_HZ, 1, p.vco_hz, &r));
	CHECK(sky72310_ratio(CS7000P_F_REF_HZ, p.ref_div, p.vco_hz, &r));
	// N = f / 8.4 MHz: 41.7 at the lowest receive LO, 68.7 at the highest LO on the high side
	uint32_t n = p.vco_hz / (CS7000P_F_REF_HZ / p.ref_div);
	CHECK(n >= SKY72310_N_MIN && n <= 69u && n <= SKY72310_N_MAX);
	// Back within one step of the 18-bit fraction: 8.4 MHz / 2^18 = 32.04 Hz
	CHECK(absdiff(sky72310_vco_hz(CS7000P_F_REF_HZ, p.ref_div, &r), p.vco_hz) <= 33u);
}

static void test_plan(void)
{
	CHECK(CS7000P_F_REF_HZ == 16800000u && CS7000P_IF_HZ == 49950000u);
	// The second LO is the reference tripled, and leaves a 450 kHz second IF
	CHECK(CS7000P_IF2_LO_HZ == 50400000u && CS7000P_IF2_LO_HZ - CS7000P_IF_HZ == 450000u);
	CHECK(CS7000P_LO_HIGH == 0);	// the side the board starts on
	for (uint32_t hz = CS7000P_RX_MIN_HZ; hz <= CS7000P_RX_MAX_HZ; hz += 6250u) {
		// Low side: the receive VCO at 350.05 to 477.05 MHz. High side: 449.95 to 576.95 MHz
		check_plan(hz, false, false, hz - CS7000P_IF_HZ);
		check_plan(hz, false, true, hz + CS7000P_IF_HZ);
		if (cs7000p_tx_band(hz)) {
			check_plan(hz, true, false, hz);	// the transmit VCO is on the carrier
			check_plan(hz, true, true, hz);		// and the LO side is receive's alone
		}
	}
	cs7000p_pll_plan_t p;
	CHECK(cs7000p_pll_plan(400000000u, false, false, 0, &p) && p.vco_hz == 350050000u);
	CHECK(cs7000p_pll_plan(527000000u, false, false, 0, &p) && p.vco_hz == 477050000u);
	CHECK(cs7000p_pll_plan(527000000u, false, true, 0, &p) && p.vco_hz == 576950000u);
}

static void test_trim(void)
{
	cs7000p_pll_plan_t p, q;
	// The trim moves what is programmed, not the nominal frequency: the LO in receive, the carrier in transmit
	CHECK(cs7000p_pll_plan(439125000u, true, false, -4500, &p));
	CHECK(p.lo_hz == 439125000u && p.vco_hz == ref_trim_tune_hz(439125000u, -4500) && p.vco_hz == 439125000u - 1976u);
	CHECK(cs7000p_pll_plan(439125000u, false, false, 10000, &p));
	CHECK(p.lo_hz == 439125000u - CS7000P_IF_HZ && p.vco_hz == ref_trim_tune_hz(p.lo_hz, 10000) && p.vco_hz > p.lo_hz);
	CHECK(cs7000p_pll_plan(439125000u, false, true, -10000, &q));
	CHECK(q.vco_hz == ref_trim_tune_hz(439125000u + CS7000P_IF_HZ, -10000) && q.vco_hz < q.lo_hz);
	// Over the whole trim range the divider and the ratio stay good
	for (int32_t ppb = -10000; ppb <= 10000; ppb += 2500) {
		sky72310_ratio_t r;
		CHECK(cs7000p_pll_plan(400000000u, false, false, (int16_t)ppb, &p) && p.ref_div == 2);
		CHECK(sky72310_ratio(CS7000P_F_REF_HZ, p.ref_div, p.vco_hz, &r));
		CHECK(cs7000p_pll_plan(512000000u, true, false, (int16_t)ppb, &p) && p.ref_div == 2);
		CHECK(sky72310_ratio(CS7000P_F_REF_HZ, p.ref_div, p.vco_hz, &r));
	}
}

// The synthesizer's words for a plan, through the driver the board runs
static void test_pll_words(void)
{
	cs7000p_pll_plan_t p;
	uint16_t w[SKY72310_PROGRAM_WORDS];
	CHECK(cs7000p_pll_plan(446000000u, true, false, 0, &p));
	// The driver's power_mux argument is register 7's data, where the board's Mux_out choice sits above the power bits
	CHECK(sky72310_program_words(CS7000P_F_REF_HZ, p.ref_div, p.vco_hz, CS7000P_PLL_PD_GAIN,
				     (uint16_t)(CS7000P_PLL_MUX << SKY72310_MUX_SHIFT), w) == SKY72310_PROGRAM_WORDS);
	CHECK(CS7000P_PLL_PD_GAIN <= 31u);
	// 446 MHz / 8.4 MHz = 53.095: N 53, register 53 - 32
	bool seen = false, seen_mux = false;
	for (unsigned i = 0; i < SKY72310_PROGRAM_WORDS; i++) {
		if ((w[i] >> 12) == SKY72310_REG_DIVIDER) {
			CHECK((w[i] & 0x1FFu) == 53u - 32u);
			seen = true;
		}
		// Mux_out is an open pin on this board: tri-stated, everything powered
		if ((w[i] >> 12) == SKY72310_REG_POWER_MUX) {
			CHECK((w[i] & 0xFFFu) == SKY72310_MUX_TRISTATE);
			seen_mux = true;
		}
	}
	CHECK(seen && seen_mux);
}

static void test_if_words(void)
{
	uint16_t w[AK2365A_SETUP_WORDS];
	// A 25 kHz channel: everything on, the wide level, the +-6 kHz filter, the 50.4 MHz LO
	CHECK(cs7000p_if_cfg.ctrl1_wide == 0xF1 && cs7000p_if_cfg.ctrl1_narrow == 0xE9);
	CHECK(cs7000p_if_cfg.ctrl2 == 0x1D && cs7000p_if_cfg.ctrl3 == 0x00 && cs7000p_if_cfg.ctrl4 == 0x01 &&
	      cs7000p_if_cfg.ctrl5 == 0x80);
	CHECK((cs7000p_if_cfg.ctrl1_wide & 3u) == 1u && (cs7000p_if_cfg.ctrl1_narrow & 3u) == 1u);	// LOFREQ 50.4 MHz
	CHECK((cs7000p_if_cfg.ctrl2 & AK2365A_CTRL2_CAL) != 0);	// every set-up calibrates the discriminator
	CHECK(ak2365a_setup_words(&cs7000p_if_cfg, true, w) == AK2365A_SETUP_WORDS);
	CHECK(w[0] == ak2365a_word(AK2365A_REG_SRST, AK2365A_SRST_VALUE));
	CHECK(w[1] == ak2365a_word(AK2365A_REG_CTRL1, 0xF1) && w[1] == ((0x01u << 9) | 0xF1u));
	CHECK(w[2] == ak2365a_word(AK2365A_REG_CTRL2, 0x1D) && w[3] == ak2365a_word(AK2365A_REG_CTRL3, 0x00));
	CHECK(w[4] == ak2365a_word(AK2365A_REG_CTRL4, 0x01) && w[5] == ak2365a_word(AK2365A_REG_CTRL5, 0x80));
	// 12.5 kHz: only control register 1 differs (the +-3 kHz filter, the narrow level)
	uint16_t n[AK2365A_SETUP_WORDS];
	CHECK(ak2365a_setup_words(&cs7000p_if_cfg, false, n) == AK2365A_SETUP_WORDS);
	CHECK(n[1] == ak2365a_word(AK2365A_REG_CTRL1, 0xE9));
	for (unsigned i = 0; i < AK2365A_SETUP_WORDS; i++) {
		CHECK(i == 1 || n[i] == w[i]);
		CHECK((w[i] & 0x8100u) == 0u);	// a write (the first bit 0), and the empty slot after the address
	}
	// The data sheet's calibration procedure: 500 us from the word that sets the operating mode to the word that
	// starts the calibration, on both widths, and no wait ahead of any other word. The reset comes first: the
	// chip is in standby when the mode is written, so the wait is due at every set-up
	CHECK(CS7000P_IF_MODE_US == 500u);
	for (unsigned width = 0; width < 2; width++) {
		const uint16_t *set = width != 0 ? w : n;
		int mode_at = -1, cal_at = -1;
		uint32_t total = 0;
		for (unsigned i = 0; i < AK2365A_SETUP_WORDS; i++) {
			uint32_t us = cs7000p_if_word_wait_us(set[i]);
			total += us;
			if ((set[i] >> 9) == AK2365A_REG_CTRL1) {
				mode_at = (int)i;
				CHECK((set[i] & 0xE0u) == 0xE0u);	// operating mode 7: everything the calibration needs is on
			}
			if (us != 0u) {
				CHECK(cal_at < 0 && us == CS7000P_IF_MODE_US);
				CHECK((set[i] >> 9) == AK2365A_REG_CTRL2 && (set[i] & AK2365A_CTRL2_CAL) != 0u);
				cal_at = (int)i;
			}
		}
		CHECK(mode_at > 0 && cal_at > mode_at && total == CS7000P_IF_MODE_US);
		CHECK((set[0] >> 9) == AK2365A_REG_SRST);
	}
	CHECK(cs7000p_if_word_wait_us(ak2365a_word(AK2365A_REG_CTRL2, 0x1C)) == CS7000P_IF_MODE_US);
	CHECK(cs7000p_if_word_wait_us(ak2365a_word(AK2365A_REG_CTRL4, 0x02)) == 0u);	// data that looks like the address
	CHECK(cs7000p_if_word_wait_us(ak2365a_word(AK2365A_REG_CTRL1, 0x02)) == 0u);

	// The bench reaches the six registers the data sheet names and no other address: 0x05 to 0x0A are reserved
	// ("do not access"), and nothing is defined at 0 or past control 5
	for (unsigned reg = 0; reg < 0x40u; reg++) {
		bool named = reg == 0x01u || reg == 0x02u || reg == 0x03u || reg == 0x04u || reg == 0x0Bu || reg == 0x0Cu;
		CHECK(cs7000p_if_reg_ok((uint8_t)reg) == named);
	}
	CHECK(cs7000p_if_reg_ok(AK2365A_REG_CTRL1) && cs7000p_if_reg_ok(AK2365A_REG_CTRL5));
}

int main(void)
{
	test_bands();
	test_plan();
	test_trim();
	test_pll_words();
	test_if_words();
	printf("# cs7000pRfTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
