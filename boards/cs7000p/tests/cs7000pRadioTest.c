// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS hal_radio_t (radio_cs7000p.c) over the real RF chain (rfops_cs7000p.c, chainseq, the
// synthesizer's and the IF receiver's drivers on their bit-banged buses), with a fake expander, DAC, modem, burst
// runner and power state. What must hold on the radio: the microphone's amplifier and switch are on for a
// transmission, FM or DMR, and off at its end and on every way it fails or is refused; in the power's off state (a
// power-off the knob overrode) the receiver is not set up again, the speaker amplifier stays off (also behind a
// speaker call the power-off cut into) and nothing transmits, FM or DMR, and all of it comes back once that state
// ends.
#include <stdio.h>
#include <string.h>
#include "cs7000p.h"
#include "pins.h"
#include "exp_cs7000p.h"
#include "speaker_cs7000p.h"
#include "rfops_cs7000p.h"
#include "rfplan_cs7000p.h"
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

void stm32_gpio_clock(GPIO_TypeDef *port) { (void)port; }
void stm32_pin_force(GPIO_TypeDef *port, uint16_t pins, bool level) { (void)port; (void)pins; (void)level; }
void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool od)
{
	(void)port; (void)pins; (void)level; (void)od;
}
void stm32_pin_in(GPIO_TypeDef *port, uint16_t pins, uint32_t pull) { (void)port; (void)pins; (void)pull; }
void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool level) { (void)port; (void)pins; (void)level; }
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

const board_t board = { .battery_critical_mv = 6000 };

// --- The expander and the DAC, with the interlock of exp_cs7000p.h ---

static uint32_t shadow = EXP_RESET_WORD;
static uint16_t dac_now;
static int mic_writes;			// writes that moved a microphone bit

// As the real writer: the mask first, then the check and the word
bool cs7000p_exp_set(uint32_t set, uint32_t clear)
{
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
void cs7000p_c6000_mode(bool dmr, uint32_t hz) { (void)hz; modem.dmr = dmr; }
void cs7000p_c6000_retune(uint32_t hz) { (void)hz; modem.retunes++; }
void cs7000p_c6000_rx_hold(void) {}
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

int main(void)
{
	test_fm_mic();
	test_dmr_mic();
	test_off_state();
	printf("# cs7000pRadioTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
