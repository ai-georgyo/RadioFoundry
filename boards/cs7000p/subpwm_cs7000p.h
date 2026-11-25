// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS sub-audio out on PC8 (docs/spec/hardware-cs7000p.md 7.6): the PWM's duty for a generator sample,
// and the depth for the maker's sub-audio level. Pure: tests/cs7000pSubPwmTest.c checks every sample at every depth
// and the level's rule at every level.
//
// PC8 is a PWM into the reference oscillator's control voltage. Its mean level is part of that voltage and the line
// has no pull resistor, so unlike on radios that release such a pin between tones it always carries the PWM: 50 %
// with no tone, and a tone moves the duty around that. It never reaches a constant level: a duty of 0 or of the
// whole period would stop the pin toggling and step the reference. Unverified on a radio, the depth above all.
#ifndef SUBPWM_CS7000P_H
#define SUBPWM_CS7000P_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hal_radio.h"

// TIM3 channel 3 at 96 MHz / 1024 = 93.75 kHz (board.c sets it up at the idle duty)
#define CS7000P_SUBPWM_PERIOD	1024u
#define CS7000P_SUBPWM_IDLE	512u
// The generator's scale (drivers/rf/subaudio/subaudio_tx.h): samples 0..FULL around FULL / 2
#define CS7000P_SUBPWM_FULL	1024u

// Depths are in per mille of half the period: the duty's swing at the generator's full scale. 1000 swings from 0 to
// the whole period (the clamp keeps one count inside each end)
#define CS7000P_SUBPWM_DEPTH_FULL	1000u
// The depth while the maker's table gives none: a transmission on the model's rows or on the bench drive. What
// deviation it gives is unknown (the path's gain to the control voltage is 0.082; the oscillator's pull is in no
// document): a first value for the bench, which sets it against a deviation meter. The same on 12.5 and 25 kHz
#ifdef CS7000P_CTCSS_DEPTH
// The old name was a percent: 30 there is 300 here. A build that still passes it must not mean 3 %
#error "CS7000P_CTCSS_DEPTH is now CS7000P_CTCSS_DEPTH_PM, in per mille of half the period (30 % is 300)"
#endif
#ifndef CS7000P_CTCSS_DEPTH_PM
#define CS7000P_CTCSS_DEPTH_PM	300
#endif
#if CS7000P_CTCSS_DEPTH_PM < 1 || CS7000P_CTCSS_DEPTH_PM > 1000
#error "CS7000P_CTCSS_DEPTH_PM is 1 to 1000, per mille of half the period"
#endif
// DCS polarity on PC8: 1 flips the waveform. Unverified
#ifndef CS7000P_DCS_INVERT
#define CS7000P_DCS_INVERT	0
#endif
// DCS polarity on the detector's input (the sub-audio filter's output): 1 flips it. Unverified, and it may follow
// the side of the first LO
#ifndef CS7000P_DCS_RX_INVERT
#define CS7000P_DCS_RX_INVERT	0
#endif

// The duty for one generator sample: the idle duty plus the sample's distance from mid scale, scaled by depth_pm
// (per mille of half the period, 0..CS7000P_SUBPWM_DEPTH_FULL, more counts as full), kept inside 1..period - 1
static inline uint16_t cs7000p_subpwm_duty(uint16_t sample, uint16_t depth_pm)
{
	int32_t s = (int32_t)(sample > CS7000P_SUBPWM_FULL ? CS7000P_SUBPWM_FULL : sample) -
		    (int32_t)(CS7000P_SUBPWM_FULL / 2u);
	int32_t depth = depth_pm > CS7000P_SUBPWM_DEPTH_FULL ? (int32_t)CS7000P_SUBPWM_DEPTH_FULL : (int32_t)depth_pm;
	// Half the period over half the generator's scale is 1 here, so the swing is the sample's own. At most
	// 512 x 1000 x 512: inside 32 bits
	int32_t swing = s * depth * (int32_t)(CS7000P_SUBPWM_PERIOD / 2u);
	int32_t den = (int32_t)CS7000P_SUBPWM_DEPTH_FULL * (int32_t)(CS7000P_SUBPWM_FULL / 2u);
	int32_t duty = (int32_t)CS7000P_SUBPWM_IDLE + (swing + (swing < 0 ? -den / 2 : den / 2)) / den;
	if (duty < 1) {
		duty = 1;
	} else if (duty > (int32_t)CS7000P_SUBPWM_PERIOD - 1) {
		duty = (int32_t)CS7000P_SUBPWM_PERIOD - 1;
	}
	return (uint16_t)duty;
}

// The maker's sub-audio level L (its table's CTCSS or DCS level at f_tx, a 25 kHz channel's doubled: makercal_cs7000p.h)
// as a depth. The maker's PWM on this pin swings about plus or minus 4 L counts of its 2100-count period
// (calibration-cs7000p.md 5.5), 4 L of half the period's 1050: in per mille, to the nearest, (4000 L + 525) / 1050.
// The same share of half the period here: the filter after PC8 passes the duty's swing, not the timer's counts, so
// the clock and the period do not enter. Inferred, unmeasured: the deviation it gives is the maker's only if the
// tone path after the pin and the 50 % idle (the maker's is 48.76 %) leave the swing's effect as it was. L = 40:
// 152; L = 254, a 25 kHz channel's top: 968; never over 1000
static inline uint16_t cs7000p_subpwm_depth_pm(uint8_t level)
{
	return (uint16_t)(((uint32_t)level * 4000u + 525u) / 1050u);
}

// --- subtone_cs7000p.c (target only) ---

// PC8's duty now, and the bench's own idle duty in place of 50 % (0..period: the bench may hold the line low or
// high to see what it does to the carrier). False: out of range, or a tone is being sent
uint16_t cs7000p_subtone_pc8(void);
bool cs7000p_subtone_pc8_set(uint16_t duty);
#if CONFIG_SUBTONE
void cs7000p_subtone_init(void);
// The detector for a channel's selection (restarts it), paused while the receiver is off, and its verdict (true
// with no selection)
void cs7000p_subtone_rx_set(const subtone_t *tone);
void cs7000p_subtone_rx_run(bool on);
bool cs7000p_subtone_rx_match(void);
// The generator while transmitting FM, at depth_pm (cs7000p_subpwm_duty; the radio chooses it: rfops_cs7000p.h
// cs7000p_rf_tone_depth); stop puts PC8 back on its idle duty. The DCS turn-off tail: false when no DCS is being sent
void cs7000p_subtone_tx_start(const subtone_t *tone, uint16_t depth_pm);
void cs7000p_subtone_tx_stop(void);
bool cs7000p_subtone_tx_tail(bool on);
#endif

#endif
