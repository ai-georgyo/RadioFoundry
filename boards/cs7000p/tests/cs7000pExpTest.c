// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS output expander and the shared DAC's interlock: the words and rules of exp_cs7000p.h, and
// their one writer (exp_cs7000p.c, built here with the real pins.h) on a model of the three 74HC595 behind PE7, PE8
// and PE9 and a fake DAC. What must hold on the radio: the reset hook takes every pin back and sets all 24 outputs
// with one latch edge to a word that neither holds the supply nor enables transmit; no caller, in any order, gets a
// transmit bit on under a reference above the floor, or a reference above the floor under one transmit bit;
// board_rf_safe works from any state and cuts into a write; a software reset with SK1 held waits, the chain already
// in its reset state, until the key is released (or a bound), feeding the watchdog.
//
// Built a second time with a floor other than 0 (cs7000pExpTestFloor), the same runs with values at and under it.
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "cs7000p.h"
#include "exp_cs7000p.h"
#include "pins.h"
#include "periph/dac.h"
#include "romentry_cs7000p.h"
#include "hal_watchdog.h"
#include "FreeRTOS.h"

#define FLOOR			((uint16_t)(CS7000P_APC_FLOOR))
#define MASK24			0xFFFFFFu

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- What the stub platform.h and FreeRTOS.h leave to the test ---

GPIO_TypeDef fake_ports[5];
RCC_TypeDef fake_rcc;
DAC_TypeDef fake_dac;
int fake_primask;
int fake_irq_depth;
uint32_t fake_nops;
DWT_Type fake_dwt;			// stands still: each "millisecond" of a busy wait is its pass cap
uint32_t SystemCoreClock = 4000;	// 4 passes a millisecond

// --- The chain as drawn: PE9 into IC619's SER, each QH' into the next chip, PE7 to every SRCLK, PE8 to every RCLK.
// Bit 0 of a word is IC619 QA, bit 23 IC621 QH. Time is the count of the half period loop's passes ---

static struct {
	uint32_t sr, out;		// the shift registers and the output pins
	bool clk, latch, ser;
	uint32_t t_ser, t_rise, t_fall, t_latch;
	int rises, latches;
	int rises_since_latch, rises_at_latch;
	uint32_t shown[8];		// every word the pins took, in order
	int n_shown;
	int timing_errors;		// a set-up time or pulse shorter than one half period
	int unmasked;			// a line or the DAC moved outside a masked section where one is due
	bool first_edge_all_analog;	// at the first write to a chain line, every other pin was in analog mode
	bool seen_edge;
} hw;

static bool want_masked;		// the call under test must do its work inside a masked section
static uint16_t forced[5];		// pins stm32_pin_force made outputs
static void (*on_rise)(int n);		// the test's fault or probe on the n-th clock edge of a write
static uint32_t seq, seq_latch, seq_dac_off, seq_wait;

static void chain_power_up(uint32_t held)
{
	memset(&hw, 0, sizeof(hw));
	hw.sr = 0x5A3C96u;		// no reset on the chips: whatever they woke up with
	hw.out = held & MASK24;
	on_rise = NULL;
}

static bool others_analog(void)
{
	return fake_ports[0].MODER == 0xEBFFFFFFu && fake_ports[1].MODER == 0xFFFFFFFFu &&
	       fake_ports[2].MODER == 0xFFFFFFFFu && fake_ports[3].MODER == 0xFFFFFFFFu;
}

// A chain line moves inside hc595_update's own masked section, so under the call's own one it is two deep: the
// driver's alone is not the board's check and write in one section. The DAC is written by the board's code only
static void masked_check(void)
{
	if (want_masked && fake_irq_depth < 1) {
		hw.unmasked++;
	}
}

static void masked_check_line(void)
{
	if (want_masked && fake_irq_depth < 2) {
		hw.unmasked++;
	}
}

// The mask (stub FreeRTOS.h). race: a one-shot "interrupt" that runs when the outermost section is entered, just
// before the mask takes effect, as an interrupt the mask came too late for
static void (*race)(void);

void fake_mask_enter(void)
{
	if (fake_irq_depth == 0 && race != NULL) {
		void (*r)(void) = race;
		race = NULL;
		r();
	}
	fake_irq_depth++;
}

static void line_clk(bool level)
{
	if (level == hw.clk) {
		return;
	}
	hw.clk = level;
	masked_check_line();
	if (!level) {
		if (fake_nops - hw.t_rise < EXP_DELAY_PASSES) {
			hw.timing_errors++;	// the clock's high time
		}
		hw.t_fall = fake_nops;
		return;
	}
	if (fake_nops - hw.t_ser < EXP_DELAY_PASSES || fake_nops - hw.t_fall < EXP_DELAY_PASSES) {
		hw.timing_errors++;		// data set-up, and the clock's low time
	}
	hw.t_rise = fake_nops;
	hw.sr = ((hw.sr << 1) | (hw.ser ? 1u : 0u)) & MASK24;
	hw.rises++;
	hw.rises_since_latch++;
	if (on_rise != NULL) {
		on_rise(hw.rises_since_latch);
	}
}

