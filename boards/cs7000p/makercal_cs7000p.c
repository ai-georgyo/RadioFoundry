// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS: the maker's factory calibration table (makercal_cs7000p.h; docs/spec/calibration-cs7000p.md).
// Pure.
#include <stddef.h>
#include <string.h>
#include "makercal_cs7000p.h"
#include "rfcal_cs7000p.h"
#include "rfplan_cs7000p.h"

typedef struct {
	uint16_t off;		// in the table
	uint16_t group;		// CS7000P_MK_F_*, 0: none (shown at most)
	bool rx;		// on the RX points at f_rx
	bool used;		// the maker's program reads it: a point at 0xFF is refused (spec 7)
} row_desc_t;

static const row_desc_t rows[CS7000P_MK_ROWS] = {
	[CS7000P_MK_TUNE]        = { 0x044u, CS7000P_MK_F_TUNE, true, true },
	[CS7000P_MK_SQ9_OPEN]    = { 0x04Cu, CS7000P_MK_F_SQUELCH, true, true },
	[CS7000P_MK_SQ9_CLOSE]   = { 0x054u, CS7000P_MK_F_SQUELCH, true, true },
	[CS7000P_MK_SQ1_OPEN]    = { 0x05Cu, CS7000P_MK_F_SQUELCH, true, true },
	[CS7000P_MK_SQ1_CLOSE]   = { 0x064u, CS7000P_MK_F_SQUELCH, true, true },
	[CS7000P_MK_POWER_HI]    = { 0x06Cu, CS7000P_MK_F_POWER, false, true },
	[CS7000P_MK_POWER_LO]    = { 0x074u, CS7000P_MK_F_POWER, false, true },
	[CS7000P_MK_CTCSS_67]    = { 0x084u, CS7000P_MK_F_CTCSS, false, true },
	[CS7000P_MK_CTCSS_254]   = { 0x08Cu, CS7000P_MK_F_CTCSS, false, true },
	[CS7000P_MK_DCS]         = { 0x094u, CS7000P_MK_F_DCS, false, true },
	[CS7000P_MK_TONE_SINGLE] = { 0x09Cu, 0, false, true },
	[CS7000P_MK_TONE_DTMF]   = { 0x0A4u, 0, false, true },
	[CS7000P_MK_TONE_MSK]    = { 0x0ACu, 0, false, true },
	[CS7000P_MK_DMR_MOD2]    = { 0x0B4u, CS7000P_MK_F_DMR_PAIR, false, true },
	[CS7000P_MK_DMR_MOD1]    = { 0x0BCu, CS7000P_MK_F_DMR_PAIR, false, true },
	[CS7000P_MK_FM_MOD2]     = { 0x0C4u, CS7000P_MK_F_FM_PAIR, false, true },
	[CS7000P_MK_FM_MOD1]     = { 0x0CCu, CS7000P_MK_F_FM_PAIR, false, true },
	[CS7000P_MK_HI_ADV]      = { 0x0ECu, CS7000P_MK_F_BURST_HI, false, true },
	[CS7000P_MK_HI_START]    = { 0x0F4u, CS7000P_MK_F_BURST_HI, false, true },
	[CS7000P_MK_HI_RISE]     = { 0x0FCu, CS7000P_MK_F_BURST_HI, false, true },
	[CS7000P_MK_LO_ADV]      = { 0x124u, CS7000P_MK_F_BURST_LO, false, true },
	[CS7000P_MK_LO_START]    = { 0x12Cu, CS7000P_MK_F_BURST_LO, false, true },
	[CS7000P_MK_LO_RISE]     = { 0x134u, CS7000P_MK_F_BURST_LO, false, true },
	// Only the maker's tuner reads these two; element 4 of each is what 0x040 / 0x041 hold
	[CS7000P_MK_RSSI_LO_ROW] = { 0x15Cu, 0, true, false },
	[CS7000P_MK_RSSI_HI_ROW] = { 0x164u, 0, true, false },
};

static const row_desc_t rows16[CS7000P_MK_ROWS16] = {
	[CS7000P_MK_HI_HOLD] = { 0x104u, CS7000P_MK_F_BURST_HI, false, true },
	[CS7000P_MK_HI_OFF]  = { 0x114u, CS7000P_MK_F_BURST_HI, false, true },
	[CS7000P_MK_LO_HOLD] = { 0x13Cu, CS7000P_MK_F_BURST_LO, false, true },
	[CS7000P_MK_LO_OFF]  = { 0x14Cu, CS7000P_MK_F_BURST_LO, false, true },
};

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint8_t rb(const uint8_t *raw, unsigned row, unsigned i) { return raw[rows[row].off + i]; }
static uint16_t rw(const uint8_t *raw, unsigned row, unsigned i)
{
	const uint8_t *p = raw + rows16[row].off + 2u * i;
	return (uint16_t)(p[0] | p[1] << 8);
}
static uint16_t rb_off(unsigned row, unsigned i) { return (uint16_t)(rows[row].off + i); }
static uint16_t rw_off(unsigned row, unsigned i) { return (uint16_t)(rows16[row].off + 2u * i); }

