// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS RF chain under drivers/rf/chainseq (rfops_cs7000p.h; docs/spec/hardware-cs7000p.md 7, 13)
#include <stddef.h>
#include "rfops_cs7000p.h"
#include "rfplan_cs7000p.h"
#include "exp_cs7000p.h"
#include "rf/sky72310/sky72310.h"

// --- The sequencer's ops ---

// The sequencer's lines as expander bits. The IF chip's hardware reset is not a line: start-up does it once
static const uint32_t line_bit[CHAINSEQ_LINE_COUNT] = {
	[CHAINSEQ_LINE_SYNTH_RAIL] = EXP_SYNTH_RAIL,
	[CHAINSEQ_LINE_RX_RAIL] = EXP_RX_RAIL,
	[CHAINSEQ_LINE_TX_RAIL] = EXP_TX_RAIL,
	[CHAINSEQ_LINE_VCO_RX] = EXP_VCO_LOW,
	[CHAINSEQ_LINE_APC_LOOP] = EXP_APC_LOOP,
	[CHAINSEQ_LINE_IF_RUN] = EXP_AK_RUN,
};

// One expander write; one the board's interlock turned down is counted with the DAC's
static bool exp_write(cs7000p_rf_t *rf, uint32_t set, uint32_t clear)
{
	if (!rf->io->exp_set(rf->io->ctx, set, clear)) {
		rf->dac_refused++;
		return false;
	}
	return true;
}

static bool op_line(void *ctx, uint8_t line, bool on)
{
	cs7000p_rf_t *rf = ctx;
	if (line >= CHAINSEQ_LINE_COUNT) {
		return false;
	}
	uint32_t bit = line_bit[line];
	if (on && rf->safed && (bit & EXP_TX_BITS) != 0u) {
		return false;	// a safe request stands: the sequence that asked ends here
	}
	return on ? exp_write(rf, bit, 0) : exp_write(rf, 0, bit);
}

static bool op_dac(void *ctx, uint16_t v)
{
	cs7000p_rf_t *rf = ctx;
	if (v != 0u && rf->safed) {
		return false;
	}
	if (!rf->io->dac(rf->io->ctx, v)) {
		rf->dac_refused++;
		return false;
	}
	return true;
}

// The divider follows from the frequency (rfplan_cs7000p.c picked the same one): the sequencer hands on only the
// frequency. Also from the burst runner's interrupt
static bool op_synth(void *ctx, uint32_t vco_hz)
{
	cs7000p_rf_t *rf = ctx;
	uint8_t ref_div = sky72310_pick_ref_div(CS7000P_F_REF_HZ, vco_hz);
	if (ref_div == 0 || !rf->io->synth(rf->io->ctx, ref_div, vco_hz)) {
		return false;
	}
	rf->pll_r = ref_div;
	rf->vco_hz = vco_hz;
	return true;
}

static bool op_locked(void *ctx)
{
	cs7000p_rf_t *rf = ctx;
	return rf->io->locked(rf->io->ctx);
}

static bool op_if_setup(void *ctx, bool wide)
{
	cs7000p_rf_t *rf = ctx;
	rf->ak_ok = rf->io->if_setup(rf->io->ctx, wide);
	return rf->ak_ok;
}

static void op_delay(void *ctx, uint32_t us)
{
	cs7000p_rf_t *rf = ctx;
	rf->io->delay_us(rf->io->ctx, us);
}

static bool op_rx_plan(void *ctx, uint32_t hz, uint32_t *vco_hz, uint16_t *tune_dac)
{
	cs7000p_rf_t *rf = ctx;
	cs7000p_pll_plan_t p;
	if (!cs7000p_pll_plan(hz, false, rf->lo_high, rf->trim_ppb, &p)) {
		return false;
	}
	*vco_hz = p.vco_hz;
	*tune_dac = rf->tune_override != 0u ? cs7000p_cal_tune_clamp(rf->tune_override) : cs7000p_cal_rx_tune(rf->cal, hz);
	rf->rx_tune = *tune_dac;
	return true;
}