static void line_latch(bool level)
{
	if (level == hw.latch) {
		return;
	}
	hw.latch = level;
	masked_check_line();
	if (!level) {
		if (fake_nops - hw.t_latch < EXP_DELAY_PASSES) {
			hw.timing_errors++;
		}
		return;
	}
	if (fake_nops - hw.t_rise < EXP_DELAY_PASSES) {
		hw.timing_errors++;		// the last clock edge to the latch edge
	}
	hw.t_latch = fake_nops;
	hw.out = hw.sr;
	hw.latches++;
	hw.rises_at_latch = hw.rises_since_latch;
	hw.rises_since_latch = 0;
	seq_latch = ++seq;
	if (hw.n_shown < (int)(sizeof(hw.shown) / sizeof(hw.shown[0]))) {
		hw.shown[hw.n_shown++] = hw.out;
	}
}

void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool level)
{
	port->ODR = level ? (port->ODR | pins) : (port->ODR & ~(uint32_t)pins);
	if (port != &fake_ports[4]) {
		return;
	}
	if ((pins & 0x0380u) != 0 && !hw.seen_edge) {
		hw.seen_edge = true;
		hw.first_edge_all_analog = others_analog();
	}
	if (pins & (1u << 9)) {		// PE9, DS
		if (level != hw.ser) {
			masked_check_line();
			hw.ser = level;
			hw.t_ser = fake_nops;
		}
	}
	if (pins & (1u << 7)) {		// PE7, CLK
		line_clk(level);
	}
	if (pins & (1u << 8)) {		// PE8, CS: the latch
		line_latch(level);
	}
}

bool stm32_pin_read(GPIO_TypeDef *port, uint16_t pin)
{
	return (port->IDR & pin) != 0;
}

// As platform/stm32/gpio.c: the clock, the level, then push-pull output
void stm32_pin_force(GPIO_TypeDef *port, uint16_t pins, bool level)
{
	unsigned idx = (unsigned)(port - fake_ports);
	fake_rcc.AHB4ENR |= 1u << idx;
	stm32_pin_write(port, pins, level);
	port->OTYPER &= ~(uint32_t)pins;
	for (unsigned n = 0; n < 16; n++) {
		if (pins & (1u << n)) {
			port->MODER = (port->MODER & ~(3u << (2 * n))) | (1u << (2 * n));
		}
	}
	forced[idx] |= pins;
}

// --- The DAC behind periph/dac.h's inline write, as platform/stm32h7/periph/dac.c drives the registers ---

static struct {
	int inits, offs, force_zeros, waits;
	uint32_t wait_ns;
	uint32_t wait_dhr;		// the data register and the enable while the settle wait ran
	bool wait_en;
} dac;

void stm32_dac_init(uint8_t ch, uint16_t value)
{
	CHECK(ch == 1);
	fake_rcc.APB1LENR |= RCC_APB1LENR_DAC12EN;
	fake_dac.DHR12R1 = value;
	fake_dac.CR |= DAC_CR_EN1;
	dac.inits++;
}

void stm32_dac_off(uint8_t ch)
{
	CHECK(ch == 1);
	masked_check();
	fake_dac.DHR12R1 = 0;
	fake_dac.CR &= ~DAC_CR_EN1;
	dac.offs++;
	seq_dac_off = ++seq;
}

void stm32_dac_force_zero(uint8_t ch)
{
	CHECK(ch == 1);
	dac.force_zeros++;
	if (fake_rcc.APB1LENR & RCC_APB1LENR_DAC12EN) {
		fake_dac.DHR12R1 = 0;
		fake_dac.CR &= ~DAC_CR_EN1;
		seq_dac_off = ++seq;
	}
}

void stm32_wait_ns(uint32_t ns)
{
	masked_check();
	dac.waits++;
	dac.wait_ns = ns;
	dac.wait_dhr = fake_dac.DHR12R1;
	dac.wait_en = (fake_dac.CR & DAC_CR_EN1) != 0;
	seq_wait = ++seq;
}

// --- The radio's sequencer, told by board_rf_safe ---

static struct {
	int calls;
	uint32_t saw_out;
	uint16_t saw_dac;
	int saw_depth;
} radio;

void cs7000p_radio_rf_safe(void)
{
	radio.calls++;
	radio.saw_out = hw.out;
	radio.saw_dac = cs7000p_apc_dac_get();
	radio.saw_depth = fake_irq_depth;
}

// --- exp_cs7000p.h: the bits against the drawing, the words, the rules ---

// The schematic's output map, written out again: chip (0 = IC619, the first in the chain), output (0 = QA .. 7 = QH),
// the net on that pin's wire
static const struct { uint8_t chip, q; const char *net; uint32_t bit; } nets[] = {
	{ 0, 0, "A-CSN", EXP_AK_CSN },		{ 0, 1, "(no label)", EXP_SPARE },
	{ 0, 2, "MOT-C", EXP_MOTOR },		{ 0, 3, "RF_APC_SW", EXP_APC_LOOP },
	{ 0, 4, "BLPOWER_ON", EXP_BT_ON },	{ 0, 5, "EXT-MIC-C", EXP_MIC_EXT },
	{ 0, 6, "A-PDN", EXP_AK_RUN },		{ 0, 7, "DMR_SLEEP", EXP_DMR_SLEEP },
	{ 1, 0, "VCOVCC_SW", EXP_VCO_RX },	{ 1, 1, "5TC", EXP_TX_RAIL },
	{ 1, 2, "MIC-C", EXP_MIC_INT },		{ 1, 3, "SAVE", EXP_SYNTH_RAIL },
	{ 1, 4, "RX_LED", EXP_LED_GREEN },	{ 1, 5, "TX_LED", EXP_LED_RED },
	{ 1, 6, "AFCO", EXP_AMP },		{ 1, 7, "5RC", EXP_RX_RAIL },
	{ 2, 0, "LAMP", EXP_LAMP },		{ 2, 1, "PHONE_C", EXP_SPK_INT_OFF },
	{ 2, 2, "MICPOW_C", EXP_MIC_POWER },	{ 2, 3, "SPK_C", EXP_SPK_EXT_OFF },
	{ 2, 4, "POW_C", EXP_POWER_HOLD },	{ 2, 5, "MD-SCL", EXP_ACC_SCL },
	{ 2, 6, "FM_SW", EXP_AUDIO_RAIL },	{ 2, 7, "AF_MUTE", EXP_AF_MUTE },
};

