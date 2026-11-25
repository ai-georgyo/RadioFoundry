// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS sub-audio: CTCSS / DCS decode in software from QT_DQT_IN (PA2, a rank of the board's ADC scan) and
// encode as PWM on CTC/DCS_OUT (PC8, TIM3 CH3) into the reference oscillator's control voltage
// (docs/spec/hardware-cs7000p.md 7.6). The DSP is drivers/rf/subaudio; this file is the glue to the scan and the
// timer. Nothing here has run on a radio.
//
// Receive: the scan hands over the rank's samples in batches of 8, 2,400 a second, from its DMA interrupt. The
// sub-audio filter ahead of the pin runs from the audio rail, which every receive set-up turns on.
//
// Transmit: one generator sample per scan tick (the scan timer's interrupt, 2,400 a second) becomes the PWM's duty
// by the rule of subpwm_cs7000p.h, at the depth the radio gave for the transmission (the maker's level for the tone at
// f_tx, or the build's CS7000P_CTCSS_DEPTH_PM). The tick is on only while a tone is sent.
//
// PC8 always carries the PWM: its mean level is part of the reference's control voltage and the line has no pull
// resistor. With no tone the duty is 50 % (board.c starts the timer there), or the value the bench put in its place.
#include "config.h"
#include <stdbool.h>
#include "cs7000p.h"
#include "subpwm_cs7000p.h"
#include "periph/pwm.h"

#define PWM_TIMER	TIM3
#define PWM_CHANNEL	3u

static uint16_t pc8_idle = CS7000P_SUBPWM_IDLE;			// the duty with no tone
static volatile uint16_t pc8_duty = CS7000P_SUBPWM_IDLE;	// what the timer has now
static volatile bool tx_running;				// the tick's interrupt moves the duty
static volatile uint16_t tx_depth_pm;				// ... by this depth, set before it runs

static void pc8_write(uint16_t duty)
{
	stm32_pwm_set(PWM_TIMER, PWM_CHANNEL, duty);
	pc8_duty = duty;
}

uint16_t cs7000p_subtone_pc8(void)
{
	return pc8_duty;
}

// The bench's idle duty (board chip_reg 3:2). 0 and the whole period are let through here, and only here: a line
// held low or high is driven, not floating, and what that does to the carrier is what the bench wants to see
bool cs7000p_subtone_pc8_set(uint16_t duty)
{
	if (duty > CS7000P_SUBPWM_PERIOD || tx_running) {
		return false;
	}
	pc8_idle = duty;
	pc8_write(duty);
	return true;
}

#if CONFIG_SUBTONE
#include "FreeRTOS.h"
#include "rf/subaudio/subaudio_sel.h"
#include "rf/subaudio/subaudio_rx.h"
#include "rf/subaudio/subaudio_tx.h"

// Smallest tone amplitude the detector accepts, in ADC counts (12 bit): 0 is its default, a guess until the level on
// PA2 has been measured
#define DET_MIN_AMP	0

static subaudio_rx_t det;
static bool det_set;		// a selection was given
// The batch callback feeds the detector. Cleared by the radio task around a change of the detector's state: the
// callback's interrupt runs between two of the task's instructions, never inside one, so no lock is needed
static volatile bool det_live;
static subaudio_tx_t gen;

// --- Receive ---

// The scan's DMA interrupt, every 3.3 ms: 8 samples of the sub-audio rank, oldest first
static void det_batch(const volatile uint16_t *first, uint16_t n, uint16_t stride)
{
	if (!det_live) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		subaudio_rx_feed(&det, (int16_t)first[i * stride]);
	}
}

void cs7000p_subtone_init(void)
{
	subaudio_rx_init(&det, CS7000P_SCAN_HZ, DET_MIN_AMP);
	cs7000p_subaudio_feed(det_batch, NULL);
}

void cs7000p_subtone_rx_set(const subtone_t *tone)
{
	det_live = false;
	subaudio_rx_set(&det, (subaudio_kind_t)subaudio_kind(tone), tone->value, CS7000P_DCS_RX_INVERT != 0);
	det_set = true;
	det_live = true;
}

// Off while the receiver is (transmitting, asleep): the input then carries nothing of a signal's
void cs7000p_subtone_rx_run(bool on)
{
	det_live = on && det_set;
}

bool cs7000p_subtone_rx_match(void)
{
	return !det_set || subaudio_rx_match(&det);
}

// --- Transmit ---

// The scan timer's interrupt, once per scan
static void gen_tick(void)
{
	if (tx_running) {
		pc8_write(cs7000p_subpwm_duty(subaudio_tx_next(&gen), tx_depth_pm));
	}
}

void cs7000p_subtone_tx_stop(void)
{
	if (!tx_running) {
		return;
	}
	tx_running = false;
	cs7000p_subaudio_feed(det_batch, NULL);	// the tick's interrupt off
	pc8_write(pc8_idle);			// never released: back to the idle duty
}

void cs7000p_subtone_tx_start(const subtone_t *tone, uint16_t depth_pm)
{
	cs7000p_subtone_tx_stop();
	subaudio_kind_t kind = (subaudio_kind_t)subaudio_kind(tone);
	if (kind == SUBAUDIO_NONE) {
		return;
	}
	// The generator at its full scale; the depth is the duty rule's (subpwm_cs7000p.h), the caller's for this tone,
	// frequency and bandwidth. What deviation it gives is unmeasured
	subaudio_tx_init(&gen, CS7000P_SCAN_HZ, CS7000P_SUBPWM_FULL, 100);
	subaudio_tx_start(&gen, kind, tone->value, CS7000P_DCS_INVERT != 0);
	tx_depth_pm = depth_pm;	// the tick is off (stopped above) until tx_running
	tx_running = true;
	cs7000p_subaudio_feed(det_batch, gen_tick);
}

bool cs7000p_subtone_tx_tail(bool on)
{
	if (!tx_running) {
		return false;
	}
	// The generator runs in the tick's interrupt
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	bool ok = subaudio_tx_tail(&gen, on);
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	return ok;
}
#endif
