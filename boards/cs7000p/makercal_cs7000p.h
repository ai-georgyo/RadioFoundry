// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS: the maker's factory calibration table, read, checked and looked up
// (docs/spec/calibration-cs7000p.md). Pure: no flash, no RTOS, no pins; tests/cs7000pMakerCalTest.c runs it on
// invented tables. Nothing on the radio calls it yet: reading the table, choosing its source and using its values
// come later, each behind its own fallback (docs/spec/hardware-cs7000p.md 13).
//
// The table is 512 bytes (data flash 0x001000-0x0011FF). The maker's program checks none of its values (spec 2.2),
// so this reader refuses what the spec's section 7 refuses, in two kinds of rule:
// - integrity: spec 7's rules, with the reader's own trim bound where spec 7 leaves it to the reader. A table failing
//   one is not valid (cs7000p_maker_check() false): it is never copied, never restored, and nothing is looked up in
//   it. Its group's bit is clear too.
// - use: the port's own caution beyond spec 7. A table failing only these is valid; only that group's bit is clear,
//   so what depends on the group falls back to today's value (or, for transmit, stays refused).
// The arithmetic is the maker's (spec 4.3-4.4), exact, with every case the maker's 32-bit sums would have wrapped
// flagged instead. Lookups add the port's rules on top: VCO switch windows, extension past the end points, the band.
#ifndef MAKERCAL_CS7000P_H
#define MAKERCAL_CS7000P_H

#include <stdint.h>
#include <stdbool.h>

#define CS7000P_MAKER_SIZE	512u
#define CS7000P_MAKER_POINTS	8u

// The layout (spec 3), offsets from the table's start. Points are u32 big-endian Hz, the burst hold and PA-off
// arrays u16 little-endian, every other row one byte per point
#define CS7000P_MK_TX_POINTS	0x000u
#define CS7000P_MK_RX_POINTS	0x020u
#define CS7000P_MK_RSSI_LO	0x040u	// RSSI code at the low reference level (-120 dBm, inferred)
#define CS7000P_MK_RSSI_HI	0x041u	// at the high one (-70 dBm, inferred)
#define CS7000P_MK_VOX9		0x042u	// shown at most: the port has no VOX
#define CS7000P_MK_VOX0		0x043u
#define CS7000P_MK_TRIM_A	0x0DCu	// receive and FM transmit, offset binary
#define CS7000P_MK_BATTERY	0x0DDu	// a POW_DET code: a cross-check, shown only
#define CS7000P_MK_TONE_IN	0x0DFu	// the tone input's DC level: shown at most, the port has no tone input
#define CS7000P_MK_TRIM_B	0x174u	// DMR transmit, offset binary

// The rows, one byte per point. rx: looked up at f_rx on the RX points, else at f_tx on the TX points (spec 4.2)
typedef enum {
	CS7000P_MK_TUNE,		// 0x044 rx: the front-end tune, a PA4 code (16 c in 12 bits)
	CS7000P_MK_SQ9_OPEN,		// 0x04C rx: squelch level 9, open (noise codes)
	CS7000P_MK_SQ9_CLOSE,		// 0x054 rx
	CS7000P_MK_SQ1_OPEN,		// 0x05C rx: squelch level 1, open
	CS7000P_MK_SQ1_CLOSE,		// 0x064 rx
	CS7000P_MK_POWER_HI,		// 0x06C: the drive at every nonzero power level, a PA4 code
	CS7000P_MK_POWER_LO,		// 0x074: the drive at level 0
	CS7000P_MK_CTCSS_67,		// 0x084: sub-audio level at 67.0 Hz
	CS7000P_MK_CTCSS_254,		// 0x08C: at 254.1 Hz
	CS7000P_MK_DCS,			// 0x094
	CS7000P_MK_TONE_SINGLE,		// 0x09C: the tone DAC's levels, shown at most (the port has no tone DAC)
	CS7000P_MK_TONE_DTMF,		// 0x0A4
	CS7000P_MK_TONE_MSK,		// 0x0AC
	CS7000P_MK_DMR_MOD2,		// 0x0B4: DMR pair, to 4:0x45
	CS7000P_MK_DMR_MOD1,		// 0x0BC: to 4:0x46
	CS7000P_MK_FM_MOD2,		// 0x0C4: FM pair, to 4:0x45
	CS7000P_MK_FM_MOD1,		// 0x0CC: to 4:0x46
	CS7000P_MK_HI_ADV,		// 0x0EC: DMR high power, slot advance (4:0x12 bits 5-0)
	CS7000P_MK_HI_START,		// 0x0F4: ramp start S, 2-tick units
	CS7000P_MK_HI_RISE,		// 0x0FC: rise end R
	CS7000P_MK_LO_ADV,		// 0x124: DMR low power
	CS7000P_MK_LO_START,		// 0x12C
	CS7000P_MK_LO_RISE,		// 0x134
	CS7000P_MK_RSSI_LO_ROW,		// 0x15C rx: the tuner's per-point RSSI readings, shown at most
	CS7000P_MK_RSSI_HI_ROW,		// 0x164 rx
	CS7000P_MK_ROWS
} cs7000p_mk_row_t;

