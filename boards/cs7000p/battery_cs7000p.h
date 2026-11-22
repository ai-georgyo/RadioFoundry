// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS power: the pure part (docs/spec/hardware-cs7000p.md 5). The scales of the supply sense, of the ADC
// and of the volume pot, the rule for holding the supply, and the rule for coming back after a power-off the knob
// overrode. No hardware access: power_cs7000p.c feeds them, tests/cs7000pBatteryTest.c checks them. Every figure is
// from the schematic and unverified on a radio.
//
// POW_DET is the only analog path from the battery, and it is behind the knob's switch: knob off reads 0 V within a
// millisecond, whatever holds the supply. It is not a plain divider. The node also feeds the latch transistor's
// input through a diode, so POW_DET = 0.2496 x Vbat + 0.061 V (10k from the switch; 100k / 47k to the pin; a diode
// drop of 0.5 V and a base-emitter drop of 0.65 V into the transistor's 47k input resistor). That resistor's
// tolerance alone moves the result by 4 % either way: an accurate reading needs a per-unit value.
#ifndef BATTERY_CS7000P_H
#define BATTERY_CS7000P_H

#include <stdint.h>
#include <stdbool.h>
#include "cs7000p.h"
#include "radio/power_switch.h"

// Vbat in mV = (gain x POW_DET in mV) / 1000 - offset: the model above turned round (bench knobs,
// docs/testing/cs7000p-bringup.md)
#ifndef CS7000P_BAT_GAIN_PPK
#define CS7000P_BAT_GAIN_PPK	4006
#endif
#ifndef CS7000P_BAT_OFFSET_MV
#define CS7000P_BAT_OFFSET_MV	245
#endif
// The VOL-DET reading with the knob at full volume. No source has it: the pot's top is fed DC through 10k from the
// 5 V rail and the pot's own resistance is not on the drawing. The ADC's full scale until it is measured, so the
// position reads low rather than wrong at the top
#ifndef CS7000P_VOL_FULL_RAW
#define CS7000P_VOL_FULL_RAW	4095
#endif

#define CS7000P_ADC_FULL_SCALE	4095u
// The 3.3 V rail by its regulator's divider, for a reading taken before (or without) the chip's own measurement
#define CS7000P_VDDA_NOMINAL_MV	3240u

// The knob's switch open: POW_DET under this. Open, the node behind the switch has no source and the pin reads 0 V,
// whatever holds the supply and whatever the battery (hardware-cs7000p.md 5); closed, it reads about a quarter of the
// battery: 1.56 V at 6.0 V, over 1 V for a pack sagged to 4.0 V. So this level is far from both, and a pack that
// sags under a transmission never reads as a switch-off: a flat battery is the core's battery monitor's business,
// on the shared thresholds
#define CS7000P_KNOB_OPEN_POW_DET_MV	700u
// The same level as a battery reading by the model below, the unit every rule here takes
#define CS7000P_KNOB_OPEN_MV	((uint32_t)(CS7000P_BAT_GAIN_PPK) * CS7000P_KNOB_OPEN_POW_DET_MV / 1000u - \
				 (uint32_t)(CS7000P_BAT_OFFSET_MV))
_Static_assert(CS7000P_KNOB_OPEN_MV > 0u && CS7000P_KNOB_OPEN_MV < PWRSW_OFF_MV,
	       "the open threshold is a battery reading over 0 and under the shared off threshold");

// A raw 12-bit reading as mV at the pin
static inline uint32_t cs7000p_adc_to_mv(uint32_t raw, uint32_t vdda_mv)
{
	return raw * vdda_mv / CS7000P_ADC_FULL_SCALE;
}

// The battery from POW_DET. 0 with the knob off (under 62 mV with the default constants): no reading
static inline uint32_t cs7000p_battery_mv(uint32_t pow_det_mv)
{
	const uint32_t scaled = (uint32_t)(CS7000P_BAT_GAIN_PPK) * pow_det_mv;	// a 16-bit mV figure can't overflow it
	const uint32_t offset = (uint32_t)(CS7000P_BAT_OFFSET_MV) * 1000u;
	return scaled > offset ? (scaled - offset) / 1000u : 0u;
}

