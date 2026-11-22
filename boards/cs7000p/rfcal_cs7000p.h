// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS RF calibration (docs/spec/hardware-cs7000p.md 13): the model's tables, how they are read, and the
// verdict "this radio may transmit here". Pure: tests/cs7000pCalTest.c runs it on the tables as shipped and on a
// made-up one.
//
// No calibration of this radio is known and none is guessed: every table in the source is unset, so the verdict is
// "no" at every frequency and level, in FM and in DMR. The values come from the bench (docs/testing/
// power-calibration.md for the PA drives, a deviation meter for the modulation pairs, a counter for the reference).
// The maker's own calibration, wherever it is in the data flash, is never read.
//
// A row is nine points, 400 to 520 MHz in steps of 15 MHz, read with linear interpolation between two points and
// held flat past the ends. A row with one point unset is unset as a whole: half a row is never used.
#ifndef RFCAL_CS7000P_H
#define RFCAL_CS7000P_H

#include <stdint.h>
#include <stdbool.h>

#define CS7000P_POWER_LEVELS	2u		// board.c's power names: 1 W, 4 W
#define CS7000P_CAL_POINTS	9u
#define CS7000P_CAL_FIRST_HZ	400000000u
#define CS7000P_CAL_STEP_HZ	15000000u
#define CS7000P_CAL_UNSET	0xFFFFu		// a point nobody measured
#define CS7000P_DAC_MAX		4095u		// the shared DAC's range: the largest PA drive
// The receive tuning value stays inside the DAC buffer's linear range: 0.2 V from either rail
#define CS7000P_TUNE_MIN	250u
#define CS7000P_TUNE_MAX	3850u

// The front-end tuning DAC code while the table has none: mid scale, unverified
#ifndef CS7000P_RX_TUNE_NOMINAL
#define CS7000P_RX_TUNE_NOMINAL	2048
#endif
// The modem's reference offset (hrc6000_set_ref_offset) while none is measured. Unverified: which register sets
// the reference oscillator's control voltage and what level is on frequency are the first RF measurement
#ifndef CS7000P_REF_OFFSET
#define CS7000P_REF_OFFSET	0
#endif
// The gain ahead of the IF chip, dB, taken off its RSSI reading. Unmeasured: the reading is not an absolute level
#ifndef CS7000P_FE_GAIN_DB
#define CS7000P_FE_GAIN_DB	0
#endif

typedef struct {
	uint16_t drive[CS7000P_POWER_LEVELS][CS7000P_CAL_POINTS];	// PA drive, DAC codes 1..4095
	uint16_t floor;				// the largest drive that gives no RF output (0: none measured)
	uint16_t rx_tune[CS7000P_CAL_POINTS];	// the front end's tuning voltage, DAC codes
	// The modem's two-point modulation pair (4:0x45, 4:0x46), 0..255 each, for FM and for DMR
	uint16_t fm_mod2[CS7000P_CAL_POINTS], fm_mod1[CS7000P_CAL_POINTS];
	uint16_t dmr_mod2[CS7000P_CAL_POINTS], dmr_mod1[CS7000P_CAL_POINTS];
	bool ref_set;				// ref_offset was measured
	int8_t ref_offset;
} cs7000p_cal_t;

// The tables in the source: all unset
extern const cs7000p_cal_t cs7000p_cal_model;

// The PA drive for a power level at hz. False: no such level, the row is unset, or it holds a value no DAC takes
bool cs7000p_cal_drive(const cs7000p_cal_t *c, uint32_t hz, uint8_t level, uint16_t *drive);
// The modulation pair for hz. False: unset
bool cs7000p_cal_mod_pair(const cs7000p_cal_t *c, uint32_t hz, bool dmr, uint8_t *mod2, uint8_t *mod1);
// The reference offset to write: the measured one, or CS7000P_REF_OFFSET
int8_t cs7000p_cal_ref_offset(const cs7000p_cal_t *c);
// The front end's tuning value for hz: the table's, or CS7000P_RX_TUNE_NOMINAL, inside CS7000P_TUNE_MIN..MAX
uint16_t cs7000p_cal_rx_tune(const cs7000p_cal_t *c, uint32_t hz);
static inline uint16_t cs7000p_cal_tune_clamp(uint32_t v)
{
	return (uint16_t)(v < CS7000P_TUNE_MIN ? CS7000P_TUNE_MIN : v > CS7000P_TUNE_MAX ? CS7000P_TUNE_MAX : v);
}

// This radio may transmit on hz at this level in this mode: the level's PA row, the mode's modulation pair and the
// reference offset are all set, and the drive is over the floor. The band is not asked here (rfplan_cs7000p.h)
bool cs7000p_cal_tx_ok(const cs7000p_cal_t *c, uint32_t hz, uint8_t level, bool dmr);
// Some level could transmit in some mode: what the view shows as cal=model (false: cal=none, nothing transmits)
bool cs7000p_cal_any_tx(const cs7000p_cal_t *c);

// The IF chip's RSSI output in millivolts as dBm at its input, from the data sheet's two typical points (0.3 V at
// -100 dBm, 2.0 V at -30 dBm), less CS7000P_FE_GAIN_DB. A relative reading until it has been measured
int16_t cs7000p_rssi_dbm(uint32_t mv);
// The IF chip's noise level (12-bit raw reading) on the squelch's scale: larger = more noise. 255 while the receiver
// is off: it can't hear
uint8_t cs7000p_noise(uint32_t raw, bool receiving);

#endif