// --- The rules ---

typedef struct {
	uint8_t state;
	bool valid;		// no integrity rule failed
	uint16_t fields;
	uint8_t why, use_why;	// the first integrity and the first use failure
	uint16_t off, use_off;
} verdict_t;

static void whole(verdict_t *v, uint8_t state, uint8_t why, uint16_t off)
{
	v->state = state;
	v->valid = false;
	v->fields = 0;
	v->why = why;
	v->off = off;
}

static void refuse(verdict_t *v, uint16_t group, bool integrity, uint8_t why, uint16_t off)
{
	v->fields &= (uint16_t)~group;
	if (integrity) {
		if (v->valid) {
			v->valid = false;
			v->why = why;
			v->off = off;
		}
	} else if (v->use_why == CS7000P_MK_WHY_NONE) {
		v->use_why = why;
		v->use_off = off;
	}
}

// A tune, power or pair row: every code 1-254 (0 and 255 are what an erased or cleared table gives, spec 2.2)
static void codes(const uint8_t *raw, verdict_t *v, unsigned row)
{
	for (unsigned i = 0; i < CS7000P_MAKER_POINTS; i++) {
		uint8_t c = rb(raw, row, i);
		if (c == 0u || c == 0xFFu) {
			refuse(v, rows[row].group, true, CS7000P_MK_WHY_CODE, rb_off(row, i));
		}
	}
}

static void trim(const uint8_t *raw, verdict_t *v, uint16_t off, uint16_t group)
{
	int16_t d = cs7000p_maker_trim_d(raw[off]);
	if (d > CS7000P_MAKER_TRIM_MAX || d < -CS7000P_MAKER_TRIM_MAX) {
		refuse(v, group, true, CS7000P_MK_WHY_TRIM, off);
	}
}

static void burst(const uint8_t *raw, verdict_t *v, unsigned s_row, unsigned r_row, unsigned h_row, unsigned f_row)
{
	uint16_t group = rows[s_row].group;
	for (unsigned i = 0; i < CS7000P_MAKER_POINTS; i++) {
		uint8_t s = rb(raw, s_row, i), r = rb(raw, r_row, i);
		uint16_t h = rw(raw, h_row, i), f = rw(raw, f_row, i);
		if (s >= r) {
			refuse(v, group, true, CS7000P_MK_WHY_BURST_START, rb_off(s_row, i));
		} else if (2u * (unsigned)(r - s) < 5u) {
			refuse(v, group, true, CS7000P_MK_WHY_BURST_RISE, rb_off(r_row, i));
		}
		if (h <= r) {
			refuse(v, group, true, CS7000P_MK_WHY_BURST_HOLD, rw_off(h_row, i));
		}
		if (f < h) {
			refuse(v, group, true, CS7000P_MK_WHY_BURST_OFF, rw_off(f_row, i));
		}
	}
	// The maker's 16-bit interpolation is known only for eight equal values (spec 4.4): the one case used
	for (unsigned i = 1; i < CS7000P_MAKER_POINTS; i++) {
		if (rw(raw, h_row, i) != rw(raw, h_row, 0)) {
			refuse(v, group, false, CS7000P_MK_WHY_BURST_U16, rw_off(h_row, i));
		}
		if (rw(raw, f_row, i) != rw(raw, f_row, 0)) {
			refuse(v, group, false, CS7000P_MK_WHY_BURST_U16, rw_off(f_row, i));
		}
	}
}

