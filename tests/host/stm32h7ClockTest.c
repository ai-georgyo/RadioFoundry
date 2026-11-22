// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32H7's clock table arithmetic (platform/stm32h7/clock_math.h): the rates of a table, each limit of the
// voltage scale the platform runs at (a table one step over it is refused), the flash wait state rule, the register
// codes, the table that gives the same rates from the internal oscillator, and the ADC's clock and BOOST range.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../../platform/stm32h7/clock_math.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

#define MHZ	1000000u

// A 25 MHz crystal to 192 MHz: 1 MHz into the medium VCO, 384 MHz, / 2; HCLK 96 MHz, every APB 48 MHz, pll1_q 48 MHz,
// two wait states. The table of the platform's first board
static const stm32_clock_t crystal25 = {
	.hse_hz = 25 * MHZ, .hse_bypass = false, .pll_m = 25, .pll_n = 384, .pll_p = 2, .pll_q = 8, .vco_medium = true,
	.cpu_div = 1, .ahb_div = 2, .apb_div = 2, .flash_latency = 2,
};

static bool ok(const stm32_clock_t *c)
{
	stm32h7_clock_hz_t hz;
	return stm32h7_clock_check(c, &hz);
}

static void test_rates(void)
{
	stm32h7_clock_hz_t hz = { 0 };
	CHECK(stm32h7_clock_check(&crystal25, &hz));
	CHECK(hz.pll_in == 1 * MHZ);
	CHECK(hz.vco == 384 * MHZ);
	CHECK(hz.sys == 192 * MHZ);
	CHECK(hz.cpu == 192 * MHZ);
	CHECK(hz.hclk == 96 * MHZ);
	CHECK(hz.pclk == 48 * MHZ);
	CHECK(hz.timer == 96 * MHZ);
	CHECK(hz.usb == 48 * MHZ);

	// The timers run at their bus clock only when it is undivided
	stm32_clock_t c = crystal25;
	c.pll_p = 4;		// 96 MHz
	c.ahb_div = 1;
	c.apb_div = 2;
	CHECK(stm32h7_clock_check(&c, &hz) && hz.hclk == 96 * MHZ && hz.pclk == 48 * MHZ && hz.timer == 96 * MHZ);
	c.pll_p = 8;		// 48 MHz
	c.apb_div = 1;
	c.flash_latency = 1;
	CHECK(stm32h7_clock_check(&c, &hz) && hz.hclk == 48 * MHZ && hz.pclk == 48 * MHZ && hz.timer == 48 * MHZ);
	c.pll_p = 2;
	c.cpu_div = 2;		// the CPU divider is ahead of the bus divider
	c.ahb_div = 2;
	c.apb_div = 4;
	CHECK(stm32h7_clock_check(&c, &hz) && hz.cpu == 96 * MHZ && hz.hclk == 48 * MHZ && hz.pclk == 12 * MHZ &&
	      hz.timer == 24 * MHZ);
}