// The volume knob's position, 0..100
static inline uint8_t cs7000p_volume_pct(uint32_t raw)
{
	const uint32_t pct = raw * 100u / (uint32_t)(CS7000P_VOL_FULL_RAW);
	return (uint8_t)(pct > 100u ? 100u : pct);
}

// The hold rule. The expander keeps the hold through an MCU reset and the ROM bootloader never writes it, so the
// supply is held only on evidence that the knob is on: the scan runs, and every one of the first readings (mv, the
// battery by the model, PWRSW_SAMPLE_MS apart) is at or above the open threshold. One reading below: this start is a
// reset under a supply that is going away (or a debugger's), and the hold stays clear. No ADC: the knob can't be
// read, the hold is never set, and the radio runs for as long as the knob powers it
#define CS7000P_HOLD_READINGS	PWRSW_SAMPLES

static inline cs7000p_knob_t cs7000p_knob_from_readings(bool adc_ok, const uint32_t *mv, unsigned n)
{
	if (!adc_ok) {
		return CS7000P_KNOB_NOADC;
	}
	if (n < CS7000P_HOLD_READINGS) {
		return CS7000P_KNOB_OFF;
	}
	for (unsigned i = 0; i < n; i++) {
		if (mv[i] < CS7000P_KNOB_OPEN_MV) {
			return CS7000P_KNOB_OFF;
		}
	}
	return CS7000P_KNOB_ON;
}

// Only a knob read on holds the supply
static inline bool cs7000p_knob_holds(cs7000p_knob_t knob)
{
	return knob == CS7000P_KNOB_ON;
}

// What the shared debounce (pwrsw_on, which knows one threshold, PWRSW_OFF_MV) is fed for one battery reading: 0
// for the switch open, a figure over its threshold for anything else, a flat or sagging pack included
static inline uint32_t cs7000p_knob_reading(uint32_t mv)
{
	return mv < CS7000P_KNOB_OPEN_MV ? 0u : mv < PWRSW_OFF_MV ? (uint32_t)PWRSW_OFF_MV : mv;
}

// hal_power_t.switch_on. reading_on: the debounced reading (pwrsw_on), asked only while the knob was on. Without an
// ADC the answer is always "on": the radio must not switch itself off on a reading it does not have
static inline bool cs7000p_knob_switch_on(cs7000p_knob_t knob, bool reading_on)
{
	if (knob == CS7000P_KNOB_NOADC || knob == CS7000P_KNOB_UNKNOWN) {
		return true;
	}
	return knob == CS7000P_KNOB_ON && reading_on;
}

// After the hold was cleared for a power-off. If the radio is still running, the knob powers it: it stays in its off
// state for as long as the knob stays where it is, and restarts only when the reading has been under the open
// threshold and has come back, the user switching off and on again faster than the supply falls. Never before
// CS7000P_OFF_SETTLE_MS after the hold was cleared, the time the supply is given to go away, and never without a
// reading of the switch open: a power-off the radio asked for itself (a critical battery) is not undone, a pack that
// sags and recovers is not the knob, and nothing here loops through resets
#define CS7000P_OFF_SETTLE_MS	500u

typedef struct {
	bool seen_off;		// a reading of the switch open since (or right before) the power-off
	uint8_t on_count;	// readings in a row of it closed since
} cs7000p_offwatch_t;

static inline void cs7000p_offwatch_start(cs7000p_offwatch_t *w, bool knob_read_off)
{
	w->seen_off = knob_read_off;
	w->on_count = 0;
}

// One battery reading, at least PWRSW_SAMPLE_MS after the one before, since_ms after the hold was cleared. True:
// restart
static inline bool cs7000p_offwatch_restart(cs7000p_offwatch_t *w, uint32_t mv, uint32_t since_ms)
{
	if (mv < CS7000P_KNOB_OPEN_MV) {
		w->seen_off = true;
		w->on_count = 0;
		return false;
	}
	if (!w->seen_off) {
		return false;
	}
	if (w->on_count < PWRSW_SAMPLES) {
		w->on_count++;
	}
	return w->on_count >= PWRSW_SAMPLES && since_ms >= CS7000P_OFF_SETTLE_MS;
}

#endif