static void judge(const uint8_t *raw, uint16_t floor12, verdict_t *v)
{
	memset(v, 0, sizeof *v);
	v->state = CS7000P_MK_OK;
	v->valid = true;
	v->fields = CS7000P_MK_F_ALL;

	bool blank = true, zero = true;
	for (unsigned i = 0; i < CS7000P_MAKER_SIZE; i++) {
		blank = blank && raw[i] == 0xFFu;
		zero = zero && raw[i] == 0x00u;
	}
	if (blank) {
		whole(v, CS7000P_MK_BLANK, CS7000P_MK_WHY_BLANK, 0);
		return;
	}
	if (zero) {
		whole(v, CS7000P_MK_ZERO, CS7000P_MK_WHY_ZERO, 0);
		return;
	}
	static const uint16_t sets[2] = { CS7000P_MK_TX_POINTS, CS7000P_MK_RX_POINTS };
	for (unsigned s = 0; s < 2u; s++) {
		uint32_t prev = 0;
		for (unsigned i = 0; i < CS7000P_MAKER_POINTS; i++) {
			uint16_t off = (uint16_t)(sets[s] + 4u * i);
			uint32_t hz = be32(raw + off);
			uint8_t why = hz == 0xFFFFFFFFu ? CS7000P_MK_WHY_PT_UNSET
				    : hz < CS7000P_MAKER_POINT_MIN_HZ || hz > CS7000P_MAKER_POINT_MAX_HZ ? CS7000P_MK_WHY_PT_RANGE
				    : i > 0u && hz <= prev ? CS7000P_MK_WHY_PT_ORDER
				    : CS7000P_MK_WHY_NONE;
			if (why != CS7000P_MK_WHY_NONE) {
				whole(v, CS7000P_MK_POINTS, why, off);
				return;
			}
			prev = hz;
		}
	}

	// Every row the maker's program uses: no point unset
	for (unsigned r = 0; r < CS7000P_MK_ROWS; r++) {
		for (unsigned i = 0; rows[r].used && i < CS7000P_MAKER_POINTS; i++) {
			if (rb(raw, r, i) == 0xFFu) {
				refuse(v, rows[r].group, true, CS7000P_MK_WHY_ROW_UNSET, rb_off(r, i));
			}
		}
	}
	for (unsigned r = 0; r < CS7000P_MK_ROWS16; r++) {
		for (unsigned i = 0; i < CS7000P_MAKER_POINTS; i++) {
			if (rw(raw, r, i) == 0xFFFFu) {
				refuse(v, rows16[r].group, true, CS7000P_MK_WHY_ROW_UNSET, rw_off(r, i));
			}
		}
	}

	codes(raw, v, CS7000P_MK_TUNE);

	codes(raw, v, CS7000P_MK_POWER_HI);
	codes(raw, v, CS7000P_MK_POWER_LO);
	for (unsigned i = 0; i < CS7000P_MAKER_POINTS; i++) {
		if (rb(raw, CS7000P_MK_POWER_HI, i) <= rb(raw, CS7000P_MK_POWER_LO, i)) {
			refuse(v, CS7000P_MK_F_POWER, true, CS7000P_MK_WHY_POWER_ORDER, rb_off(CS7000P_MK_POWER_HI, i));
		}
	}
	for (unsigned r = CS7000P_MK_POWER_HI; r <= CS7000P_MK_POWER_LO; r++) {
		for (unsigned i = 0; i < CS7000P_MAKER_POINTS; i++) {
			if (16u * rb(raw, r, i) <= floor12) {
				refuse(v, CS7000P_MK_F_POWER, true, CS7000P_MK_WHY_POWER_FLOOR, rb_off(r, i));
			}
		}
	}

	codes(raw, v, CS7000P_MK_FM_MOD2);
	codes(raw, v, CS7000P_MK_FM_MOD1);
	codes(raw, v, CS7000P_MK_DMR_MOD2);
	codes(raw, v, CS7000P_MK_DMR_MOD1);

	trim(raw, v, CS7000P_MK_TRIM_A, CS7000P_MK_F_TRIM_A);
	trim(raw, v, CS7000P_MK_TRIM_B, CS7000P_MK_F_TRIM_B);

	// Squelch: hysteresis at both levels, and level 9 the tight one (less noise opens it)
	for (unsigned i = 0; i < CS7000P_MAKER_POINTS; i++) {
		uint8_t o9 = rb(raw, CS7000P_MK_SQ9_OPEN, i), c9 = rb(raw, CS7000P_MK_SQ9_CLOSE, i);
		uint8_t o1 = rb(raw, CS7000P_MK_SQ1_OPEN, i), c1 = rb(raw, CS7000P_MK_SQ1_CLOSE, i);
		if (o9 >= c9) {
			refuse(v, CS7000P_MK_F_SQUELCH, true, CS7000P_MK_WHY_SQ_HYST, rb_off(CS7000P_MK_SQ9_OPEN, i));
		}
		if (o1 >= c1) {
			refuse(v, CS7000P_MK_F_SQUELCH, true, CS7000P_MK_WHY_SQ_HYST, rb_off(CS7000P_MK_SQ1_OPEN, i));
		}
		if (o1 <= o9) {
			refuse(v, CS7000P_MK_F_SQUELCH, true, CS7000P_MK_WHY_SQ_LEVEL, rb_off(CS7000P_MK_SQ1_OPEN, i));
		}
		if (c1 <= c9) {
			refuse(v, CS7000P_MK_F_SQUELCH, true, CS7000P_MK_WHY_SQ_LEVEL, rb_off(CS7000P_MK_SQ1_CLOSE, i));
		}
	}

	uint8_t lo = raw[CS7000P_MK_RSSI_LO], hi = raw[CS7000P_MK_RSSI_HI];
	if (hi <= lo) {
		refuse(v, CS7000P_MK_F_RSSI, true, CS7000P_MK_WHY_RSSI_ORDER, CS7000P_MK_RSSI_HI);
	}
	// An end at the ADC's rail is no reading of a reference level
	if (lo == 0u || lo == 0xFFu) {
		refuse(v, CS7000P_MK_F_RSSI, false, CS7000P_MK_WHY_RSSI_END, CS7000P_MK_RSSI_LO);
	}
	if (hi == 0u || hi == 0xFFu) {
		refuse(v, CS7000P_MK_F_RSSI, false, CS7000P_MK_WHY_RSSI_END, CS7000P_MK_RSSI_HI);
	}

	// A sub-audio level of 0 sends no tone: a channel that asks for one would go without
	static const uint8_t levels[3] = { CS7000P_MK_CTCSS_67, CS7000P_MK_CTCSS_254, CS7000P_MK_DCS };
	for (unsigned l = 0; l < 3u; l++) {
		for (unsigned i = 0; i < CS7000P_MAKER_POINTS; i++) {
			if (rb(raw, levels[l], i) == 0u) {
				refuse(v, rows[levels[l]].group, false, CS7000P_MK_WHY_LEVEL_ZERO, rb_off(levels[l], i));
			}
		}
	}

	burst(raw, v, CS7000P_MK_HI_START, CS7000P_MK_HI_RISE, CS7000P_MK_HI_HOLD, CS7000P_MK_HI_OFF);
	burst(raw, v, CS7000P_MK_LO_START, CS7000P_MK_LO_RISE, CS7000P_MK_LO_HOLD, CS7000P_MK_LO_OFF);

	if (raw[CS7000P_MK_BATTERY] == 0u || raw[CS7000P_MK_BATTERY] == 0xFFu) {
		refuse(v, CS7000P_MK_F_BATTERY, false, CS7000P_MK_WHY_BATTERY, CS7000P_MK_BATTERY);
	}

	if (v->valid) {
		v->why = v->use_why;
		v->off = v->use_off;
	}
}