static void test_pll_limits(void)
{
	stm32_clock_t c;
	stm32h7_clock_hz_t hz;

	// The input of the medium VCO: 1 to 2 MHz
	c = crystal25;
	c.hse_hz = 24 * MHZ;
	c.pll_m = 24;		// 1 MHz
	CHECK(ok(&c));
	c.pll_m = 25;		// 960 kHz
	c.pll_n = 400;		// still 384 MHz
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.pll_in == 960000u && hz.vco == 384 * MHZ);
	c = crystal25;
	c.pll_m = 10;		// 2.5 MHz
	c.pll_n = 96;		// 240 MHz
	c.pll_q = 5;
	c.flash_latency = 1;	// CPU 120 MHz, HCLK 60 MHz
	CHECK(!ok(&c));
	c.vco_medium = false;	// the wide VCO takes it
	CHECK(ok(&c));

	// The input of the wide VCO: above 2 MHz, to 16 MHz. 2 MHz itself is the medium VCO's (RM0433 8.5.5)
	c = crystal25;
	c.vco_medium = false;	// 1 MHz
	CHECK(!ok(&c));
	c.hse_hz = 4 * MHZ;
	c.pll_m = 2;		// 2 MHz
	c.pll_n = 96;		// 192 MHz
	c.pll_q = 4;
	c.flash_latency = 1;	// CPU 96 MHz, HCLK 48 MHz
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.pll_in == 2 * MHZ && hz.vco == 192 * MHZ);
	c.vco_medium = true;
	CHECK(ok(&c));
	c.vco_medium = false;
	c.hse_hz = 24 * MHZ;
	c.pll_m = 2;		// 12 MHz
	c.pll_n = 16;		// 192 MHz
	c.pll_q = 4;
	c.flash_latency = 1;	// CPU 96 MHz, HCLK 48 MHz
	CHECK(ok(&c));
	c.pll_m = 1;		// 24 MHz
	c.pll_n = 8;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.vco == 192 * MHZ);
	c.hse_hz = 16 * MHZ;	// 16 MHz, the top
	c.pll_n = 12;
	CHECK(ok(&c));

	// The PLL input is a whole number of Hz: the half hertz here would be 12 Hz at the USB clock
	c = crystal25;
	c.pll_m = 26;
	CHECK(!ok(&c));
	c = crystal25;
	c.hse_hz = 6 * MHZ + 1;
	c.pll_m = 2;		// 3 MHz when rounded down
	c.vco_medium = false;
	c.pll_n = 64;		// 192 MHz
	c.pll_q = 4;
	c.flash_latency = 1;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.pll_in == 3 * MHZ && hz.usb == 48 * MHZ);
	c.hse_hz = 6 * MHZ;
	CHECK(ok(&c));
	c.pll_m = 0;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.pll_in == 0 && hz.vco == 0 && hz.sys == 0);
	c = crystal25;
	c.hse_hz = 0;		// the HSI
	c.pll_m = 64;		// 1 MHz, but DIVM1 has six bits
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.pll_in == 1 * MHZ && hz.cpu == 192 * MHZ);

	// The medium VCO: 150 to 420 MHz
	c = crystal25;
	c.pll_n = 192;		// 192 MHz
	c.pll_q = 4;
	c.flash_latency = 1;	// CPU 96 MHz, HCLK 48 MHz
	CHECK(ok(&c));
	c.pll_n = 144;		// 144 MHz
	c.pll_q = 3;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.usb == 48 * MHZ);
	c = crystal25;
	c.pll_n = 432;		// 432 MHz
	c.pll_p = 4;
	c.pll_q = 9;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.usb == 48 * MHZ && hz.cpu == 108 * MHZ);

	// The wide VCO: 192 to 836 MHz
	c = crystal25;
	c.hse_hz = 0;		// the HSI: 64 MHz
	c.pll_m = 20;		// 3.2 MHz
	c.vco_medium = false;
	c.pll_n = 60;		// 192 MHz
	c.pll_q = 4;
	c.flash_latency = 1;
	CHECK(ok(&c));
	c.pll_n = 45;		// 144 MHz
	c.pll_q = 3;
	CHECK(!ok(&c));
	c.pll_n = 255;		// 816 MHz
	c.pll_p = 6;		// 136 MHz
	c.pll_q = 17;
	c.flash_latency = 1;	// HCLK 68 MHz
	CHECK(stm32h7_clock_check(&c, &hz) && hz.vco == 816 * MHZ);
	c.pll_n = 270;		// 864 MHz
	c.pll_q = 18;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.usb == 48 * MHZ && hz.cpu == 144 * MHZ);

	// DIVN1: 4 to 512. 64 MHz x 512 does not fit 32 bits and must not wrap into range
	c = crystal25;
	c.pll_n = 3;
	CHECK(!ok(&c));
	c.pll_n = 513;
	CHECK(!ok(&c));
	c.hse_hz = 0;
	c.pll_m = 1;
	c.pll_n = 512;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.vco == 0xFFFFFFFFu);

	// The P output: 200 MHz at most, whatever the CPU divider makes of it; DIVP1 is 1 or even
	c = crystal25;
	c.pll_p = 1;		// 384 MHz
	c.cpu_div = 2;		// CPU 192 MHz, HCLK 96 MHz
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.sys == 384 * MHZ && hz.cpu == 192 * MHZ && hz.hclk == 96 * MHZ);
	c = crystal25;
	c.pll_p = 3;		// 128 MHz, inside every rate limit: refused for being odd
	c.ahb_div = 2;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.cpu == 128 * MHZ && hz.hclk == 64 * MHZ);
	c.pll_p = 4;
	CHECK(ok(&c));
	c.pll_p = 130;		// even, but DIVP1 has seven bits
	c.flash_latency = 0;
	CHECK(!ok(&c));
	c.pll_p = 128;		// 3 MHz
	CHECK(ok(&c));
	c.pll_p = 0;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.sys == 0 && hz.cpu == 0);
	c = crystal25;
	c.pll_n = 192;		// 192 MHz: DIVP1 = 1 is allowed
	c.pll_p = 1;
	c.pll_q = 4;
	CHECK(stm32h7_clock_check(&c, &hz) && hz.cpu == 192 * MHZ);
	c.pll_n = 240;		// 240 MHz from P
	c.pll_q = 5;
	CHECK(!ok(&c));

	// The Q output: 200 MHz at most, and 48 MHz to the Hz (USB, the SPI1-3 kernel clock)
	c = crystal25;
	c.pll_q = 1;		// 384 MHz
	CHECK(!ok(&c));
	c.pll_q = 6;		// 64 MHz
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.usb == 64 * MHZ);
	c.pll_q = 16;		// 24 MHz: under is refused like over
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.usb == 24 * MHZ);
	c.pll_q = 0;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.usb == 0);
	c = crystal25;
	c.pll_n = 385;		// 385 MHz / 8 is 48.125 MHz
	CHECK(!ok(&c));
	c = crystal25;
	c.hse_hz = 12952381u;	// x 63 = 816 000 003 Hz: / 17 is 48 MHz and 3 / 17 Hz
	c.pll_m = 1;
	c.vco_medium = false;
	c.pll_n = 63;
	c.pll_p = 6;
	c.pll_q = 17;
	c.flash_latency = 1;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.usb == 48 * MHZ && hz.vco == 816000003u);
}