// Only for the transmission cs7000p_rf_tx_admit let through, whatever level is asked: a burst's tune asks for level
// 0 and takes the frequency and the floor, and FM's key-up asks for the admitted level
static bool op_tx_plan(void *ctx, uint32_t hz, uint8_t power, uint32_t *vco_hz, uint16_t *drive, uint16_t *floor)
{
	cs7000p_rf_t *rf = ctx;
	cs7000p_pll_plan_t p;
	(void)power;
	if (!rf->admit || hz != rf->admit_hz || !cs7000p_pll_plan(hz, true, false, rf->trim_ppb, &p)) {
		return false;
	}
	*vco_hz = p.vco_hz;
	*drive = rf->admit_drive;
	*floor = rf->cal->floor;
	return true;
}

// The VCO by the channel's frequency (cs7000p_vco_low). The bench's 3:6 gives receive the rule by direction back;
// transmit always follows f_tx. Also from the burst runner's interrupt: reads only
static bool op_vco_line(void *ctx, uint32_t hz, bool tx)
{
	const cs7000p_rf_t *rf = ctx;
	return cs7000p_vco_low(hz, tx, rf->lo_high || (!tx && rf->vco_rx_dir));
}

// --- Set-up, receive ---

void cs7000p_rf_init(cs7000p_rf_t *rf, const cs7000p_rf_io_t *io, const cs7000p_cal_t *cal)
{
	*rf = (cs7000p_rf_t){
		.io = io,
		.cal = cal,
		.lo_high = CS7000P_LO_HIGH != 0,
		.ref_offset = cs7000p_cal_ref_offset(cal),
		.ops = {
			.line = op_line,
			.synth = op_synth,
			.locked = op_locked,
			.if_setup = op_if_setup,
			.dac = op_dac,
			.delay_us = op_delay,
			.rx_plan = op_rx_plan,
			.tx_plan = op_tx_plan,
			.vco_line = op_vco_line,
			.rail_settle_us = CS7000P_RX_RAIL_US,
			.lock_timeout_us = CS7000P_LOCK_US,
			.tx_rail_lead_us = CS7000P_TX_RAIL_US,
		},
	};
	rf->ops.ctx = rf;
	chainseq_init(&rf->seq, &rf->ops);
}

bool cs7000p_rf_start(cs7000p_rf_t *rf, uint32_t now_ms)
{
	if (!exp_write(rf, EXP_SYNTH_RAIL, 0)) {
		return false;
	}
	rf->io->delay_us(rf->io->ctx, CS7000P_RAIL_SETTLE_US);
	rf->synth_on_ms = now_ms;
	rf->synth_on = true;
	return true;
}

bool cs7000p_rf_rx(cs7000p_rf_t *rf, uint32_t hz, bool wide)
{
	rf->safed = false;
	rf->admit = false;	// a receive set-up ends whatever transmission there was
	if (chainseq_rx(&rf->seq, hz, wide) != CHAINSEQ_OK) {
		return false;
	}
	// The sub-audio filter ahead of the detector's ADC input runs from this rail
	return exp_write(rf, EXP_AUDIO_RAIL, 0);
}

void cs7000p_rf_sleep(cs7000p_rf_t *rf, bool sleep)
{
	if (sleep) {
		chainseq_sleep(&rf->seq, true);
		return;
	}
	rf->safed = false;
	if (rf->seq.state == CHAINSEQ_ST_SLEEP) {
		chainseq_sleep(&rf->seq, false);
	} else if (rf->seq.state == CHAINSEQ_ST_IDLE && rf->seq.rx_hz != 0u) {
		// A safe request came while asleep: the sequencer no longer knows it slept
		(void)cs7000p_rf_rx(rf, rf->seq.rx_hz, rf->seq.wide);
	}
}

// --- Transmit ---

void cs7000p_rf_refuse(cs7000p_rf_t *rf, cs7000p_tx_why_t why)
{
	rf->admit = false;
	rf->tx_refused++;
	rf->tx_refused_why = (uint8_t)why;
}

