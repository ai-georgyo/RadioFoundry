// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS hal_radio_t (radio_cs7000p.c) over the real RF chain (rfops_cs7000p.c, chainseq, the
// synthesizer's and the IF receiver's drivers on their bit-banged buses), with a fake expander, DAC, modem, burst
// runner and power state. What must hold on the radio: the microphone's amplifier and switch are on for a
// transmission, FM or DMR, and off at its end and on every way it fails or is refused; in the power's off state (a
// power-off the knob overrode) the receiver is not set up again, the speaker amplifier stays off (also behind a
// speaker call the power-off cut into) and nothing transmits, FM or DMR, and all of it comes back once that state
// ends.
//
// The maker's factory table, on core/storage and the factory copy as the board builds them (the board's map, kept
// bytes, claimed first) over the simulated NOR flash: the table in use taken at init, the source and the view's keys
// for a blank, zero, unstable and damaged window, with and without a valid record, a conflict, a use rule's failure;
// a restore's request taken only at the status poll, never under a transmission, and receive on the restored table
// from the set-up after it (a receiver asleep: the wake, then the poll after it). Receive on the table: a refused
// table (and none) writes exactly what a radio without one writes and reads the same dBm; a valid one changes only
// the tune and the held level's bias (4:0x04 kept), trim A over its receive bound and a use rule's group falling back
// alone; the bench's 3:0 and 3:4 over all of it. The bench's 3:7: the IF chip's words (read off its pins) the port's
// or the maker's for the mode, and no part of a key-up; the maker's squelch thresholds at f_rx shown. The owner's
// transmit arm (hal_radio_t.tx_arm): its handshake against the CRC of the table in use (the window's, the record's,
// a restored one's), every refusal (a stage over the top, another id, no table, the bench's coding, the off state, a
// restore pending or its request untaken), the restore refused while armed, a lower stage always taken, the arm gone
// at a safe request and a start; unarmed, armed without a list and armed with one, no key-up the table would serve
// goes ahead; the bench drive's key-up writes as without a table, at every stage, but for the bias it inherits. Built
// again without CONFIG_FACTORY_COPY (cs7000pRadioNoCopyTest): the window alone. Every table here is invented.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "cs7000p.h"
#include "pins.h"
#include "exp_cs7000p.h"
#include "speaker_cs7000p.h"
#include "rfops_cs7000p.h"
#include "rfplan_cs7000p.h"
#include "makercal_cs7000p.h"
#include "store_cs7000p.h"
#include "storage/storage.h"
#include "storage/factory_copy.h"
#include "debuglink/dlog.h"
#include "debuglink/dl_frame.h"
#include "sim/sim_flash.h"
#include "radio/tx_limit.h"
#include "radio/pa_keyup.h"
#include "dmr_keyup/dmr_keyup.h"
#include "hal_time.h"
#include "task.h"
#include "semphr.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define MHZ		1000000u
#define MIC_BITS	(EXP_MIC_POWER | EXP_MIC_INT)

// --- The platform: pins, time, the interrupt mask ---

GPIO_TypeDef fake_ports[5];
RCC_TypeDef fake_rcc;
DAC_TypeDef fake_dac;
int fake_primask;
int fake_irq_depth;
uint32_t fake_nops;

static bool pll_ld = true;		// the synthesizer's lock line
static uint32_t now_us = 1000000u;

// Every write to the hardware the fakes see (pins, the expander, the DAC, the modem's calls), hashed in order: two
// runs that wrote the same things in the same order hash the same
static uint32_t hw_hash = 2166136261u;
// ... and the same writes kept in order, for the runs that compare where two differ
typedef struct { uint32_t kind, a, b; } hw_ev_t;
#define HW_LOG_MAX	(1u << 17)
static hw_ev_t hw_log[HW_LOG_MAX];
static uint32_t hw_n;

// The kinds the receive comparisons look at: the shared DAC's writes, and the modem's held reference as the board
// gives it at every receive entry (the level's offset, 4:0x04)
#define HW_DAC		5u
#define HW_REF		9u

static void hw(uint32_t kind, uint32_t a, uint32_t b)
{
	const uint32_t w[3] = { kind, a, b };
	if (hw_n < HW_LOG_MAX) {
		hw_log[hw_n] = (hw_ev_t){ kind, a, b };
	}
	hw_n++;
	for (unsigned i = 0; i < 3; i++) {
		for (unsigned k = 0; k < 4; k++) {
			hw_hash = (hw_hash ^ ((w[i] >> (8u * k)) & 0xFFu)) * 16777619u;
		}
	}
}

// The IF receiver's frames as its pins and its select carry them: the data line at each rising clock while the
// select (an expander bit) is low. A frame of 16 clocks with the data line driven throughout is a write; a read
// turns the line around and is not kept
static struct {
	bool sel, data, read;
	uint32_t word;
	unsigned bits;
} akd;
#define AK_LOG_MAX	64u
static uint16_t ak_log[AK_LOG_MAX];
static unsigned ak_n;		// the writes since the test last cleared it

static void ak_pin(GPIO_TypeDef *port, uint16_t pins, bool level)
{
	if (port == PIN_PORT(PIN_AK_SDATA) && pins == PIN_MASK(PIN_AK_SDATA)) {
		akd.data = level;
	} else if (port == PIN_PORT(PIN_AK_SCLK) && pins == PIN_MASK(PIN_AK_SCLK) && level && akd.sel) {
		akd.word = akd.word << 1 | (akd.data ? 1u : 0u);
		akd.bits++;
	}
}

static void ak_select(uint32_t set, uint32_t clear)
{
	if ((clear & EXP_AK_CSN) != 0u) {
		akd.sel = true;
		akd.read = false;
		akd.word = 0;
		akd.bits = 0;
	} else if ((set & EXP_AK_CSN) != 0u && akd.sel) {
		akd.sel = false;
		if (!akd.read && akd.bits == AK2365A_WORD_BITS && ak_n < AK_LOG_MAX) {
			ak_log[ak_n++] = (uint16_t)akd.word;
		}
	}
}

void stm32_gpio_clock(GPIO_TypeDef *port) { (void)port; }
void stm32_pin_force(GPIO_TypeDef *port, uint16_t pins, bool level) { (void)port; (void)pins; (void)level; }
void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool od)
{
	hw(1, (uint32_t)(port - fake_ports) << 16 | pins, (uint32_t)level << 1 | od);
	ak_pin(port, pins, level);
}
void stm32_pin_in(GPIO_TypeDef *port, uint16_t pins, uint32_t pull)
{
	hw(2, (uint32_t)(port - fake_ports) << 16 | pins, pull);
	if (port == PIN_PORT(PIN_AK_SDATA) && pins == PIN_MASK(PIN_AK_SDATA) && akd.sel) {
		akd.read = true;
	}
}
void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool level)
{
	hw(3, (uint32_t)(port - fake_ports) << 16 | pins, level);
	ak_pin(port, pins, level);
}
void stm32_wait_ns(uint32_t ns) { now_us += ns / 1000u; }

bool stm32_pin_read(GPIO_TypeDef *port, uint16_t pin)
{
	return port == GPIOD && pin == GPIO_PIN_11 ? pll_ld : false;
}

uint32_t hal_millis(void) { return now_us / 1000u; }
uint32_t hal_micros(void) { return now_us; }
void hal_delay_us(uint32_t us) { now_us += us; }
void hal_delay_ms(uint32_t ms) { now_us += ms * 1000u; }

BaseType_t xTaskGetSchedulerState(void) { return taskSCHEDULER_NOT_STARTED; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int m; return &m; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) { (void)m; (void)wait; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { (void)m; return pdTRUE; }

// The storage layer's locks: one task here
void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

// The log ring: the last line kept
static char log_last[96];

#if CONFIG_DEBUGLINK
void dlog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(log_last, sizeof(log_last), fmt, ap);
	va_end(ap);
}
#endif

const board_t board = {
	.battery_critical_mv = 6000,
	.flash = &sim_flash,
#if CONFIG_FACTORY_COPY
	.factory_copy = &cs7000p_factory_copy,
#endif
};

// --- The expander and the DAC, with the interlock of exp_cs7000p.h ---

static uint32_t shadow = EXP_RESET_WORD;
static uint16_t dac_now;
static int mic_writes;			// writes that moved a microphone bit

// As the real writer: the mask first, then the check and the word
bool cs7000p_exp_set(uint32_t set, uint32_t clear)
{
	hw(4, set, clear);
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	bool ok = exp_set_ok(set, dac_now, CS7000P_APC_FLOOR);
	if (ok) {
		uint32_t after = (shadow & ~clear) | set;
		if (!exp_dac_ok(after, dac_now, CS7000P_APC_FLOOR)) {
			dac_now = 0;
		}
		if (((after ^ shadow) & MIC_BITS) != 0u) {
			mic_writes++;
		}
		shadow = after;
		ak_select(set, clear);
	}
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	return ok;
}

uint32_t cs7000p_exp_get(void) { return shadow; }

bool cs7000p_apc_dac(uint16_t v)
{
	hw(HW_DAC, v, 0);
	if (v > CS7000P_DAC_MAX || (v != 0u && !exp_dac_ok(shadow, v, CS7000P_APC_FLOOR))) {
		return false;
	}
	dac_now = v;
	return true;
}

uint16_t cs7000p_apc_dac_get(void) { return dac_now; }

// The radio's other side of board_rf_safe: the lines first, then the sequencer told
static void rf_safe_now(void)
{
	dac_now = 0;
	shadow = exp_rf_safe_word(shadow);
	cs7000p_radio_rf_safe();
}

// board_rf_safe() itself (exp_cs7000p.c): the transmit arm's lowering ends a transmission it no longer covers by it
static int board_safes;
void board_rf_safe(void)
{
	board_safes++;
	rf_safe_now();
}

// --- The power side: the supply reading and the off state ---

static bool off_state;

bool cs7000p_power_off_state(void) { return off_state; }

// latch(false) with the knob on, as power_cs7000p.c does it: the off state first, then the safe request, then the
// amplifier and the LEDs cleared
static void latch_off(void)
{
	off_state = true;
	rf_safe_now();
	shadow &= ~(EXP_AMP | EXP_LED_GREEN | EXP_LED_RED);
}

// The interrupt mask (stub/FreeRTOS.h, CS7000P_STUB_MASK_HOOK). Taking it is the last moment another task can run:
// with cut_in set, the start task's latch(false) runs there, once, as it would preempt this code on the radio
static bool cut_in;

void fake_mask_enter(void)
{
	if (cut_in && fake_irq_depth == 0) {
		cut_in = false;
		latch_off();
	}
	fake_irq_depth++;
}

uint16_t cs7000p_adc_mv(uint8_t rank)
{
	return rank == CS7000P_ADC_POW_DET ? 1910u : 500u;	// 7.4 V by the model; the rest a placeholder
}

// The RSSI's 12-bit reading: code 110 (raw >> 4), -95 dBm on the invented table's two points
static uint16_t rssi_raw = 110u << 4;

uint16_t cs7000p_adc_raw(uint8_t rank) { return rank == CS7000P_ADC_RSSI ? rssi_raw : 0u; }

// --- The modem ---

static struct {
	bool dmr, fm_tx, audio;
	int retunes, fm_tx_ends;
	bool refuse_fm_tx;
} modem;

// The held reference as the real IF host asks for it at a receive entry (c6000_cs7000p.c's ref_offset and
// rx_tx_offset): the level's offset, then 4:0x04
static void ref_entry(void)
{
	hw(HW_REF, (uint32_t)(int32_t)cs7000p_rf_rx_ref(&cs7000p_rf), (uint8_t)cs7000p_rf.ref_offset);
}

// A run with no table at all, as before the board read one (the radio's own pointer cleared once set)
static bool null_maker;

bool cs7000p_c6000_init(void)
{
	if (null_maker) {
		cs7000p_rf.maker = NULL;
	}
	ref_entry();
	return true;
}
void cs7000p_c6000_mode(bool dmr, uint32_t hz) { hw(6, dmr, hz); modem.dmr = dmr; ref_entry(); }
// What runs on the debug link's task while the radio task waits inside a receive set-up (NULL: nothing)
static void (*mid_setup)(void);

void cs7000p_c6000_retune(uint32_t hz)
{
	hw(7, hz, 0);
	modem.retunes++;
	ref_entry();
	if (mid_setup != NULL) {
		mid_setup();
	}
}
void cs7000p_c6000_rx_hold(void) { hw(8, 0, 0); }
bool cs7000p_c6000_fm_rx(void) { return !modem.dmr && !modem.fm_tx; }
void cs7000p_c6000_fm_audio(bool on) { modem.audio = on; }
bool cs7000p_c6000_reg(uint8_t page, uint8_t reg, uint32_t *val, bool write)
{
	(void)page; (void)reg; (void)val; (void)write;
	return false;
}

