// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32H7's clock table (stm32_clock_t) and the arithmetic on it, no hardware access (host tested,
// tests/host/stm32h7ClockTest.c): every rate a table gives, whether they are inside the limits of the voltage scale
// the platform runs at (VOS3, the reset scale: clock.c never changes it), the table that gives the same rates from the
// internal oscillator, and the register codes of the fields. Limits: RM0433 8.5.5 and Table 17, DS12110 PLL and
// general operating conditions (the smaller figure where the silicon revisions differ).
#ifndef STM32H7_CLOCK_MATH_H
#define STM32H7_CLOCK_MATH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t hse_hz;		// the crystal; 0: PLL1 runs from the HSI (64 MHz), the fallback tree
	bool hse_bypass;
	uint8_t pll_m;			// DIVM1: the PLL input, 1-2 MHz with vco_medium, over 2 to 16 without
	uint16_t pll_n;			// DIVN1, 4..512
	uint8_t pll_p, pll_q;		// DIVP1 (1 or even), DIVQ1: sys_ck and the 48 MHz USB clock
	bool vco_medium;		// VCOL 150-420 MHz, else VCOH from 192 MHz
	uint8_t cpu_div, ahb_div;	// D1CPRE, HPRE as plain divisors (1, 2, 4, ...)
	uint8_t apb_div;		// D1PPRE, D2PPRE1, D2PPRE2, D3PPRE: one divisor for the four
	uint8_t flash_latency;		// wait states (FLASH_ACR LATENCY); WRHIGHFREQ follows RM0433 Table 17
} stm32_clock_t;

typedef struct { uint32_t pll_in, vco, sys, cpu, hclk, pclk, timer, usb; } stm32h7_clock_hz_t;

#define STM32H7_HSI_HZ			64000000u
#define STM32H7_USB_HZ			48000000u	// pll1_q: the USB core's clock and the SPI1-3 kernel clock
#define STM32H7_PLL_IN_MIN_HZ		1000000u	// medium VCO: 1 to 2 MHz
#define STM32H7_PLL_IN_SPLIT_HZ		2000000u	// wide VCO: above 2 MHz, to 16 MHz
#define STM32H7_PLL_IN_MAX_HZ		16000000u
#define STM32H7_VCO_MEDIUM_MIN_HZ	150000000u
#define STM32H7_VCO_MEDIUM_MAX_HZ	420000000u
#define STM32H7_VCO_WIDE_MIN_HZ		192000000u
#define STM32H7_VCO_WIDE_MAX_HZ		836000000u	// revision Y's figure (revision V: 960 MHz)
#define STM32H7_PLL_OUT_MAX_HZ		200000000u	// the P and Q outputs
#define STM32H7_CPU_MAX_HZ		200000000u
#define STM32H7_HCLK_MAX_HZ		100000000u	// also the flash interface's (AXI) clock
#define STM32H7_PCLK_MAX_HZ		50000000u
#define STM32H7_TIMER_MAX_HZ		100000000u
#define STM32H7_FLASH_WS_MAX		4u		// the last row of Table 17
#define STM32H7_FALLBACK_M		20u		// the HSI tree's DIVM1: a 3.2 MHz PLL input, wide VCO

// The 4-bit code of D1CPRE and HPRE for a divisor (there is no divide by 32), 0xFF: not a divisor of that prescaler
static inline uint8_t stm32h7_cpu_div_code(uint16_t div)
{
	switch (div) {
	case 1: return 0x0;
	case 2: return 0x8;
	case 4: return 0x9;
	case 8: return 0xA;
	case 16: return 0xB;
	case 64: return 0xC;
	case 128: return 0xD;
	case 256: return 0xE;
	case 512: return 0xF;
	default: return 0xFF;
	}
}

// The 3-bit code of D1PPRE, D2PPRE1, D2PPRE2 and D3PPRE for a divisor, 0xFF: not one
static inline uint8_t stm32h7_apb_div_code(uint8_t div)
{
	switch (div) {
	case 1: return 0x0;
	case 2: return 0x4;
	case 4: return 0x5;
	case 8: return 0x6;
	case 16: return 0x7;
	default: return 0xFF;
	}
}