// The bench drive may be used for this transmission (hardware-cs7000p.md 13.2). Arming it is not enough: any
// transmission would take it, a PTT press or a beacon included, and here it lifts the calibration's refusal
static cs7000p_tx_why_t override_ok(cs7000p_rf_t *rf, const cs7000p_tx_req_t *q)
{
	if (!q->debuglink) {
		return CS7000P_TX_OVR_NO_LINK;
	}
	if (!q->limit_list) {
		return CS7000P_TX_OVR_NO_LIST;	// the bench harness named no frequency, or not this one
	}
	if (q->dmr && !q->bench_dmr) {
		return CS7000P_TX_OVR_DMR;	// the burst key-up has not run on a radio
	}
	if ((uint32_t)(q->now_ms - rf->ovr_ms) > CS7000P_OVERRIDE_MS) {
		rf->ovr_drive = 0;		// it does not wait for a transmission that never came
		return CS7000P_TX_OVR_EXPIRED;
	}
	return CS7000P_TX_OK;
}

cs7000p_tx_why_t cs7000p_rf_tx_admit(cs7000p_rf_t *rf, const cs7000p_tx_req_t *q)
{
	cs7000p_tx_why_t why = CS7000P_TX_OK;
	uint16_t drive = 0;
	bool ovr = false;

	if (q->off) {
		// First: a radio that switched itself off keys nothing, whatever the pack reads since and whatever is
		// armed. The speaker and the LEDs are off in that state, so nothing would show the transmission
		cs7000p_rf_refuse(rf, CS7000P_TX_OFF);
		return CS7000P_TX_OFF;
	}
	rf->safed = false;
	rf->admit = false;
	if (!cs7000p_tx_band(q->hz)) {
		why = CS7000P_TX_BAND;
	} else {
		uint16_t armed = rf->ovr_drive;
		cs7000p_tx_why_t ovr_why = armed != 0u ? override_ok(rf, q) : CS7000P_TX_UNCAL;
		if (ovr_why == CS7000P_TX_OK && rf->ovr_drive != armed) {
			// Armed anew (another task) between the two reads: the time just checked may be the new drive's
			// and the drive the old one. Neither is used; the next key-up takes the new pair
			ovr_why = CS7000P_TX_UNCAL;
		}
		if (ovr_why == CS7000P_TX_OK) {
			drive = armed;
			ovr = true;
		} else if (!cs7000p_cal_tx_ok(rf->cal, q->hz, q->power, q->dmr) ||
			   !cs7000p_cal_drive(rf->cal, q->hz, q->power, &drive)) {
			why = ovr_why;	// never key without calibration
		}
	}
	if (why == CS7000P_TX_OK && (drive == 0u || drive > CS7000P_DAC_MAX || drive <= rf->cal->floor)) {
		why = CS7000P_TX_UNCAL;	// a drive that keys nothing, or one the DAC can't take
	}
	if (why == CS7000P_TX_OK && q->battery_mv < q->battery_critical_mv) {
		why = CS7000P_TX_BATTERY;
	}
	if (why == CS7000P_TX_OK &&
	    (!rf->synth_on || (uint32_t)(q->now_ms - rf->synth_on_ms) < (uint32_t)(CS7000P_REF_SETTLE_MS))) {
		why = CS7000P_TX_REF_SETTLE;
	}
	if (why != CS7000P_TX_OK) {
		cs7000p_rf_refuse(rf, why);
		return why;
	}
	rf->admit_hz = q->hz;
	rf->admit_power = q->power;
	rf->admit_drive = drive;
	rf->admit_ovr = ovr;
	rf->admit_dmr = q->dmr;
	rf->admit = true;
	return CS7000P_TX_OK;
}

bool cs7000p_rf_tx_begin(cs7000p_rf_t *rf)
{
	if (!rf->admit) {
		return false;
	}
	if (!op_dac(rf, 0) || !exp_write(rf, 0, EXP_RX_RAIL)) {
		chainseq_safe(&rf->seq);
		cs7000p_rf_refuse(rf, CS7000P_TX_BUS);
		return false;
	}
	return true;
}