bool cs7000p_maker_parse(const uint8_t *raw, uint32_t crc, uint16_t floor12, cs7000p_maker_t *m)
{
	// Whole, padding included: two parses of the same bytes compare equal
	memset(m, 0, sizeof *m);
	m->crc = crc;
	m->floor12 = floor12;
	if (raw == NULL) {
		m->state = CS7000P_MK_UNREAD;
		m->why = CS7000P_MK_WHY_UNREAD;
		return false;
	}
	verdict_t v;
	judge(raw, floor12, &v);
	m->state = v.state;
	m->valid = v.valid;
	m->fields = v.fields;
	m->why = v.why;
	m->why_off = v.off;

	for (unsigned i = 0; i < CS7000P_MAKER_POINTS; i++) {
		m->tx_hz[i] = be32(raw + CS7000P_MK_TX_POINTS + 4u * i);
		m->rx_hz[i] = be32(raw + CS7000P_MK_RX_POINTS + 4u * i);
		m->tx_p10[i] = m->tx_hz[i] / 10u;
		m->rx_p10[i] = m->rx_hz[i] / 10u;
		for (unsigned r = 0; r < CS7000P_MK_ROWS; r++) {
			m->row[r][i] = rb(raw, r, i);
		}
		for (unsigned r = 0; r < CS7000P_MK_ROWS16; r++) {
			m->row16[r][i] = rw(raw, r, i);
		}
	}
	m->rssi_lo = raw[CS7000P_MK_RSSI_LO];
	m->rssi_hi = raw[CS7000P_MK_RSSI_HI];
	m->vox9 = raw[CS7000P_MK_VOX9];
	m->vox0 = raw[CS7000P_MK_VOX0];
	m->trim_a = raw[CS7000P_MK_TRIM_A];
	m->trim_b = raw[CS7000P_MK_TRIM_B];
	m->battery = raw[CS7000P_MK_BATTERY];
	m->tone_in = raw[CS7000P_MK_TONE_IN];
	return m->valid;
}

