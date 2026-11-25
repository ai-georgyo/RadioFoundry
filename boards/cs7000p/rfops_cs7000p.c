// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS RF chain under drivers/rf/chainseq (rfops_cs7000p.h; docs/spec/hardware-cs7000p.md 7, 13)
#include <stddef.h>
#include "rfops_cs7000p.h"
#include "rfplan_cs7000p.h"
#include "exp_cs7000p.h"
#include "subpwm_cs7000p.h"
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
	// The bench's value (chip_reg 3:0), else the maker's tune row, else the model's or the nominal value
	if (rf->tune_override != 0u) {
		*tune_dac = cs7000p_cal_tune_clamp(rf->tune_override);
		rf->tune_src = CS7000P_SRC_BENCH;
	} else {
		*tune_dac = cs7000p_cal_rx_tune_m(rf->cal, rf->maker, hz, &rf->tune_src);
	}
	rf->rx_tune = *tune_dac;
	return true;
}

// Only for the transmission cs7000p_rf_tx_admit let through, whatever level is asked: a burst's tune asks for level
// 0 and takes the frequency and the floor, and FM's key-up asks for the admitted level
static bool op_tx_plan(void *ctx, uint32_t hz, uint8_t power, uint32_t *vco_hz, uint16_t *drive, uint16_t *floor)
{
	cs7000p_rf_t *rf = ctx;
	cs7000p_pll_plan_t p;
	// Once: the core's ref_trim stores it from any task, at any time
	const int16_t trim = rf->trim_ppb;
	(void)power;
	if (!rf->admit || hz != rf->admit_hz || !cs7000p_pll_plan(hz, true, false, trim, &p)) {
		return false;
	}
	if (rf->admit_arm && !cs7000p_arm_covers(rf->arm, rf->admit_power, rf->admit_dmr)) {
		return false;	// on the maker's values, and the arm lowered since the admission: nothing keys
	}
	if (rf->admit_arm && trim != 0) {
		// On the maker's values, and a reference trim stored since the admission, which refuses one with a trim
		// (16): the plan would carry it on top of trim A. Between FM's admission and its tune, or before any tune of a
		// DMR transmission on its arm (the first, and each after the receiver came back between transmissions)
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
		.rx_ref = cs7000p_cal_ref_offset(cal),
		.r04 = cs7000p_cal_ref_offset(cal),
		.ref_why = CS7000P_RXREF_WHY_TABLE,	// until the radio gives the table in use and receives
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
	rf->admit_arm = false;
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
	rf->admit_arm = false;
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
	bool ovr = false, on_arm = false;
	// A transmission on the arm not yet followed by a receive set-up (the arm lowered under a DMR transmission, and
	// its next arm before the receiver came back) holds the PA as the arm does (cs7000p_rf_arm_holds)
	const bool was_arm = rf->admit && rf->admit_arm;

	if (q->off) {
		// First: a radio that switched itself off keys nothing, whatever the pack reads since and whatever is
		// armed. The speaker and the LEDs are off in that state, so nothing would show the transmission
		cs7000p_rf_refuse(rf, CS7000P_TX_OFF);
		return CS7000P_TX_OFF;
	}
	rf->safed = false;
	rf->admit = false;
	rf->admit_arm = false;
	if (rf->ref_coding != CS7000P_REF_HOLD || q->ref_other) {
		// A receive test of the maker's coding: transmit was never looked at under it, and it is no bench state to
		// key a carrier in. Asked for, or still the modem's after the bench went back to the hold and before the
		// status poll's set-up took that: a burst sequence would keep the modem's (its hold write is the coding
		// last taken). Before the bench drive, which is neither used nor dropped
		why = CS7000P_TX_REF_CODING;
	} else if (!cs7000p_tx_band(q->hz)) {
		why = CS7000P_TX_BAND;
	} else {
		uint16_t armed = rf->ovr_drive;
		cs7000p_tx_why_t ovr_why = CS7000P_TX_UNCAL;
		if (armed != 0u && (rf->arm != CS7000P_ARM_OFF || was_arm)) {
			// The bench drive and the owner's arm exclude each other (13.2): neither is set while the other is
			// (cs7000p_rf_drive, the radio's tx_arm), so this is the rule's second line. The drive is neither used
			// nor dropped: the view shows it until the bench clears it
			ovr_why = CS7000P_TX_ARM_BENCH;
		} else if (armed != 0u) {
			ovr_why = override_ok(rf, q);
		}
		if (ovr_why == CS7000P_TX_OK && rf->ovr_drive != armed) {
			// Armed anew (another task) between the two reads: the time just checked may be the new drive's
			// and the drive the old one. Neither is used; the next key-up takes the new pair
			ovr_why = CS7000P_TX_UNCAL;
		}
		if (ovr_why == CS7000P_TX_ARM_BENCH) {
			why = ovr_why;
		} else if (ovr_why == CS7000P_TX_OK) {
			drive = armed;
			ovr = true;
		} else if (ovr_why == CS7000P_TX_UNCAL && cs7000p_cal_maker_tx(rf->maker, q->hz, q->power, q->dmr)) {
			// The maker's table has this transmission's values, and wins over the model's (it is this unit's).
			// No bench drive armed: a bench key-up keeps its own reasons. The owner's arm decides first, then a
			// list must name hz; then the transmission keys on the table's drive at hz, the pair and the trim
			// following from the same getter: FM's in the modem's set-up, DMR's at the burst plan (the pair at
			// f_tx, trim B in 4:0x04). An FM tone channel also needs the table's level for its tone at hz
			// (hardware-cs7000p.md 13.2), else it is refused as without values, never on the model's rows nor at
			// the build's depth. The bursts' timing stays the port's key-up
			cs7000p_fm_tx_t v;
			cs7000p_dmr_tx_t w;
			uint8_t tone_level;
			why = cs7000p_arm_why(rf->arm, q->power, q->dmr, q->limit_list);
			if (why == CS7000P_TX_OK && (q->pc8_bench || rf->trim_ppb != 0 || cs7000p_rf_ref_bench(rf))) {
				// Trim A is the table's whole correction of the reference: a bench PC8 idle, a stored trim
				// or the bench's reference offset on top of it is an offset nobody checked with the table
				why = CS7000P_TX_ARM_REF;
			}
			if (why == CS7000P_TX_OK && q->bench_chips) {
				why = CS7000P_TX_ARM_CHIPS;	// a bench register in either chip, not the driver's
			}
			if (why == CS7000P_TX_OK && q->dmr && q->bench_keyup) {
				why = CS7000P_TX_ARM_BENCH;	// the bursts' key-up would be the bench's, a boost over the drive
			}
			if (why == CS7000P_TX_OK) {
				if (q->dmr ? !cs7000p_rf_maker_dmr(rf, q->hz, q->power, &w)
					   : (!cs7000p_rf_maker_fm(rf, q->hz, q->power, &v) ||
					      (q->tone && !cs7000p_cal_maker_tone(rf->maker, q->hz, &q->tone_sel, false,
										  &tone_level)))) {
					why = CS7000P_TX_UNCAL;
				} else {
					drive = q->dmr ? w.drive : v.drive;
					on_arm = true;
				}
			}
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
	rf->admit_arm = on_arm;	// else the model's values or the bench drive: the arm has no say
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
	rf->admit_arm = false;
	rf->ovr_drive = 0;	// one transmission only
	if (rf->seq.state != CHAINSEQ_ST_RX) {
		chainseq_tx_off(&rf->seq);	// down in order, then the whole receive set-up
	} else if (bursts && !keep) {
		// A burst transmission's last switch put the receiver back without the IF chip's set-up, and the
		// sequencer ends nothing while it receives: the whole set-up here
		(void)chainseq_rx(&rf->seq, rf->seq.rx_hz, rf->seq.wide);
	}
}

// --- The owner's transmit arm ---

bool cs7000p_rf_arm_ends(const cs7000p_rf_t *rf, uint8_t stage)
{
	return rf->admit && rf->admit_arm && !cs7000p_arm_covers(stage, rf->admit_power, rf->admit_dmr);
}

void cs7000p_rf_arm_set(cs7000p_rf_t *rf, uint8_t stage, uint32_t id)
{
	rf->arm = CS7000P_ARM_OFF;	// never the new id under the old stage
	rf->arm_id = stage != CS7000P_ARM_OFF ? id : 0u;
	rf->arm = stage;
}

// The DMR transmission admitted on the maker's values at hz, and its values while the arm still covers it
static bool armed_dmr(const cs7000p_rf_t *rf, uint32_t hz, cs7000p_dmr_tx_t *v)
{
	return rf->admit && rf->admit_arm && rf->admit_dmr && hz == rf->admit_hz &&
	       cs7000p_rf_maker_dmr(rf, hz, rf->admit_power, v);
}

void cs7000p_rf_burst_drives(const cs7000p_rf_t *rf, uint16_t *drive, uint16_t *top, uint16_t *floor)
{
	uint16_t t = 0;
	cs7000p_dmr_tx_t v;
	if (rf->admit && rf->admit_arm) {
		// On the maker's values: the admitted drive and the top level's from the same getter (the stage that covers
		// DMR covers both levels), never the model's; nothing once the arm no longer covers the transmission, so
		// the plan refuses (a drive of 0)
		*floor = rf->cal->floor;
		if (!armed_dmr(rf, rf->admit_hz, &v)) {
			*drive = *top = 0;
			return;
		}
		*drive = rf->admit_drive;
		*top = cs7000p_rf_maker_dmr(rf, rf->admit_hz, CS7000P_POWER_LEVELS - 1u, &v) && v.drive > *drive ? v.drive
														  : *drive;
		return;
	}
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
	rf->arm = CS7000P_ARM_OFF;	// the owner arms again, after a fault path or the power's off state
	rf->arm_id = 0;
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
	if (drive > CS7000P_DAC_MAX || (drive != 0u && cs7000p_rf_arm_holds(rf))) {
		return false;
	}
	rf->ovr_drive = 0;	// never a new time with the old drive
	rf->ovr_ms = now_ms;
	rf->ovr_drive = drive;
	return true;
}

bool cs7000p_rf_maker_fm(const cs7000p_rf_t *rf, uint32_t hz, uint8_t level, cs7000p_fm_tx_t *v)
{
	const uint8_t arm = rf->arm;	// once: the debug link's task may change it
	return cs7000p_arm_covers(arm, level, false) && cs7000p_cal_maker_fm(rf->maker, hz, level, v);
}

bool cs7000p_rf_maker_dmr(const cs7000p_rf_t *rf, uint32_t hz, uint8_t level, cs7000p_dmr_tx_t *v)
{
	const uint8_t arm = rf->arm;
	return cs7000p_arm_covers(arm, level, true) && cs7000p_cal_maker_dmr(rf->maker, hz, level, v);
}

uint16_t cs7000p_rf_drive_level(const cs7000p_rf_t *rf, uint32_t hz, uint8_t level)
{
	uint16_t d;
	cs7000p_fm_tx_t v;
	if (!cs7000p_tx_band(hz)) {
		return 0u;
	}
	if (cs7000p_cal_maker_tx(rf->maker, hz, level, false)) {
		// The table's level, as the key-up would take it: only under the arm
		return cs7000p_rf_maker_fm(rf, hz, level, &v) ? v.drive : 0u;
	}
	return cs7000p_cal_drive(rf->cal, hz, level, &d) ? d : 0u;
}

uint16_t cs7000p_rf_drive_top(const cs7000p_rf_t *rf)
{
	return rf->seq.state == CHAINSEQ_ST_TX ? cs7000p_rf_drive_level(rf, rf->seq.tx_hz, CS7000P_POWER_LEVELS - 1u) : 0u;
}

bool cs7000p_rf_drive_now_ok(const cs7000p_rf_t *rf, uint16_t drive)
{
	if (!cs7000p_rf_arm_holds(rf)) {
		return true;
	}
	// Only on the transmission the arm admitted, and never above its drive: an armed radio has no other carrier
	return rf->admit && rf->admit_arm && drive <= rf->admit_drive;
}

int8_t cs7000p_rf_rx_ref(cs7000p_rf_t *rf)
{
	if (rf->ref_override) {
		rf->ref_src = CS7000P_SRC_BENCH;
		rf->ref_why = CS7000P_RXREF_WHY_NONE;
		rf->rx_ref = rf->ref_offset;
	} else {
		rf->rx_ref = cs7000p_cal_rx_ref_m(rf->cal, rf->maker, &rf->ref_src, &rf->ref_why);
	}
	return rf->rx_ref;
}

int16_t cs7000p_rf_rssi(const cs7000p_rf_t *rf, uint32_t raw12, uint32_t mv, uint8_t *src)
{
	if (rf->rssi_sheet) {
		if (src != NULL) {
			*src = CS7000P_SRC_BENCH;
		}
		return cs7000p_rssi_dbm(mv);
	}
	return cs7000p_cal_rssi_m(rf->maker, raw12, mv, src);
}

// The FM transmission admitted on the maker's values at hz, and its values while the arm still covers it
static bool armed_fm(const cs7000p_rf_t *rf, uint32_t hz, cs7000p_fm_tx_t *v)
{
	return rf->admit && rf->admit_arm && !rf->admit_dmr && hz == rf->admit_hz &&
	       cs7000p_rf_maker_fm(rf, hz, rf->admit_power, v);
}

int8_t cs7000p_rf_ref_for(cs7000p_rf_t *rf, bool tx_setup)
{
	cs7000p_fm_tx_t v;
	if (!tx_setup) {
		return cs7000p_rf_rx_ref(rf);
	}
	// 4:0x04 in FM transmit: trim A, in the maker's own coding there (d mod 256). The bench's 3:4 moves receive and
	// transmit together, as without the table
	if (!rf->ref_override && armed_fm(rf, rf->admit_hz, &v)) {
		return v.trim;
	}
	return rf->ref_offset;
}

int8_t cs7000p_rf_r04(cs7000p_rf_t *rf, bool dmr, bool tx_setup)
{
	cs7000p_dmr_tx_t v;
	int16_t d;
	int8_t r = rf->ref_offset;	// today's, and the bench's 3:4 while it is set
	if (dmr && !rf->ref_override) {
		if (tx_setup) {
			// The DMR key-up: trim B for the transmission admitted on the table while the arm covers it; a bench
			// key-up or the model's keeps today's
			if (armed_dmr(rf, rf->admit_hz, &v)) {
				r = v.trim;
			}
		} else if (cs7000p_arm_covers(rf->arm, 0, true) && cs7000p_maker_trim(rf->maker, true, &d)) {
			// A DMR receive entry while the arm covers DMR: what the next burst carries (nothing else writes 4:0x04
			// before it), and the maker's own state in DMR receive after a transmission (calibration-cs7000p.md
			// 6.4). Within CS7000P_MAKER_TRIM_MAX of 0: the group passed
			r = (int8_t)d;
		}
	}
	rf->r04 = r;
	return r;
}

bool cs7000p_rf_mod_pair(const cs7000p_rf_t *rf, uint32_t hz, bool dmr, uint8_t *mod2, uint8_t *mod1)
{
	cs7000p_fm_tx_t v;
	cs7000p_dmr_tx_t w;
	if (rf->admit && rf->admit_arm) {
		// The table's pair for the mode and frequency admitted, or none: never the model's, nor the bench's carrier,
		// nor DMR's safe gain under a key-up on the table's drive
		if (dmr ? !armed_dmr(rf, hz, &w) : !armed_fm(rf, hz, &v)) {
			return false;
		}
		*mod2 = dmr ? w.mod2 : v.mod2;
		*mod1 = dmr ? w.mod1 : v.mod1;
		return true;
	}
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

// The maker's FM transmit registers (calibration-cs7000p.md 5.13): 4:0x3F the limiter (0x04 narrow, 0x07 wide), 4:0x35
// the voice deviation (0x20, the one value recorded: on wide it under-deviates if the maker's differs, the safe side),
// 4:0x34 0xBC, 4:0x3E 0x06, and the microphone gain 4:0xE4 (0x20 narrow, 0x22 wide) as a register, so the IF host
// writes the set-up's gain back with the rest at the end. The receive registers (4:0x37, the AK2365A words) stay the
// port's
static const hrc6000_if_row_t maker_fm_rows[2][5] = {
	{ { 4, 0x3F, 0x04 }, { 4, 0x35, 0x20 }, { 4, 0x34, 0xBC }, { 4, 0x3E, 0x06 }, { 4, 0xE4, 0x20 } },
	{ { 4, 0x3F, 0x07 }, { 4, 0x35, 0x20 }, { 4, 0x34, 0xBC }, { 4, 0x3E, 0x06 }, { 4, 0xE4, 0x22 } },
};

bool cs7000p_rf_tone_depth(const cs7000p_rf_t *rf, uint32_t hz, bool wide, const subtone_t *tone, uint16_t *depth_pm)
{
	cs7000p_fm_tx_t v;
	uint8_t level;
	if (!rf->admit || rf->admit_dmr) {
		return false;
	}
	if (!rf->admit_arm) {
		*depth_pm = (uint16_t)(CS7000P_CTCSS_DEPTH_PM);	// the model's rows or the bench drive: the build's depth
		return true;
	}
	// The table's level or none, while the arm still covers the transmission: never the build's depth on it
	if (!armed_fm(rf, hz, &v) || !cs7000p_cal_maker_tone(rf->maker, hz, tone, wide, &level)) {
		return false;
	}
	*depth_pm = cs7000p_subpwm_depth_pm(level);
	return true;
}

const hrc6000_if_row_t *cs7000p_rf_fm_tx_rows(const cs7000p_rf_t *rf, uint32_t hz, bool wide, uint8_t *n)
{
	cs7000p_fm_tx_t v;
	if (!armed_fm(rf, hz, &v)) {
		*n = 0;
		return NULL;
	}
	*n = (uint8_t)(sizeof(maker_fm_rows[0]) / sizeof(maker_fm_rows[0][0]));
	return maker_fm_rows[wide ? 1 : 0];
}
