// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32H7's clock tree from a board's table (stm32_clock_t, clock_math.h), on the RCC's registers. Every wait has
// a bound and none depends on a time base, so a crystal that does not start costs a tenth of a second and never the
// radio: the same rates then come from the internal oscillator.
//
//   STM32H7_CLOCK_HSE      the board's table: crystal, PLL1, sys_ck = pll1_p, USB and the SPI1-3 kernel clock = pll1_q.
//                          The clock security system watches the crystal: if it stops, the NMI restarts the radio
//   STM32H7_CLOCK_HSI_PLL  the same VCO and dividers from the HSI (stm32h7_clock_fallback): every peripheral runs at
//                          the rate its driver computed, with the HSI's accuracy (about 1 %). USB from HSI48
//   STM32H7_CLOCK_HSI      no PLL locked, or the table is outside the limits: the bare 64 MHz HSI behind the bus
//                          dividers. pll1_q is off: SPI1-3 have no kernel clock. USB from HSI48
//
// The voltage scale is never changed (VOS3, the reset scale: the limits of clock_math.h are that scale's). PLL2,
// PLL3, the CSI and the LSE stay off; ADCSEL and CKPERSEL stay at reset (the ADC driver uses the synchronous clock,
// the bus clock or on revision V sys_ck, so it has a clock on every tree). The LSI starts with the watchdog.
#include "config.h"
#include "platform.h"

#define HSE_START_MS		100u
#define READY_MS		10u	// a PLL locks in 150 us at most, HSI48 starts in 4 us (DS12110)
#define SWITCH_SPINS		100000u	// the system clock switch and the flash's read back: a few cycles each
// CRS_CFGR for the USB start of frame: the reset value's reload (48 MHz / 1 kHz - 1) and error limit, with SYNCSRC 0,
// the full-speed core's SOF (RM0433 Table 87; the reset value selects the other core's)
#define CRS_CFGR_USB_SOF	((0x22u << CRS_CFGR_FELIM_Pos) | 0xBB7Fu)
#define REV_SOF_SYNC		0x2000u	// DBGMCU REV_ID from here on (X 0x2001, V 0x2003): after revision Y

static stm32h7_clock_source_t source = STM32H7_CLOCK_HSI;

stm32h7_clock_source_t stm32h7_clock_source(void)
{
	return source;
}

// Waits until (*reg & mask) == want, for at most the CPU cycles of ms at the HSI's 64 MHz, on the cycle counter (it
// runs: main() starts it before the board's early init). The pass count is capped by the same number, so the wait
// also ends if the counter stands still, some multiple of ms later. Behind a CPU divider the wait is that much
// longer; the two waits made after the switch to a faster PLL are shorter, for things that take microseconds
static bool wait_bits(const volatile uint32_t *reg, uint32_t mask, uint32_t want, uint32_t ms)
{
	const uint32_t cycles = ms * (STM32H7_HSI_HZ / 1000u);
	const uint32_t start = DWT->CYCCNT;
	for (uint32_t pass = 0; pass < cycles && DWT->CYCCNT - start < cycles; pass++) {
		if ((*reg & mask) == want) {
			return true;
		}
	}
	return (*reg & mask) == want;
}

// LATENCY with the programming delay that belongs to it, read back as RM0433 4.3.8 asks
static void flash_ws(uint8_t ws)
{
	const uint32_t acr = ((uint32_t)ws << FLASH_ACR_LATENCY_Pos) |
			     ((uint32_t)stm32h7_flash_wrhighfreq(ws) << FLASH_ACR_WRHIGHFREQ_Pos);
	FLASH->ACR = (FLASH->ACR & ~(FLASH_ACR_LATENCY | FLASH_ACR_WRHIGHFREQ)) | acr;
	for (uint32_t i = 0; i < SWITCH_SPINS && (FLASH->ACR & (FLASH_ACR_LATENCY | FLASH_ACR_WRHIGHFREQ)) != acr; i++) {
	}
}

static bool switch_to(uint32_t sw, uint32_t sws)
{
	RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | sw;
	for (uint32_t i = 0; i < SWITCH_SPINS; i++) {
		if ((RCC->CFGR & RCC_CFGR_SWS) == sws) {
			return true;
		}
	}
	return false;
}

static void hse_off(void)
{
	RCC->CR &= ~RCC_CR_HSEON;
	(void)wait_bits(&RCC->CR, RCC_CR_HSERDY, 0, READY_MS);
	RCC->CR &= ~RCC_CR_HSEBYP;	// takes a write only while the HSE is off
}

