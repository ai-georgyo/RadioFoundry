// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS RF calibration (rfcal_cs7000p.h; docs/spec/hardware-cs7000p.md 13). Pure.
#include <stddef.h>
#include "rfcal_cs7000p.h"

#define U	CS7000P_CAL_UNSET
#define UNSET_ROW	{ U, U, U, U, U, U, U, U, U }

// Nothing here is measured, so nothing is set, and the radio refuses every transmission. A measured radio's values
// replace the rows one by one; the verdict follows the rows, there is no switch to flip.
// The floor is the shared DAC's interlock threshold (exp_cs7000p.c): the build's CS7000P_APC_FLOOR when it gives
// one, so that the two can't differ, and 0 otherwise
const cs7000p_cal_t cs7000p_cal_model = {
	.drive = { UNSET_ROW, UNSET_ROW },
#ifdef CS7000P_APC_FLOOR
	.floor = CS7000P_APC_FLOOR,
#else
	.floor = 0,
#endif
	.rx_tune = UNSET_ROW,
	.fm_mod2 = UNSET_ROW,
	.fm_mod1 = UNSET_ROW,
	.dmr_mod2 = UNSET_ROW,
	.dmr_mod1 = UNSET_ROW,
	.ref_set = false,
	.ref_offset = 0,
};

// A row at hz: between two points a straight line, past the ends the end's value. False: a point is unset or over max
static bool row_at(const uint16_t *row, uint32_t hz, uint16_t max, uint16_t *out)
{
	for (uint32_t i = 0; i < CS7000P_CAL_POINTS; i++) {
		if (row[i] == CS7000P_CAL_UNSET || row[i] > max) {
			return false;
		}
	}
	if (hz <= CS7000P_CAL_FIRST_HZ) {
		*out = row[0];
		return true;
	}
	uint32_t off = hz - CS7000P_CAL_FIRST_HZ;
	uint32_t i = off / CS7000P_CAL_STEP_HZ;
	if (i >= CS7000P_CAL_POINTS - 1u) {
		*out = row[CS7000P_CAL_POINTS - 1u];
		return true;
	}
	// In kHz, so the products stay inside 32 bits (4095 x 15000)
	int32_t part = (int32_t)((off - i * CS7000P_CAL_STEP_HZ) / 1000u);
	int32_t span = (int32_t)(CS7000P_CAL_STEP_HZ / 1000u);
	int32_t d = (int32_t)row[i + 1u] - (int32_t)row[i];
	*out = (uint16_t)((int32_t)row[i] + (d * part + (d < 0 ? -span / 2 : span / 2)) / span);
	return true;
}

bool cs7000p_cal_drive(const cs7000p_cal_t *c, uint32_t hz, uint8_t level, uint16_t *drive)
{
	return level < CS7000P_POWER_LEVELS && row_at(c->drive[level], hz, CS7000P_DAC_MAX, drive);
}

bool cs7000p_cal_mod_pair(const cs7000p_cal_t *c, uint32_t hz, bool dmr, uint8_t *mod2, uint8_t *mod1)
{
	uint16_t m2, m1;
	if (!row_at(dmr ? c->dmr_mod2 : c->fm_mod2, hz, 0xFFu, &m2) ||
	    !row_at(dmr ? c->dmr_mod1 : c->fm_mod1, hz, 0xFFu, &m1)) {
		return false;
	}
	*mod2 = (uint8_t)m2;
	*mod1 = (uint8_t)m1;
	return true;
}

int8_t cs7000p_cal_ref_offset(const cs7000p_cal_t *c)
{
	return c->ref_set ? c->ref_offset : (int8_t)(CS7000P_REF_OFFSET);
}

uint16_t cs7000p_cal_rx_tune(const cs7000p_cal_t *c, uint32_t hz)
{
	uint16_t v;
	if (!row_at(c->rx_tune, hz, CS7000P_DAC_MAX, &v)) {
		v = (uint16_t)(CS7000P_RX_TUNE_NOMINAL);
	}
	return cs7000p_cal_tune_clamp(v);
}

bool cs7000p_cal_tx_ok(const cs7000p_cal_t *c, uint32_t hz, uint8_t level, bool dmr)
{
	uint16_t drive;
	uint8_t mod2, mod1;
	// A drive of 0, or one at or under the floor, keys nothing: a table that says so is not a calibration
	return c->ref_set && cs7000p_cal_drive(c, hz, level, &drive) && drive != 0u && drive > c->floor &&
	       cs7000p_cal_mod_pair(c, hz, dmr, &mod2, &mod1);
}

bool cs7000p_cal_any_tx(const cs7000p_cal_t *c)
{
	for (uint32_t i = 0; i < CS7000P_CAL_POINTS; i++) {
		uint32_t hz = CS7000P_CAL_FIRST_HZ + i * CS7000P_CAL_STEP_HZ;
		for (uint8_t level = 0; level < CS7000P_POWER_LEVELS; level++) {
			if (cs7000p_cal_tx_ok(c, hz, level, false) || cs7000p_cal_tx_ok(c, hz, level, true)) {
				return true;
			}
		}
	}
	return false;
}