static void test_bits(void)
{
	uint32_t all = 0;

	CHECK(EXP_BITS == 24u && sizeof(nets) / sizeof(nets[0]) == 24u);
	for (unsigned i = 0; i < sizeof(nets) / sizeof(nets[0]); i++) {
		if (nets[i].bit != 1u << (8u * nets[i].chip + nets[i].q)) {
			printf("FAIL %s is not chip %u output %u\n", nets[i].net, nets[i].chip, nets[i].q);
			failures++;
		}
		CHECK((all & nets[i].bit) == 0);	// no bit twice
		all |= nets[i].bit;
	}
	CHECK(all == MASK24);
}

static void test_reset_word(void)
{
	const uint32_t w = EXP_RESET_WORD;

	CHECK(w == 0x2A0181u);
	// No RF: the transmitter's rail and the PA loop off, the receive VCO selected, the other rails off
	CHECK((w & EXP_TX_RAIL) == 0 && (w & EXP_APC_LOOP) == 0 && (w & EXP_VCO_RX) != 0);
	CHECK((w & (EXP_RX_RAIL | EXP_SYNTH_RAIL | EXP_AUDIO_RAIL)) == 0);
	// The supply is NOT held: a reset with the knob off ends with the radio off
	CHECK((w & EXP_POWER_HOLD) == 0);
	// Both chips parked: the IF receiver deselected and in power-down, the modem asleep
	CHECK((w & EXP_AK_CSN) != 0 && (w & EXP_AK_RUN) == 0 && (w & EXP_DMR_SLEEP) != 0);
	// Audio: the amplifier off, both speakers disconnected, the microphones and their amplifier off
	CHECK((w & EXP_AMP) == 0 && (w & EXP_SPK_INT_OFF) != 0 && (w & EXP_SPK_EXT_OFF) != 0);
	CHECK((w & (EXP_MIC_INT | EXP_MIC_EXT | EXP_MIC_POWER)) == 0);
	// Lights, and what is not used: Bluetooth and the motor off, the accelerometer's clock idle high
	CHECK((w & (EXP_LED_GREEN | EXP_LED_RED | EXP_LAMP)) == 0);
	CHECK((w & (EXP_BT_ON | EXP_MOTOR | EXP_SPARE | EXP_AF_MUTE)) == 0 && (w & EXP_ACC_SCL) != 0);
	CHECK(EXP_TX_BITS == (EXP_TX_RAIL | EXP_APC_LOOP));
	// The half period's count exists (the delay hook itself is watched on the model below)
	CHECK(EXP_DELAY_PASSES > 0u);
}

static void test_rf_safe_word(void)
{
	uint32_t bad = 0;

	for (uint32_t s = 0; s <= MASK24; s++) {	// every shadow
		uint32_t w = exp_rf_safe_word(s);
		bad += (w & EXP_TX_BITS) != 0 || (w & EXP_VCO_RX) == 0 ||
		       ((w ^ s) & ~(EXP_TX_BITS | EXP_VCO_RX)) != 0;
	}
	CHECK(bad == 0);
	CHECK(exp_rf_safe_word(EXP_RESET_WORD) == EXP_RESET_WORD);
	// The hold is the knob's business, not a fault path's
	CHECK((exp_rf_safe_word(EXP_POWER_HOLD | EXP_TX_BITS) & EXP_POWER_HOLD) != 0);
}

static void test_interlock_rule(void)
{
	static const uint32_t tx[4] = { 0, EXP_TX_RAIL, EXP_APC_LOOP, EXP_TX_BITS };
	static const uint16_t floors[3] = { 0, 300, 4094 };

	for (unsigned f = 0; f < 3; f++) {
		const uint16_t fl = floors[f];
		const uint16_t dacs[3] = { (uint16_t)(fl > 0 ? fl - 1 : 0), fl, (uint16_t)(fl + 1) };	// below, at, above
		for (unsigned t = 0; t < 4; t++) {
			for (unsigned d = 0; d < 3; d++) {
				const bool above = dacs[d] > fl;
				// Any bit beside the transmit bits changes nothing
				for (unsigned other = 0; other < 2; other++) {
					const uint32_t w = tx[t] | (other ? (MASK24 & ~EXP_TX_BITS) : 0u);
					// Above the floor: only as a drive (both bits) or as a tuning voltage (neither)
					CHECK(exp_dac_ok(w, dacs[d], fl) == (!above || t == 0 || t == 3));
					// A transmit bit comes on only under a reference at or below the floor
					CHECK(exp_set_ok(w, dacs[d], fl) == (!above || t == 0));
				}
			}
		}
	}
}