bool cs7000p_c6000_fm_tx(uint32_t hz, bool wide)
{
	(void)hz;
	(void)wide;
	if (modem.refuse_fm_tx) {
		return false;
	}
	modem.fm_tx = true;
	return true;
}

void cs7000p_c6000_fm_tx_end(void)
{
	modem.fm_tx = false;
	modem.fm_tx_ends++;
	ref_entry();
}

void cs7000p_c6000_state(bool *up, uint8_t *r10, uint8_t *r25)
{
	*up = true;
	*r10 = 0;
	*r25 = 0;
}

// --- Sub-audio (subtone_cs7000p.c) ---

void cs7000p_subtone_init(void) {}
void cs7000p_subtone_rx_set(const subtone_t *t) { (void)t; }
void cs7000p_subtone_rx_run(bool on) { (void)on; }
bool cs7000p_subtone_rx_match(void) { return true; }
void cs7000p_subtone_tx_start(const subtone_t *t) { (void)t; }
void cs7000p_subtone_tx_stop(void) {}
bool cs7000p_subtone_tx_tail(bool on) { (void)on; return false; }
uint16_t cs7000p_subtone_pc8(void) { return 0; }
bool cs7000p_subtone_pc8_set(uint16_t duty) { (void)duty; return false; }

// --- The burst runner (platform/stm32/dmr_keyup) and the modem's DMR glue ---

static struct {
	bool answer;		// what the next arm says
	bool plan;		// ... once the board's plan has said yes, as the real runner asks it
	bool armed;
	int arms, disarms;
} runner;
static const dmr_keyup_ops_t *runner_ops;

void dmr_keyup_init(const dmr_keyup_ops_t *ops) { runner_ops = ops; }
void dmr_keyup_keying(hal_dmr_keying_t *k) { (void)k; }

bool dmr_keyup_arm(uint32_t hz, uint8_t power)
{
	pa_keyup_t ku;
	bool ok = runner.answer;
	runner.arms++;
	if (ok && runner.plan) {
		ok = runner_ops != NULL && runner_ops->plan(runner_ops->ctx, hz, power, &ku);
	}
	runner.armed = ok;	// a refusal drops an earlier arm too
	return ok;
}

void dmr_keyup_disarm(void)
{
	runner.disarms++;
	runner.armed = false;
}

bool stm32_hrc_dmr_tx_end(void) { return true; }

// --- The checks ---

static bool mic_on(void)
{
	return (shadow & MIC_BITS) == MIC_BITS;
}

static bool mic_off(void)
{
	return (shadow & (MIC_BITS | EXP_MIC_EXT)) == 0u;
}

static void start(void)
{
	shadow = EXP_RESET_WORD;
	dac_now = 0;
	off_state = false;
	cut_in = false;
	memset(&modem, 0, sizeof(modem));
	memset(&runner, 0, sizeof(runner));
	cs7000p_radio.init();
	cs7000p_radio.set_mode(RADIO_MODE_FM);
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_25K, NULL));
	hal_delay_ms(CS7000P_REF_SETTLE_MS + 1u);
	CHECK(mic_off() && cs7000p_rf.seq.state == CHAINSEQ_ST_RX);
	// The bench harness's test TX limit, a list that names the channel: the only way to key without calibration
	static const uint32_t limit[1] = { 446u * MHZ };
	CHECK(tx_limit_set(false, limit, 1));
}

static void test_fm_mic(void)
{
	start();
	// On the bench drive: keyed, with the microphone, and off with the transmission
	CHECK(cs7000p_radio.tx_drive(600) && cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL));
	CHECK(cs7000p_rf.seq.state == CHAINSEQ_ST_TX && mic_on() && modem.fm_tx);
	cs7000p_radio.tx_off();
	CHECK(mic_off() && cs7000p_rf.seq.state == CHAINSEQ_ST_RX && !modem.fm_tx);
	// Refused before anything is written (no drive armed, no calibration): the microphone never came on
	int writes = mic_writes;
	CHECK(!cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL) && mic_off() && mic_writes == writes);
	// The modem refuses its set-up
	modem.refuse_fm_tx = true;
	CHECK(cs7000p_radio.tx_drive(600) && !cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL) && mic_off());
	modem.refuse_fm_tx = false;
	// The synthesizer does not lock on the carrier: the key-up fails after the microphone came on, and takes it off
	pll_ld = false;
	CHECK(cs7000p_radio.tx_drive(600) && !cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL) && mic_off());
	pll_ld = true;
	// A retune while keyed (the core's rx() ends it), a mode change while keyed
	CHECK(cs7000p_radio.tx_drive(600) && cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL) && mic_on());
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_25K, NULL) && mic_off());
	CHECK(cs7000p_radio.tx_drive(600) && cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL) && mic_on());
	cs7000p_radio.set_mode(RADIO_MODE_FM);
	CHECK(mic_off() && !modem.fm_tx);
	CHECK(fake_irq_depth == 0);
}

static void test_dmr_mic(void)
{
	start();
	cs7000p_radio.set_mode(RADIO_MODE_DMR);
	// The arm brings the microphone up, as FM's key-up does; each end takes it away
	runner.answer = true;
	CHECK(cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL) && mic_on());
	cs7000p_radio.tx_off();
	CHECK(mic_off() && runner.disarms == 1);
	CHECK(cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL) && mic_on());
	cs7000p_radio_dmr_tx_end();	// the glue's end after the last burst
	CHECK(mic_off());
	// Refused by the runner: never on. Refused behind an earlier arm: that arm's microphone goes too
	runner.answer = false;
	int writes = mic_writes;
	CHECK(!cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL) && mic_off() && mic_writes == writes);
	runner.answer = true;
	CHECK(cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL) && mic_on());
	runner.answer = false;
	CHECK(!cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL) && mic_off());
	// The hotspot's retune ends an armed transmission, in band and out of it; a mode change does
	runner.answer = true;
	CHECK(cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL) && mic_on());
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_12K5, NULL) && mic_off());
	CHECK(cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL) && mic_on());
	CHECK(!cs7000p_radio.rx(600u * MHZ, RADIO_BW_12K5, NULL) && mic_off());
	CHECK(cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL) && mic_on());
	// A safe request while armed (a fault path's board_rf_safe): the restore waits for the transmission's end, and
	// that end takes the microphone away
	radio_status_t st;
	rf_safe_now();
	cs7000p_radio.status(&st);
	CHECK(mic_on() && cs7000p_rf.restore);
	cs7000p_radio.tx_off();
	cs7000p_radio.status(&st);
	CHECK(mic_off() && !cs7000p_rf.restore);
	CHECK(cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL) && mic_on());
	cs7000p_radio.set_mode(RADIO_MODE_FM);
	CHECK(mic_off() && !runner.armed);
	// The external microphone is never switched in
	CHECK((shadow & EXP_MIC_EXT) == 0u);
}

static void test_off_state(void)
{
	radio_status_t st;
	start();
	cs7000p_radio.audio(true);		// the squelch open: the amplifier on
	CHECK((shadow & EXP_AMP) != 0u && modem.audio);

	// latch(false) with the knob on: the off state, then the safe request, the amplifier and LEDs cleared
	latch_off();
	int retunes = modem.retunes;
	for (int i = 0; i < 20; i++) {
		cs7000p_radio.status(&st);
	}
	// No receiver set-up, and the request is kept for the end of the off state
	CHECK(modem.retunes == retunes && cs7000p_rf.restore && cs7000p_rf.seq.state != CHAINSEQ_ST_RX);
	// The squelch and a beep go on asking for the speaker: it stays off
	cs7000p_radio.audio(true);
	cs7000p_speaker_pcm(true);
	CHECK((shadow & EXP_AMP) == 0u && !modem.audio);
	cs7000p_speaker_pcm(false);
	CHECK((shadow & EXP_AMP) == 0u);
	// Nothing transmits there, and the reason says so: FM's key-up and the burst plan alike, with a pack that
	// reads well and a bench drive armed. No microphone, nothing keyed, no receiver set-up behind the refusal
	int writes = mic_writes;
	CHECK(cs7000p_radio.tx_drive(600) && !cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL));
	CHECK(cs7000p_rf.tx_refused_why == CS7000P_TX_OFF && mic_off() && mic_writes == writes && !modem.fm_tx);
	cs7000p_radio.set_mode(RADIO_MODE_DMR);
	runner.answer = runner.plan = true;
	cs7000p_rf.tx_refused_why = 0;
	CHECK(!cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL) && !runner.armed && runner.arms == 1);
	CHECK(cs7000p_rf.tx_refused_why == CS7000P_TX_OFF && mic_off() && mic_writes == writes);
	cs7000p_radio.set_mode(RADIO_MODE_FM);
	CHECK(modem.retunes == retunes && cs7000p_rf.restore && cs7000p_rf.seq.state != CHAINSEQ_ST_RX);
	CHECK((shadow & EXP_TX_BITS) == 0u && dac_now == 0);

	// latch(true): the next poll sets the receiver up again and the speaker as the core wants it
	off_state = false;
	cs7000p_radio.status(&st);
	CHECK(modem.retunes == retunes + 1 && !cs7000p_rf.restore && cs7000p_rf.seq.state == CHAINSEQ_ST_RX);
	CHECK((shadow & EXP_AMP) != 0u && modem.audio && mic_off());
	cs7000p_radio.status(&st);
	CHECK(modem.retunes == retunes + 1);	// once
	// Outside the off state a safe request is taken at the next poll, as before
	rf_safe_now();
	cs7000p_radio.status(&st);
	CHECK(modem.retunes == retunes + 2 && cs7000p_rf.seq.state == CHAINSEQ_ST_RX);
	// And a transmission goes ahead again, on the bench drive that was armed in the off state. The burst plan is
	// asked and answers with its own reason (this build has no CS7000P_BENCH_DMR_TX)
	CHECK(cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL) && mic_on() && cs7000p_rf.seq.state == CHAINSEQ_ST_TX);
	cs7000p_radio.tx_off();
	CHECK(mic_off() && cs7000p_rf.seq.state == CHAINSEQ_ST_RX);
	cs7000p_radio.set_mode(RADIO_MODE_DMR);
	CHECK(cs7000p_radio.tx_drive(600) && !cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL));
	CHECK(cs7000p_rf.tx_refused_why == CS7000P_TX_OVR_DMR && mic_off());
	cs7000p_radio.set_mode(RADIO_MODE_FM);

	// latch(false) from the task above, cutting into a speaker call at the last moment it can (the call has looked
	// at the off state and is about to write): the amplifier is not left on. The squelch's call, then a beep's
	for (int beep = 0; beep < 2; beep++) {
		start();
		cut_in = true;
		if (beep != 0) {
			cs7000p_speaker_pcm(true);
		} else {
			cs7000p_radio.audio(true);
		}
		CHECK(!cut_in && off_state && (shadow & EXP_AMP) == 0u && fake_irq_depth == 0);
		cs7000p_speaker_pcm(false);
		cs7000p_radio.audio(false);
	}
}

// --- The maker's factory table ---

#define WIN	CS7000P_STORE_TABLE_ADDR
#define REC	CS7000P_STORE_CAL_ADDR
#define N	CS7000P_MAKER_SIZE

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