static void test_bus_limits(void)
{
	stm32_clock_t c;
	stm32h7_clock_hz_t hz;

	// HCLK: 100 MHz at most
	c = crystal25;
	c.ahb_div = 1;		// 192 MHz
	c.apb_div = 4;		// PCLK 48 MHz: only HCLK is over
	c.flash_latency = 4;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.hclk == 192 * MHZ && hz.pclk == 48 * MHZ);
	// PCLK: 50 MHz at most
	c = crystal25;
	c.apb_div = 1;		// 96 MHz
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.pclk == 96 * MHZ);
	c.apb_div = 2;
	CHECK(ok(&c));
	// The nearest rates over each limit that a table with a 48 MHz pll1_q can reach, from an 816 MHz VCO: HCLK
	// 102 MHz, PCLK 51 MHz (nothing gives 50 or 100 MHz exactly, and 48 and 96 MHz pass above)
	c = crystal25;
	c.hse_hz = 0;		// the HSI: 64 MHz
	c.pll_m = 20;		// 3.2 MHz
	c.vco_medium = false;
	c.pll_n = 255;		// 816 MHz
	c.pll_q = 17;
	c.pll_p = 8;		// 102 MHz
	c.ahb_div = 1;
	c.apb_div = 4;		// PCLK 25.5 MHz, timers 51 MHz: only HCLK is over
	c.flash_latency = 2;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.cpu == 102 * MHZ && hz.hclk == 102 * MHZ && hz.pclk == 25500000u);
	c.ahb_div = 2;		// HCLK 51 MHz
	CHECK(stm32h7_clock_check(&c, &hz) && hz.hclk == 51 * MHZ);
	c.pll_p = 16;		// 51 MHz
	c.ahb_div = 1;
	c.apb_div = 1;		// PCLK 51 MHz: only PCLK is over
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.hclk == 51 * MHZ && hz.pclk == 51 * MHZ && hz.timer == 51 * MHZ);
	c.apb_div = 2;
	CHECK(stm32h7_clock_check(&c, &hz) && hz.pclk == 25500000u);
	// Not a divisor of its prescaler
	c = crystal25;
	c.cpu_div = 3;
	CHECK(!ok(&c));
	c = crystal25;
	c.ahb_div = 32;		// the prescaler skips it
	CHECK(!ok(&c));
	c = crystal25;
	c.ahb_div = 0;
	CHECK(!stm32h7_clock_check(&c, &hz) && hz.hclk == 0 && hz.pclk == 0);
	c = crystal25;
	c.apb_div = 32;
	CHECK(!ok(&c));
	c.apb_div = 16;
	CHECK(ok(&c));
}

