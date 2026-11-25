// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS RF calibration (docs/spec/hardware-cs7000p.md 13): the model's tables, how they are read, and the
// verdict "this radio may transmit here". Pure: tests/cs7000pCalTest.c runs it on the tables as shipped and on a
// made-up one.
//
// No value is guessed: every table in the source is unset, so the verdict is "no" at every frequency and level, in FM
// and in DMR. The values come from the bench (docs/testing/power-calibration.md for the PA drives, a deviation meter
// for the modulation pairs, a counter for the reference). The maker's own factory table (data flash
// 0x001000-0x0011FF, docs/spec/calibration-cs7000p.md, read by makercal_cs7000p.h) serves receive only so far: the
// front-end tune, the receive reference level and the RSSI scale (the cs7000p_cal_*_m getters below), each from its
// own field group, with today's value whenever that group or that lookup gives none. Its FM transmit values (the
// drive, the pair, trim A: cs7000p_cal_maker_fm) and DMR's (the drive, the DMR pair, trim B: cs7000p_cal_maker_dmr)
// serve a transmission only behind the owner's arm, which the RF chain asks (rfops_cs7000p.h).
//
// A row is nine points, 400 to 520 MHz in steps of 15 MHz, read with linear interpolation between two points and
// held flat past the ends. A row with one point unset is unset as a whole: half a row is never used.
#ifndef RFCAL_CS7000P_H
#define RFCAL_CS7000P_H

#include <stdint.h>
#include <stdbool.h>
#include "makercal_cs7000p.h"

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
// Receive takes the maker's table (1), or never (0): today's tune, reference level and RSSI scale whatever the table
// holds, the lasting way back should a valid table receive worse than today's values (the bench's 3:0, 3:4 and 3:8
// are RAM only). The table is still read, checked, shown and copied either way, and transmit is not affected
#ifndef CS7000P_RX_MAKER
#define CS7000P_RX_MAKER	1
#endif
#if CS7000P_RX_MAKER != 0 && CS7000P_RX_MAKER != 1
#error "CS7000P_RX_MAKER is 0 or 1"
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

// --- Receive, the maker's table first (hardware-cs7000p.md 13.1) ---
// m is the table in use (the window's or the copy's record): NULL, or one not valid, is none. Each value comes from
// the maker's table only when its own field group passed and its lookup gives one, and the build has
// CS7000P_RX_MAKER, else from the model and the build's default exactly as without the table, so a refused table
// changes nothing. src (may be NULL): where the value came
// from. The bench's knobs (chip_reg 3:0, 3:4, 3:8) are the caller's, and win over all of this
enum { CS7000P_SRC_DEFAULT = 0, CS7000P_SRC_MAKER = 1, CS7000P_SRC_BENCH = 2 };
// Why the receive reference level is not trim A (cs7000p_cal_rx_ref_m)
enum {
	CS7000P_RXREF_WHY_NONE = 0,	// trim A is in use
	CS7000P_RXREF_WHY_TABLE,	// no valid table in use
	CS7000P_RXREF_WHY_GROUP,	// the table's TRIM_A group was refused
	CS7000P_RXREF_WHY_BOUND,	// trim A is farther than CS7000P_MAKER_RX_TRIM_MAX from 128
	CS7000P_RXREF_WHY_BUILD,	// the build has CS7000P_RX_MAKER 0
};
// The front end's tuning value for hz: the tune row (0x044 at f_rx, 16 c, CS7000P_TUNE_MIN..MAX), else
// cs7000p_cal_rx_tune(c, hz)
uint16_t cs7000p_cal_rx_tune_m(const cs7000p_cal_t *c, const cs7000p_maker_t *m, uint32_t hz, uint8_t *src);
// The reference offset the held level takes in receive (the bias 0x200 + it under the measured hold): trim A's d
// within CS7000P_MAKER_RX_TRIM_MAX, else cs7000p_cal_ref_offset(c). why (may be NULL): CS7000P_RXREF_WHY_*. The bias
// only: 4:0x04 keeps cs7000p_cal_ref_offset's value in receive, as what 4:0x04 does under the hold is not measured
int8_t cs7000p_cal_rx_ref_m(const cs7000p_cal_t *c, const cs7000p_maker_t *m, uint8_t *src, uint8_t *why);
// The RSSI in dBm: the table's two points on the 12-bit reading (raw12 >> 4), else cs7000p_rssi_dbm(mv). The FM
// squelch never reads it (it is the noise reading's), so FM receive cannot change through it
int16_t cs7000p_cal_rssi_m(const cs7000p_maker_t *m, uint32_t raw12, uint32_t mv, uint8_t *src);