// An invented table that passes every rule of the reader (the layout of calibration-cs7000p.md 3; the values made up,
// as cs7000pMakerCalTest's). other: a byte the maker never reads, so two radios' tables differ and both are valid
static void invented(uint8_t t[N], uint8_t other)
{
	static const uint32_t tx[8] = { 400200000u, 429800000u, 430200000u, 445000000u, 458000000u, 472000000u,
					492000000u, 511800000u };
	static const uint32_t rx[8] = { 400500000u, 420000000u, 440000000u, 460000000u, 478950000u, 479050000u,
					495000000u, 511500000u };
	static const uint8_t high[8] = { 190, 194, 195, 210, 213, 215, 212, 209 };
	static const uint8_t low[8] = { 100, 99, 99, 108, 115, 122, 126, 127 };
	static const uint8_t tune[8] = { 120, 124, 128, 132, 136, 140, 150, 160 };
	memset(t, 0, N);
	for (unsigned i = 0; i < 8; i++) {
		put32be(t, 0x000u + 4u * i, tx[i]);
		put32be(t, 0x020u + 4u * i, rx[i]);
	}
	t[0x040] = 60;
	t[0x041] = 160;
	memcpy(t + 0x044, tune, 8);
	memset(t + 0x04C, 40, 8);	// squelch level 9 open, close; level 1 open, close
	memset(t + 0x054, 50, 8);
	memset(t + 0x05C, 120, 8);
	memset(t + 0x064, 130, 8);
	memcpy(t + 0x06C, high, 8);
	memcpy(t + 0x074, low, 8);
	memset(t + 0x084, 40, 8);	// CTCSS at 67.0 and 254.1 Hz, DCS
	memset(t + 0x08C, 52, 8);
	memset(t + 0x094, 200, 8);
	memset(t + 0x0B4, 80, 8);	// the DMR and FM pairs
	memset(t + 0x0BC, 100, 8);
	memset(t + 0x0C4, 120, 8);
	memset(t + 0x0CC, 135, 8);
	t[0x0DC] = 0x7B;		// trim A
	t[0x0DD] = 0xA0;		// the battery byte
	memset(t + 0x0EC, 20, 8);	// high power bursts: advance, S, R, H, F
	memset(t + 0x0F4, 12, 8);
	memset(t + 0x0FC, 19, 8);
	fill16(t, 0x104, 300);
	fill16(t, 0x114, 310);
	memset(t + 0x124, 21, 8);	// low power
	memset(t + 0x12C, 12, 8);
	memset(t + 0x134, 20, 8);
	fill16(t, 0x13C, 301);
	fill16(t, 0x14C, 311);
	memset(t + 0x15C, 61, 8);
	memset(t + 0x164, 159, 8);
	t[0x174] = 0x7D;		// trim B
	t[0x1FF] = other;		// never read
}

static uint8_t table_a[N], table_b[N];
static char tline[128], rline[128];

// A fresh chip: the window holds w (NULL: blank), the board sector a record of the factory copy's for r (NULL: blank)
static void chip(const uint8_t *w, const uint8_t *r)
{
	sim_flash_create(CS7000P_STORE_FULL_SIZE, &cs7000p_flash_map);
	if (w != NULL) {
		memcpy(&sim_flash_data[WIN], w, N);
	}
#if CONFIG_FACTORY_COPY
	if (r != NULL) {
		static uint8_t rec[FACTORY_COPY_REC_LEN(N)];
		factory_copy_record_build(rec, WIN, r, N);
		memcpy(&sim_flash_data[REC], rec, sizeof(rec));
	}
#else
	(void)r;
#endif
}

// A start as app.c runs one: the storage layer (claimed: the owner has claimed the region by now), the factory
// copy's load, then the radio. misread: an address whose second read from here on comes back with a bit flipped
static void boot(bool claimed, uint32_t misread)
{
	sim_flash_power_on();
	(void)storage_init();
	if (claimed && storage_region() == STORAGE_REGION_FOREIGN) {
		CHECK(storage_claim());
	}
	if (misread != 0u) {
		sim_flash_misread(misread, 2);
	}
#if CONFIG_FACTORY_COPY
	factory_copy_load(&cs7000p_factory_copy);
#endif
	start();
	sim_flash_misread(0, 0);
	cs7000p_cal_table_line(tline, sizeof(tline));
	cs7000p_cal_rec_line(rline, sizeof(rline));
}

static bool line_is(const char *got, const char *fmt, ...)
{
	char want[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(want, sizeof(want), fmt, ap);
	va_end(ap);
	if (strcmp(got, want) != 0) {
		printf("  got  %s\n  want %s\n", got, want);
		return false;
	}
	return true;
}

static bool sector_blank(uint32_t a)
{
	for (uint32_t i = 0; i < CS7000P_STORE_SECTOR_SIZE; i++) {
		if (sim_flash_data[a + i] != 0xFFu) {
			return false;
		}
	}
	return true;
}

static uint32_t crc(const uint8_t *b)
{
	return storage_crc32(0, b, N);
}

static uint16_t fields_of(const uint8_t *b)
{
	cs7000p_maker_t m;
	CHECK(cs7000p_maker_parse(b, crc(b), CS7000P_APC_FLOOR, &m));
	return m.fields;
}

static void test_table_sources(void)
{
	uint8_t blank[N], zero[N], damaged[N], use[N];
	invented(table_a, 0x5A);
	invented(table_b, 0xA5);
	memset(blank, 0xFF, N);
	memset(zero, 0, N);
	memcpy(damaged, table_a, N);
	damaged[0x046] = 0;		// a tune code of 0: an integrity rule
	memcpy(use, table_a, N);
	use[0x0DD] = 0;			// the battery byte at 0: a use rule only
	CHECK(fields_of(table_a) == CS7000P_MK_F_ALL && fields_of(use) == (CS7000P_MK_F_ALL & ~CS7000P_MK_F_BATTERY));
	const uint16_t all = CS7000P_MK_F_ALL;

	// A blank window and no record: none, nothing written. The window's CRC is the one a backup's blank 512 bytes give
	chip(NULL, NULL);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=blank src=none crc=0x%08x why=blank off=0x000 fields=0x0000 bat=0x00", crc(blank)));
	CHECK(strstr(log_last, "cal: maker table blank (blank)") == log_last);
	// A zero window
	chip(zero, NULL);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=zero src=none crc=0x%08x why=zero off=0x000 fields=0x0000 bat=0x00", crc(zero)));
	// A damaged one: the rule and the byte
	chip(damaged, NULL);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=refused src=none crc=0x%08x why=code off=0x046 fields=0x0000 bat=0x00",
		      crc(damaged)));
	CHECK(strstr(log_last, "cal: maker table refused (code) at 0x046: none used") == log_last);
	// An unstable one: two reads that differ
	chip(table_a, NULL);
	boot(true, WIN + 0x40u);
	CHECK(line_is(tline, "cal_table=unread src=none crc=0x00000000 why=unstable off=0x000 fields=0x0000 bat=0x00"));
#if CONFIG_FACTORY_COPY
	CHECK(line_is(rline, "cal_rec=blank crc=- wrote=0 restore=none"));
	CHECK(sector_blank(REC));	// a window not valid is never copied

	// Each of them with a valid record: the record is the source, the window's state and reason beside it, and the
	// window is not written
	chip(NULL, table_a);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=blank src=record crc=0x%08x why=blank off=0x000 fields=0x%04x bat=0xa0",
		      crc(blank), all));
	CHECK(line_is(rline, "cal_rec=ok crc=0x%08x wrote=0 restore=none", crc(table_a)));
	CHECK(strstr(log_last, "cal: maker table ok src=record crc=") == log_last && strstr(log_last, "win=blank") != NULL);
	CHECK(memcmp(&sim_flash_data[WIN], blank, N) == 0);
	chip(zero, table_a);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=zero src=record crc=0x%08x why=zero off=0x000 fields=0x%04x bat=0xa0", crc(zero),
		      all));
	chip(damaged, table_a);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=refused src=record crc=0x%08x why=code off=0x046 fields=0x%04x bat=0xa0",
		      crc(damaged), all));
	chip(table_a, table_a);
	boot(true, WIN + 0x40u);
	CHECK(line_is(tline, "cal_table=unread src=record crc=0x00000000 why=unstable off=0x000 fields=0x%04x bat=0xa0",
		      all));

	// A valid window: in use. Its copy is written only in a region of ours: not before the claim, then once
	chip(table_a, NULL);
	boot(false, 0);
	CHECK(storage_region() == STORAGE_REGION_FOREIGN);
	CHECK(line_is(tline, "cal_table=ok src=window crc=0x%08x why=none off=0x000 fields=0x%04x bat=0xa0", crc(table_a),
		      all));
	CHECK(line_is(rline, "cal_rec=blank crc=- wrote=0 restore=none") && sector_blank(REC));
	CHECK(line_is(log_last, "cal: maker table ok src=window crc=0x%08x rec=blank fields=0x%04x", crc(table_a), all));
	boot(true, 0);
	CHECK(line_is(rline, "cal_rec=ok crc=0x%08x wrote=1 restore=none", crc(table_a)));
	CHECK(memcmp(&sim_flash_data[REC + FACTORY_COPY_REC_HDR], table_a, N) == 0);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=ok src=window crc=0x%08x why=none off=0x000 fields=0x%04x bat=0xa0", crc(table_a),
		      all));
	CHECK(line_is(rline, "cal_rec=ok crc=0x%08x wrote=0 restore=none", crc(table_a)));

	// Window and record both valid and different (another radio's table in one of them): neither is used
	chip(table_a, table_b);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=ok src=none crc=0x%08x why=conflict off=0x000 fields=0x0000 bat=0x00",
		      crc(table_a)));
	CHECK(line_is(rline, "cal_rec=ok crc=0x%08x wrote=0 restore=none", crc(table_b)));
	CHECK(line_is(log_last, "cal: window and record differ: none used"));
	CHECK(memcmp(&sim_flash_data[WIN], table_a, N) == 0);

	// Receive points out of order in the window, a valid record: the record, the window's reason beside it
	uint8_t order[N];
	memcpy(order, table_a, N);
	memcpy(order + 0x024, table_a + 0x028, 4);
	memcpy(order + 0x028, table_a + 0x024, 4);
	chip(order, table_a);
	boot(true, 0);
	CHECK(strstr(tline, "cal_table=refused src=record ") == tline && strstr(tline, " why=point_order ") != NULL);
	CHECK(memcmp(&sim_flash_data[WIN], order, N) == 0);

	// Someone else's bytes in the board sector: the valid window in use, the sector shown and never overwritten
	chip(table_a, NULL);
	memcpy(&sim_flash_data[REC + 0x100u], "not ours", 8);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=ok src=window crc=0x%08x why=none off=0x000 fields=0x%04x bat=0xa0", crc(table_a),
		      all));
	CHECK(line_is(rline, "cal_rec=other crc=- wrote=0 restore=none"));
	CHECK(memcmp(&sim_flash_data[REC + 0x100u], "not ours", 8) == 0 && sim_flash_data[REC] == 0xFFu);
	// A record of ours whose own CRC fails, beside a blank window: not a source; its bytes' CRC shown
	chip(NULL, table_a);
	sim_flash_data[REC + FACTORY_COPY_REC_HDR + 0x10u] ^= 1u;
	boot(true, 0);
	uint8_t flipped[N];
	memcpy(flipped, table_a, N);
	flipped[0x10] ^= 1u;
	CHECK(line_is(tline, "cal_table=blank src=none crc=0x%08x why=blank off=0x000 fields=0x0000 bat=0x00", crc(blank)));
	CHECK(line_is(rline, "cal_rec=bad crc=0x%08x wrote=0 restore=none", crc(flipped)));
	CHECK(strstr(log_last, "cal: maker table blank (blank) at 0x000: none used rec=bad") == log_last);

	// A table failing a use rule alone is valid: in use, copied, its group dropped
	chip(use, NULL);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=ok src=window crc=0x%08x why=none off=0x000 fields=0x%04x bat=0x00", crc(use),
		      all & ~CS7000P_MK_F_BATTERY));
	CHECK(line_is(rline, "cal_rec=ok crc=0x%08x wrote=1 restore=none", crc(use)));
#else
	CHECK(line_is(rline, "cal_rec=off crc=- wrote=0 restore=none"));
	// Without the copy: the window alone, never a record
	chip(table_a, table_b);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=ok src=window crc=0x%08x why=none off=0x000 fields=0x%04x bat=0xa0", crc(table_a),
		      all));
	CHECK(line_is(log_last, "cal: maker table ok src=window crc=0x%08x rec=off fields=0x%04x", crc(table_a), all));
	chip(use, NULL);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=ok src=window crc=0x%08x why=none off=0x000 fields=0x%04x bat=0x00", crc(use),
		      all & ~CS7000P_MK_F_BATTERY));
	CHECK(sector_blank(REC));
	chip(NULL, NULL);
	boot(true, 0);
	CHECK(line_is(tline, "cal_table=blank src=none crc=0x%08x why=blank off=0x000 fields=0x0000 bat=0x00", crc(blank)));
	// A chip that doesn't answer
	sim_flash_bad_read(WIN, N);
	boot(true, 0);
	sim_flash_bad_read(0, 0);
	CHECK(line_is(tline, "cal_table=unread src=none crc=0x00000000 why=read off=0x000 fields=0x0000 bat=0x00"));
#endif
}

// The held level's offset the modem was given at its last receive entry since hw_n was cleared (INT32_MIN: none)
static int32_t last_ref(void)
{
	for (uint32_t i = hw_n < HW_LOG_MAX ? hw_n : HW_LOG_MAX; i-- > 0u;) {
		if (hw_log[i].kind == HW_REF) {
			return (int32_t)hw_log[i].a;
		}
	}
	return INT32_MIN;
}