static void test_flash(void)
{
	// RM0433 Table 17, the VOS3 column
	CHECK(stm32h7_flash_ws_min(0) == 0);
	CHECK(stm32h7_flash_ws_min(45 * MHZ) == 0);
	CHECK(stm32h7_flash_ws_min(45 * MHZ + 1) == 1);
	CHECK(stm32h7_flash_ws_min(90 * MHZ) == 1);
	CHECK(stm32h7_flash_ws_min(90 * MHZ + 1) == 2);
	CHECK(stm32h7_flash_ws_min(135 * MHZ) == 2);
	CHECK(stm32h7_flash_ws_min(135 * MHZ + 1) == 3);
	CHECK(stm32h7_flash_ws_min(180 * MHZ) == 3);
	CHECK(stm32h7_flash_ws_min(180 * MHZ + 1) == 4);
	CHECK(stm32h7_flash_wrhighfreq(0) == 0);
	CHECK(stm32h7_flash_wrhighfreq(1) == 1);
	CHECK(stm32h7_flash_wrhighfreq(2) == 1);
	CHECK(stm32h7_flash_wrhighfreq(3) == 2);
	CHECK(stm32h7_flash_wrhighfreq(4) == 2);

	// Before the HSI is switched in behind dividers that are not the table's yet: never fewer than the undivided
	// 64 MHz HSI needs, whatever the table asks for (a second clock set-up coming from a slow tree with none)
	CHECK(stm32h7_flash_ws_min(STM32H7_HSI_HZ) == 1);
	CHECK(stm32h7_flash_ws_entry(0) == 1);
	CHECK(stm32h7_flash_ws_entry(1) == 1);
	CHECK(stm32h7_flash_ws_entry(2) == 2);
	CHECK(stm32h7_flash_ws_entry(4) == 4);

	// A table needs at least the wait states of its HCLK (96 MHz: 2), and no more than the table has rows for
	stm32_clock_t c = crystal25;
	c.flash_latency = 1;
	CHECK(!ok(&c));
	c.flash_latency = 0;
	CHECK(!ok(&c));
	c.flash_latency = 2;
	CHECK(ok(&c));
	c.flash_latency = 4;
	CHECK(ok(&c));
	c.flash_latency = 5;
	CHECK(!ok(&c));
	// 45 MHz and under needs none
	c = crystal25;
	c.pll_p = 8;		// 48 MHz
	c.flash_latency = 0;	// HCLK 24 MHz
	CHECK(ok(&c));
	c.ahb_div = 1;		// HCLK 48 MHz
	CHECK(!ok(&c));
	c.flash_latency = 1;
	CHECK(ok(&c));
}