// The HSI at 64 MHz as sys_ck, PLL1 off: where a reset starts, and where a second call (a board trying another
// table) has to start too. The HSE can't be changed while a PLL runs from it
static void to_hsi(void)
{
	RCC->CR |= RCC_CR_HSION;
	(void)wait_bits(&RCC->CR, RCC_CR_HSIRDY, RCC_CR_HSIRDY, READY_MS);
	(void)switch_to(RCC_CFGR_SW_HSI, RCC_CFGR_SWS_HSI);
	RCC->CR &= ~RCC_CR_PLL1ON;
	(void)wait_bits(&RCC->CR, RCC_CR_PLL1RDY, 0, READY_MS);
	if ((RCC->CR & RCC_CR_HSIDIV) != RCC_CR_HSIDIV_1) {
		RCC->CR = (RCC->CR & ~RCC_CR_HSIDIV) | RCC_CR_HSIDIV_1;	// ignored while an enabled PLL runs from the HSI
		(void)wait_bits(&RCC->CR, RCC_CR_HSIDIVF, RCC_CR_HSIDIVF, READY_MS);
	}
}

// PLL1 from a table that passed stm32h7_clock_check(), then sys_ck from it. Called with the HSI as sys_ck, PLL1 off,
// the bus dividers written and the flash's wait states enough for the table. False: back as it was called (HSE off)
static bool pll_tree(const stm32_clock_t *t, uint32_t pll_in)
{
	uint32_t pllsrc = RCC_PLLCKSELR_PLLSRC_HSI;
	if (t->hse_hz != 0) {
		hse_off();
		if (t->hse_bypass) {
			RCC->CR |= RCC_CR_HSEBYP;
		}
		RCC->CR |= RCC_CR_HSEON;
		if (!wait_bits(&RCC->CR, RCC_CR_HSERDY, RCC_CR_HSERDY, HSE_START_MS)) {
			hse_off();
			return false;
		}
		// The clock security system, set only (a reset clears it): a crystal that stops later moves sys_ck to
		// the HSI and raises the NMI, the fault path's (RF safe, crash record, restart). The next start tries
		// the crystal again like any start: this tree if it starts in time, the HSI's (stm32_clock_init) if
		// not, and the crash record is the trace either way. Without it the PLL would run on with no
		// reference and the radio with whatever its timers then do. Switching the HSE off here is not a
		// failure: it stops the CSS too
		RCC->CR |= RCC_CR_CSSHSEON;
		pllsrc = RCC_PLLCKSELR_PLLSRC_HSE;
	}
	// PLLSRC is one field for the three PLLs; the other two stay off. Integer mode, the R output off
	RCC->PLLCKSELR = (RCC->PLLCKSELR & ~(RCC_PLLCKSELR_PLLSRC | RCC_PLLCKSELR_DIVM1)) | pllsrc |
			 ((uint32_t)t->pll_m << RCC_PLLCKSELR_DIVM1_Pos);
	RCC->PLLCFGR = (RCC->PLLCFGR & ~(RCC_PLLCFGR_PLL1FRACEN | RCC_PLLCFGR_PLL1VCOSEL | RCC_PLLCFGR_PLL1RGE |
					 RCC_PLLCFGR_DIVP1EN | RCC_PLLCFGR_DIVQ1EN | RCC_PLLCFGR_DIVR1EN)) |
		       (t->vco_medium ? RCC_PLLCFGR_PLL1VCOSEL : 0u) |
		       ((uint32_t)stm32h7_pll_rge(pll_in, t->vco_medium) << RCC_PLLCFGR_PLL1RGE_Pos) |
		       RCC_PLLCFGR_DIVP1EN | RCC_PLLCFGR_DIVQ1EN;
	RCC->PLL1DIVR = ((uint32_t)(t->pll_n - 1u) << RCC_PLL1DIVR_N1_Pos) |
			((uint32_t)(t->pll_p - 1u) << RCC_PLL1DIVR_P1_Pos) |
			((uint32_t)(t->pll_q - 1u) << RCC_PLL1DIVR_Q1_Pos) | (1u << RCC_PLL1DIVR_R1_Pos);
	RCC->PLL1FRACR = 0;
	RCC->CR |= RCC_CR_PLL1ON;
	if (wait_bits(&RCC->CR, RCC_CR_PLL1RDY, RCC_CR_PLL1RDY, READY_MS) &&
	    switch_to(RCC_CFGR_SW_PLL1, RCC_CFGR_SWS_PLL1)) {
		return true;
	}
	to_hsi();
	hse_off();
	return false;
}