#if CONFIG_FACTORY_COPY

// A restore's changed() only raises the request; the table in use changes at the radio task's status poll, never
// under a transmission, and a receive set-up follows. busy() holds from the request until it is taken
static void test_table_request(void)
{
	radio_status_t st;
	chip(NULL, NULL);
	boot(true, 0);
	CHECK(strstr(tline, "cal_table=blank src=none") == tline);
	CHECK(!cs7000p_factory_copy.busy() && cs7000p_rf.tune_src == CS7000P_SRC_DEFAULT);

	// The copy's state moves on as a restore's job leaves it (the window written, the record after it)
	memcpy(&sim_flash_data[WIN], table_a, N);
	factory_copy_load(&cs7000p_factory_copy);
	cs7000p_factory_copy.changed();
	CHECK(cs7000p_factory_copy.busy());
	cs7000p_cal_table_line(tline, sizeof(tline));
	CHECK(strstr(tline, "cal_table=blank src=none") == tline);	// nothing taken off the radio task

	// Not under a transmission: kept until it ends
	CHECK(cs7000p_radio.tx_drive(600) && cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL));
	cs7000p_radio.status(&st);
	cs7000p_cal_table_line(tline, sizeof(tline));
	CHECK(strstr(tline, "cal_table=blank src=none") == tline && cs7000p_factory_copy.busy());
	cs7000p_radio.tx_off();
	int retunes = modem.retunes;
	cs7000p_radio.status(&st);
	cs7000p_cal_table_line(tline, sizeof(tline));
	cs7000p_cal_rec_line(rline, sizeof(rline));
	CHECK(line_is(tline, "cal_table=ok src=window crc=0x%08x why=none off=0x000 fields=0x%04x bat=0xa0",
		      crc(table_a), CS7000P_MK_F_ALL));
	CHECK(line_is(rline, "cal_rec=ok crc=0x%08x wrote=1 restore=none", crc(table_a)));
	CHECK(!cs7000p_factory_copy.busy());
	CHECK(modem.retunes == retunes + 1 && cs7000p_rf.seq.state == CHAINSEQ_ST_RX);	// the receive set-up after it
	CHECK(cs7000p_rf.tune_src == CS7000P_SRC_MAKER && cs7000p_rf.rx_ref == -5);	// ... on the table now in use
	cs7000p_radio.status(&st);
	CHECK(modem.retunes == retunes + 1);	// once
	// busy() while a transmission is up, DMR's arm included
	CHECK(cs7000p_radio.tx_drive(600) && cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL));
	CHECK(cs7000p_factory_copy.busy());
	cs7000p_radio.tx_off();
	CHECK(!cs7000p_factory_copy.busy());
	cs7000p_radio.set_mode(RADIO_MODE_DMR);
	runner.answer = true;
	CHECK(cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL) && cs7000p_factory_copy.busy());
	cs7000p_radio.tx_off();
	CHECK(!cs7000p_factory_copy.busy());
	cs7000p_radio.set_mode(RADIO_MODE_FM);
}

// A table restored while the FM receiver sleeps: the poll takes it, but sets nothing up; the wake gives the modem the
// new level, and the poll after the wake the new tune, so receive is on the restored table from then on
static void test_table_request_asleep(void)
{
	radio_status_t st;
	chip(NULL, NULL);
	boot(true, 0);
	cs7000p_radio.rx_sleep(true);
	memcpy(&sim_flash_data[WIN], table_a, N);
	factory_copy_load(&cs7000p_factory_copy);
	cs7000p_factory_copy.changed();
	int retunes = modem.retunes;
	cs7000p_radio.status(&st);
	cs7000p_cal_table_line(tline, sizeof(tline));
	CHECK(strstr(tline, "cal_table=ok src=window") == tline && !cs7000p_factory_copy.busy());
	CHECK(modem.retunes == retunes && cs7000p_rf.tune_src == CS7000P_SRC_DEFAULT);	// asleep: no set-up yet
	CHECK(cs7000p_rf.seq.state == CHAINSEQ_ST_SLEEP && st.rssi_dbm == -95);
	hw_n = 0;
	cs7000p_radio.rx_sleep(false);
	CHECK(modem.retunes == retunes + 1 && last_ref() == -5 && cs7000p_rf.ref_src == CS7000P_SRC_MAKER);
	cs7000p_radio.status(&st);
	cs7000p_maker_t m;
	uint16_t tune = 0;
	CHECK(cs7000p_maker_parse(table_a, crc(table_a), CS7000P_APC_FLOOR, &m) &&
	      cs7000p_maker_rx_tune(&m, 446u * MHZ, &tune, NULL));
	CHECK(cs7000p_rf.tune_src == CS7000P_SRC_MAKER && cs7000p_rf.rx_tune == tune && dac_now == tune);
	CHECK(cs7000p_rf.rx_ref == -5 && cs7000p_rf.seq.state == CHAINSEQ_ST_RX);
}
#endif

#if CONFIG_DEBUGLINK && CONFIG_FACTORY_COPY
// board2's lines while the radio task takes a restored table and sets receive up on it (the debug link's task runs
// while the set-up waits): none shows a mix of the two tables. Lines asked in the middle of the change wait for its end
// and say busy when it never comes (here it can't: one task); a reply's lines are all of the state its first one
// copied, a change between them included; the next reply's are the new state's, whole
static char mid_lines[CS7000P_CAL_VIEW_LINES][128];
static uint32_t mid_waited_us;

static void view_mid_setup(void)
{
	const uint32_t t0 = now_us;
	for (uint8_t k = 0; k < CS7000P_CAL_VIEW_LINES; k++) {
		cs7000p_cal_view_line(k, mid_lines[k], sizeof(mid_lines[k]));
	}
	mid_waited_us = now_us - t0;
}

static void test_view_whole(void)
{
	static const char *const keys[CS7000P_CAL_VIEW_LINES] = {
		"cal_table", "cal_rec", "ref", "rx_cal", "sq_maker", "tx_arm",
	};
	radio_status_t st;
	char line[128], want[32];
	chip(NULL, NULL);
	boot(true, 0);
	memcpy(&sim_flash_data[WIN], table_a, N);
	factory_copy_load(&cs7000p_factory_copy);
	cs7000p_factory_copy.changed();

	// A reply's first line copies the state before the take; its later lines stay with that copy
	cs7000p_cal_view_line(0, line, sizeof(line));
	CHECK(strstr(line, "cal_table=blank src=none") == line);
	cs7000p_radio.status(&st);
	cs7000p_cal_view_line(2, line, sizeof(line));
	CHECK(line_is(line, "ref=hold bias=0x200 r04=0x00 d_a=- d_b=- src=default why=table"));
	cs7000p_cal_view_line(4, line, sizeof(line));
	CHECK(line_is(line, "sq_maker=none why=table"));
	CHECK(cs7000p_rf.tune_src == CS7000P_SRC_MAKER);	// taken: only the reply's copy is the old state
	// A reply in the middle of a change (the same table taken again): no copy, every line busy after the wait,
	// nothing of either state
	factory_copy_load(&cs7000p_factory_copy);
	cs7000p_factory_copy.changed();
	mid_setup = view_mid_setup;
	cs7000p_radio.status(&st);
	mid_setup = NULL;
	for (uint8_t k = 0; k < CS7000P_CAL_VIEW_LINES; k++) {
		snprintf(want, sizeof(want), "%s=busy", keys[k]);
		CHECK(line_is(mid_lines[k], "%s", want));
	}
	CHECK(mid_waited_us >= 250000u && mid_waited_us < 300000u);
	// The next reply: the new state, every line of it
	cs7000p_cal_view_line(0, line, sizeof(line));
	CHECK(line_is(line, "cal_table=ok src=window crc=0x%08x why=none off=0x000 fields=0x%04x bat=0xa0",
		      crc(table_a), CS7000P_MK_F_ALL));
	cs7000p_cal_view_line(2, line, sizeof(line));
	CHECK(line_is(line, "ref=hold bias=0x1fb r04=0x00 d_a=-5 d_b=-3 src=maker why=none"));
	cs7000p_cal_view_line(3, line, sizeof(line));
	CHECK(line_is(line, "rx_cal=maker rssi=maker vco=low vco_rule=freq ifw=port"));
	cs7000p_cal_view_line(4, line, sizeof(line));
	CHECK(strstr(line, "sq_maker=ok ") == line);
	cs7000p_cal_view_line(CS7000P_CAL_VIEW_LINES, line, sizeof(line));
	CHECK(line[0] == '\0');
	// A receive set-up that takes no table (a bench change) changes nothing the view waits for
	mid_setup = view_mid_setup;
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_25K, NULL));
	mid_setup = NULL;
	CHECK(mid_waited_us == 0u && strstr(mid_lines[0], "cal_table=ok src=window") == mid_lines[0]);
}
#endif

#if CONFIG_FACTORY_COPY
// The one restore path on this board, as the owner's tool drives it (STORAGE ops 4 and 5, then the housekeeping task's
// job): refused while a transmission is up; the window written, the record after it; the view's restore= while the
// job is pending and after; the table in use taken at the status poll, a receive set-up after it
static void test_restore(void)
{
	radio_status_t st;
	uint8_t req[2u + FACTORY_COPY_LOAD_CHUNK], out[FACTORY_COPY_OP_OUT];
	size_t n = 0;
	chip(NULL, NULL);
	boot(true, 0);
	CHECK(line_is(rline, "cal_rec=blank crc=- wrote=0 restore=none"));
	for (uint32_t at = 0; at < N; at += FACTORY_COPY_LOAD_CHUNK) {
		uint32_t k = N - at < FACTORY_COPY_LOAD_CHUNK ? N - at : FACTORY_COPY_LOAD_CHUNK;
		req[0] = (uint8_t)at;
		req[1] = (uint8_t)(at >> 8);
		memcpy(req + 2, table_a + at, k);
		CHECK(factory_copy_load_op(&cs7000p_factory_copy, req, (uint16_t)(2u + k), out, &n) == DL_ST_OK);
	}
	uint32_t blank = storage_crc32(0, &sim_flash_data[WIN], N), a = crc(table_a);
	uint8_t r5[10] = { (uint8_t)N, (uint8_t)(N >> 8), (uint8_t)a, (uint8_t)(a >> 8), (uint8_t)(a >> 16),
			   (uint8_t)(a >> 24), (uint8_t)blank, (uint8_t)(blank >> 8), (uint8_t)(blank >> 16),
			   (uint8_t)(blank >> 24) };
	// Not while a transmission is up
	CHECK(cs7000p_radio.tx_drive(600) && cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL));
	CHECK(factory_copy_restore_op(&cs7000p_factory_copy, r5, sizeof(r5), out, &n) == DL_ST_REFUSED);
	CHECK(out[0] == FACTORY_COPY_RS_BUSY);
	cs7000p_radio.tx_off();
	CHECK(factory_copy_restore_op(&cs7000p_factory_copy, r5, sizeof(r5), out, &n) == DL_ST_OK && out[0] == 0);
	cs7000p_cal_rec_line(rline, sizeof(rline));
	CHECK(line_is(rline, "cal_rec=blank crc=- wrote=0 restore=pending"));
	storage_job_poll();
	CHECK(memcmp(&sim_flash_data[WIN], table_a, N) == 0 && cs7000p_factory_copy.busy());
	// The table in use moves at the radio task's status poll, not before
	cs7000p_cal_table_line(tline, sizeof(tline));
	cs7000p_cal_rec_line(rline, sizeof(rline));
	CHECK(strstr(tline, "cal_table=blank src=none") == tline);
	CHECK(line_is(rline, "cal_rec=blank crc=- wrote=0 restore=ok"));
	// ... and receive with it: today's values until then
	CHECK(cs7000p_rf.tune_src == CS7000P_SRC_DEFAULT && cs7000p_rf.rx_tune == CS7000P_RX_TUNE_NOMINAL);
	CHECK(cs7000p_rf.ref_src == CS7000P_SRC_DEFAULT && cs7000p_rf.rx_ref == CS7000P_REF_OFFSET);
	int retunes = modem.retunes;
	hw_n = 0;
	cs7000p_radio.status(&st);
	cs7000p_cal_table_line(tline, sizeof(tline));
	cs7000p_cal_rec_line(rline, sizeof(rline));
	CHECK(line_is(tline, "cal_table=ok src=window crc=0x%08x why=none off=0x000 fields=0x%04x bat=0xa0", a,
		      CS7000P_MK_F_ALL));
	CHECK(line_is(rline, "cal_rec=ok crc=0x%08x wrote=0 restore=ok", a));
	CHECK(!cs7000p_factory_copy.busy() && modem.retunes == retunes + 1);
	// The receive set-up after the take is on the restored table: its tune row, trim A in the held level (4:0x04
	// today's), and this same poll's RSSI on its two points (code 110: -95 dBm)
	cs7000p_maker_t m;
	uint16_t tune = 0;
	CHECK(cs7000p_maker_parse(table_a, a, CS7000P_APC_FLOOR, &m) && cs7000p_maker_rx_tune(&m, 446u * MHZ, &tune, NULL));
	CHECK(cs7000p_rf.tune_src == CS7000P_SRC_MAKER && cs7000p_rf.rx_tune == tune && dac_now == tune);
	CHECK(cs7000p_rf.rx_ref == -5 && cs7000p_rf.ref_src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_rf.ref_offset == CS7000P_REF_OFFSET && st.rssi_dbm == -95);
	CHECK(last_ref() == -5);	// what the modem was given at that receive entry