static void test_codes(void)
{
	// D1CPRE and HPRE (RM0433 8.7.8): 0xxx undivided, then 2, 4, 8, 16, 64, 128, 256, 512
	static const uint16_t cpu_divs[] = { 1, 2, 4, 8, 16, 64, 128, 256, 512 };
	static const uint8_t cpu_codes[] = { 0x0, 0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF };
	for (unsigned i = 0; i < sizeof cpu_divs / sizeof cpu_divs[0]; i++) {
		CHECK(stm32h7_cpu_div_code(cpu_divs[i]) == cpu_codes[i]);
	}
	CHECK(stm32h7_cpu_div_code(0) == 0xFF);
	CHECK(stm32h7_cpu_div_code(3) == 0xFF);
	CHECK(stm32h7_cpu_div_code(32) == 0xFF);
	CHECK(stm32h7_cpu_div_code(1024) == 0xFF);
	// The APB prescalers: 0xx undivided, then 2, 4, 8, 16
	CHECK(stm32h7_apb_div_code(1) == 0x0);
	CHECK(stm32h7_apb_div_code(2) == 0x4);
	CHECK(stm32h7_apb_div_code(4) == 0x5);
	CHECK(stm32h7_apb_div_code(8) == 0x6);
	CHECK(stm32h7_apb_div_code(16) == 0x7);
	CHECK(stm32h7_apb_div_code(0) == 0xFF);
	CHECK(stm32h7_apb_div_code(3) == 0xFF);
	CHECK(stm32h7_apb_div_code(32) == 0xFF);

	// PLL1RGE: the medium VCO has one range; the wide VCO's ranges start above 2 MHz
	CHECK(stm32h7_pll_rge(1 * MHZ, true) == 0);
	CHECK(stm32h7_pll_rge(2 * MHZ, true) == 0);
	CHECK(stm32h7_pll_rge(2 * MHZ + 1, false) == 1);
	CHECK(stm32h7_pll_rge(3200000u, false) == 1);	// the HSI fallback's input
	CHECK(stm32h7_pll_rge(4 * MHZ, false) == 1);
	CHECK(stm32h7_pll_rge(4 * MHZ + 1, false) == 2);
	CHECK(stm32h7_pll_rge(8 * MHZ, false) == 2);
	CHECK(stm32h7_pll_rge(8 * MHZ + 1, false) == 3);
	CHECK(stm32h7_pll_rge(16 * MHZ, false) == 3);
}

static void test_fallback(void)
{
	stm32_clock_t fb = { 0 };
	stm32h7_clock_hz_t hz, fb_hz;

	// The crystal's table from the HSI: 64 MHz / 20 = 3.2 MHz into the wide VCO, x 120, everything else the same
	CHECK(stm32h7_clock_fallback(&crystal25, &fb));
	CHECK(fb.hse_hz == 0 && !fb.hse_bypass && fb.pll_m == 20 && fb.pll_n == 120 && !fb.vco_medium);
	CHECK(fb.pll_p == crystal25.pll_p && fb.pll_q == crystal25.pll_q && fb.cpu_div == crystal25.cpu_div &&
	      fb.ahb_div == crystal25.ahb_div && fb.apb_div == crystal25.apb_div &&
	      fb.flash_latency == crystal25.flash_latency);
	CHECK(stm32h7_clock_check(&crystal25, &hz));
	CHECK(stm32h7_clock_check(&fb, &fb_hz));
	CHECK(fb_hz.pll_in == 3200000u);
	CHECK(fb_hz.vco == hz.vco && fb_hz.sys == hz.sys && fb_hz.cpu == hz.cpu && fb_hz.hclk == hz.hclk &&
	      fb_hz.pclk == hz.pclk && fb_hz.timer == hz.timer && fb_hz.usb == hz.usb);
	CHECK(stm32h7_pll_rge(fb_hz.pll_in, fb.vco_medium) == 1);	// 2 to 4 MHz, with room on both sides

	// Every table the check takes has one: its VCO is pll1_q's 48 MHz times Q (192 to 816 MHz), a multiple of 3.2
	for (uint16_t q = 4; q <= 17; q++) {
		stm32_clock_t t = { .hse_hz = 24 * MHZ, .pll_m = 8, .pll_n = (uint16_t)(16u * q), .pll_q = (uint8_t)q,
				    .vco_medium = false, .cpu_div = 1, .ahb_div = 2, .apb_div = 2 };
		t.pll_p = (uint8_t)((48u * q + 199u) / 200u);	// sys_ck at most 200 MHz: 1 or an even divisor
		t.pll_p = t.pll_p == 1u ? 1u : (uint8_t)((t.pll_p + 1u) & ~1u);
		t.flash_latency = stm32h7_flash_ws_min(48u * q * MHZ / t.pll_p / 2u);
		stm32_clock_t f;
		stm32h7_clock_hz_t t_hz, f_hz;
		CHECK(stm32h7_clock_check(&t, &t_hz) && t_hz.vco == 48u * q * MHZ);
		CHECK(stm32h7_clock_fallback(&t, &f) && stm32h7_clock_check(&f, &f_hz));
		CHECK(f_hz.vco == t_hz.vco && f_hz.sys == t_hz.sys && f_hz.usb == t_hz.usb && f.pll_n == 15u * q);
	}

	// A table that is already the HSI's comes back as it is
	stm32_clock_t again = { 0 };
	CHECK(stm32h7_clock_fallback(&fb, &again));
	CHECK(again.pll_m == fb.pll_m && again.pll_n == fb.pll_n && again.hse_hz == 0 && !again.vco_medium);

	// A VCO that is not a multiple of 3.2 MHz has no such table, a multiple of 2 MHz included
	stm32_clock_t c = crystal25;
	c.pll_n = 193;
	CHECK(!stm32h7_clock_fallback(&c, &fb));
	c.pll_n = 386;
	CHECK(!stm32h7_clock_fallback(&c, &fb));
	// Nor has one under the wide VCO's range (160 MHz is inside the medium VCO's), or over it
	c.pll_n = 160;
	CHECK(!stm32h7_clock_fallback(&c, &fb));
	c = crystal25;
	c.hse_hz = 16 * MHZ;
	c.pll_m = 1;
	c.pll_n = 54;		// 864 MHz
	c.vco_medium = false;
	CHECK(!stm32h7_clock_fallback(&c, &fb));
	c.pll_n = 512;		// 8192 MHz: over 32 bits
	CHECK(!stm32h7_clock_fallback(&c, &fb));
	// Nor a table without a PLL input
	c = crystal25;
	c.pll_m = 0;
	CHECK(!stm32h7_clock_fallback(&c, &fb));
	c.pll_m = 26;
	CHECK(!stm32h7_clock_fallback(&c, &fb));
	// Nor one whose own PLL input is not a whole number of Hz, even where rounding makes its VCO look right
	c = crystal25;
	c.hse_hz = 6 * MHZ + 1;
	c.pll_m = 2;
	c.pll_n = 64;
	c.vco_medium = false;
	CHECK(!stm32h7_clock_fallback(&c, &fb));
	c.hse_hz = 6 * MHZ;
	CHECK(stm32h7_clock_fallback(&c, &fb) && fb.pll_n == 60);
}