// The clocks peripherals take from somewhere else than their bus. SPI1-3 from pll1_q, SPI4-5 and the USARTs from
// their bus: the reset values, written because the ROM bootloader may have run before us. USB from pll1_q on the
// crystal's tree. On an HSI tree from HSI48, which needs no PLL, trimmed by the CRS on the host's start of frame (what
// the ROM bootloader does on this chip, AN2606 Table 145): the USB core has a clock on every tree, and a radio that
// lost its crystal can still say so. On revision Y that synchronization does not work (ES0392 2.2.3): the CRS stays
// off there and HSI48 runs at its factory trim
static void kernel_clocks(bool hsi_tree)
{
	RCC->D2CCIP1R &= ~(RCC_D2CCIP1R_SPI123SEL | RCC_D2CCIP1R_SPI45SEL);
	uint32_t usbsel = 0;	// no USB in this build: the core's clock stays off
#if CONFIG_USB
	usbsel = RCC_D2CCIP2R_USBSEL_0;	// 01: pll1_q
	if (hsi_tree) {
		RCC->CR |= RCC_CR_HSI48ON;
		(void)wait_bits(&RCC->CR, RCC_CR_HSI48RDY, RCC_CR_HSI48RDY, READY_MS);
		if (stm32h7_rev_id() >= REV_SOF_SYNC) {
			RCC->APB1HENR |= RCC_APB1HENR_CRSEN;
			(void)RCC->APB1HENR;
			CRS->CR &= ~(CRS_CR_CEN | CRS_CR_AUTOTRIMEN);	// CRS_CFGR takes a write only with the counter off
			CRS->CFGR = CRS_CFGR_USB_SOF;
			CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;
		}
		usbsel = RCC_D2CCIP2R_USBSEL;	// 11: hsi48_ck
	}
#else
	(void)hsi_tree;
#endif
	RCC->D2CCIP2R = (RCC->D2CCIP2R & ~(RCC_D2CCIP2R_USART16SEL | RCC_D2CCIP2R_USART28SEL | RCC_D2CCIP2R_USBSEL)) |
			usbsel;
	if (usbsel != RCC_D2CCIP2R_USBSEL) {
		RCC->CR &= ~RCC_CR_HSI48ON;	// only after nothing selects it
	}
}

bool stm32_clock_init(const stm32_clock_t *c)
{
	stm32h7_clock_hz_t hz, fb_hz;
	stm32_clock_t fb;
	const bool valid = stm32h7_clock_check(c, &hz);
	// A table outside the limits is not applied, its dividers included: the bare HSI then gets HCLK 32 MHz and
	// PCLK 16 MHz
	const uint8_t cpu_div = valid ? c->cpu_div : 1u, ahb_div = valid ? c->ahb_div : 2u;
	const uint8_t apb_div = valid ? c->apb_div : 2u;
	const uint32_t cpu = stm32h7_cpu_div_code(cpu_div), ahb = stm32h7_cpu_div_code(ahb_div);
	const uint32_t apb = stm32h7_apb_div_code(apb_div);
	// Wait states: enough for the HSI behind the dividers and for the table, from before the first change until the
	// tree is known
	const uint8_t ws_hsi = stm32h7_flash_ws_min(STM32H7_HSI_HZ / cpu_div / ahb_div);
	const uint8_t ws_top = valid && c->flash_latency > ws_hsi ? c->flash_latency : ws_hsi;
	// to_hsi() puts the HSI behind the dividers this call finds, not the table's yet. After a reset the wait states
	// are SystemInit's seven. A second call may come from a slower tree with fewer than the HSI needs undivided
	const uint8_t ws_entry = stm32h7_flash_ws_entry(ws_top);

	if (((FLASH->ACR & FLASH_ACR_LATENCY) >> FLASH_ACR_LATENCY_Pos) < ws_entry) {
		flash_ws(ws_entry);
	}
	to_hsi();
	// The bus dividers first: no bus is over its limit on the way, whichever tree is reached
	RCC->D1CFGR = (cpu << RCC_D1CFGR_D1CPRE_Pos) | (apb << RCC_D1CFGR_D1PPRE_Pos) | (ahb << RCC_D1CFGR_HPRE_Pos);
	RCC->D2CFGR = (apb << RCC_D2CFGR_D2PPRE1_Pos) | (apb << RCC_D2CFGR_D2PPRE2_Pos);
	RCC->D3CFGR = apb << RCC_D3CFGR_D3PPRE_Pos;

	source = STM32H7_CLOCK_HSI;
	if (valid && pll_tree(c, hz.pll_in)) {
		source = c->hse_hz != 0 ? STM32H7_CLOCK_HSE : STM32H7_CLOCK_HSI_PLL;
	} else if (valid && c->hse_hz != 0 && stm32h7_clock_fallback(c, &fb) && stm32h7_clock_check(&fb, &fb_hz) &&
		   pll_tree(&fb, fb_hz.pll_in)) {
		source = STM32H7_CLOCK_HSI_PLL;
	}
	// Down to the table's wait states only once the clock they are for is the one in use
	flash_ws(source == STM32H7_CLOCK_HSI ? ws_top : c->flash_latency);
	kernel_clocks(source != STM32H7_CLOCK_HSE);
	SystemCoreClockUpdate();
	return valid && source == (c->hse_hz != 0 ? STM32H7_CLOCK_HSE : STM32H7_CLOCK_HSI_PLL);
}