#if CONFIG_DEBUGLINK
	char line[128];
	cs7000p_ref_line(line, sizeof(line));
	CHECK(line_is(line, "ref=hold bias=0x1fb r04=0x00 d_a=-5 d_b=-3 src=maker why=none"));
	cs7000p_rx_cal_line(line, sizeof(line));
	CHECK(line_is(line, "rx_cal=maker rssi=maker vco=low vco_rule=freq ifw=port"));
#endif
	// Taken: the next restore is checked at once (the window holds a valid table now: another one is refused)
	req[0] = req[1] = 0;
	memcpy(req + 2, table_b, 8);
	CHECK(factory_copy_load_op(&cs7000p_factory_copy, req, 10, out, &n) == DL_ST_OK);
}
#endif

// The receive side, as a run of set-ups: FM wide and narrow, DMR, across the receive VCO switch, a sleep and a
// wake-up, the status polls. Its hardware writes kept (hw_log, hw_n) and hashed, and the last reading's dBm
static uint32_t rx_run(int16_t *dbm)
{
	radio_status_t st;
	hw_hash = 2166136261u;
	hw_n = 0;
	now_us = 1000000u;
	start();
	CHECK(cs7000p_radio.rx(433500000u, RADIO_BW_12K5, NULL));
	CHECK(cs7000p_radio.rx(485000000u, RADIO_BW_25K, NULL));
	cs7000p_radio.rx_sleep(true);
	cs7000p_radio.rx_sleep(false);
	cs7000p_radio.set_mode(RADIO_MODE_DMR);
	CHECK(cs7000p_radio.rx(439000000u, RADIO_BW_12K5, NULL));
	cs7000p_radio.status(&st);
	cs7000p_radio.set_mode(RADIO_MODE_FM);
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_25K, NULL));
	cs7000p_radio.status(&st);
	*dbm = st.rssi_dbm;
	CHECK(hw_n <= HW_LOG_MAX);
	return hw_hash;
}

// A run's writes, kept to compare with the next
static hw_ev_t base_log[HW_LOG_MAX];
static uint32_t base_n;

static void keep_log(void)
{
	memcpy(base_log, hw_log, sizeof(hw_ev_t) * (hw_n < HW_LOG_MAX ? hw_n : HW_LOG_MAX));
	base_n = hw_n;
}

// The last run against the kept one, write for write: what differs may only be a DAC value (dac) or the held level's
// offset with the same 4:0x04 (ref). Counts the differences of each kind
static bool log_differs_only(bool dac, bool ref, unsigned *n_dac, unsigned *n_ref)
{
	*n_dac = *n_ref = 0;
	if (hw_n != base_n) {
		printf("  %u writes, %u before\n", (unsigned)hw_n, (unsigned)base_n);
		return false;
	}
	for (uint32_t i = 0; i < hw_n && i < HW_LOG_MAX; i++) {
		const hw_ev_t *x = &hw_log[i], *y = &base_log[i];
		if (x->kind == y->kind && x->a == y->a && x->b == y->b) {
			continue;
		}
		if (dac && x->kind == HW_DAC && y->kind == HW_DAC && x->a != 0u && y->a != 0u) {
			(*n_dac)++;	// a tuning value, never a DAC switched on or off
		} else if (ref && x->kind == HW_REF && y->kind == HW_REF && x->b == y->b) {
			(*n_ref)++;
		} else {
			printf("  write %u: %u 0x%x 0x%x, before %u 0x%x 0x%x\n", (unsigned)i, (unsigned)x->kind,
			       (unsigned)x->a, (unsigned)x->b, (unsigned)y->kind, (unsigned)y->a, (unsigned)y->b);
			return false;
		}
	}
	return true;
}

// A table refused, or none: the radio receives exactly as one without a table. A valid one: the tune row and trim A
// in the held level, the RSSI on the table's points; 4:0x04, every line and every other write as before
static void test_table_rx(void)
{
	static uint8_t refused[5][N];
	static const char *const names[5] = { "blank", "zero", "points", "code", "trim" };
	uint8_t valid_bound[N], valid_32[N], use_rssi[N];
	unsigned n_dac, n_ref;
	int16_t dbm, dbm0;
	cs7000p_maker_t m;

	// No table at all: what the radio wrote before it read one
	null_maker = true;
	chip(NULL, NULL);
	boot(true, 0);
	const uint32_t h0 = rx_run(&dbm0);
	null_maker = false;
	keep_log();
	CHECK(dbm0 == cs7000p_rssi_dbm(500u) && cs7000p_rf.rx_tune == CS7000P_RX_TUNE_NOMINAL);
	CHECK(cs7000p_rf.rx_ref == CS7000P_REF_OFFSET && cs7000p_rf.ref_why == CS7000P_RXREF_WHY_TABLE);

	// Refused: blank, zero, points out of order, a tune code of 0, trim A past the reader's 64 steps; and unstable
	memset(refused[0], 0xFF, N);
	memset(refused[1], 0, N);
	memcpy(refused[2], table_a, N);
	memcpy(refused[2] + 0x024, table_a + 0x028, 4);
	memcpy(refused[2] + 0x028, table_a + 0x024, 4);
	memcpy(refused[3], table_a, N);
	refused[3][0x046] = 0;
	memcpy(refused[4], table_a, N);
	refused[4][0x0DC] = 0xC1;	// d +65
	for (unsigned i = 0; i < 6; i++) {
		chip(i < 5u ? refused[i] : table_a, NULL);
		boot(true, i < 5u ? 0u : WIN + 0x40u);
		CHECK(strstr(tline, " src=none ") != NULL);
		if (i == 5u) {
			sim_flash_misread(WIN + 0x40u, 2);	// without the copy, the run's own init reads the window again
		}
		uint32_t h = rx_run(&dbm);
		sim_flash_misread(0, 0);
		if (h != h0 || dbm != dbm0) {
			printf("  %s: the receive run differs\n", i < 5u ? names[i] : "unstable");
		}
		CHECK(h == h0 && dbm == dbm0);
		CHECK(log_differs_only(false, false, &n_dac, &n_ref));
		CHECK(cs7000p_rf.tune_src == CS7000P_SRC_DEFAULT && cs7000p_rf.ref_src == CS7000P_SRC_DEFAULT);
	}

	// Valid: only the tune and the bias move, and the dBm is the table's (code 110 on 60 / 160: -95)
	chip(table_a, NULL);
	boot(true, 0);
	CHECK(strstr(tline, "cal_table=ok src=window") == tline);
	(void)rx_run(&dbm);
	CHECK(log_differs_only(true, true, &n_dac, &n_ref) && n_dac > 0u && n_ref > 0u);
	CHECK(dbm == -95 && dbm0 != -95);
	CHECK(cs7000p_maker_parse(table_a, crc(table_a), CS7000P_APC_FLOOR, &m));
	uint16_t tune = 0;
	CHECK(cs7000p_maker_rx_tune(&m, 446u * MHZ, &tune, NULL) && cs7000p_rf.rx_tune == tune && dac_now == tune);
	CHECK(cs7000p_rf.tune_src == CS7000P_SRC_MAKER && tune != CS7000P_RX_TUNE_NOMINAL);
	CHECK(cs7000p_rf.rx_ref == -5 && cs7000p_rf.ref_src == CS7000P_SRC_MAKER);	// trim A 0x7B
	CHECK(cs7000p_rf.ref_offset == CS7000P_REF_OFFSET);	// 4:0x04 in receive: today's
	keep_log();
	char line[128];
	cs7000p_ref_line(line, sizeof(line));
	CHECK(line_is(line, "ref=hold bias=0x1fb r04=0x00 d_a=-5 d_b=-3 src=maker why=none"));
	cs7000p_rx_cal_line(line, sizeof(line));
	CHECK(line_is(line, "rx_cal=maker rssi=maker vco=low vco_rule=freq ifw=port"));
	// The same on a store not yet claimed (the window read, no copy written): using it is a reader's matter
	chip(table_a, NULL);
	boot(false, 0);
	CHECK(storage_region() == STORAGE_REGION_FOREIGN && strstr(tline, "cal_table=ok src=window") == tline);
	(void)rx_run(&dbm);
	CHECK(log_differs_only(false, false, &n_dac, &n_ref) && dbm == -95);

	// Trim A one step over the receive bound (d +33, inside the reader's 64): the tune still the table's, the bias
	// today's. At +32 it is used
	memcpy(valid_bound, table_a, N);
	valid_bound[0x0DC] = 0xA1;
	chip(valid_bound, NULL);
	boot(true, 0);
	CHECK(strstr(tline, "cal_table=ok src=window") == tline);
	(void)rx_run(&dbm);
	CHECK(log_differs_only(false, true, &n_dac, &n_ref) && n_ref > 0u && dbm == -95);
	CHECK(cs7000p_rf.rx_ref == CS7000P_REF_OFFSET && cs7000p_rf.ref_why == CS7000P_RXREF_WHY_BOUND);
	CHECK(cs7000p_rf.tune_src == CS7000P_SRC_MAKER);
	cs7000p_ref_line(line, sizeof(line));
	CHECK(line_is(line, "ref=hold bias=0x200 r04=0x00 d_a=33 d_b=-3 src=default why=rx_bound"));
	memcpy(valid_32, table_a, N);
	valid_32[0x0DC] = 0xA0;
	chip(valid_32, NULL);
	boot(true, 0);
	(void)rx_run(&dbm);
	CHECK(cs7000p_rf.rx_ref == 32 && cs7000p_rf.ref_src == CS7000P_SRC_MAKER);

	// A use rule's failure (an RSSI point at 0x00): that group alone falls back; the tune and the bias stay the table's
	memcpy(use_rssi, table_a, N);
	use_rssi[0x040] = 0;
	chip(use_rssi, NULL);
	boot(true, 0);
	CHECK(strstr(tline, "cal_table=ok src=window") == tline);
	CHECK(fields_of(use_rssi) == (CS7000P_MK_F_ALL & ~CS7000P_MK_F_RSSI));
	(void)rx_run(&dbm);
	CHECK(log_differs_only(false, false, &n_dac, &n_ref) && dbm == dbm0);
	CHECK(cs7000p_rf.tune_src == CS7000P_SRC_MAKER && cs7000p_rf.rx_ref == -5);
	cs7000p_rx_cal_line(line, sizeof(line));
	CHECK(line_is(line, "rx_cal=maker rssi=sheet vco=low vco_rule=freq ifw=port"));
#if CONFIG_FACTORY_COPY
	// The table in use from the record (a blank window): the same receive as from the window
	chip(NULL, table_a);
	boot(true, 0);
	CHECK(strstr(tline, "cal_table=blank src=record") == tline);
	(void)rx_run(&dbm);
	CHECK(log_differs_only(false, false, &n_dac, &n_ref) && dbm == -95);
#endif
}