// The ADC's analog clock on revision V and the BOOST range for it (RM0433 25.4.3, ADC_CR)
static void test_adc_clock(void)
{
	// This platform's first board: sys_ck 192 MHz, HPRE 2: 96 / 8 = 12 MHz, which is HCLK / 8 there
	CHECK(stm32h7_adc_sync_hz_v(192 * MHZ, 2) == 12 * MHZ);
	CHECK(stm32h7_adc_boost_v(12 * MHZ) == 1);
	// HPRE 4 halves HCLK, not the ADC's clock: 12 MHz still (ST's module takes HCLK 48 MHz / 8 = 6 MHz, range 0)
	CHECK(stm32h7_adc_sync_hz_v(192 * MHZ, 4) == 12 * MHZ);
	CHECK(stm32h7_adc_sync_hz_v(192 * MHZ, 512) == 12 * MHZ);
	// HPRE 1: sys_ck itself
	CHECK(stm32h7_adc_sync_hz_v(200 * MHZ, 1) == 25 * MHZ);
	CHECK(stm32h7_adc_sync_hz_v(64 * MHZ, 1) == 8 * MHZ);
	// The bare HSI behind HPRE 2: 4 MHz
	CHECK(stm32h7_adc_sync_hz_v(64 * MHZ, 2) == 4 * MHZ);
	// The four ranges, each edge on its lower side
	CHECK(stm32h7_adc_boost_v(4 * MHZ) == 0);
	CHECK(stm32h7_adc_boost_v(6250000u) == 0);
	CHECK(stm32h7_adc_boost_v(6250001u) == 1);
	CHECK(stm32h7_adc_boost_v(12500000u) == 1);
	CHECK(stm32h7_adc_boost_v(12500001u) == 2);
	CHECK(stm32h7_adc_boost_v(25 * MHZ) == 2);
	CHECK(stm32h7_adc_boost_v(25 * MHZ + 1) == 3);
	CHECK(stm32h7_adc_boost_v(50 * MHZ) == 3);
}

int main(void)
{
	test_rates();
	test_pll_limits();
	test_bus_limits();
	test_flash();
	test_codes();
	test_fallback();
	test_adc_clock();
	printf("stm32h7ClockTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