// PLL1RGE for a PLL input (RM0433 8.7.12): 0 is the 1-2 MHz range, the only one the medium VCO takes; the wide VCO's
// input is above 2 MHz, from range 1
static inline uint8_t stm32h7_pll_rge(uint32_t pll_in, bool vco_medium)
{
	if (vco_medium) {
		return 0;
	}
	return pll_in <= 4000000u ? 1 : pll_in <= 8000000u ? 2 : 3;
}

// The fewest wait states the flash takes at an AXI clock (= HCLK) at VOS3 (RM0433 Table 17)
static inline uint8_t stm32h7_flash_ws_min(uint32_t hclk)
{
	return hclk <= 45000000u ? 0 : hclk <= 90000000u ? 1 : hclk <= 135000000u ? 2 : hclk <= 180000000u ? 3 : 4;
}

// The wait states to have before the HSI is switched in behind bus dividers that are not the table's yet: ws, what the
// table and the HSI behind the table's dividers need, and no fewer than the undivided HSI needs. After a reset the
// dividers are 1; on a second call of the clock set-up they are the last table's, whatever that was
static inline uint8_t stm32h7_flash_ws_entry(uint8_t ws)
{
	const uint8_t hsi = stm32h7_flash_ws_min(STM32H7_HSI_HZ);
	return ws > hsi ? ws : hsi;
}

// WRHIGHFREQ, the programming delay that goes with a number of wait states (the same table). Nothing here programs
// the flash; the pair is kept as the table has it
static inline uint8_t stm32h7_flash_wrhighfreq(uint8_t ws)
{
	return ws == 0 ? 0 : ws <= 2u ? 1 : 2;
}

// Every rate of a table, and whether it is inside the limits above (false: out is still filled, with 0 for a rate a
// zero divider leaves undefined). The PLL input must be a whole number of Hz, and pll1_q must be 48 MHz exactly: the
// USB core and the SPI1-3 kernel clock run from it
static inline bool stm32h7_clock_check(const stm32_clock_t *c, stm32h7_clock_hz_t *out)
{
	bool ok = true;
	uint32_t src = c->hse_hz != 0 ? c->hse_hz : STM32H7_HSI_HZ;
	uint32_t cpu_div = stm32h7_cpu_div_code(c->cpu_div) != 0xFF ? c->cpu_div : 0;
	uint32_t ahb_div = stm32h7_cpu_div_code(c->ahb_div) != 0xFF ? c->ahb_div : 0;
	uint32_t apb_div = stm32h7_apb_div_code(c->apb_div) != 0xFF ? c->apb_div : 0;

	out->pll_in = c->pll_m != 0 ? src / c->pll_m : 0;
	if (c->pll_m == 0 || c->pll_m > 63u || src % c->pll_m != 0) {
		ok = false;
	}
	// A reference of 2 MHz or less must take the medium VCO, else the output is not guaranteed (RM0433 8.5.5): the
	// data sheet's two tables both list 2 MHz, the manual's rule says which
	if (c->vco_medium) {
		ok = ok && out->pll_in >= STM32H7_PLL_IN_MIN_HZ && out->pll_in <= STM32H7_PLL_IN_SPLIT_HZ;
	} else {
		ok = ok && out->pll_in > STM32H7_PLL_IN_SPLIT_HZ && out->pll_in <= STM32H7_PLL_IN_MAX_HZ;
	}

	uint64_t vco = (uint64_t)out->pll_in * c->pll_n;	// 64 MHz x 512 does not fit 32 bits
	out->vco = vco > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)vco;
	if (c->pll_n < 4u || c->pll_n > 512u) {
		ok = false;
	}
	if (c->vco_medium) {
		ok = ok && out->vco >= STM32H7_VCO_MEDIUM_MIN_HZ && out->vco <= STM32H7_VCO_MEDIUM_MAX_HZ;
	} else {
		ok = ok && out->vco >= STM32H7_VCO_WIDE_MIN_HZ && out->vco <= STM32H7_VCO_WIDE_MAX_HZ;
	}

	out->sys = c->pll_p != 0 ? out->vco / c->pll_p : 0;
	out->usb = c->pll_q != 0 ? out->vco / c->pll_q : 0;
	if (c->pll_p == 0 || c->pll_p > 128u || (c->pll_p != 1u && (c->pll_p & 1u) != 0)) {
		ok = false;	// DIVP1: 1 or an even divisor
	}
	if (c->pll_q == 0 || c->pll_q > 128u || out->vco % c->pll_q != 0) {
		ok = false;	// DIVQ1, and 48 MHz to the Hz
	}
	ok = ok && out->sys <= STM32H7_PLL_OUT_MAX_HZ && out->usb <= STM32H7_PLL_OUT_MAX_HZ;
	ok = ok && out->usb == STM32H7_USB_HZ;

	out->cpu = cpu_div != 0 ? out->sys / cpu_div : 0;
	out->hclk = ahb_div != 0 ? out->cpu / ahb_div : 0;
	out->pclk = apb_div != 0 ? out->hclk / apb_div : 0;
	// TIMPRE is 0: a timer runs at its bus clock when that is undivided, at twice it otherwise
	out->timer = apb_div == 1u ? out->pclk : 2u * out->pclk;
	ok = ok && cpu_div != 0 && ahb_div != 0 && apb_div != 0;
	ok = ok && out->cpu <= STM32H7_CPU_MAX_HZ && out->hclk <= STM32H7_HCLK_MAX_HZ &&
	     out->pclk <= STM32H7_PCLK_MAX_HZ && out->timer <= STM32H7_TIMER_MAX_HZ;

	ok = ok && c->flash_latency >= stm32h7_flash_ws_min(out->hclk) && c->flash_latency <= STM32H7_FLASH_WS_MAX;
	return ok;
}