// --- exp_cs7000p.c on the model ---

// What the outputs, the shadow, the DAC and its registers must agree on after every call
static void check_state(void)
{
	const uint16_t v = cs7000p_apc_dac_get();

	CHECK(hw.out == cs7000p_exp_get());
	CHECK(exp_dac_ok(cs7000p_exp_get(), v, FLOOR));
	if (v == 0) {
		CHECK((fake_dac.CR & DAC_CR_EN1) == 0);		// "0" is the channel off: a true 0 V
	} else {
		CHECK((fake_dac.CR & DAC_CR_EN1) != 0 && fake_dac.DHR12R1 == v);
	}
	CHECK(fake_irq_depth == 0 && hw.unmasked == 0 && hw.timing_errors == 0);
	CHECK(!hw.clk && !hw.latch);
}

static bool set(uint32_t s, uint32_t c)
{
	want_masked = true;
	bool ok = cs7000p_exp_set(s, c);
	want_masked = false;
	return ok;
}

static bool apc(uint16_t v)
{
	want_masked = true;
	bool ok = cs7000p_apc_dac(v);
	want_masked = false;
	return ok;
}

// The reset hook, as the reset handler calls it: nothing set up, the pins as the ROM bootloader's Leave may have left
// them, the chain holding whatever it held, hold and transmit bits included
static void test_reset_hook(void)
{
	for (int round = 0; round < 2; round++) {	// a second run gives the same: the hook keeps no state
		memset(fake_ports, 0, sizeof(fake_ports));
		memset(&fake_rcc, 0, sizeof(fake_rcc));
		memset(&fake_dac, 0, sizeof(fake_dac));
		memset(forced, 0, sizeof(forced));
		memset(&dac, 0, sizeof(dac));
		for (unsigned p = 0; p < 5; p++) {
			fake_ports[p].MODER = 0x5A5A1234u * (p + 3u);	// outputs and alternate functions everywhere
			fake_ports[p].OTYPER = 0xFFFFu;
			fake_ports[p].OSPEEDR = 0xFFFFFFFFu;
			fake_ports[p].PUPDR = 0x55555555u;
		}
		chain_power_up(0xD7FE7Eu | EXP_POWER_HOLD | EXP_TX_BITS);
		fake_nops = 1000;
		fake_irq_depth = 0;

		board_reset_safe();

		CHECK((fake_rcc.AHB4ENR & 0x1Fu) == 0x1Fu);
		// Every pin analog; PA13 / PA14 the debug port; PE4 and the chain's three lines outputs
		CHECK(fake_ports[0].MODER == 0xEBFFFFFFu && fake_ports[1].MODER == 0xFFFFFFFFu);
		CHECK(fake_ports[2].MODER == 0xFFFFFFFFu && fake_ports[3].MODER == 0xFFFFFFFFu);
		CHECK(fake_ports[4].MODER == ((0xFFFFFFFFu & ~((3u << 8) | (3u << 14) | (3u << 16) | (3u << 18))) |
					      (1u << 8) | (1u << 14) | (1u << 16) | (1u << 18)));
		CHECK(hw.first_edge_all_analog);	// taken back before the first chain line moved
		for (unsigned p = 0; p < 5; p++) {
			CHECK(fake_ports[p].OTYPER == 0);
			CHECK(fake_ports[p].OSPEEDR == (p == 0 ? 0x0C000000u : 0u));
			CHECK(fake_ports[p].PUPDR == (p == 0 ? 0x24000000u : 0u));
			CHECK(forced[p] == (p == 4 ? 0x0390u : 0u));
		}
		// PE4, the data flash's select, high; clock and latch back low (the data line keeps the last bit)
		CHECK((fake_ports[4].ODR & 0x0190u) == 0x0010u);
		// One latch edge after 24 bits, and the pins went from what they held straight to the reset word
		CHECK(hw.latches == 1 && hw.rises == 24 && hw.rises_at_latch == 24);
		CHECK(hw.n_shown == 1 && hw.shown[0] == EXP_RESET_WORD && hw.out == 0x2A0181u);
		CHECK((hw.out & (EXP_POWER_HOLD | EXP_TX_BITS)) == 0);
		// The half period ran between every pair of edges: 2 a bit and 1 for the latch pulse
		CHECK(hw.timing_errors == 0 && fake_nops - 1000u == 49u * EXP_DELAY_PASSES);
		// The DAC is not touched: PA4 stays in analog mode and the pull-down keeps the PA's reference at 0 V
		CHECK(fake_dac.CR == 0 && dac.inits + dac.offs + dac.force_zeros + dac.waits == 0);
		CHECK(fake_rcc.APB1LENR == 0 && fake_irq_depth == 0);
	}
}

// main(): board_rf_safe before anything is set up, then the DAC as board_early_init sets it up
static void test_start(void)
{
	CHECK(cs7000p_exp_get() == EXP_RESET_WORD && cs7000p_apc_dac_get() == 0);	// the shadow exists from the start
	int latches = hw.latches;
	board_rf_safe();
	CHECK(hw.latches == latches + 1 && hw.out == EXP_RESET_WORD && radio.calls == 1);
	CHECK(dac.force_zeros == 1 && fake_dac.CR == 0);	// its clock is off: nothing written
	check_state();
	// No reference before the DAC is set up, whatever the lines say
	CHECK(!apc(100) && !apc(4095) && fake_dac.CR == 0 && fake_dac.DHR12R1 == 0 && cs7000p_apc_dac_get() == 0);
	CHECK(apc(0) && dac.waits == 0 && dac.offs == 0);
	stm32_dac_init(1, 0);
	stm32_dac_off(1);
	check_state();
}