bool cs7000p_maker_check(const uint8_t *raw, uint32_t n, uint16_t floor12, uint8_t *why, uint16_t *off)
{
	verdict_t v = { .why = CS7000P_MK_WHY_NONE };
	if (raw == NULL) {
		v.why = CS7000P_MK_WHY_UNREAD;
	} else if (n != CS7000P_MAKER_SIZE) {
		v.why = CS7000P_MK_WHY_SIZE;
	} else {
		judge(raw, floor12, &v);
	}
	bool ok = raw != NULL && n == CS7000P_MAKER_SIZE && v.valid;
	if (why) {
		*why = ok ? (uint8_t)CS7000P_MK_WHY_NONE : v.why;
	}
	if (off) {
		*off = ok ? 0u : v.off;
	}
	return ok;
}

// --- The arithmetic ---

uint8_t cs7000p_maker_line(int32_t x0, int32_t x1, uint8_t y0, uint8_t y1, int32_t x, uint8_t *flags)
{
	uint8_t dummy = 0;
	if (flags == NULL) {
		flags = &dummy;
	}
	if (x0 == x1) {
		return (uint8_t)(((unsigned)y0 + y1) / 2u);
	}
	// The maker forms both in 32 bits (spec 4.4). INT32_MIN itself would fit; flagged anyway, the safe side
	int64_t p = 4 * ((int64_t)y1 - y0) * ((int64_t)x - x0);
	if (p > INT32_MAX || p < -(int64_t)INT32_MAX) {
		*flags |= CS7000P_MK_OVERFLOW;
	}
	int64_t t = 4 * (int64_t)y0 + p / ((int64_t)x1 - x0);	// C division: toward zero, as the maker's
	if (t > INT32_MAX || t < INT32_MIN) {
		*flags |= CS7000P_MK_OVERFLOW;
	}
	if (t >= 1020) {
		*flags |= CS7000P_MK_CLAMP;
		return 255u;
	}
	if (t < 0) {
		*flags |= CS7000P_MK_CLAMP;
		return 0u;
	}
	// A quarter step rounded half up
	return (uint8_t)((t >> 2) + ((t >> 1) & 1));
}

uint8_t cs7000p_maker_row(const uint32_t p10[CS7000P_MAKER_POINTS], const uint8_t y[CS7000P_MAKER_POINTS], uint32_t hz,
			  uint8_t *flags)
{
	uint32_t f = hz / 10u;
	unsigned k = 0;
	while (k < CS7000P_MAKER_POINTS - 2u && f > p10[k + 1u]) {
		k++;
	}
	return cs7000p_maker_line((int32_t)p10[k], (int32_t)p10[k + 1u], y[k], y[k + 1u], (int32_t)f, flags);
}

// --- Lookups ---

static bool fail(uint8_t *why, uint8_t lk)
{
	if (why) {
		*why = lk;
	}
	return false;
}

static bool done(uint8_t *why)
{
	if (why) {
		*why = CS7000P_MK_LK_OK;
	}
	return true;
}

static bool usable(const cs7000p_maker_t *m, uint16_t group, uint8_t *why)
{
	if (m == NULL || m->state != CS7000P_MK_OK || !m->valid) {
		return fail(why, CS7000P_MK_LK_TABLE);
	}
	if ((m->fields & group) != group) {
		return fail(why, CS7000P_MK_LK_GROUP);
	}
	return true;
}

// A row with the VCO switch at s10 (the last 10 Hz step on the low VCO). Values at points on the two sides of the
// switch were set on two VCOs: between those two points the maker's line mixes them, so the end value on the
// frequency's side is taken instead (transmit: only when the two are close). No extension past the ends may use
// the other VCO's value: not across the switch when every point lies on one side of it, and not along the switch
// segment's line when that segment is the first or the last. Returns a cs7000p_mk_lookup_t
static uint8_t switched(const uint32_t *p10, const uint8_t *y, uint32_t hz, uint32_t s10, bool tx, uint8_t *v,
			uint8_t *flags)
{
	uint32_t f = hz / 10u;
	bool low = f <= s10;
	for (unsigned j = 0; j + 1u < CS7000P_MAKER_POINTS; j++) {
		if (p10[j] <= s10 && s10 < p10[j + 1u]) {
			if ((j == 0u && f < p10[0]) || (j + 2u == CS7000P_MAKER_POINTS && f > p10[j + 1u])) {
				return CS7000P_MK_LK_SWITCH;
			}
			if (p10[j] < f && f < p10[j + 1u]) {
				if (tx && (p10[j + 1u] - p10[j]) * 10u > CS7000P_MAKER_WINDOW_MAX_HZ) {
					return CS7000P_MK_LK_WINDOW;
				}
				*v = low ? y[j] : y[j + 1u];
				return CS7000P_MK_LK_OK;
			}
			*v = cs7000p_maker_row(p10, y, hz, flags);
			return CS7000P_MK_LK_OK;
		}
	}
	// No switch inside the points: they all lie on one side of it
	if (low != (p10[0] <= s10)) {
		return CS7000P_MK_LK_SWITCH;
	}
	*v = cs7000p_maker_row(p10, y, hz, flags);
	return CS7000P_MK_LK_OK;
}