// The u16 rows (little-endian)
typedef enum {
	CS7000P_MK_HI_HOLD,		// 0x104: hold end H, 2-tick units
	CS7000P_MK_HI_OFF,		// 0x114: PA off F
	CS7000P_MK_LO_HOLD,		// 0x13C
	CS7000P_MK_LO_OFF,		// 0x14C
	CS7000P_MK_ROWS16
} cs7000p_mk_row16_t;

// The field groups, a bit each in cs7000p_maker_t.fields: set when the group passed every rule of both kinds
#define CS7000P_MK_F_TUNE	0x0001u
#define CS7000P_MK_F_POWER	0x0002u
#define CS7000P_MK_F_FM_PAIR	0x0004u
#define CS7000P_MK_F_DMR_PAIR	0x0008u
#define CS7000P_MK_F_TRIM_A	0x0010u
#define CS7000P_MK_F_TRIM_B	0x0020u
#define CS7000P_MK_F_SQUELCH	0x0040u
#define CS7000P_MK_F_RSSI	0x0080u
#define CS7000P_MK_F_CTCSS	0x0100u
#define CS7000P_MK_F_DCS	0x0200u
#define CS7000P_MK_F_BURST_HI	0x0400u
#define CS7000P_MK_F_BURST_LO	0x0800u
#define CS7000P_MK_F_BATTERY	0x1000u
#define CS7000P_MK_F_ALL	0x1FFFu

// Whole-table states: anything but OK refuses every field
typedef enum {
	CS7000P_MK_UNREAD,		// no bytes given
	CS7000P_MK_OK,
	CS7000P_MK_BLANK,		// all 0xFF
	CS7000P_MK_ZERO,		// all 0x00
	CS7000P_MK_POINTS,		// a frequency point unset, out of range or out of order
} cs7000p_mk_state_t;

