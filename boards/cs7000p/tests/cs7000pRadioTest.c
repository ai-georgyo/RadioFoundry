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
// a restore's request taken only at the status poll, never under a transmission; and no receive write changed by a
// table. Built again without CONFIG_FACTORY_COPY (cs7000pRadioNoCopyTest): the window alone. Every table here is
// invented.
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

static void hw(uint32_t kind, uint32_t a, uint32_t b)
{
	const uint32_t w[3] = { kind, a, b };
	for (unsigned i = 0; i < 3; i++) {
		for (unsigned k = 0; k < 4; k++) {
			hw_hash = (hw_hash ^ ((w[i] >> (8u * k)) & 0xFFu)) * 16777619u;
		}
	}
}

void stm32_gpio_clock(GPIO_TypeDef *port) { (void)port; }
void stm32_pin_force(GPIO_TypeDef *port, uint16_t pins, bool level) { (void)port; (void)pins; (void)level; }
void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool od)
{
	hw(1, (uint32_t)(port - fake_ports) << 16 | pins, (uint32_t)level << 1 | od);
}
void stm32_pin_in(GPIO_TypeDef *port, uint16_t pins, uint32_t pull) { hw(2, (uint32_t)(port - fake_ports) << 16 | pins, pull); }
void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool level) { hw(3, (uint32_t)(port - fake_ports) << 16 | pins, level); }
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
	}
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	return ok;
}

uint32_t cs7000p_exp_get(void) { return shadow; }

bool cs7000p_apc_dac(uint16_t v)
{
	hw(5, v, 0);
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

uint16_t cs7000p_adc_raw(uint8_t rank) { (void)rank; return 0; }

// --- The modem ---

static struct {
	bool dmr, fm_tx, audio;
	int retunes, fm_tx_ends;
	bool refuse_fm_tx;
} modem;

bool cs7000p_c6000_init(void) { return true; }
void cs7000p_c6000_mode(bool dmr, uint32_t hz) { hw(6, dmr, hz); modem.dmr = dmr; }
void cs7000p_c6000_retune(uint32_t hz) { hw(7, hz, 0); modem.retunes++; }
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

#if CONFIG_FACTORY_COPY
// A restore's changed() only raises the request; the table in use changes at the radio task's status poll, never
// under a transmission, and a receive set-up follows. busy() holds from the request until it is taken
static void test_table_request(void)
{
	radio_status_t st;
	chip(NULL, NULL);
	boot(true, 0);
	CHECK(strstr(tline, "cal_table=blank src=none") == tline);
	CHECK(!cs7000p_factory_copy.busy());

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
	int retunes = modem.retunes;
	cs7000p_radio.status(&st);
	cs7000p_cal_table_line(tline, sizeof(tline));
	cs7000p_cal_rec_line(rline, sizeof(rline));
	CHECK(line_is(tline, "cal_table=ok src=window crc=0x%08x why=none off=0x000 fields=0x%04x bat=0xa0", a,
		      CS7000P_MK_F_ALL));
	CHECK(line_is(rline, "cal_rec=ok crc=0x%08x wrote=0 restore=ok", a));
	CHECK(!cs7000p_factory_copy.busy() && modem.retunes == retunes + 1);
	// Taken: the next restore is checked at once (the window holds a valid table now: another one is refused)
	req[0] = req[1] = 0;
	memcpy(req + 2, table_b, 8);
	CHECK(factory_copy_load_op(&cs7000p_factory_copy, req, 10, out, &n) == DL_ST_OK);
}
#endif

// The receive side, as a run of set-ups: FM wide and narrow, DMR, across the receive VCO switch, a sleep and a
// wake-up, the status polls. Its hardware writes hashed, and the readings
static uint32_t rx_run(int16_t *dbm)
{
	radio_status_t st;
	hw_hash = 2166136261u;
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
	return hw_hash;
}

// Nothing receives on the table yet: every receive write and reading is the same with a valid table, with none and
// with a damaged one
static void test_table_rx_unchanged(void)
{
	uint8_t damaged[N];
	memcpy(damaged, table_a, N);
	damaged[0x046] = 0;
	int16_t dbm[4];
	uint32_t h[4];
	chip(NULL, NULL);
	boot(true, 0);
	h[0] = rx_run(&dbm[0]);
	chip(table_a, NULL);
	boot(true, 0);
	CHECK(strstr(tline, "cal_table=ok src=window") == tline);
	h[1] = rx_run(&dbm[1]);
	chip(damaged, NULL);
	boot(true, 0);
	h[2] = rx_run(&dbm[2]);
	chip(NULL, NULL);
	boot(true, 0);
	h[3] = rx_run(&dbm[3]);
	CHECK(h[0] == h[1] && h[1] == h[2] && h[2] == h[3]);
	CHECK(dbm[0] == dbm[1] && dbm[1] == dbm[2] && dbm[2] == dbm[3]);
}

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

int main(void)
{
	test_fm_mic();
	test_dmr_mic();
	test_off_state();
	test_table_sources();
#if CONFIG_FACTORY_COPY
	test_table_request();
	test_restore();
#endif
	test_table_rx_unchanged();
#if CONFIG_DEBUGLINK
	test_ref_coding();
#endif
	sim_flash_free();
	printf("# cs7000pRadio%sTest %s (%d failures)\n", CONFIG_FACTORY_COPY ? "" : "NoCopy", failures ? "FAILED" : "PASSED",
	       failures);
	return failures ? 1 : 0;
}