bool cs7000p_maker_tx_row(const cs7000p_maker_t *m, cs7000p_mk_row_t row, uint32_t hz, uint8_t *v, uint8_t *why)
{
	if ((unsigned)row >= CS7000P_MK_ROWS || rows[row].rx) {
		return fail(why, CS7000P_MK_LK_ROW);
	}
	if (!usable(m, rows[row].group, why)) {
		return false;
	}
	if (!cs7000p_tx_band(hz)) {
		return fail(why, CS7000P_MK_LK_BAND);
	}
	uint8_t flags = 0, out = 0;
	uint8_t lk = switched(m->tx_p10, m->row[row], hz, CS7000P_MAKER_TX_SWITCH_10HZ, true, &out, &flags);
	if (lk != CS7000P_MK_LK_OK) {
		return fail(why, lk);
	}
	if (flags != 0u) {
		return fail(why, CS7000P_MK_LK_MATH);
	}
	// 0 and 255 are what an erased or cleared table gives: never a transmit value
	if (out == 0u || out == 0xFFu) {
		return fail(why, CS7000P_MK_LK_CODE);
	}
	*v = out;
	return done(why);
}

bool cs7000p_maker_rx_row(const cs7000p_maker_t *m, cs7000p_mk_row_t row, uint32_t hz, uint8_t *v, uint8_t *why)
{
	if ((unsigned)row >= CS7000P_MK_ROWS || !rows[row].rx) {
		return fail(why, CS7000P_MK_LK_ROW);
	}
	if (!usable(m, rows[row].group, why)) {
		return false;
	}
	// Spec 7: no extension past the band's edge (nothing is received there either)
	if (!cs7000p_rx_band(hz)) {
		return fail(why, CS7000P_MK_LK_BAND);
	}
	uint8_t flags = 0, out = 0;
	uint8_t lk = switched(m->rx_p10, m->row[row], hz, CS7000P_MAKER_RX_SWITCH_10HZ, false, &out, &flags);
	if (lk != CS7000P_MK_LK_OK) {
		return fail(why, lk);
	}
	// Held at 0 or 255 is the maker's value; a wrapped sum is not
	if (flags & CS7000P_MK_OVERFLOW) {
		return fail(why, CS7000P_MK_LK_MATH);
	}
	*v = out;
	return done(why);
}

bool cs7000p_maker_rx_tune(const cs7000p_maker_t *m, uint32_t hz, uint16_t *dac, uint8_t *why)
{
	uint8_t c;
	if (!cs7000p_maker_rx_row(m, CS7000P_MK_TUNE, hz, &c, why)) {
		return false;
	}
	// The maker extrapolates past the DAC buffer's range; the port clamps (hardware-cs7000p.md 7.8)
	*dac = cs7000p_cal_tune_clamp(16u * c);
	return true;
}

bool cs7000p_maker_drive(const cs7000p_maker_t *m, uint32_t hz, uint8_t level, uint16_t *dac, uint8_t *why)
{
	if (level >= CS7000P_POWER_LEVELS) {
		return fail(why, CS7000P_MK_LK_ROW);
	}
	uint8_t c;
	if (!cs7000p_maker_tx_row(m, level ? CS7000P_MK_POWER_HI : CS7000P_MK_POWER_LO, hz, &c, why)) {
		return false;
	}
	uint16_t d = (uint16_t)(16u * c);
	if (d <= m->floor12) {
		return fail(why, CS7000P_MK_LK_FLOOR);
	}
	*dac = d;
	return true;
}

bool cs7000p_maker_pair(const cs7000p_maker_t *m, uint32_t hz, bool dmr, uint8_t *mod2, uint8_t *mod1, uint8_t *why)
{
	uint8_t a, b;
	if (!cs7000p_maker_tx_row(m, dmr ? CS7000P_MK_DMR_MOD2 : CS7000P_MK_FM_MOD2, hz, &a, why) ||
	    !cs7000p_maker_tx_row(m, dmr ? CS7000P_MK_DMR_MOD1 : CS7000P_MK_FM_MOD1, hz, &b, why)) {
		return false;
	}
	*mod2 = a;
	*mod1 = b;
	return true;
}