#if CONFIG_DEBUGLINK
// The bench's knobs win over the table: 3:0 the tune, 3:4 the held level and 4:0x04 together, and 0x100 gives both
// back. The owner's A/B on the radio: 3:4 = 0 is today's level, 0x100 the table's
static void test_table_rx_bench(void)
{
	radio_status_t st;
	char line[128];
	uint32_t v = 0;
	chip(table_a, NULL);
	boot(true, 0);
	CHECK(cs7000p_rf.tune_src == CS7000P_SRC_MAKER && cs7000p_rf.rx_ref == -5);

	v = 1500;
	CHECK(cs7000p_chip_reg(3, 0, 0, &v, true) && dac_now == 1500);
	CHECK(cs7000p_rf.rx_tune == 1500 && cs7000p_rf.tune_src == CS7000P_SRC_BENCH);	// the view at once
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_25K, NULL));
	CHECK(cs7000p_rf.rx_tune == 1500 && cs7000p_rf.tune_src == CS7000P_SRC_BENCH);
	v = 0;
	CHECK(cs7000p_chip_reg(3, 0, 0, &v, true));
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_25K, NULL) && cs7000p_rf.tune_src == CS7000P_SRC_MAKER);
	// The A/B on one channel: 0 gives the table's row back at the next status poll, no channel change needed
	const uint16_t row = cs7000p_rf.rx_tune;
	v = 2048;
	CHECK(cs7000p_chip_reg(3, 0, 0, &v, true) && dac_now == 2048 && row != 2048u);
	v = 0;
	CHECK(cs7000p_chip_reg(3, 0, 0, &v, true) && dac_now == 2048);
	cs7000p_radio.status(&st);
	CHECK(dac_now == row && cs7000p_rf.rx_tune == row && cs7000p_rf.tune_src == CS7000P_SRC_MAKER);

	// 3:4 reads 0x100 while none is set; 0 is today's level, over trim A, and moves 4:0x04 as before
	CHECK(cs7000p_chip_reg(3, 0, 4, &v, false) && v == 0x100u);
	v = 0;
	CHECK(cs7000p_chip_reg(3, 0, 4, &v, true) && v == 0u);
	cs7000p_radio.status(&st);	// the receive set-up it asks for
	CHECK(cs7000p_rf.rx_ref == 0 && cs7000p_rf.ref_src == CS7000P_SRC_BENCH && cs7000p_rf.ref_offset == 0);
	cs7000p_ref_line(line, sizeof(line));
	CHECK(line_is(line, "ref=hold bias=0x200 r04=0x00 d_a=-5 d_b=-3 src=bench why=none"));
	v = 0xFFFFFFF0u;	// -16, sign extended
	CHECK(cs7000p_chip_reg(3, 0, 4, &v, true) && v == 0xF0u);
	cs7000p_radio.status(&st);
	CHECK(cs7000p_rf.rx_ref == -16 && cs7000p_rf.ref_offset == -16);
	cs7000p_ref_line(line, sizeof(line));
	CHECK(line_is(line, "ref=hold bias=0x1f0 r04=0xf0 d_a=-5 d_b=-3 src=bench why=none"));
	v = 0x101;
	CHECK(!cs7000p_chip_reg(3, 0, 4, &v, true) && cs7000p_rf.ref_override);
	// Released: the table's trim A again, 4:0x04 today's
	v = 0x100;
	CHECK(cs7000p_chip_reg(3, 0, 4, &v, true) && v == 0x100u);
	cs7000p_radio.status(&st);
	CHECK(cs7000p_rf.rx_ref == -5 && cs7000p_rf.ref_src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_rf.ref_offset == CS7000P_REF_OFFSET && !cs7000p_rf.ref_override);
	// The maker's coding (3:5) takes the same offset in its own form: the signed level
	v = CS7000P_REF_MAKER;
	CHECK(cs7000p_chip_reg(3, 0, 5, &v, true));
	cs7000p_radio.status(&st);
	cs7000p_ref_line(line, sizeof(line));
	CHECK(line_is(line, "ref=maker bias=0x3fb r04=0x00 d_a=-5 d_b=-3 src=maker why=none"));
	v = CS7000P_REF_HOLD;
	CHECK(cs7000p_chip_reg(3, 0, 5, &v, true));
	cs7000p_radio.status(&st);
	// Never stored: a new start has no bench offset
	v = 7;
	CHECK(cs7000p_chip_reg(3, 0, 4, &v, true));
	start();
	CHECK(!cs7000p_rf.ref_override && cs7000p_rf.rx_ref == -5);
}
#endif

#if CONFIG_DEBUGLINK
// chip_reg 3:5, the reference DAC's coding for a receive test: RAM only, taken at the next status poll by a receive
// set-up, and while it is not the hold every transmission is refused, the bench drive's too
static void test_ref_coding(void)
{
	radio_status_t st;
	uint32_t v = 0;
	start();
	CHECK(cs7000p_chip_reg(3, 0, 5, &v, false) && v == CS7000P_REF_HOLD);
	v = 3;
	CHECK(!cs7000p_chip_reg(3, 0, 5, &v, true) && cs7000p_rf.ref_coding == CS7000P_REF_HOLD);
	v = CS7000P_REF_MAKER;
	CHECK(cs7000p_chip_reg(3, 0, 5, &v, true) && v == CS7000P_REF_MAKER && cs7000p_rf.ref_coding == CS7000P_REF_MAKER);
	int retunes = modem.retunes;
	cs7000p_radio.status(&st);
	CHECK(modem.retunes == retunes + 1);	// a receive set-up, which asks the modem's coding again
	cs7000p_radio.status(&st);
	CHECK(modem.retunes == retunes + 1);
	int writes = mic_writes;
	CHECK(cs7000p_radio.tx_drive(600) && !cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL));
	CHECK(cs7000p_rf.tx_refused_why == CS7000P_TX_REF_CODING && !modem.fm_tx && mic_writes == writes);
	CHECK((shadow & EXP_TX_BITS) == 0u && cs7000p_rf.ovr_drive == 600);
	cs7000p_radio.set_mode(RADIO_MODE_DMR);
	runner.answer = runner.plan = true;
	cs7000p_rf.tx_refused_why = 0;
	CHECK(!cs7000p_radio.tx(446u * MHZ, RADIO_BW_12K5, 0, NULL) && !runner.armed);
	CHECK(cs7000p_rf.tx_refused_why == CS7000P_TX_REF_CODING);
	cs7000p_radio.set_mode(RADIO_MODE_FM);
	v = CS7000P_REF_MAKER_ADC;
	CHECK(cs7000p_chip_reg(3, 0, 5, &v, true) && v == CS7000P_REF_MAKER_ADC);
	// Back to the hold: the bench drive keys again
	v = CS7000P_REF_HOLD;
	CHECK(cs7000p_chip_reg(3, 0, 5, &v, true) && cs7000p_rf.ref_coding == CS7000P_REF_HOLD);
	cs7000p_radio.status(&st);
	CHECK(cs7000p_radio.tx_drive(600) && cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL) && modem.fm_tx);
	cs7000p_radio.tx_off();
	CHECK(!modem.fm_tx);
	// Never stored: a new start is the hold
	v = CS7000P_REF_MAKER;
	CHECK(cs7000p_chip_reg(3, 0, 5, &v, true));
	start();
	CHECK(cs7000p_rf.ref_coding == CS7000P_REF_HOLD);
}
#endif

#if CONFIG_DEBUGLINK
// The IF chip's words since ak_n was cleared: one set-up, the reset, then control 1 and 2 as given, 3 to 5 the port's
static bool ak_setup_is(uint8_t c1, uint8_t c2)
{
	const uint16_t want[AK2365A_SETUP_WORDS] = {
		ak2365a_word(AK2365A_REG_SRST, AK2365A_SRST_VALUE), ak2365a_word(AK2365A_REG_CTRL1, c1),
		ak2365a_word(AK2365A_REG_CTRL2, c2), ak2365a_word(AK2365A_REG_CTRL3, 0x00),
		ak2365a_word(AK2365A_REG_CTRL4, 0x01), ak2365a_word(AK2365A_REG_CTRL5, 0x80),
	};
	if (ak_n != AK2365A_SETUP_WORDS) {
		printf("  %u IF words, want one set-up\n", ak_n);
		return false;
	}
	for (unsigned i = 0; i < AK2365A_SETUP_WORDS; i++) {
		if (ak_log[i] != want[i]) {
			printf("  IF word %u: 0x%04x, want 0x%04x\n", i, ak_log[i], want[i]);
			return false;
		}
	}
	return true;
}

// A bench key-up on 446 MHz, FM: its writes hashed, from the call to the carrier, and the IF words among them
static uint32_t keyup_hash(unsigned *ak)
{
	hw_hash = 2166136261u;
	ak_n = 0;
	CHECK(cs7000p_radio.tx_drive(600) && cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL) && modem.fm_tx);
	*ak = ak_n;
	return hw_hash;
}

// chip_reg 3:7, the maker's IF chip words for a receive test of the table's squelch and RSSI points: one
// configuration per mode (FM wide, FM narrow, DMR), RAM only, taken at the next status poll by a receive set-up and
// again at a mode change. Receive only: the chip has no part in a transmission, which is neither refused nor
// changed by it
static void test_if_words(void)
{
	radio_status_t st;
	char line[128];
	uint32_t v = 0;
	unsigned ak_off, ak_on;
	start();
	// The port's: 0xF1 on a 25 kHz FM channel, 0xE9 on 12.5 kHz and in DMR whatever the channel says, 0x1D
	ak_n = 0;
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_25K, NULL) && ak_setup_is(0xF1, 0x1D));
	ak_n = 0;
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_12K5, NULL) && ak_setup_is(0xE9, 0x1D));
	cs7000p_radio.set_mode(RADIO_MODE_DMR);
	ak_n = 0;
	cs7000p_radio.status(&st);
	CHECK(ak_n == 0u);	// with the port's words a mode change asks for no set-up: as before the knob
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_25K, NULL) && ak_setup_is(0xE9, 0x1D));
	cs7000p_radio.set_mode(RADIO_MODE_FM);
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_25K, NULL));
	CHECK(cs7000p_chip_reg(3, 0, 7, &v, false) && v == 0u);
	cs7000p_rx_cal_line(line, sizeof(line));
	CHECK(strstr(line, " ifw=port") != NULL);
	// The port's key-up, to compare
	const uint32_t h_off = keyup_hash(&ak_off);
	cs7000p_radio.tx_off();
	CHECK(!modem.fm_tx && cs7000p_rf.seq.state == CHAINSEQ_ST_RX);

	v = 2;
	CHECK(!cs7000p_chip_reg(3, 0, 7, &v, true) && !cs7000p_rf.if_maker);
	v = 1;
	ak_n = 0;
	CHECK(cs7000p_chip_reg(3, 0, 7, &v, true) && v == 1u && cs7000p_rf.if_maker && ak_n == 0u);
	cs7000p_radio.status(&st);	// the receive set-up it asks for: FM wide
	CHECK(ak_setup_is(0xF5, 0x1F));
	ak_n = 0;
	cs7000p_radio.status(&st);
	CHECK(ak_n == 0u);
	cs7000p_rx_cal_line(line, sizeof(line));
	CHECK(strstr(line, " ifw=maker") != NULL);
	ak_n = 0;
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_12K5, NULL) && ak_setup_is(0xE9, 0x1F));
	// DMR: the mode change asks for the DMR word, on either channel width
	cs7000p_radio.set_mode(RADIO_MODE_DMR);
	ak_n = 0;
	cs7000p_radio.status(&st);
	CHECK(ak_setup_is(0xE5, 0x1F));
	ak_n = 0;
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_25K, NULL) && ak_setup_is(0xE5, 0x1F));
	cs7000p_radio.set_mode(RADIO_MODE_FM);
	ak_n = 0;
	cs7000p_radio.status(&st);
	CHECK(ak_setup_is(0xF5, 0x1F));

	// Receive only: the same key-up, write for write, and no IF word in it; the way back sets the maker's up again
	const uint32_t h_on = keyup_hash(&ak_on);
	CHECK(h_on == h_off && ak_on == 0u && ak_off == 0u && cs7000p_rf.tx_refused_why != CS7000P_TX_REF_CODING);
	ak_n = 0;
	cs7000p_radio.tx_off();
	CHECK(!modem.fm_tx && ak_setup_is(0xF5, 0x1F));

	// Back to the port's at the next status poll; never stored: a new start is the port's
	v = 0;
	ak_n = 0;
	CHECK(cs7000p_chip_reg(3, 0, 7, &v, true) && v == 0u && !cs7000p_rf.if_maker);
	cs7000p_radio.status(&st);
	CHECK(ak_setup_is(0xF1, 0x1D));
	v = 1;
	CHECK(cs7000p_chip_reg(3, 0, 7, &v, true));
	ak_n = 0;
	start();
	CHECK(!cs7000p_rf.if_maker && ak_setup_is(0xF1, 0x1D));
}
#endif

