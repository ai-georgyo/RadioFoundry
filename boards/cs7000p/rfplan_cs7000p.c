// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS frequency plan (rfplan_cs7000p.h; docs/spec/hardware-cs7000p.md 7). Pure.
#include "rfplan_cs7000p.h"
#include "rf/sky72310/sky72310.h"
#include "radio/ref_trim.h"

bool cs7000p_rx_band(uint32_t hz)
{
	return hz >= CS7000P_RX_MIN_HZ && hz <= CS7000P_RX_MAX_HZ;
}

bool cs7000p_tx_band(uint32_t hz)
{
	return (hz >= CS7000P_TX_MIN_HZ && hz <= CS7000P_TX_GAP_LO_HZ) ||
	       (hz >= CS7000P_TX_GAP_HI_HZ && hz <= CS7000P_TX_MAX_HZ);
}

bool cs7000p_pll_plan(uint32_t hz, bool tx, bool lo_high, int16_t trim_ppb, cs7000p_pll_plan_t *p)
{
	if (tx ? !cs7000p_tx_band(hz) : !cs7000p_rx_band(hz)) {
		return false;
	}
	// No multiplier anywhere: the transmit VCO is on the carrier, the receive VCO on the first LO
	uint32_t lo_hz = tx ? hz : lo_high ? hz + CS7000P_IF_HZ : hz - CS7000P_IF_HZ;
	uint32_t vco_hz = ref_trim_tune_hz(lo_hz, trim_ppb);
	// The smallest divider the part takes: 1 would put the ratio over its range, so 2 everywhere in this band
	uint8_t ref_div = sky72310_pick_ref_div(CS7000P_F_REF_HZ, vco_hz);
	if (ref_div == 0) {
		return false;
	}
	p->lo_hz = lo_hz;
	p->vco_hz = vco_hz;
	p->ref_div = ref_div;
	return true;
}

const ak2365a_cfg_t cs7000p_if_cfg = {
	.ctrl1_wide = AK2365A_CTRL1(7, true, 0, 1),	// 0xF1
	.ctrl1_narrow = AK2365A_CTRL1(7, false, 2, 1),	// 0xE9
	.ctrl2 = 0x1Cu | AK2365A_CTRL2_CAL,		// 0x1D: the AGC setting the data sheet asks for, calibration start
	.ctrl3 = 0x00,					// IF buffer gain 6 dB
	.ctrl4 = 0x01,
	.ctrl5 = 0x80,
};