bool cs7000p_maker_squelch(const cs7000p_maker_t *m, uint32_t hz, uint8_t level, uint8_t *open, uint8_t *close,
			   uint8_t *why)
{
	if (!usable(m, CS7000P_MK_F_SQUELCH, why)) {
		return false;
	}
	if (level == 0u || level >= 10u) {
		*open = *close = 0xFFu;
		return done(why);
	}
	uint8_t o9, c9, o1, c1;
	if (!cs7000p_maker_rx_row(m, CS7000P_MK_SQ9_OPEN, hz, &o9, why) ||
	    !cs7000p_maker_rx_row(m, CS7000P_MK_SQ9_CLOSE, hz, &c9, why) ||
	    !cs7000p_maker_rx_row(m, CS7000P_MK_SQ1_OPEN, hz, &o1, why) ||
	    !cs7000p_maker_rx_row(m, CS7000P_MK_SQ1_CLOSE, hz, &c1, why)) {
		return false;
	}
	// Over the level, 1 to 9: no product of these can wrap
	*open = cs7000p_maker_line(1, 9, o1, o9, level, NULL);
	*close = cs7000p_maker_line(1, 9, c1, c9, level, NULL);
	return done(why);
}

// A sub-audio level on a channel: a 25 kHz one doubles it, to at most 254 (spec 5.5)
static uint8_t widened(uint8_t level, bool wide)
{
	return wide ? (uint8_t)(2u * level > 254u ? 254u : 2u * level) : level;
}

bool cs7000p_maker_ctcss(const cs7000p_maker_t *m, uint32_t hz, uint16_t tone_dhz, bool wide, uint8_t *level,
			 uint8_t *why)
{
	uint8_t l67, l254;
	if (!cs7000p_maker_tx_row(m, CS7000P_MK_CTCSS_67, hz, &l67, why) ||
	    !cs7000p_maker_tx_row(m, CS7000P_MK_CTCSS_254, hz, &l254, why)) {
		return false;
	}
	uint8_t flags = 0;
	uint8_t l = cs7000p_maker_line(CS7000P_MAKER_CTCSS_X0, CS7000P_MAKER_CTCSS_X1, l67, l254, tone_dhz, &flags);
	if (flags != 0u) {
		return fail(why, CS7000P_MK_LK_MATH);
	}
	if (l == 0u || l == 0xFFu) {
		return fail(why, CS7000P_MK_LK_CODE);
	}
	*level = widened(l, wide);
	return done(why);
}

bool cs7000p_maker_dcs(const cs7000p_maker_t *m, uint32_t hz, bool wide, uint8_t *level, uint8_t *why)
{
	uint8_t l;
	if (!cs7000p_maker_tx_row(m, CS7000P_MK_DCS, hz, &l, why)) {
		return false;
	}
	*level = widened(l, wide);
	return true;
}

bool cs7000p_maker_burst(const cs7000p_maker_t *m, uint32_t hz, uint8_t level, cs7000p_mk_burst_t *b, uint8_t *why)
{
	if (level >= CS7000P_POWER_LEVELS) {
		return fail(why, CS7000P_MK_LK_ROW);
	}
	cs7000p_mk_burst_t out;
	if (!cs7000p_maker_tx_row(m, level ? CS7000P_MK_HI_ADV : CS7000P_MK_LO_ADV, hz, &out.adv, why) ||
	    !cs7000p_maker_tx_row(m, level ? CS7000P_MK_HI_START : CS7000P_MK_LO_START, hz, &out.start, why) ||
	    !cs7000p_maker_tx_row(m, level ? CS7000P_MK_HI_RISE : CS7000P_MK_LO_RISE, hz, &out.rise, why)) {
		return false;
	}
	// The group passed, so all eight values are equal: the maker's 16-bit form returns that value (spec 4.4)
	out.hold = m->row16[level ? CS7000P_MK_HI_HOLD : CS7000P_MK_LO_HOLD][0];
	out.off = m->row16[level ? CS7000P_MK_HI_OFF : CS7000P_MK_LO_OFF][0];
	// The points passed spec 7's burst rules; a line extended past the end points need not
	if (out.start >= out.rise || 2u * (unsigned)(out.rise - out.start) < 5u || out.hold <= out.rise ||
	    out.off < out.hold) {
		return fail(why, CS7000P_MK_LK_BURST);
	}
	*b = out;
	return true;
}

bool cs7000p_maker_trim(const cs7000p_maker_t *m, bool b, int16_t *d)
{
	if (!usable(m, b ? CS7000P_MK_F_TRIM_B : CS7000P_MK_F_TRIM_A, NULL)) {
		return false;
	}
	*d = cs7000p_maker_trim_d(b ? m->trim_b : m->trim_a);
	return true;
}