// --- Receive, the maker's table first ---

static void src_is(uint8_t *src, uint8_t v)
{
	if (src != NULL) {
		*src = v;
	}
}

uint16_t cs7000p_cal_rx_tune_m(const cs7000p_cal_t *c, const cs7000p_maker_t *m, uint32_t hz, uint8_t *src)
{
	uint16_t dac;
	// The reader refuses for a NULL or not valid table, a refused group and a lookup that wrapped or crosses the VCO
	// switch past the end points: each of them today's value
	if (CS7000P_RX_MAKER && m != NULL && cs7000p_maker_rx_tune(m, hz, &dac, NULL)) {
		src_is(src, CS7000P_SRC_MAKER);
		return dac;
	}
	src_is(src, CS7000P_SRC_DEFAULT);
	return cs7000p_cal_rx_tune(c, hz);
}

int8_t cs7000p_cal_rx_ref_m(const cs7000p_cal_t *c, const cs7000p_maker_t *m, uint8_t *src, uint8_t *why)
{
	int16_t d;
	uint8_t w = CS7000P_RXREF_WHY_NONE;
	if (!CS7000P_RX_MAKER) {
		w = CS7000P_RXREF_WHY_BUILD;
	} else if (m == NULL || !m->valid) {
		w = CS7000P_RXREF_WHY_TABLE;
	} else if (!cs7000p_maker_trim(m, false, &d)) {
		w = CS7000P_RXREF_WHY_GROUP;
	} else if (d > CS7000P_MAKER_RX_TRIM_MAX || d < -CS7000P_MAKER_RX_TRIM_MAX) {
		// Farther than the bring-up's passband sweep allows at mid-scale: either the coding under the hold is not
		// 0x200 + d or the table is not this unit's. Receive keeps today's level
		w = CS7000P_RXREF_WHY_BOUND;
	}
	if (why != NULL) {
		*why = w;
	}
	if (w == CS7000P_RXREF_WHY_NONE) {
		src_is(src, CS7000P_SRC_MAKER);
		return (int8_t)d;	// within 32 of 0
	}
	src_is(src, CS7000P_SRC_DEFAULT);
	return cs7000p_cal_ref_offset(c);
}

int16_t cs7000p_cal_rssi_m(const cs7000p_maker_t *m, uint32_t raw12, uint32_t mv, uint8_t *src)
{
	int16_t dbm;
	if (CS7000P_RX_MAKER && m != NULL && cs7000p_maker_rssi_dbm(m, raw12, &dbm)) {
		src_is(src, CS7000P_SRC_MAKER);
		return dbm;
	}
	src_is(src, CS7000P_SRC_DEFAULT);
	return cs7000p_rssi_dbm(mv);
}

// --- Transmit, the maker's table: asked, not served ---

bool cs7000p_cal_maker_tx(const cs7000p_maker_t *m, uint32_t hz, uint8_t level, bool dmr)
{
	uint16_t drive;
	uint8_t mod2, mod1;
	int16_t d;
	cs7000p_mk_burst_t b;
	if (m == NULL || !m->valid || !cs7000p_maker_drive(m, hz, level, &drive, NULL) ||
	    !cs7000p_maker_pair(m, hz, dmr, &mod2, &mod1, NULL) || !cs7000p_maker_trim(m, false, &d)) {
		return false;
	}
	return !dmr || (cs7000p_maker_trim(m, true, &d) && cs7000p_maker_burst(m, hz, level, &b, NULL));
}

// AK2365A data sheet, typical: 0.3 V at -100 dBm and 2.0 V at -30 dBm at the chip's input, a straight line between
#define RSSI_LO_MV	300
#define RSSI_LO_DBM	(-100)
#define RSSI_HI_MV	2000
#define RSSI_HI_DBM	(-30)
#define RSSI_MAX_MV	3300	// past the supply nothing is a reading

int16_t cs7000p_rssi_dbm(uint32_t mv)
{
	int32_t v = (int32_t)(mv > RSSI_MAX_MV ? RSSI_MAX_MV : mv) - RSSI_LO_MV;
	int32_t num = v * (RSSI_HI_DBM - RSSI_LO_DBM);
	int32_t den = RSSI_HI_MV - RSSI_LO_MV;
	// Rounded down on both sides of the low point, so the mapping never steps back
	int32_t q = num >= 0 ? num / den : -((-num + den - 1) / den);
	return (int16_t)(RSSI_LO_DBM + q - (CS7000P_FE_GAIN_DB));
}

uint8_t cs7000p_noise(uint32_t raw, bool receiving)
{
	if (!receiving) {
		return UINT8_MAX;
	}
	return (uint8_t)((raw > 4095u ? 4095u : raw) >> 4);
}