// Why: the first failing rule (an integrity rule before a use rule), with the byte it failed at
typedef enum {
	CS7000P_MK_WHY_NONE,
	CS7000P_MK_WHY_UNREAD,
	CS7000P_MK_WHY_SIZE,		// not 512 bytes
	CS7000P_MK_WHY_BLANK,
	CS7000P_MK_WHY_ZERO,
	CS7000P_MK_WHY_PT_UNSET,	// a point at 0xFFFFFFFF
	CS7000P_MK_WHY_PT_RANGE,	// a point outside 400.0-527.0 MHz
	CS7000P_MK_WHY_PT_ORDER,	// a set not strictly rising
	// Integrity, per group
	CS7000P_MK_WHY_ROW_UNSET,	// a row the maker's program uses with a point at 0xFF (u16: 0xFFFF)
	CS7000P_MK_WHY_CODE,		// a tune, power or pair code outside 1-254
	CS7000P_MK_WHY_POWER_ORDER,	// high not above low at a point
	CS7000P_MK_WHY_POWER_FLOOR,	// 16 c at or under the PA floor
	CS7000P_MK_WHY_TRIM,		// farther than CS7000P_MAKER_TRIM_MAX from 128
	CS7000P_MK_WHY_SQ_HYST,		// open not below close at a point
	CS7000P_MK_WHY_SQ_LEVEL,	// level 1 not above level 9
	CS7000P_MK_WHY_RSSI_ORDER,	// 0x041 not above 0x040
	CS7000P_MK_WHY_BURST_START,	// S not below R
	CS7000P_MK_WHY_BURST_RISE,	// N = 2 (R - S) under 5
	CS7000P_MK_WHY_BURST_HOLD,	// H not after R
	CS7000P_MK_WHY_BURST_OFF,	// F before H
	// Use only
	CS7000P_MK_WHY_RSSI_END,	// 0x040 or 0x041 at 0x00 or 0xFF
	CS7000P_MK_WHY_LEVEL_ZERO,	// a sub-audio level of 0
	CS7000P_MK_WHY_BURST_U16,	// a u16 array whose values differ (only the equal case's form is known)
	CS7000P_MK_WHY_BATTERY,		// 0x00 or 0xFF
	CS7000P_MK_WHY_COUNT
} cs7000p_mk_why_t;

// The reader's own bounds (spec 7 leaves them to the reader; conservative, owner's to widen)
// A trim farther than this from 128 is not this oscillator's: 64 steps is about 1.3 kHz at 433 MHz (spec 5.2's 18-21
// Hz a step), about 3 ppm
#define CS7000P_MAKER_TRIM_MAX		64
// Receive uses trim A only this close to 128: about 650 Hz, what the bring-up's passband sweep allows on frequency
// at mid-scale. Farther, the bias keeps today's level (a fallback, not a refusal of the group)
#define CS7000P_MAKER_RX_TRIM_MAX	32
// Transmit takes the same-VCO end value inside a switch segment at most this wide, and refuses inside a wider one
#define CS7000P_MAKER_WINDOW_MAX_HZ	1000000u
// The last 10 Hz step on the low VCO (expander bit 8 = 1, spec 5.12): receive below 479.00001 MHz, transmit below
// 430.00001 MHz. Points on either side of these hold that VCO's values
#define CS7000P_MAKER_RX_SWITCH_10HZ	47900000u
#define CS7000P_MAKER_TX_SWITCH_10HZ	43000000u
// The points' range (spec 7)
#define CS7000P_MAKER_POINT_MIN_HZ	400000000u
#define CS7000P_MAKER_POINT_MAX_HZ	527000000u
// The RSSI reference levels (from the service manual's item order: inferred)
#define CS7000P_MAKER_RSSI_LO_DBM	(-120)
#define CS7000P_MAKER_RSSI_HI_DBM	(-70)
// The sub-audio interpolation's x: the tone in 0.1 Hz, 67.0 to 254.1 Hz (spec 4.5)
#define CS7000P_MAKER_CTCSS_X0		670
#define CS7000P_MAKER_CTCSS_X1		2541

// The arithmetic's flags (OR'ed in)
#define CS7000P_MK_OVERFLOW	0x01u	// a 32-bit product or sum of the maker's would have wrapped: refused
#define CS7000P_MK_CLAMP	0x02u	// the result was held at 0 or 255

// Why a lookup gave no value
typedef enum {
	CS7000P_MK_LK_OK,
	CS7000P_MK_LK_TABLE,		// no table, or one not valid
	CS7000P_MK_LK_GROUP,		// the field's group was refused
	CS7000P_MK_LK_ROW,		// not a row of this direction, or no such power level
	CS7000P_MK_LK_BAND,		// outside the band for the direction
	CS7000P_MK_LK_SWITCH,		// past the end points across a VCO switch: another VCO's value
	CS7000P_MK_LK_WINDOW,		// inside a VCO switch segment wider than CS7000P_MAKER_WINDOW_MAX_HZ
	CS7000P_MK_LK_MATH,		// overflowed, or (transmit) clamped
	CS7000P_MK_LK_CODE,		// (transmit) a result of 0 or 255
	CS7000P_MK_LK_FLOOR,		// a drive at or under the PA floor
	CS7000P_MK_LK_BURST,		// burst fields that fail spec 7's burst rules at this frequency
	CS7000P_MK_LK_COUNT
} cs7000p_mk_lookup_t;