bool cs7000p_maker_rx_bias(const cs7000p_maker_t *m, uint16_t *bias)
{
	int16_t d;
	if (!cs7000p_maker_trim(m, false, &d) || d > CS7000P_MAKER_RX_TRIM_MAX || d < -CS7000P_MAKER_RX_TRIM_MAX) {
		return false;
	}
	*bias = cs7000p_maker_bias_hold(d);
	return true;
}

bool cs7000p_maker_rssi_dbm(const cs7000p_maker_t *m, uint32_t raw12, int16_t *dbm)
{
	if (!usable(m, CS7000P_MK_F_RSSI, NULL)) {
		return false;
	}
	int32_t code = (int32_t)((raw12 > 4095u ? 4095u : raw12) >> 4);
	int32_t num = (CS7000P_MAKER_RSSI_HI_DBM - CS7000P_MAKER_RSSI_LO_DBM) * (code - m->rssi_lo);
	int32_t den = (int32_t)m->rssi_hi - m->rssi_lo;	// above 0: the group passed
	// A floor on both sides of the low point, so the scale never steps back
	int32_t q = num >= 0 ? num / den : -((-num + den - 1) / den);
	*dbm = (int16_t)(CS7000P_MAKER_RSSI_LO_DBM + q);
	return true;
}

// --- Names ---

static const char *const state_names[] = {
	[CS7000P_MK_UNREAD] = "unread", [CS7000P_MK_OK] = "ok", [CS7000P_MK_BLANK] = "blank",
	[CS7000P_MK_ZERO] = "zero", [CS7000P_MK_POINTS] = "points",
};

static const char *const why_names[CS7000P_MK_WHY_COUNT] = {
	[CS7000P_MK_WHY_NONE] = "none",
	[CS7000P_MK_WHY_UNREAD] = "unread",
	[CS7000P_MK_WHY_SIZE] = "size",
	[CS7000P_MK_WHY_BLANK] = "blank",
	[CS7000P_MK_WHY_ZERO] = "zero",
	[CS7000P_MK_WHY_PT_UNSET] = "point_unset",
	[CS7000P_MK_WHY_PT_RANGE] = "point_range",
	[CS7000P_MK_WHY_PT_ORDER] = "point_order",
	[CS7000P_MK_WHY_ROW_UNSET] = "row_unset",
	[CS7000P_MK_WHY_CODE] = "code",
	[CS7000P_MK_WHY_POWER_ORDER] = "power_order",
	[CS7000P_MK_WHY_POWER_FLOOR] = "power_floor",
	[CS7000P_MK_WHY_TRIM] = "trim",
	[CS7000P_MK_WHY_SQ_HYST] = "sq_hyst",
	[CS7000P_MK_WHY_SQ_LEVEL] = "sq_level",
	[CS7000P_MK_WHY_RSSI_ORDER] = "rssi_order",
	[CS7000P_MK_WHY_BURST_START] = "burst_start",
	[CS7000P_MK_WHY_BURST_RISE] = "burst_rise",
	[CS7000P_MK_WHY_BURST_HOLD] = "burst_hold",
	[CS7000P_MK_WHY_BURST_OFF] = "burst_off",
	[CS7000P_MK_WHY_RSSI_END] = "rssi_end",
	[CS7000P_MK_WHY_LEVEL_ZERO] = "level_zero",
	[CS7000P_MK_WHY_BURST_U16] = "burst_u16",
	[CS7000P_MK_WHY_BATTERY] = "battery",
};

static const char *const lookup_names[CS7000P_MK_LK_COUNT] = {
	[CS7000P_MK_LK_OK] = "ok",
	[CS7000P_MK_LK_TABLE] = "table",
	[CS7000P_MK_LK_GROUP] = "group",
	[CS7000P_MK_LK_ROW] = "row",
	[CS7000P_MK_LK_BAND] = "band",
	[CS7000P_MK_LK_SWITCH] = "switch",
	[CS7000P_MK_LK_WINDOW] = "window",
	[CS7000P_MK_LK_MATH] = "math",
	[CS7000P_MK_LK_CODE] = "code",
	[CS7000P_MK_LK_FLOOR] = "floor",
	[CS7000P_MK_LK_BURST] = "burst",
};

const char *cs7000p_maker_state_name(uint8_t state)
{
	return state < sizeof state_names / sizeof state_names[0] ? state_names[state] : "?";
}

const char *cs7000p_maker_why_name(uint8_t why)
{
	return why < CS7000P_MK_WHY_COUNT ? why_names[why] : "?";
}

const char *cs7000p_maker_lookup_name(uint8_t why)
{
	return why < CS7000P_MK_LK_COUNT ? lookup_names[why] : "?";
}