#if CONFIG_DEBUGLINK
// The maker's squelch thresholds at the receive frequency (board.sq_maker): the reader's lookup at f_rx, levels 1 and
// 9, shown and never applied; none with the lookup's reason when there is no table to take them from
static void test_sq_maker(void)
{
	static uint8_t sloped[N], levels[N];
	char line[128], want[128];
	cs7000p_maker_t m;
	uint8_t o1, c1, o9, c9;
	invented(table_a, 0x5A);

	// None at all
	chip(NULL, NULL);
	boot(true, 0);
	cs7000p_sq_maker_line(line, sizeof(line));
	CHECK(line_is(line, "sq_maker=none why=table"));

	// The invented table's flat rows: the same at every frequency
	chip(table_a, NULL);
	boot(true, 0);
	cs7000p_sq_maker_line(line, sizeof(line));
	CHECK(line_is(line, "sq_maker=ok o1=120 c1=130 o9=40 c9=50"));

	// Rows that rise with the points: the line over the two points around f_rx (test_table_rx: the receive writes
	// with a valid table differ only in the tune and the bias, so nothing of these is applied)
	memcpy(sloped, table_a, N);
	for (unsigned i = 0; i < 8; i++) {
		sloped[0x04C + i] = (uint8_t)(30 + 2 * i);	// level 9 open
		sloped[0x054 + i] = (uint8_t)(38 + 2 * i);	// level 9 close
		sloped[0x05C + i] = (uint8_t)(100 + 10 * i);	// level 1 open
		sloped[0x064 + i] = (uint8_t)(110 + 10 * i);	// level 1 close
	}
	CHECK(cs7000p_maker_parse(sloped, crc(sloped), CS7000P_APC_FLOOR, &m) && m.valid);
	CHECK((m.fields & CS7000P_MK_F_SQUELCH) != 0u);
	chip(sloped, NULL);
	boot(true, 0);
	const uint32_t hz[2] = { 446u * MHZ, 470u * MHZ };
	for (unsigned k = 0; k < 2; k++) {
		CHECK(cs7000p_radio.rx(hz[k], RADIO_BW_12K5, NULL));
		CHECK(cs7000p_maker_squelch(&m, hz[k], 1, &o1, &c1, NULL) &&
		      cs7000p_maker_squelch(&m, hz[k], 9, &o9, &c9, NULL));
		snprintf(want, sizeof(want), "sq_maker=ok o1=%u c1=%u o9=%u c9=%u", o1, c1, o9, c9);
		cs7000p_sq_maker_line(line, sizeof(line));
		CHECK(line_is(line, "%s", want));
		// Between the two points around it (440 and 460 MHz, then 460 and 478.95 MHz), not at either
		CHECK(o1 > 120u + 10u * k && o1 < 130u + 10u * k && o9 > 34u + 2u * k && o9 < 36u + 2u * k);
	}
	// The same pinned by hand (spec 4.4): at a point, both sides of the 479 MHz switch segment (the end value on the
	// frequency's VCO side, not the line across it) and past the last point (extended)
	static const struct {
		uint32_t hz;
		const char *want;
	} vec[] = {
		{ 446000000u, "sq_maker=ok o1=123 c1=133 o9=35 c9=43" },
		{ 460000000u, "sq_maker=ok o1=130 c1=140 o9=36 c9=44" },
		{ 470000000u, "sq_maker=ok o1=135 c1=145 o9=37 c9=45" },
		{ 479000000u, "sq_maker=ok o1=140 c1=150 o9=38 c9=46" },
		{ 479040000u, "sq_maker=ok o1=150 c1=160 o9=40 c9=48" },
		{ 520000000u, "sq_maker=ok o1=175 c1=185 o9=45 c9=53" },
	};
	for (unsigned k = 0; k < sizeof(vec) / sizeof(vec[0]); k++) {
		CHECK(cs7000p_radio.rx(vec[k].hz, RADIO_BW_12K5, NULL) && cs7000p_rf.seq.rx_hz == vec[k].hz);
		cs7000p_sq_maker_line(line, sizeof(line));
		CHECK(line_is(line, "%s", vec[k].want));
	}
	// Shown only: a receive set-up writes the same with other squelch rows (the port's squelch is the noise reading)
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_12K5, NULL));
	hw_hash = 2166136261u;
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_25K, NULL));
	const uint32_t h_sloped = hw_hash;
#if CONFIG_FACTORY_COPY
	// From the record when the window is blank: the same thresholds
	chip(NULL, sloped);
	boot(true, 0);
	CHECK(strstr(tline, " src=record ") != NULL);
	cs7000p_sq_maker_line(line, sizeof(line));
	CHECK(line_is(line, "%s", vec[0].want));
#endif
	chip(table_a, NULL);
	boot(true, 0);
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_12K5, NULL));
	hw_hash = 2166136261u;
	CHECK(cs7000p_radio.rx(446u * MHZ, RADIO_BW_25K, NULL));
	CHECK(hw_hash == h_sloped);

	// A damaged squelch row (level 9 open over its close at a point) is an integrity rule: no table in use
	memcpy(levels, sloped, N);
	sloped[0x04C + 3] = 60;
	chip(sloped, NULL);
	boot(true, 0);
	CHECK(strstr(tline, " src=none ") != NULL && strstr(tline, " why=sq_hyst ") != NULL);
	cs7000p_sq_maker_line(line, sizeof(line));
	CHECK(line_is(line, "sq_maker=none why=table"));
	// ... and so are levels out of order (level 1's open not above level 9's at a point)
	levels[0x05C + 2] = levels[0x04C + 2];
	chip(levels, NULL);
	boot(true, 0);
	CHECK(strstr(tline, " src=none ") != NULL && strstr(tline, " why=sq_level ") != NULL);
	cs7000p_sq_maker_line(line, sizeof(line));
	CHECK(line_is(line, "sq_maker=none why=table"));
}
#endif

#if CONFIG_DEBUGLINK
// --- The owner's transmit arm (hal_radio_t.tx_arm, hardware-cs7000p.md 13.2) ---

#define ARM_TOP	(CONFIG_DMR ? HAL_TX_ARM_ALL : HAL_TX_ARM_FM)

static hal_tx_arm_info_t arm_info(void)
{
	hal_tx_arm_info_t i;
	memset(&i, 0x5A, sizeof(i));
	cs7000p_radio.tx_arm_info(&i);
	return i;
}

// A key-up on the test TX limit's frequency, FM at a level or DMR's arm, refused with why: nothing keyed, no
// microphone, nothing admitted
static void keyup_refused(uint8_t level, bool dmr, uint8_t why)
{
	int writes = mic_writes;
	cs7000p_rf.tx_refused_why = 0;
	if (dmr) {
		cs7000p_radio.set_mode(RADIO_MODE_DMR);
		runner.answer = runner.plan = true;
	}
	CHECK(!cs7000p_radio.tx(446u * MHZ, dmr ? RADIO_BW_12K5 : RADIO_BW_25K, level, NULL));
	CHECK(cs7000p_rf.tx_refused_why == why && !modem.fm_tx && !runner.armed && mic_writes == writes);
	CHECK((shadow & EXP_TX_BITS) == 0u && dac_now <= CS7000P_TUNE_MAX && !cs7000p_rf.admit);
	if (dmr) {
		runner.answer = runner.plan = false;
		cs7000p_radio.set_mode(RADIO_MODE_FM);
	}
}

// Every key-up the invented table would serve, at both levels and in both modes, with the arm's reason for the stage
// armed now; listed: the test TX limit names 446 MHz (start() sets it)
static void keyups_refused(uint8_t stage, bool listed)
{
	static const uint8_t from[2][2] = { { 1, 2 }, { 3, 3 } };	// [dmr][level]: the first stage that covers it
	for (uint8_t level = 0; level < 2; level++) {
		for (int dmr = 0; dmr < (CONFIG_DMR ? 2 : 1); dmr++) {
			const bool covered = stage >= from[dmr][level];
			keyup_refused(level, dmr != 0, !covered ? CS7000P_TX_UNARMED :
						      (listed ? CS7000P_TX_UNCAL : CS7000P_TX_ARM_NO_LIST));
		}
	}
}

// Before the radio's init (its chain not up): every arm but off refused, a state; the info says off
static void test_tx_arm_before_init(void)
{
	CHECK(cs7000p_radio.tx_arm_info != NULL && cs7000p_radio.tx_arm != NULL);
	for (uint8_t stage = HAL_TX_ARM_FM_LOW; stage <= ARM_TOP; stage++) {
		CHECK(cs7000p_radio.tx_arm(stage, 0) == HAL_TX_ARM_STATE && cs7000p_rf.arm == CS7000P_ARM_OFF);
	}
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_OFF, 0) == HAL_TX_ARM_OK && cs7000p_rf.arm == CS7000P_ARM_OFF);
	hal_tx_arm_info_t i = arm_info();
	CHECK(i.stage == HAL_TX_ARM_OFF && i.max == ARM_TOP && !i.cal_ok && i.crc == 0u);
}