typedef struct {
	uint8_t state;			// cs7000p_mk_state_t
	uint8_t why;			// cs7000p_mk_why_t
	uint16_t why_off;		// the failing byte's offset in the table (0 with WHY_NONE)
	uint16_t fields;		// CS7000P_MK_F_*: the groups that passed
	bool valid;			// state OK and every integrity rule passed: cs7000p_maker_check()
	uint16_t floor12;		// the PA floor the table was checked against, 12-bit DAC code
	uint32_t crc;			// as the caller gave it (CRC-32 of the 512 bytes): shown, the arm's identity
	uint32_t tx_hz[CS7000P_MAKER_POINTS], rx_hz[CS7000P_MAKER_POINTS];
	uint32_t tx_p10[CS7000P_MAKER_POINTS], rx_p10[CS7000P_MAKER_POINTS];	// the same in 10 Hz units (spec 4.2)
	uint8_t row[CS7000P_MK_ROWS][CS7000P_MAKER_POINTS];
	uint16_t row16[CS7000P_MK_ROWS16][CS7000P_MAKER_POINTS];
	uint8_t rssi_lo, rssi_hi, vox9, vox0, trim_a, trim_b, battery, tone_in;
} cs7000p_maker_t;

// The maker's line through (x0, y0) and (x1, y1) at x (spec 4.4), computed wide: OVERFLOW when the maker's 32-bit
// product or sum would have wrapped, CLAMP when held at 0 or 255. x0 = x1: the two values' mean
uint8_t cs7000p_maker_line(int32_t x0, int32_t x1, uint8_t y0, uint8_t y1, int32_t x, uint8_t *flags);
// A row at hz on points in 10 Hz units (spec 4.3): the segment that ends at or after the frequency, extended past
// the ends. The raw arithmetic, no window, band or code rule
uint8_t cs7000p_maker_row(const uint32_t p10[CS7000P_MAKER_POINTS], const uint8_t y[CS7000P_MAKER_POINTS], uint32_t hz,
			  uint8_t *flags);

// Decode and check the 512 bytes (raw NULL: state UNREAD). crc is the caller's CRC-32 of them, floor12 the PA floor
// (CS7000P_APC_FLOOR). Returns m->valid
bool cs7000p_maker_parse(const uint8_t *raw, uint32_t crc, uint16_t floor12, cs7000p_maker_t *m);
// The integrity rules alone, without decoding (a small stack): what a copy or a restore of the table is checked
// with. why and off may be NULL
bool cs7000p_maker_check(const uint8_t *raw, uint32_t n, uint16_t floor12, uint8_t *why, uint16_t *off);

// Lookups. Each needs a valid table and the field's group; on false the outputs are left as they were and *why
// (may be NULL) says which rule refused. Transmit lookups (at f_tx on the TX points) are refused outside the
// transmit band, past the end points across the VCO switch, inside a switch segment wider than
// CS7000P_MAKER_WINDOW_MAX_HZ, and on a result that overflowed, clamped or is 0 or 255. Inside a narrower switch
// segment they take the end value on the frequency's VCO side. Receive lookups (at f_rx on the RX points) take the
// same-side end value inside any switch segment, extend and clamp past the ends, and are refused only outside the
// receive band, on an overflow or past the end points across the switch: the caller keeps today's value then.
// "Across the switch" past the ends: every point on the other side of it, or the switch segment the first or the last
// one, whose extended line mixes the two VCOs' values
bool cs7000p_maker_tx_row(const cs7000p_maker_t *m, cs7000p_mk_row_t row, uint32_t hz, uint8_t *v, uint8_t *why);
bool cs7000p_maker_rx_row(const cs7000p_maker_t *m, cs7000p_mk_row_t row, uint32_t hz, uint8_t *v, uint8_t *why);
// The front-end tune at f_rx as a 12-bit DAC code: 16 c, inside CS7000P_TUNE_MIN..MAX
bool cs7000p_maker_rx_tune(const cs7000p_maker_t *m, uint32_t hz, uint16_t *dac, uint8_t *why);
// The PA drive at f_tx for a power level (0: the low row, 1: the high row) as a 12-bit DAC code, over the floor
bool cs7000p_maker_drive(const cs7000p_maker_t *m, uint32_t hz, uint8_t level, uint16_t *dac, uint8_t *why);
// The mode's modulation pair at f_tx: mod2 to 4:0x45, mod1 to 4:0x46
bool cs7000p_maker_pair(const cs7000p_maker_t *m, uint32_t hz, bool dmr, uint8_t *mod2, uint8_t *mod1, uint8_t *why);
// The squelch thresholds at f_rx for a level (spec 4.5, 5.7): 0 or 10 and above give 255 both (always open)
bool cs7000p_maker_squelch(const cs7000p_maker_t *m, uint32_t hz, uint8_t level, uint8_t *open, uint8_t *close,
			   uint8_t *why);