static void test_expander_writes(void)
{
	int latches = hw.latches, rises = hw.rises;
	uint32_t t = fake_nops;

	CHECK(set(EXP_LED_GREEN | EXP_SYNTH_RAIL, 0));
	CHECK(hw.out == (EXP_RESET_WORD | EXP_LED_GREEN | EXP_SYNTH_RAIL));
	CHECK(hw.latches == latches + 1 && hw.rises == rises + 24 && fake_nops - t == 49u * EXP_DELAY_PASSES);
	check_state();
	CHECK(set(EXP_LED_RED, EXP_LED_GREEN) && hw.out == (EXP_RESET_WORD | EXP_LED_RED | EXP_SYNTH_RAIL));
	CHECK(set(0, EXP_LED_RED | EXP_SYNTH_RAIL) && hw.out == EXP_RESET_WORD);
	// The same bit in both: set wins, as the driver's update
	CHECK(set(EXP_LAMP, EXP_LAMP) && (hw.out & EXP_LAMP) != 0 && set(0, EXP_LAMP));
	// A bit above the chain is dropped
	CHECK(set(1u << 24, 0) && cs7000p_exp_get() == EXP_RESET_WORD);
	check_state();
}

static void test_interlock_at_the_sink(void)
{
	const uint16_t tune = (uint16_t)(FLOOR + 1700u), drive = (uint16_t)(FLOOR + 900u);
	int latches;

	// Receiving: the DAC is the front end's tuning voltage, both transmit bits clear
	CHECK(apc(tune) && cs7000p_apc_dac_get() == tune && fake_dac.DHR12R1 == tune && (fake_dac.CR & DAC_CR_EN1));
	check_state();
	// No transmit bit comes on under it, alone, together, or among other bits; nothing is written
	latches = hw.latches;
	CHECK(!set(EXP_TX_RAIL, 0) && !set(EXP_APC_LOOP, 0) && !set(EXP_TX_BITS, 0));
	CHECK(!set(EXP_TX_RAIL | EXP_LED_RED, EXP_VCO_RX));
	CHECK(hw.latches == latches && hw.out == EXP_RESET_WORD && cs7000p_apc_dac_get() == tune);
	check_state();
	// "DAC 0": the data register to 0 with the buffer on, the settle time, then the channel off. All of it masked
	int waits = dac.waits, offs = dac.offs;
	CHECK(apc(0) && cs7000p_apc_dac_get() == 0);
	CHECK(dac.waits == waits + 1 && dac.wait_ns == 20000u && dac.wait_dhr == 0 && dac.wait_en);
	CHECK(dac.offs == offs + 1 && seq_dac_off > seq_wait);
	check_state();
	// Off already: nothing at all (a write of 0 would switch the channel on)
	CHECK(apc(0) && dac.waits == waits + 1 && dac.offs == offs + 1 && (fake_dac.CR & DAC_CR_EN1) == 0);
	// Now the transmit rail may come on. With one transmit bit there is no value above the floor
	CHECK(set(EXP_TX_RAIL, EXP_VCO_RX | EXP_RX_RAIL));
	CHECK(!apc(drive) && !apc(4095) && cs7000p_apc_dac_get() == 0 && (fake_dac.CR & DAC_CR_EN1) == 0);
	if (FLOOR > 0) {
		// At and under the floor no RF comes out: allowed in any state
		CHECK(apc(FLOOR) && cs7000p_apc_dac_get() == FLOOR && apc(1) && apc(FLOOR));
	}
	check_state();
	CHECK(set(EXP_APC_LOOP, 0));		// the loop closes on a reference at or below the floor
	CHECK(apc(drive) && apc(4095) && apc(drive) && cs7000p_apc_dac_get() == drive);
	CHECK(!apc(4096) && !apc(0xFFFF) && cs7000p_apc_dac_get() == drive);	// not a DAC code
	check_state();
	// Other lines move freely while transmitting; setting a transmit bit that is set already is still "setting"
	CHECK(set(EXP_LED_RED, 0) && !set(EXP_TX_RAIL, 0) && cs7000p_apc_dac_get() == drive);
	// A clear is never refused. One transmit bit cleared under the drive: the reference goes first
	latches = hw.latches;
	offs = dac.offs;
	CHECK(set(0, EXP_APC_LOOP) && hw.latches == latches + 1 && dac.offs == offs + 1 && seq_dac_off < seq_latch);
	CHECK(cs7000p_apc_dac_get() == 0 && (hw.out & EXP_TX_BITS) == EXP_TX_RAIL);
	check_state();
	// Both cleared in one write: the rule holds with the value as it is (nothing drives the PA)
	CHECK(set(EXP_APC_LOOP, 0) && apc(drive));
	offs = dac.offs;
	CHECK(set(EXP_VCO_RX, EXP_TX_BITS | EXP_LED_RED) && dac.offs == offs && cs7000p_apc_dac_get() == drive);
	check_state();
	CHECK(apc(0) && hw.out == EXP_RESET_WORD);
	check_state();
}