cs7000p_tx_why_t cs7000p_rf_tx_key(cs7000p_rf_t *rf)
{
	cs7000p_tx_why_t why;
	if (!rf->admit) {
		return CS7000P_TX_UNCAL;
	}
	switch (chainseq_tx(&rf->seq, rf->admit_hz, rf->admit_power)) {
	case CHAINSEQ_OK: return CS7000P_TX_OK;
	case CHAINSEQ_UNLOCKED: why = CS7000P_TX_UNLOCKED; break;	// no carrier off frequency
	case CHAINSEQ_REFUSED_CAL: why = CS7000P_TX_UNCAL; break;
	default: why = CS7000P_TX_BUS; break;
	}
	cs7000p_rf_refuse(rf, why);
	return why;
}

void cs7000p_rf_tx_off(cs7000p_rf_t *rf, bool keep)
{
	bool bursts = rf->admit && rf->admit_dmr;
	rf->safed = false;
	rf->admit = false;
	rf->ovr_drive = 0;	// one transmission only
	if (rf->seq.state != CHAINSEQ_ST_RX) {
		chainseq_tx_off(&rf->seq);	// down in order, then the whole receive set-up
	} else if (bursts && !keep) {
		// A burst transmission's last switch put the receiver back without the IF chip's set-up, and the
		// sequencer ends nothing while it receives: the whole set-up here
		(void)chainseq_rx(&rf->seq, rf->seq.rx_hz, rf->seq.wide);
	}
}

void cs7000p_rf_burst_drives(const cs7000p_rf_t *rf, uint16_t *drive, uint16_t *top, uint16_t *floor)
{
	uint16_t t = 0;
	*drive = rf->admit ? rf->admit_drive : 0u;
	if (!cs7000p_cal_drive(rf->cal, rf->admit_hz, CS7000P_POWER_LEVELS - 1u, &t) || t < *drive) {
		t = *drive;
	}
	*top = t;
	*floor = rf->cal->floor;
}

void cs7000p_rf_safe(cs7000p_rf_t *rf)
{
	if (rf->io == NULL) {
		return;		// before cs7000p_rf_init: nothing is set up and nothing is on
	}
	rf->safed = true;	// first: what a sequence writes from here on is checked against it
	rf->restore = true;
	// The state only. The caller has written the four lines itself, and this is the end of every task's deepest
	// path (an assert into reboot()): a second pass through the ops and the interlock does not fit the stacks
	chainseq_safed(&rf->seq);
}

bool cs7000p_rf_restore_take(cs7000p_rf_t *rf)
{
	if (!rf->restore) {
		return false;
	}
	rf->restore = false;
	return true;
}

// --- The PA drive for the bench ---

bool cs7000p_rf_drive(cs7000p_rf_t *rf, uint16_t drive, uint32_t now_ms)
{
	if (drive > CS7000P_DAC_MAX) {
		return false;
	}
	rf->ovr_drive = 0;	// never a new time with the old drive
	rf->ovr_ms = now_ms;
	rf->ovr_drive = drive;
	return true;
}

uint16_t cs7000p_rf_drive_level(const cs7000p_rf_t *rf, uint32_t hz, uint8_t level)
{
	uint16_t d;
	return cs7000p_tx_band(hz) && cs7000p_cal_drive(rf->cal, hz, level, &d) ? d : 0u;
}

uint16_t cs7000p_rf_drive_top(const cs7000p_rf_t *rf)
{
	return rf->seq.state == CHAINSEQ_ST_TX ? cs7000p_rf_drive_level(rf, rf->seq.tx_hz, CS7000P_POWER_LEVELS - 1u) : 0u;
}

bool cs7000p_rf_mod_pair(const cs7000p_rf_t *rf, uint32_t hz, bool dmr, uint8_t *mod2, uint8_t *mod1)
{
	if (cs7000p_cal_mod_pair(rf->cal, hz, dmr, mod2, mod1)) {
		return true;
	}
	if (!dmr && rf->admit && rf->admit_ovr && !rf->admit_dmr && hz == rf->admit_hz) {
		*mod2 = 0;	// the bench's carrier: no pair is known, so no modulation
		*mod1 = 0;
		return true;
	}
	return false;
}