// The sub-audio level L at f_tx: CTCSS over the tone in 0.1 Hz, DCS from its row; a 25 kHz channel doubles it, to at
// most 254 (spec 5.5)
bool cs7000p_maker_ctcss(const cs7000p_maker_t *m, uint32_t hz, uint16_t tone_dhz, bool wide, uint8_t *level,
			 uint8_t *why);
bool cs7000p_maker_dcs(const cs7000p_maker_t *m, uint32_t hz, bool wide, uint8_t *level, uint8_t *why);

// A DMR burst's fields at f_tx for a power level (spec 5.4): read, checked and shown, not applied. Refused (LK_BURST)
// when the fields at that frequency, an extension past the end points included, fail spec 7's burst rules
typedef struct {
	uint8_t adv;		// 4:0x12 bits 5-0
	uint8_t start, rise;	// S, R
	uint16_t hold, off;	// H, F
} cs7000p_mk_burst_t;
bool cs7000p_maker_burst(const cs7000p_maker_t *m, uint32_t hz, uint8_t level, cs7000p_mk_burst_t *b, uint8_t *why);

// The trims (spec 5.2): a byte v is d = v - 128. Codings of d: the port's held reference (4:0x25 = 0x0C, unsigned
// over the DAC's range, mid-scale on frequency: 0x200 + d, inferred), the maker's signed bias (10 bits, two's
// complement: 4:0x47 the low byte, 4:0x48 bits 9-8) and 4:0x04 (8 bits, two's complement)
static inline int16_t cs7000p_maker_trim_d(uint8_t v) { return (int16_t)((int16_t)v - 128); }
static inline uint16_t cs7000p_maker_bias_hold(int16_t d) { return (uint16_t)(0x200 + d); }
static inline uint16_t cs7000p_maker_bias_signed(int16_t d) { return (uint16_t)((uint16_t)d & 0x3FFu); }
static inline uint8_t cs7000p_maker_reg04(int16_t d) { return (uint8_t)((uint16_t)d & 0xFFu); }
// Trim A (b false) or trim B as d, when its group passed
bool cs7000p_maker_trim(const cs7000p_maker_t *m, bool b, int16_t *d);
// The receive bias under the port's hold: 0x200 + d_A, only within CS7000P_MAKER_RX_TRIM_MAX
bool cs7000p_maker_rx_bias(const cs7000p_maker_t *m, uint16_t *bias);
// The IF chip's RSSI (12-bit raw reading) as dBm on the table's two points: -120 + floor(50 (code - lo) / (hi - lo)),
// code = raw >> 4. The dBm figures are inferred (spec 5.8)
bool cs7000p_maker_rssi_dbm(const cs7000p_maker_t *m, uint32_t raw12, int16_t *dbm);

// Names for the view and the log
const char *cs7000p_maker_state_name(uint8_t state);
const char *cs7000p_maker_why_name(uint8_t why);
const char *cs7000p_maker_lookup_name(uint8_t why);

#endif