// The interlock's check and its write in one masked section: an interrupt that comes just before the section, and
// takes the other half of the rule, is seen by the check. Checked before the section, both calls would win
static bool race_ok;
static uint16_t race_drive;

static void race_apc(void)
{
	race_ok = cs7000p_apc_dac(race_drive);
}

static void race_tx_rail(void)
{
	race_ok = cs7000p_exp_set(EXP_TX_RAIL, EXP_VCO_RX);
}

static void test_check_inside_the_mask(void)
{
	const uint16_t drive = (uint16_t)(FLOOR + 900u);

	// Receiving, the DAC at 0. A transmit bit is asked for while an interrupt puts the tuning voltage on: refused
	CHECK(apc(0) && (cs7000p_exp_get() & EXP_TX_BITS) == 0u);
	race_drive = drive;
	race = race_apc;
	race_ok = false;
	const bool set_ok = set(EXP_TX_RAIL, EXP_VCO_RX);
	CHECK(race == NULL && race_ok && !set_ok);
	CHECK(cs7000p_apc_dac_get() == drive && (hw.out & EXP_TX_BITS) == 0u && (hw.out & EXP_VCO_RX) != 0u);
	check_state();
	// The DAC at 0 again. A reference above the floor is asked for while an interrupt sets one transmit bit:
	// refused
	CHECK(apc(0));
	race = race_tx_rail;
	race_ok = false;
	const bool apc_ok = apc(drive);
	CHECK(race == NULL && race_ok && !apc_ok);
	CHECK(cs7000p_apc_dac_get() == 0u && (hw.out & EXP_TX_BITS) == EXP_TX_RAIL);
	check_state();
	CHECK(set(EXP_VCO_RX, EXP_TX_RAIL) && hw.out == EXP_RESET_WORD);
	check_state();
}

static void rise_probe_shadow(int n)
{
	// The shadow is the safe word before the first bit of it is out
	if (n == 1) {
		CHECK((cs7000p_exp_get() & EXP_TX_BITS) == 0 && (cs7000p_exp_get() & EXP_VCO_RX) != 0);
		CHECK(cs7000p_apc_dac_get() == 0 && (fake_dac.CR & DAC_CR_EN1) == 0);	// and the DAC went first
	}
}

static void test_rf_safe(void)
{
	const uint32_t others = EXP_POWER_HOLD | EXP_SYNTH_RAIL | EXP_LED_RED | EXP_AMP;
	const uint16_t drive = (uint16_t)(FLOOR + 2000u);

	// From a transmission: both bits, the transmit VCO, a drive on the DAC
	CHECK(set(others | EXP_TX_RAIL, EXP_VCO_RX) && set(EXP_APC_LOOP, 0) && apc(drive));
	int calls = radio.calls, waits = dac.waits;
	on_rise = rise_probe_shadow;
	board_rf_safe();
	on_rise = NULL;
	CHECK(hw.out == (EXP_RESET_WORD | others) && cs7000p_exp_get() == hw.out);	// the rest as it was, the hold too
	CHECK(dac.waits == waits);			// no wait in a fault path
	// The radio is told after the lines moved and before the mask is lifted: it writes nothing itself, so a burst
	// point that ran in between would stay keyed
	CHECK(radio.calls == calls + 1 && radio.saw_out == hw.out && radio.saw_dac == 0 && radio.saw_depth == 1);
	check_state();
	// Code that runs on can't bring the transmit bits back with its next update
	CHECK(set(EXP_LED_GREEN, EXP_LED_RED) && (hw.out & EXP_TX_BITS) == 0);
	// From each state with one transmit bit, and from receive with a tuning value: the same end
	for (unsigned k = 0; k < 3; k++) {
		CHECK(apc(0) && set(k == 0 ? EXP_TX_RAIL : k == 1 ? EXP_APC_LOOP : 0u, k == 2 ? 0u : EXP_VCO_RX));
		if (k == 2) {
			CHECK(apc(drive));
		}
		board_rf_safe();
		CHECK((hw.out & EXP_TX_BITS) == 0 && (hw.out & EXP_VCO_RX) != 0 && cs7000p_apc_dac_get() == 0);
		check_state();
	}
	board_rf_safe();	// any number of times
	check_state();
	CHECK(set(0, others | EXP_LED_GREEN) && hw.out == EXP_RESET_WORD);
}

// A fault in the middle of a write: the handler's word goes out whole, built from the word being written
static jmp_buf fault_env;
static int fault_at;

static void rise_fault(int n)
{
	if (n == fault_at) {
		on_rise = NULL;
		board_rf_safe();
		longjmp(fault_env, 1);		// the interrupted write is never resumed: a reset follows
	}
}