static void test_tx_arm(void)
{
	radio_status_t st;
	char line[128];
	const uint32_t a = crc(table_a), b = crc(table_b);
	uint8_t blank[N];
	memset(blank, 0xFF, sizeof(blank));
	const uint32_t blank_crc = crc(blank);
	CHECK(cs7000p_radio.tx_arm != NULL && cs7000p_radio.tx_arm_info != NULL);

	// A valid table in the window: the info, off at the start
	chip(table_a, NULL);
	boot(true, 0);
	hal_tx_arm_info_t i = arm_info();
	CHECK(i.stage == HAL_TX_ARM_OFF && i.max == ARM_TOP && i.addr == WIN && i.len == N && i.crc == a && i.cal_ok);
	cs7000p_tx_arm_line(line, sizeof(line));
	CHECK(line_is(line, "tx_arm=off id=0x00000000 list=1"));
	// Unarmed: every key-up the table would serve refused, why 13; nothing keyed
	keyups_refused(HAL_TX_ARM_OFF, true);
	// Refused, each leaving it off: a stage over the top, another table's CRC, the window's own read back as 0
	CHECK(cs7000p_radio.tx_arm(ARM_TOP + 1u, a) == HAL_TX_ARM_STAGE);
	CHECK(cs7000p_radio.tx_arm(0xFF, a) == HAL_TX_ARM_STAGE);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, b) == HAL_TX_ARM_ID && cs7000p_rf.arm == CS7000P_ARM_OFF);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, 0) == HAL_TX_ARM_ID && cs7000p_rf.arm == CS7000P_ARM_OFF);
	int retunes0 = modem.retunes;
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_OFF, b) == HAL_TX_ARM_OK);	// off: always
	CHECK(strstr(log_last, "radio: transmit arm off id=0x00000000") == log_last);
	cs7000p_radio.status(&st);
	CHECK(modem.retunes == retunes0);	// off while off: no receive set-up

	// Each stage, against the table's CRC: a receive set-up at the next poll; the arm's reasons at each, and with a
	// list nothing goes ahead either (none of the table's transmit values is served)
	for (uint8_t stage = HAL_TX_ARM_FM_LOW; stage <= ARM_TOP; stage++) {
		int retunes = modem.retunes;
		CHECK(cs7000p_radio.tx_arm(stage, a) == HAL_TX_ARM_OK && cs7000p_rf.arm == stage && cs7000p_rf.arm_id == a);
		cs7000p_radio.status(&st);
		CHECK(modem.retunes == retunes + 1 && arm_info().stage == stage);
		CHECK(cs7000p_radio.tx_arm(stage, a) == HAL_TX_ARM_OK);	// the same stage again: no set-up
		cs7000p_radio.status(&st);
		CHECK(modem.retunes == retunes + 1);
		keyups_refused(stage, true);
		CHECK(tx_limit_set(false, NULL, 0));	// no list: 15 where the stage covers it
		keyups_refused(stage, false);
		cs7000p_tx_arm_line(line, sizeof(line));
		CHECK(line_is(line, "tx_arm=%s id=0x%08x list=0", stage == 1 ? "fm-low" : stage == 2 ? "fm" : "dmr", a));
		// A block is no list either (the radio core refuses before the board; the view says list=0)
		CHECK(tx_limit_set(true, NULL, 0));
		cs7000p_tx_arm_line(line, sizeof(line));
		CHECK(strstr(line, " list=0") != NULL);
		static const uint32_t limit[1] = { 446u * MHZ };
		CHECK(tx_limit_set(false, limit, 1));
#if CONFIG_FACTORY_COPY
		CHECK(cs7000p_factory_copy.busy());	// the restore is refused while armed
#endif
	}
	cs7000p_tx_arm_line(line, sizeof(line));
	CHECK(line_is(line, "tx_arm=%s id=0x%08x list=1", CONFIG_DMR ? "dmr" : "fm", a));
	CHECK(strstr(log_last, "radio: transmit arm ") == log_last);
	// The same stage again with another id: refused, the arm as it was. A lower one: always taken, the id kept
	CHECK(cs7000p_radio.tx_arm(ARM_TOP, b) == HAL_TX_ARM_ID && cs7000p_rf.arm == ARM_TOP && cs7000p_rf.arm_id == a);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, 0) == HAL_TX_ARM_OK && cs7000p_rf.arm == HAL_TX_ARM_FM_LOW);
	CHECK(cs7000p_rf.arm_id == a);
	cs7000p_tx_arm_line(line, sizeof(line));
	CHECK(line_is(line, "tx_arm=fm-low id=0x%08x list=1", a));
	// A bench carrier is not the arm's: a disarm leaves it up (no safe request), and the arm's lowering ends only a
	// transmission on the table's values (cs7000pRadioSeqTest)
	const int safes = board_safes;
	CHECK(cs7000p_radio.tx_drive(600) && cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL));
	CHECK(cs7000p_rf.seq.state == CHAINSEQ_ST_TX && cs7000p_rf.admit_ovr && !cs7000p_rf.admit_arm);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_OFF, 0) == HAL_TX_ARM_OK && cs7000p_rf.arm == CS7000P_ARM_OFF);
	CHECK(board_safes == safes && cs7000p_rf.seq.state == CHAINSEQ_ST_TX && modem.fm_tx);
	cs7000p_radio.tx_off();
	cs7000p_tx_arm_line(line, sizeof(line));
	CHECK(line_is(line, "tx_arm=off id=0x00000000 list=1"));
	// One on the table's values (none is admitted on them yet: this carrier marked as one, FM at level 1 under stage
	// 2). Stage 2 again keeps it; stage 1 ends it at once by the safe request, and the new stage stands after it
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM, a) == HAL_TX_ARM_OK);
	CHECK(cs7000p_radio.tx_drive(600) && cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 1, NULL));
	cs7000p_rf.admit_arm = true;
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM, a) == HAL_TX_ARM_OK && board_safes == safes);
	CHECK(cs7000p_rf.seq.state == CHAINSEQ_ST_TX && (shadow & EXP_TX_BITS) == EXP_TX_BITS && dac_now == 600);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, 0) == HAL_TX_ARM_OK && board_safes == safes + 1);
	CHECK((shadow & EXP_TX_BITS) == 0u && dac_now == 0 && cs7000p_rf.safed);
	CHECK(cs7000p_rf.arm == CS7000P_ARM_FM_LOW && cs7000p_rf.arm_id == a);
	CHECK(!cs7000p_radio.tx_drive_now(600) && (shadow & EXP_TX_BITS) == 0u);	// nothing keys it again
	cs7000p_radio.tx_off();
	cs7000p_radio.status(&st);
	CHECK(cs7000p_rf.seq.state == CHAINSEQ_ST_RX && (shadow & EXP_TX_BITS) == 0u && mic_off());
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_OFF, 0) == HAL_TX_ARM_OK);

	// A bench state forbids it: the maker's reference coding (3:5); back to the hold, it is taken
	uint32_t v = CS7000P_REF_MAKER;
	CHECK(cs7000p_chip_reg(3, 0, 5, &v, true));
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, a) == HAL_TX_ARM_STATE && cs7000p_rf.arm == CS7000P_ARM_OFF);
	v = CS7000P_REF_HOLD;
	CHECK(cs7000p_chip_reg(3, 0, 5, &v, true));
	cs7000p_radio.status(&st);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, a) == HAL_TX_ARM_OK);
	// The power's off state: its safe request disarms, and no arm is taken in it
	latch_off();
	CHECK(cs7000p_rf.arm == CS7000P_ARM_OFF && cs7000p_rf.arm_id == 0u);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, a) == HAL_TX_ARM_STATE && cs7000p_rf.arm == CS7000P_ARM_OFF);
	off_state = false;
	cs7000p_radio.status(&st);
	// Any safe request (a fault path) disarms; and every start is off
	CHECK(cs7000p_radio.tx_arm(ARM_TOP, a) == HAL_TX_ARM_OK);
	rf_safe_now();
	CHECK(cs7000p_rf.arm == CS7000P_ARM_OFF && arm_info().stage == HAL_TX_ARM_OFF);
	cs7000p_radio.status(&st);
	CHECK(cs7000p_radio.tx_arm(ARM_TOP, a) == HAL_TX_ARM_OK);
	start();
	CHECK(cs7000p_rf.arm == CS7000P_ARM_OFF && arm_info().stage == HAL_TX_ARM_OFF);

	// No table in use: refused; off still taken
	chip(NULL, NULL);
	boot(true, 0);
	i = arm_info();
	CHECK(i.crc == 0u && !i.cal_ok && i.stage == HAL_TX_ARM_OFF);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, blank_crc) == HAL_TX_ARM_NO_CAL);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, 0) == HAL_TX_ARM_NO_CAL && cs7000p_rf.arm == CS7000P_ARM_OFF);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_OFF, 0) == HAL_TX_ARM_OK);
	keyup_refused(0, false, CS7000P_TX_UNCAL);	// and nothing the table would serve: 2, as before the arm
#if CONFIG_FACTORY_COPY
	// The record as the source (the window blank): the handshake is against the record's CRC, not the window's
	chip(NULL, table_a);
	boot(true, 0);
	CHECK(strstr(tline, "cal_table=blank src=record ") == tline);
	i = arm_info();
	CHECK(i.crc == a && i.cal_ok);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, blank_crc) == HAL_TX_ARM_ID);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, a) == HAL_TX_ARM_OK);
	keyups_refused(HAL_TX_ARM_FM_LOW, true);
	// Window and record both valid and different: neither is used, nothing to arm
	chip(table_a, table_b);
	boot(true, 0);
	CHECK(strstr(tline, " src=none ") != NULL && !arm_info().cal_ok);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, a) == HAL_TX_ARM_NO_CAL);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, b) == HAL_TX_ARM_NO_CAL);
#endif
}

#if CONFIG_FACTORY_COPY
// The arm and the restore exclude each other. A restore's job pending, its outcome not taken by the copy, or its
// changed() not taken by the radio task: every arm but off refused (reason 4), the table about to change. Armed: the
// restore refused (busy). Once taken, the handshake is against the restored table's CRC
static void test_tx_arm_restore(void)
{
	radio_status_t st;
	uint8_t req[2u + FACTORY_COPY_LOAD_CHUNK], out[FACTORY_COPY_OP_OUT];
	size_t n = 0;
	const uint32_t a = crc(table_a);
	chip(NULL, NULL);
	boot(true, 0);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, a) == HAL_TX_ARM_NO_CAL);
	for (uint32_t at = 0; at < N; at += FACTORY_COPY_LOAD_CHUNK) {
		uint32_t k = N - at < FACTORY_COPY_LOAD_CHUNK ? N - at : FACTORY_COPY_LOAD_CHUNK;
		req[0] = (uint8_t)at;
		req[1] = (uint8_t)(at >> 8);
		memcpy(req + 2, table_a + at, k);
		CHECK(factory_copy_load_op(&cs7000p_factory_copy, req, (uint16_t)(2u + k), out, &n) == DL_ST_OK);
	}
	uint32_t w = storage_crc32(0, &sim_flash_data[WIN], N);
	uint8_t r5[10] = { (uint8_t)N, (uint8_t)(N >> 8), (uint8_t)a, (uint8_t)(a >> 8), (uint8_t)(a >> 16),
			   (uint8_t)(a >> 24), (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24) };
	CHECK(factory_copy_restore_op(&cs7000p_factory_copy, r5, sizeof(r5), out, &n) == DL_ST_OK);
	// The job pending: refused, the id right or not; off taken
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, a) == HAL_TX_ARM_STATE);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, 0) == HAL_TX_ARM_STATE && cs7000p_rf.arm == CS7000P_ARM_OFF);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_OFF, 0) == HAL_TX_ARM_OK);
	// The job done: its outcome and the board's request not taken yet: refused still
	storage_job_poll();
	CHECK(memcmp(&sim_flash_data[WIN], table_a, N) == 0 && factory_copy_result() == FACTORY_COPY_RS_OK);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, a) == HAL_TX_ARM_STATE && cs7000p_rf.arm == CS7000P_ARM_OFF);
	CHECK(!arm_info().cal_ok);	// the table in use is still the one before the restore
	// Taken at the status poll: the restored table in use, and the arm against its CRC
	cs7000p_radio.status(&st);
	CHECK(arm_info().cal_ok && arm_info().crc == a);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, a) == HAL_TX_ARM_OK);
	// Armed: busy() holds, and a restore is refused before any check of its own
	CHECK(cs7000p_factory_copy.busy());
	req[0] = req[1] = 0;
	memcpy(req + 2, table_a, 8);
	CHECK(factory_copy_load_op(&cs7000p_factory_copy, req, 10, out, &n) == DL_ST_OK);
	CHECK(factory_copy_restore_op(&cs7000p_factory_copy, r5, sizeof(r5), out, &n) == DL_ST_REFUSED);
	CHECK(out[0] == FACTORY_COPY_RS_BUSY);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_OFF, 0) == HAL_TX_ARM_OK && !cs7000p_factory_copy.busy());
	// A changed() alone (its request untaken by the radio task): refused until the poll takes it
	factory_copy_load(&cs7000p_factory_copy);
	cs7000p_factory_copy.changed();
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, a) == HAL_TX_ARM_STATE);
	factory_copy_taken();	// the copy's own state taken: the board's request alone still refuses
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, a) == HAL_TX_ARM_STATE);
	cs7000p_radio.status(&st);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_FM_LOW, a) == HAL_TX_ARM_OK);
	CHECK(cs7000p_radio.tx_arm(HAL_TX_ARM_OFF, 0) == HAL_TX_ARM_OK);
}
#endif

// The bench drive's key-up with a valid table in use writes what it writes without one, from the call to the
// carrier, at every stage of the arm: the one difference is the receive bias the modem kept from the receive entry
// before it, which carries trim A (-5) with the table and today's 0 without
static void test_arm_bench_keyup(void)
{
	radio_status_t st;
	unsigned nd = 0, nr = 0;
	chip(NULL, NULL);
	boot(true, 0);
	CHECK(last_ref() == CS7000P_REF_OFFSET);
	hw_n = 0;
	CHECK(cs7000p_radio.tx_drive(600) && cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL) && modem.fm_tx);
	CHECK(cs7000p_rf.admit_ovr && hw_n > 0u && last_ref() == INT32_MIN);	// the key-up asks no receive level
	keep_log();
	cs7000p_radio.tx_off();
	for (uint8_t stage = HAL_TX_ARM_OFF; stage <= ARM_TOP; stage++) {
		chip(table_a, NULL);
		boot(true, 0);
		if (stage != HAL_TX_ARM_OFF) {
			CHECK(cs7000p_radio.tx_arm(stage, crc(table_a)) == HAL_TX_ARM_OK);
		}
		cs7000p_radio.status(&st);
		CHECK(last_ref() == -5);	// what the key-up inherits
		hw_n = 0;
		CHECK(cs7000p_radio.tx_drive(600) && cs7000p_radio.tx(446u * MHZ, RADIO_BW_25K, 0, NULL) && modem.fm_tx);
		CHECK(cs7000p_rf.admit_ovr && !cs7000p_rf.admit_arm);
		CHECK(log_differs_only(false, false, &nd, &nr) && nd == 0u && nr == 0u);
		cs7000p_radio.tx_off();
	}
}
#endif

int main(void)
{
#if CONFIG_DEBUGLINK
	test_tx_arm_before_init();	// first: the chain is not up yet
#endif
	test_fm_mic();
	test_dmr_mic();
	test_off_state();
	test_table_sources();
#if CONFIG_FACTORY_COPY
	test_table_request();
	test_table_request_asleep();
#if CONFIG_DEBUGLINK
	test_view_whole();
#endif
	test_restore();
#endif
	test_table_rx();
#if CONFIG_DEBUGLINK
	test_ref_coding();
	test_table_rx_bench();
	test_if_words();
	test_sq_maker();
	test_tx_arm();
	test_arm_bench_keyup();
#if CONFIG_FACTORY_COPY
	test_tx_arm_restore();
#endif
#endif
	sim_flash_free();
	printf("# cs7000pRadio%sTest %s (%d failures)\n", CONFIG_FACTORY_COPY ? "" : "NoCopy", failures ? "FAILED" : "PASSED",
	       failures);
	return failures ? 1 : 0;
}