// The table that gives c's VCO and every rate of c from the HSI: hse_hz 0, DIVM1 20 (3.2 MHz, clear of every PLL1RGE
// edge), wide VCO, N = c's VCO / 3.2 MHz, the same P, Q and dividers. False when c's VCO is not a multiple of 3.2 MHz
// inside the wide range. A table that passes stm32h7_clock_check() always has one: its VCO is a multiple of the 48 MHz
// pll1_q (a multiple of 3.2 MHz) of at least 192 MHz
static inline bool stm32h7_clock_fallback(const stm32_clock_t *c, stm32_clock_t *out)
{
	const uint32_t in = STM32H7_HSI_HZ / STM32H7_FALLBACK_M;
	uint32_t src = c->hse_hz != 0 ? c->hse_hz : STM32H7_HSI_HZ;
	if (c->pll_m == 0 || src % c->pll_m != 0) {
		return false;
	}
	uint64_t vco = (uint64_t)(src / c->pll_m) * c->pll_n;
	if (vco < STM32H7_VCO_WIDE_MIN_HZ || vco > STM32H7_VCO_WIDE_MAX_HZ || (uint32_t)vco % in != 0) {
		return false;
	}
	*out = *c;
	out->hse_hz = 0;
	out->hse_bypass = false;
	out->pll_m = STM32H7_FALLBACK_M;
	out->pll_n = (uint16_t)((uint32_t)vco / in);
	out->vco_medium = false;
	return true;
}

// The ADC's analog clock in the synchronous mode CKMODE 11 on silicon revision V (RM0433 25.4.3): adc_sclk / 4, and
// / 2 again at the analog block, where adc_sclk is sys_ck with HPRE 1 and sys_ck / 2 with any other HPRE. That is not
// HCLK / 8 unless the CPU divider is 1 and HPRE at most 2; ST's ADC module works BOOST out from HCLK. (Revision Y
// runs it at adc_hclk / 4, which ST's module has right)
static inline uint32_t stm32h7_adc_sync_hz_v(uint32_t sys_hz, uint16_t ahb_div)
{
	return (ahb_div == 1u ? sys_hz : sys_hz / 2u) / 8u;
}

// ADC_CR BOOST[1:0] on revision V for an analog clock (RM0433 25.6.3)
static inline uint8_t stm32h7_adc_boost_v(uint32_t adc_hz)
{
	return adc_hz <= 6250000u ? 0 : adc_hz <= 12500000u ? 1 : adc_hz <= 25000000u ? 2 : 3;
}

#endif