static void test_rf_safe_cuts_a_write(void)
{
	static const int at[] = { 1, 7, 13, 24 };

	for (volatile unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++) {	// volatile: it lives across the longjmp
		CHECK(set(EXP_TX_RAIL, EXP_VCO_RX));
		fault_at = at[i];
		hw.n_shown = 0;
		if (setjmp(fault_env) == 0) {
			on_rise = rise_fault;
			(void)cs7000p_exp_set(EXP_APC_LOOP | EXP_LED_RED, 0);	// the loop closing when the fault comes
			CHECK(!"the fault did not come");
		}
		fake_irq_depth = 0;		// (the test goes on where the radio would restart)
		// The pins never showed the word the fault cut, only the safe one, with the LED of the cut write
		CHECK(hw.n_shown == 1 && hw.shown[0] == (EXP_RESET_WORD | EXP_LED_RED));
		CHECK(hw.out == cs7000p_exp_get() && (hw.out & EXP_TX_BITS) == 0);
		// The cut write's bits were pushed out of the chain by the handler's 24. Its last clock pulse was cut short:
		// whether the chips took that bit does not matter under 24 more
		CHECK(hw.rises_at_latch == fault_at + 24 && hw.timing_errors <= 1);
		hw.timing_errors = 0;
		CHECK(set(0, EXP_LED_RED));
		check_state();
	}
}

// Every caller in every order: the rule holds after each call, and each answer is the header's rule on the state
// before it
static void test_any_order(void)
{
	static const uint16_t values[] = { 0, 1, FLOOR, (uint16_t)(FLOOR + 1u), 2048, 4095, 4096 };
	uint32_t rnd = 0x2545F491u;
	int refused_set = 0, refused_dac = 0, done = 0;

	for (int i = 0; i < 20000; i++) {
		rnd ^= rnd << 13;
		rnd ^= rnd >> 17;
		rnd ^= rnd << 5;
		const uint32_t before = cs7000p_exp_get();
		const uint16_t dac_before = cs7000p_apc_dac_get();
		const int latches = hw.latches;
		const unsigned op = rnd % 7u;
		if (op < 3) {
			// Mostly a few bits, the transmit bits among them often
			uint32_t s = (rnd >> 4) & (rnd >> 9) & MASK24, c = (rnd >> 7) & (rnd >> 13) & MASK24;
			if (op == 0) {
				s = (rnd & 0x100u) ? EXP_TX_RAIL : EXP_APC_LOOP;
				c = (rnd & 0x200u) ? EXP_VCO_RX : 0u;
			} else if (op == 1) {
				s = 0;
				c &= EXP_TX_BITS | EXP_LED_RED;
			}
			const bool want = exp_set_ok(s, dac_before, FLOOR);
			CHECK(set(s, c) == want);
			if (want) {
				CHECK(cs7000p_exp_get() == ((before & ~c) | s) && hw.latches == latches + 1);
				done++;
			} else {
				CHECK(cs7000p_exp_get() == before && hw.latches == latches &&
				      cs7000p_apc_dac_get() == dac_before);
				refused_set++;
			}
		} else if (op < 6) {
			const uint16_t v = values[(rnd >> 8) % (sizeof(values) / sizeof(values[0]))];
			const bool want = v == 0 || (v <= 4095 && exp_dac_ok(before, v, FLOOR));
			CHECK(apc(v) == want);
			CHECK(cs7000p_apc_dac_get() == (want ? v : dac_before) && hw.latches == latches);
			refused_dac += !want;
		} else {
			board_rf_safe();
			CHECK(cs7000p_exp_get() == exp_rf_safe_word(before) && cs7000p_apc_dac_get() == 0);
		}
		check_state();
		if (failures > 20) {
			return;
		}
	}
	CHECK(refused_set > 100 && refused_dac > 100 && done > 1000);	// the sweep did meet the interlock
	board_rf_safe();
	CHECK(set(EXP_RESET_WORD, MASK24 & ~EXP_RESET_WORD) && hw.out == EXP_RESET_WORD);
}

// The watchdog, fed during the SK1 wait; and SK1's line (PD15, low = down) as the wait finds it at each feed
static struct {
	int feeds;
	int release_at;			// the feed at which the key is let go (0: never)
	int bounce_at;			// ... and at which it is down again for one step (0: never)
	uint32_t out_at_first;		// the chain's outputs at the first feed
	uint32_t moder_at_first;	// port D's modes then
} wd;

void hal_watchdog_feed(void)
{
	wd.feeds++;
	if (wd.feeds == 1) {
		wd.out_at_first = hw.out;
		wd.moder_at_first = fake_ports[3].MODER;
	}
	if (wd.feeds == wd.release_at) {
		fake_ports[3].IDR |= 1u << 15;
	}
	if (wd.feeds == wd.bounce_at) {
		fake_ports[3].IDR &= ~(1u << 15);
	} else if (wd.bounce_at != 0 && wd.feeds == wd.bounce_at + 1) {
		fake_ports[3].IDR |= 1u << 15;
	}
}

static void sk1(bool down)
{
	fake_ports[3].IDR = down ? 0u : 1u << 15;
}