// --- Transmit, the maker's table (hardware-cs7000p.md 13.2): served behind the owner's arm ---
// The table in use (m: NULL or not valid is none) holds every value a transmission in this mode at this power level
// would need at hz: FM the level's drive, the FM pair and trim A; DMR also the DMR pair, trim B and the level's burst
// fields (checked though not applied: a damaged timing field is a damaged table). Each from its own field group,
// with the reader's transmit rules at hz (the band, the VCO switch, no clamped, wrapped, 0 or 255 result). Asks
// nothing of the arm: this is what an armed key-up would find. A sub-audio tone is not among them: a tone channel
// is never served from the table yet (its level would be a guess)
bool cs7000p_cal_maker_tx(const cs7000p_maker_t *m, uint32_t hz, uint8_t level, bool dmr);
// An FM transmission's values from the table in use, f_tx = hz, power level level (0 the low row 0x074, 1 the high row
// 0x06C): the drive (16 c, 12-bit), the FM pair (mod2 to 4:0x45, mod1 to 4:0x46) and trim A as 4:0x04's offset
// (d_A, the maker's own coding there). False, v as it was: cs7000p_cal_maker_tx(m, hz, level, false) is false. The
// arm is not asked here: the caller's (cs7000p_rf_maker_fm, rfops_cs7000p.h) is the only one that serves these
typedef struct {
	uint16_t drive;
	uint8_t mod2, mod1;
	int8_t trim;
} cs7000p_fm_tx_t;
bool cs7000p_cal_maker_fm(const cs7000p_maker_t *m, uint32_t hz, uint8_t level, cs7000p_fm_tx_t *v);
// A DMR transmission's values from the table in use, f_tx = hz, power level level (the rows as FM's): the drive (16
// c, 12-bit; P, not the maker's P + step of its burst rise: the factory set P, calibration-cs7000p.md 5.4), the DMR
// pair (0x0B4 to 4:0x45, 0x0BC to 4:0x46) and trim B as 4:0x04's offset (d_B, d mod 256 there, spec 5.2). The
// level's burst fields are checked, not given: the port's key-up times the bursts. False, v as it was:
// cs7000p_cal_maker_tx(m, hz, level, true) is false. The arm is not asked here (cs7000p_rf_maker_dmr)
typedef struct {
	uint16_t drive;
	uint8_t mod2, mod1;
	int8_t trim;
} cs7000p_dmr_tx_t;
bool cs7000p_cal_maker_dmr(const cs7000p_maker_t *m, uint32_t hz, uint8_t level, cs7000p_dmr_tx_t *v);
// The table in use holds an FM transmission's values somewhere in the transmit ranges, at some level (every 100 kHz
// from 400 to 512 MHz): what the view shows as cal=maker, a radio that transmits there once the owner arms it
bool cs7000p_cal_maker_any_fm(const cs7000p_maker_t *m);
// ... a DMR transmission's, the same way (cal=maker on a build with DMR, where the top stage serves it)
bool cs7000p_cal_maker_any_dmr(const cs7000p_maker_t *m);

// The IF chip's RSSI output in millivolts as dBm at its input, from the data sheet's two typical points (0.3 V at
// -100 dBm, 2.0 V at -30 dBm), less CS7000P_FE_GAIN_DB. A relative reading until it has been measured
int16_t cs7000p_rssi_dbm(uint32_t mv);
// The IF chip's noise level (12-bit raw reading) on the squelch's scale: larger = more noise. 255 while the receiver
// is off: it can't hear
uint8_t cs7000p_noise(uint32_t raw, bool receiving);

#endif