// Before a reset and before the jump to the ROM bootloader: from any state the chain gets the reset word (the hold
// clear), and the modem's configuration port is deselected. No state is read: a stale shadow changes nothing
static void test_prepare_hooks(void)
{
	sk1(false);
	for (int which = 0; which < 2; which++) {
		const uint32_t on = EXP_POWER_HOLD | EXP_SYNTH_RAIL | EXP_RX_RAIL | EXP_AMP | EXP_LED_GREEN | EXP_AK_RUN;
		CHECK(set(on, EXP_DMR_SLEEP | EXP_SPK_INT_OFF) && (hw.out & EXP_POWER_HOLD) != 0);
		memset(forced, 0, sizeof(forced));
		fake_ports[1].ODR = 0;
		fake_ports[1].MODER = 0xFFFFFFFFu;
		hw.n_shown = 0;
		int waits = dac.waits;
		if (which == 0) {
			board_reset_prepare();
		} else {
			board_sysboot_prepare();
		}
		CHECK(hw.n_shown == 1 && hw.out == EXP_RESET_WORD && (hw.out & (EXP_POWER_HOLD | EXP_TX_BITS)) == 0);
		CHECK(forced[1] == (1u << 12) && (fake_ports[1].ODR & (1u << 12)) != 0);	// PB12 high
		CHECK(forced[0] == 0 && forced[2] == 0 && forced[3] == 0 && forced[4] == 0);
		CHECK(dac.waits == waits && hw.timing_errors == 0 && fake_irq_depth == 0);
		// (the image's own state is stale now, as before any reset: bring the model back in line for the next round)
		CHECK(set(EXP_RESET_WORD, MASK24 & ~EXP_RESET_WORD));
	}
}

// A software reset with SK1 held: the chain first, then the wait, PD15 read as an input whatever mode it was in
static int reset_with_sk1(int release_at, int bounce_at, uint32_t moder_d)
{
	const uint32_t on = EXP_POWER_HOLD | EXP_SYNTH_RAIL | EXP_RX_RAIL | EXP_AMP | EXP_TX_RAIL;
	CHECK(set(on, EXP_VCO_RX));
	memset(&wd, 0, sizeof(wd));
	wd.release_at = release_at;
	wd.bounce_at = bounce_at;
	fake_ports[3].MODER = moder_d;
	sk1(true);
	hw.n_shown = 0;
	int waits = dac.waits;
	board_reset_prepare();
	// The reset word went out before the first step, once, and nothing else moved the chain
	CHECK(wd.feeds == 0 || (wd.out_at_first == EXP_RESET_WORD && (wd.moder_at_first & (3u << 30)) == 0u));
	CHECK(hw.n_shown == 1 && hw.out == EXP_RESET_WORD && dac.waits == waits && fake_irq_depth == 0);
	// PD15 an input; port D's other pins as they were
	CHECK((fake_ports[3].MODER & (3u << 30)) == 0u && (fake_ports[3].MODER | (3u << 30)) == (moder_d | (3u << 30)));
	CHECK(set(EXP_RESET_WORD, MASK24 & ~EXP_RESET_WORD));
	return wd.feeds;
}

static void test_reset_waits_for_sk1(void)
{
	// The rule: the key up for CS7000P_SK1_UP_MS steps in a row, or the bound
	cs7000p_sk1_wait_t w = { 0 };
	CHECK(CS7000P_SK1_UP_MS == 10u && CS7000P_SK1_WAIT_MS == 30000u);
	for (unsigned i = 0; i < 100; i++) {
		CHECK(cs7000p_sk1_wait_more(&w, true));
	}
	for (unsigned i = 1; i < CS7000P_SK1_UP_MS; i++) {
		CHECK(cs7000p_sk1_wait_more(&w, false));
	}
	CHECK(cs7000p_sk1_wait_more(&w, true));		// a bounce starts the count again
	for (unsigned i = 1; i < CS7000P_SK1_UP_MS; i++) {
		CHECK(cs7000p_sk1_wait_more(&w, false));
	}
	CHECK(!cs7000p_sk1_wait_more(&w, false) && w.steps == 100u + 2u * CS7000P_SK1_UP_MS);
	w = (cs7000p_sk1_wait_t){ 0 };
	unsigned n = 0;
	while (cs7000p_sk1_wait_more(&w, true)) {
		n++;
	}
	CHECK(n + 1u == CS7000P_SK1_WAIT_MS);

	// Released at once: no wait. Before the pin set-up port D is analog (IDR 0 there; the mode goes to input first)
	sk1(false);
	memset(&wd, 0, sizeof(wd));
	fake_ports[3].MODER = 0xFFFFFFFFu;
	board_reset_prepare();
	CHECK(wd.feeds == 0 && (fake_ports[3].MODER & (3u << 30)) == 0u && fake_ports[3].MODER == 0x3FFFFFFFu);
	CHECK(set(EXP_RESET_WORD, MASK24 & ~EXP_RESET_WORD));
	// Held, let go after 500 steps: the reset after 10 steps up, the watchdog fed at every step
	CHECK(reset_with_sk1(500, 0, 0xFFFFFFFFu) == 500 + (int)CS7000P_SK1_UP_MS - 1);
	// A bounce on the way: the 10 steps from the last press
	CHECK(reset_with_sk1(500, 505, 0x55555555u) == 505 + 1 + (int)CS7000P_SK1_UP_MS - 1);
	// Never let go: the bound, then the reset goes ahead (into the ROM bootloader, the radio dark, nothing held)
	CHECK(reset_with_sk1(0, 0, 0) == (int)CS7000P_SK1_WAIT_MS);
	sk1(false);
}

int main(void)
{
	test_bits();
	test_reset_word();
	test_rf_safe_word();
	test_interlock_rule();
	test_reset_hook();
	test_start();
	test_expander_writes();
	test_interlock_at_the_sink();
	test_check_inside_the_mask();
	test_rf_safe();
	test_rf_safe_cuts_a_write();
	test_any_order();
	test_prepare_hooks();
	test_reset_waits_for_sk1();
	printf("# cs7000pExpTest%s %s (%d failures)\n", FLOOR ? "Floor" : "", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
