// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS RF chain (rfops_cs7000p.c under drivers/rf/chainseq) on a fake expander and DAC that carry
// the board's own interlock (exp_cs7000p.h), a fake synthesizer and IF chip. Checked at every write: the transmit
// rail only changes with the DAC at or under the floor and the loop open, the VCO line only moves with the transmit
// rail off and the loop open, with the transmit rail on it is the line cs7000p_vco_low gives the carrier, and the
// interlock's rule holds. Then the orders of receive, FM transmit and a burst, and every way a transmission is
// refused: no calibration (nothing is written), the bench drive's conditions one by one, the reference's settle
// time, the battery, the power's off state, an unlocked synthesizer, a refused write, a safe request from outside.
// Last the VCO line by frequency: the owner's 433.5 MHz receive set-up and bench key-up write for write as before
// the rule, and the line on both sides of each switch point. Then the same run with the maker's table in use: only
// the receive tuning values move, and nothing transmits on it unarmed. Last the owner's transmit arm over a table that
// holds every transmit value (invented): why 13 unarmed, 15 armed without a list, 14 under the bench's coding, at
// every level and mode, and no transmit value from any getter while the arm doesn't cover it; armed and listed, FM
// admitted on the table's drive at f_tx for its level (pinned values), its pair, trim A and the maker's registers, a
// tone channel too where the table gives its tone's level, at that level's depth (pinned), and a tone without a level
// 2; DMR at the top stage on the table's drive, its DMR pair at f_tx (a split channel too) and trim B, with bursts
// under the chain's rules; the switch window and the extensions refused; the arm lowered between the check and the
// key-up keys nothing; the bench drive's key-up as before at every stage; a lower stage ending a covered
// transmission; 4:0x04 trim B in DMR on the top stage only.
#include <stdio.h>
#include <string.h>
#include "rfops_cs7000p.h"
#include "rfplan_cs7000p.h"
#include "exp_cs7000p.h"
#include "subpwm_cs7000p.h"
#include "radio/pa_keyup.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define MHZ	1000000u

// --- The fake board ---

enum { EV_EXP, EV_DAC, EV_SYNTH, EV_IF, EV_DELAY };
typedef struct {
	uint8_t type;
	uint32_t a, b;	// EXP: set, clear. DAC: value. SYNTH: divider, Hz. IF: wide. DELAY: us
} ev_t;

#define EV_MAX	512
static ev_t ev[EV_MAX];
static int ev_n;

static cs7000p_rf_t rf;
static uint32_t shadow;		// the expander's outputs
static uint16_t dac_now;	// the shared DAC, 0 = off
static uint16_t il_floor;	// the interlock's floor (the build's CS7000P_APC_FLOOR)
static bool pll_locked;
static uint32_t us_total;
static int trips;		// writes the interlock refused
static int inv_fail;		// writes that broke a rule of the chain
static uint32_t refuse_set;	// expander bits the fake refuses to set (a fault to inject)
static int dac_fail_in;		// the n-th DAC write over the floor from now fails (0: none)
static int safe_in_delays;	// a safe request from outside lands in the n-th delay from now (0: none)
static int safe_at_ev;		// the log's length when it landed (-1: not yet)
static uint16_t drive_cap;	// the largest reference a closed loop may see

static void log_ev(uint8_t type, uint32_t a, uint32_t b)
{
	if (ev_n < EV_MAX) {
		ev[ev_n++] = (ev_t){ type, a, b };
	} else {
		inv_fail++;
	}
}

// After every write that was let through
static void rules(uint32_t before)
{
	if (((before ^ shadow) & EXP_TX_RAIL) != 0u && (dac_now > il_floor || ((before | shadow) & EXP_APC_LOOP) != 0u)) {
		inv_fail++;	// the transmit rail moved under a reference or a closed loop
	}
	if (((before ^ shadow) & EXP_VCO_LOW) != 0u && ((before | shadow) & EXP_TX_BITS) != 0u) {
		inv_fail++;	// the VCO line moved with the transmit rail on or the loop closed
	}
	if ((shadow & EXP_TX_RAIL) != 0u && rf.admit_hz != 0u &&
	    ((shadow & EXP_VCO_LOW) != 0u) != cs7000p_vco_low(rf.admit_hz, true, false)) {
		inv_fail++;	// the transmitter powered with another VCO than the carrier's
	}
	if ((shadow & EXP_APC_LOOP) != 0u && ((shadow & EXP_TX_RAIL) == 0u || dac_now > drive_cap)) {
		inv_fail++;	// the loop closed without its rail, or on something that is no drive
	}
	if (!exp_dac_ok(shadow, dac_now, il_floor)) {
		inv_fail++;
	}
	if ((shadow & (EXP_POWER_HOLD | EXP_MOTOR | EXP_BT_ON | EXP_SPARE | EXP_AF_MUTE)) != 0u) {
		inv_fail++;	// not the radio's bits
	}
}

// cs7000p_exp_set as exp_cs7000p.c has it: a transmit bit is refused over the floor, a clear never
static bool f_exp(void *ctx, uint32_t set, uint32_t clear)
{
	(void)ctx;
	if ((set & refuse_set) != 0u) {
		return false;
	}
	if (!exp_set_ok(set, dac_now, il_floor)) {
		trips++;
		return false;
	}
	uint32_t before = shadow;
	uint32_t after = (shadow & ~clear) | set;
	if (!exp_dac_ok(after, dac_now, il_floor)) {
		dac_now = 0;
	}
	shadow = after;
	log_ev(EV_EXP, set, clear);
	rules(before);
	return true;
}

// cs7000p_apc_dac: over the floor only with both transmit bits or neither
static bool f_dac(void *ctx, uint16_t v)
{
	(void)ctx;
	if (v > il_floor && dac_fail_in != 0 && --dac_fail_in == 0) {
		return false;
	}
	if (v > CS7000P_DAC_MAX || (v != 0u && !exp_dac_ok(shadow, v, il_floor))) {
		trips++;
		return false;
	}
	dac_now = v;
	log_ev(EV_DAC, v, 0);
	rules(shadow);
	return true;
}

static bool f_synth(void *ctx, uint8_t ref_div, uint32_t vco_hz)
{
	(void)ctx;
	log_ev(EV_SYNTH, ref_div, vco_hz);
	return true;
}

static bool f_if(void *ctx, bool wide)
{
	(void)ctx;
	log_ev(EV_IF, wide, 0);
	return true;
}

static bool f_locked(void *ctx)
{
	(void)ctx;
	return pll_locked;
}

// board_rf_safe() as exp_cs7000p.c has it: the DAC off, the shadow made safe and shifted, then the radio's part,
// which writes nothing itself (it is the end of every task's deepest path)
static int safe_wrote;
static void board_rf_safe_fake(void)
{
	dac_now = 0;
	shadow = exp_rf_safe_word(shadow);
	int n = ev_n;
	cs7000p_rf_safe(&rf);
	safe_wrote += ev_n - n;
}

static void f_delay(void *ctx, uint32_t us)
{
	(void)ctx;
	us_total += us;
	log_ev(EV_DELAY, us, 0);
	if (safe_in_delays != 0 && --safe_in_delays == 0) {
		safe_at_ev = ev_n;
		board_rf_safe_fake();	// another task or a fault path, in the middle of this sequence
	}
}

static const cs7000p_rf_io_t io = {
	.exp_set = f_exp, .dac = f_dac, .synth = f_synth, .if_setup = f_if, .locked = f_locked, .delay_us = f_delay,
};

// A calibration as a measured radio would have it
static const cs7000p_cal_t cal_ok = {
	.drive = {
		{ 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000 },
		{ 3000, 3000, 3000, 3000, 3000, 3000, 3000, 3000, 3000 },
	},
	.floor = 300,
	.rx_tune = { 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500 },
	.fm_mod2 = { 40, 40, 40, 40, 40, 40, 40, 40, 40 },
	.fm_mod1 = { 50, 50, 50, 50, 50, 50, 50, 50, 50 },
	.dmr_mod2 = { 60, 60, 60, 60, 60, 60, 60, 60, 60 },
	.dmr_mod1 = { 70, 70, 70, 70, 70, 70, 70, 70, 70 },
	.ref_set = true,
	.ref_offset = 3,
};

#define T0		1000u			// when the synthesizer rail came on
#define T_SETTLED	(T0 + CS7000P_REF_SETTLE_MS)

static void clear_log(void)
{
	ev_n = 0;
}

// The maker's table the next bring-up gives the chain (NULL: none, as every test but test_maker_table runs), and the
// owner's transmit arm it sets after the init
static const cs7000p_maker_t *run_maker;
static uint8_t run_arm;

// A radio after reset and start-up, with the given calibration; receiving on rx_hz when that is not 0
static void bring_up(const cs7000p_cal_t *cal, uint32_t rx_hz)
{
	shadow = EXP_RESET_WORD;
	dac_now = 0;
	il_floor = cal->floor;
	pll_locked = true;
	us_total = 0;
	trips = 0;
	inv_fail = 0;
	refuse_set = 0;
	dac_fail_in = 0;
	safe_in_delays = 0;
	drive_cap = CS7000P_DAC_MAX;
	clear_log();
	cs7000p_rf_init(&rf, &io, cal);
	rf.maker = run_maker;	// as the radio sets it after the init
	CHECK(rf.arm == CS7000P_ARM_OFF && rf.arm_id == 0u);	// every start: off
	cs7000p_rf_arm_set(&rf, run_arm, 0x5EEDu);
	CHECK(cs7000p_rf_start(&rf, T0));
	if (rx_hz != 0) {
		CHECK(cs7000p_rf_rx(&rf, rx_hz, true));
	}
	clear_log();
}

static cs7000p_tx_req_t req(uint32_t hz, uint8_t power, bool dmr)
{
	return (cs7000p_tx_req_t){ .hz = hz, .power = power, .dmr = dmr, .now_ms = T_SETTLED, .battery_mv = 7400,
				   .battery_critical_mv = 6000, .debuglink = true, .limit_list = false, .bench_dmr = false };
}

// The first event from index from on that sets (or clears) an expander bit, writes a DAC value, ...; -1: none
static int find_exp(int from, uint32_t bit, bool on)
{
	for (int i = from; i < ev_n; i++) {
		if (ev[i].type == EV_EXP && ((on ? ev[i].a : ev[i].b) & bit) != 0u) {
			return i;
		}
	}
	return -1;
}

static int find_ev(int from, uint8_t type)
{
	for (int i = from; i < ev_n; i++) {
		if (ev[i].type == type) {
			return i;
		}
	}
	return -1;
}

static int find_dac_over(int from, uint16_t over)
{
	for (int i = from; i < ev_n; i++) {
		if (ev[i].type == EV_DAC && ev[i].a > over) {
			return i;
		}
	}
	return -1;
}

static bool tx_bits_ever(void)
{
	return find_exp(0, EXP_TX_RAIL, true) >= 0 || find_exp(0, EXP_APC_LOOP, true) >= 0;
}

static bool is_safe(void)
{
	return (shadow & EXP_TX_BITS) == 0u && (shadow & EXP_VCO_LOW) != 0u;
}

// --- Start-up and receive ---

static void test_start(void)
{
	shadow = EXP_RESET_WORD | EXP_TX_RAIL | EXP_APC_LOOP;	// whatever a fault left: the init makes it safe
	shadow &= ~EXP_VCO_LOW;
	dac_now = 0;
	il_floor = 0;
	drive_cap = CS7000P_DAC_MAX;
	inv_fail = 0;
	clear_log();
	cs7000p_rf_init(&rf, &io, &cs7000p_cal_model);
	CHECK(is_safe() && dac_now == 0 && rf.seq.state == CHAINSEQ_ST_OFF);
	CHECK(find_exp(0, EXP_APC_LOOP, false) < find_exp(0, EXP_TX_RAIL, false));	// the loop opens before its rail goes
	CHECK(find_exp(0, EXP_TX_RAIL, false) < find_exp(0, EXP_VCO_LOW, true));
	CHECK(!rf.lo_high && rf.ref_offset == CS7000P_REF_OFFSET && !rf.synth_on);
	// The synthesizer rail, its settle time, and from when the reference counts as settling
	clear_log();
	us_total = 0;
	CHECK(cs7000p_rf_start(&rf, 5000));
	CHECK((shadow & EXP_SYNTH_RAIL) != 0u && us_total == CS7000P_RAIL_SETTLE_US);
	CHECK(rf.synth_on && rf.synth_on_ms == 5000);
	CHECK(find_exp(0, EXP_SYNTH_RAIL, true) == 0 && find_ev(0, EV_DELAY) == 1);
	CHECK(CS7000P_RAIL_SETTLE_US == 2000 && CS7000P_REF_SETTLE_MS == 30);
}

static void test_receive(void)
{
	bring_up(&cs7000p_cal_model, 0);
	// Out of band: refused, nothing written
	CHECK(!cs7000p_rf_rx(&rf, 399999999u, true) && !cs7000p_rf_rx(&rf, 527000001u, false));
	CHECK(ev_n == 0 && rf.seq.state == CHAINSEQ_ST_OFF);

	CHECK(cs7000p_rf_rx(&rf, 446u * MHZ, true));
	CHECK(rf.seq.state == CHAINSEQ_ST_RX && inv_fail == 0 && trips == 0);
	// The order: safe first (the DAC is about to be a tuning voltage), the VCO line, the first LO on the low
	// side with the divider 2, the receive rail and the IF chip, 1 ms, the IF set-up, the tuning value, the audio rail
	int i_dac0 = find_ev(0, EV_DAC);
	int i_loop = find_exp(0, EXP_APC_LOOP, false), i_txr = find_exp(0, EXP_TX_RAIL, false);
	int i_vco = find_exp(0, EXP_VCO_LOW, true), i_synth = find_ev(0, EV_SYNTH);
	int i_rxr = find_exp(0, EXP_RX_RAIL, true), i_run = find_exp(0, EXP_AK_RUN, true);
	int i_if = find_ev(0, EV_IF), i_tune = find_dac_over(0, 0), i_aud = find_exp(0, EXP_AUDIO_RAIL, true);
	CHECK(i_dac0 == 0 && ev[0].a == 0);
	CHECK(i_dac0 < i_loop && i_loop < i_txr && i_txr < i_vco && i_vco < i_synth && i_synth < i_rxr && i_rxr < i_run);
	CHECK(i_run < i_if && i_if < i_tune && i_tune < i_aud && i_aud == ev_n - 1);
	CHECK(ev[i_synth].a == 2 && ev[i_synth].b == 446u * MHZ - CS7000P_IF_HZ);
	CHECK(ev[i_if].a == 1);
	// 1 ms between the receive rail and the IF chip's set-up: its second LO comes up with that rail
	bool waited = false;
	for (int i = i_run; i < i_if; i++) {
		waited = waited || (ev[i].type == EV_DELAY && ev[i].a == CS7000P_RX_RAIL_US);
	}
	CHECK(waited && CS7000P_RX_RAIL_US == 1000u);
	// The tuning value: the nominal one, with both transmit bits off
	CHECK(ev[i_tune].a == CS7000P_RX_TUNE_NOMINAL && dac_now == CS7000P_RX_TUNE_NOMINAL && rf.rx_tune == dac_now);
	CHECK(is_safe() && (shadow & (EXP_SYNTH_RAIL | EXP_RX_RAIL | EXP_AK_RUN | EXP_AUDIO_RAIL)) ==
			   (EXP_SYNTH_RAIL | EXP_RX_RAIL | EXP_AK_RUN | EXP_AUDIO_RAIL));
	CHECK(rf.pll_r == 2 && rf.vco_hz == 446u * MHZ - CS7000P_IF_HZ && rf.ak_ok);

	// The other LO side (the bench switch), a 12.5 kHz channel, the trim, a bench tuning value
	rf.lo_high = true;
	rf.trim_ppb = 1000;
	rf.tune_override = 4000;
	clear_log();
	CHECK(cs7000p_rf_rx(&rf, 446u * MHZ, false));
	i_synth = find_ev(0, EV_SYNTH);
	CHECK(ev[i_synth].b == 446u * MHZ + CS7000P_IF_HZ + 496u);	// 495.95 MHz + 1 ppm
	CHECK(ev[find_ev(0, EV_IF)].a == 0 && dac_now == CS7000P_TUNE_MAX && inv_fail == 0);
	rf.tune_override = 0;

	// An unlocked receive LO is counted and receive goes on
	pll_locked = false;
	CHECK(cs7000p_rf_rx(&rf, 430u * MHZ, true) && rf.seq.state == CHAINSEQ_ST_RX && rf.seq.unlocked == 1);
	pll_locked = true;

	// Sleep: the receive rail off, the synthesizer rail and the IF chip kept; the way back is a full set-up
	cs7000p_rf_sleep(&rf, true);
	CHECK(rf.seq.state == CHAINSEQ_ST_SLEEP && (shadow & EXP_RX_RAIL) == 0u);
	CHECK((shadow & (EXP_SYNTH_RAIL | EXP_AK_RUN)) == (EXP_SYNTH_RAIL | EXP_AK_RUN));
	cs7000p_rf_sleep(&rf, false);
	CHECK(rf.seq.state == CHAINSEQ_ST_RX && (shadow & EXP_RX_RAIL) != 0u && inv_fail == 0 && trips == 0);
}

// --- What is refused ---

static void test_uncalibrated(void)
{
	bring_up(&cs7000p_cal_model, 446u * MHZ);
	uint32_t shadow0 = shadow;
	uint16_t dac0 = dac_now;
	// Every frequency, level and mode: refused, counted, and no line and no DAC value changes (406 MHz, in the
	// beacon band's guard, as out of band)
	uint32_t n = 0;
	for (uint32_t hz = 400u * MHZ; hz <= 512u * MHZ; hz += 2u * MHZ) {
		const cs7000p_tx_why_t want = cs7000p_tx_band(hz) ? CS7000P_TX_UNCAL : CS7000P_TX_BAND;
		for (uint8_t power = 0; power < CS7000P_POWER_LEVELS; power++) {
			cs7000p_tx_req_t q = req(hz, power, false);
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == want);
			q.dmr = true;
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == want);
			n += 2;
		}
	}
	CHECK(rf.tx_refused == n && rf.tx_refused_why == CS7000P_TX_UNCAL && !rf.admit);
	// And the steps behind the check do nothing without its yes
	CHECK(!cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) != CS7000P_TX_OK);
#if CONFIG_DMR
	CHECK(!chainseq_burst_tune(&rf.seq, 446u * MHZ));
	CHECK(!chainseq_burst_point(&rf.seq, CHAINSEQ_PT_LOOP, 0) && !chainseq_burst_point(&rf.seq, PA_KEYUP_PT_PA, 2000));
#endif
	CHECK(ev_n == 0 && shadow == shadow0 && dac_now == dac0 && rf.seq.state == CHAINSEQ_ST_RX);
	CHECK(cs7000p_rf_drive_level(&rf, 446u * MHZ, 0) == 0 && cs7000p_rf_drive_level(&rf, 446u * MHZ, 1) == 0);
	CHECK(cs7000p_rf_drive_top(&rf) == 0);
	uint8_t m2 = 9, m1 = 9;
	CHECK(!cs7000p_rf_mod_pair(&rf, 446u * MHZ, false, &m2, &m1));
	CHECK(!cs7000p_rf_mod_pair(&rf, 446u * MHZ, true, &m2, &m1));
	// The plain chainseq call with no admission behind it is refused by the plan as well
	CHECK(chainseq_tx(&rf.seq, 446u * MHZ, 0) == CHAINSEQ_REFUSED_CAL && ev_n == 0);
}

// The bench drive lifts the calibration's refusal only with every one of its conditions
static void test_override(void)
{
	bring_up(&cs7000p_cal_model, 446u * MHZ);
	cs7000p_tx_req_t good = req(446u * MHZ, 0, false);
	good.limit_list = true;
	cs7000p_tx_req_t q;

	CHECK(!cs7000p_rf_drive(&rf, CS7000P_DAC_MAX + 1u, T_SETTLED));	// over the DAC's range: not armed
	CHECK(cs7000p_rf_tx_admit(&rf, &good) == CS7000P_TX_UNCAL);		// nothing armed: no list helps

	CHECK(cs7000p_rf_drive(&rf, 600, T_SETTLED));
	// 1: a test TX limit that lists the frequency
	q = good;
	q.limit_list = false;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OVR_NO_LIST && rf.tx_refused_why == CS7000P_TX_OVR_NO_LIST);
	// 2: FM, unless the build allows the bench drive in DMR
	q = good;
	q.dmr = true;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OVR_DMR);
	q.bench_dmr = true;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit && rf.admit_drive == 600 && rf.admit_ovr);
	// 4: the debug link is built
	q = good;
	q.debuglink = false;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OVR_NO_LINK && !rf.admit);
	// 3: at most 60 s since it was armed. A second before the end it holds, a millisecond after it is dropped
	q = good;
	q.now_ms = T_SETTLED + CS7000P_OVERRIDE_MS;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
	q.now_ms = T_SETTLED + CS7000P_OVERRIDE_MS + 1u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OVR_EXPIRED && rf.ovr_drive == 0);
	CHECK(cs7000p_rf_tx_admit(&rf, &good) == CS7000P_TX_UNCAL);	// and stays dropped
	CHECK(CS7000P_OVERRIDE_MS == 60000u);
	CHECK(ev_n == 0);	// none of this wrote anything

	// All four: admitted at the bench drive, with an unmodulated carrier (no pair is known)
	uint8_t m2 = 9, m1 = 9;
	CHECK(cs7000p_rf_drive(&rf, 600, T_SETTLED));
	CHECK(cs7000p_rf_tx_admit(&rf, &good) == CS7000P_TX_OK && rf.admit_hz == 446u * MHZ);
	CHECK(cs7000p_rf_mod_pair(&rf, 446u * MHZ, false, &m2, &m1) && m2 == 0 && m1 == 0);
	CHECK(!cs7000p_rf_mod_pair(&rf, 447u * MHZ, false, &m2, &m1));
	CHECK(!cs7000p_rf_mod_pair(&rf, 446u * MHZ, true, &m2, &m1));
	drive_cap = 600;
	CHECK(cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) == CS7000P_TX_OK);
	CHECK(rf.seq.state == CHAINSEQ_ST_TX && dac_now == 600 && (shadow & EXP_TX_BITS) == EXP_TX_BITS);
	// One transmission: the end drops it, and the next key-up is refused as uncalibrated
	cs7000p_rf_tx_off(&rf, false);
	CHECK(rf.seq.state == CHAINSEQ_ST_RX && is_safe() && rf.ovr_drive == 0 && !rf.admit);
	CHECK(cs7000p_rf_tx_admit(&rf, &good) == CS7000P_TX_UNCAL);
	CHECK(inv_fail == 0 && trips == 0);

	// Outside the transmit ranges nothing helps, the bench drive included
	CHECK(cs7000p_rf_drive(&rf, 600, T_SETTLED));
	q = good;
	q.hz = 406050000u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
	q.hz = 512000001u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
	q.hz = 520u * MHZ;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
	// A drive of 0 disarms
	CHECK(cs7000p_rf_drive(&rf, 0, T_SETTLED) && cs7000p_rf_tx_admit(&rf, &good) == CS7000P_TX_UNCAL);
}

// A calibration with one part missing: the transmission of the mode that needs that part is refused, the other one
// goes ahead, and nothing is written. And the bench drive at or under a measured floor keys nothing
static void cal_unset(uint16_t *row)
{
	for (unsigned i = 0; i < CS7000P_CAL_POINTS; i++) {
		row[i] = CS7000P_CAL_UNSET;
	}
}

static void admit_both(const cs7000p_cal_t *cal, cs7000p_tx_why_t want_fm, cs7000p_tx_why_t want_dmr)
{
	for (uint8_t level = 0; level < CS7000P_POWER_LEVELS; level++) {
		bring_up(cal, 446u * MHZ);
		cs7000p_tx_req_t q = req(446u * MHZ, level, false);
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == want_fm && rf.admit == (want_fm == CS7000P_TX_OK));
		CHECK(want_fm == CS7000P_TX_OK || rf.tx_refused_why == want_fm);
		cs7000p_rf_tx_off(&rf, false);
		clear_log();
		q = req(446u * MHZ, level, true);
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == want_dmr && rf.admit == (want_dmr == CS7000P_TX_OK));
		CHECK(want_dmr == CS7000P_TX_OK || rf.tx_refused_why == want_dmr);
		CHECK(ev_n == 0);	// an admission writes nothing either way
	}
}

static void test_partial_cal(void)
{
	static cs7000p_cal_t c;

	admit_both(&cal_ok, CS7000P_TX_OK, CS7000P_TX_OK);
	// DMR's modulation pair missing: FM goes, DMR does not (judged on its own rows, not on FM's)
	c = cal_ok;
	cal_unset(c.dmr_mod2);
	admit_both(&c, CS7000P_TX_OK, CS7000P_TX_UNCAL);
	c = cal_ok;
	cal_unset(c.dmr_mod1);
	admit_both(&c, CS7000P_TX_OK, CS7000P_TX_UNCAL);
	// FM's missing: the reverse
	c = cal_ok;
	cal_unset(c.fm_mod1);
	admit_both(&c, CS7000P_TX_UNCAL, CS7000P_TX_OK);
	c = cal_ok;
	cal_unset(c.fm_mod2);
	admit_both(&c, CS7000P_TX_UNCAL, CS7000P_TX_OK);
	// No reference offset measured: neither, at any level, though every drive is there
	c = cal_ok;
	c.ref_set = false;
	admit_both(&c, CS7000P_TX_UNCAL, CS7000P_TX_UNCAL);

	// The bench drive on an uncalibrated radio with a measured floor: at or under it nothing keys, one over it does
	c = cs7000p_cal_model;
	c.floor = 300;
	static const struct { uint16_t drive; cs7000p_tx_why_t want; } bench[] = {
		{ 200, CS7000P_TX_UNCAL }, { 300, CS7000P_TX_UNCAL }, { 301, CS7000P_TX_OK },
	};
	for (unsigned i = 0; i < sizeof(bench) / sizeof(bench[0]); i++) {
		bring_up(&c, 446u * MHZ);
		cs7000p_tx_req_t q = req(446u * MHZ, 0, false);
		q.limit_list = true;
		CHECK(cs7000p_rf_drive(&rf, bench[i].drive, T_SETTLED));
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == bench[i].want && rf.admit == (bench[i].want == CS7000P_TX_OK));
		CHECK(ev_n == 0);
		if (bench[i].want != CS7000P_TX_OK) {
			// And the sequence finds no admission behind it: nothing over the floor reaches the DAC
			CHECK(!cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) != CS7000P_TX_OK);
			CHECK(!tx_bits_ever() && find_dac_over(0, 0) < 0);
		}
	}
	CHECK(inv_fail == 0 && trips == 0);
}

static void test_settle_battery_band(void)
{
	bring_up(&cal_ok, 446u * MHZ);
	cs7000p_tx_req_t q = req(446u * MHZ, 0, false);
	// The reference: nothing until 30 ms after the synthesizer rail came on
	q.now_ms = T0;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_REF_SETTLE);
	q.now_ms = T0 + CS7000P_REF_SETTLE_MS - 1u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_REF_SETTLE && rf.tx_refused_why == CS7000P_TX_REF_SETTLE);
	q.now_ms = T0 + CS7000P_REF_SETTLE_MS;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
	// The battery: under its critical level, or without a reading
	q = req(446u * MHZ, 0, false);
	q.battery_mv = 5999;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BATTERY);
	q.battery_mv = 0;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BATTERY);
	q.battery_mv = 6000;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
	// The band, calibrated or not: the grant's rows less half a channel at the gap, the gap, nothing over 512 MHz
	q = req(405987500u, 1, false);
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
	q.hz = 405987501u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
	q.hz = 406u * MHZ;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
	q.hz = 406100000u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
	q.hz = 406112499u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
	q.hz = 406112500u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
	q.hz = 512u * MHZ;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
	q.hz = 512000001u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
	q.hz = 399999999u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
	// No such level
	q = req(446u * MHZ, CS7000P_POWER_LEVELS, false);
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNCAL);
	CHECK(ev_n == 0);
	// A radio whose start-up never ran has no settled reference
	cs7000p_rf_init(&rf, &io, &cal_ok);
	q = req(446u * MHZ, 0, false);
	q.now_ms = 0xFFFFFFF0u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_REF_SETTLE);

	// The drive levels the wattmeter procedure reads
	bring_up(&cal_ok, 446u * MHZ);
	CHECK(cs7000p_rf_drive_level(&rf, 446u * MHZ, 0) == 1000 && cs7000p_rf_drive_level(&rf, 446u * MHZ, 1) == 3000);
	CHECK(cs7000p_rf_drive_level(&rf, 406050000u, 0) == 0 && cs7000p_rf_drive_level(&rf, 520u * MHZ, 1) == 0);
	CHECK(cs7000p_rf_drive_level(&rf, 446u * MHZ, 2) == 0);
}

// The power's off state (a power-off the knob overrode): every transmission is refused, a calibrated one and one on
// the bench drive, FM and DMR, before anything else is looked at
static void test_off_state(void)
{
	CHECK(CS7000P_TX_OFF == 12);	// the hardware page's number: append only
	for (int dmr = 0; dmr < 2; dmr++) {
		bring_up(&cal_ok, 446u * MHZ);
		cs7000p_tx_req_t q = req(446u * MHZ, 1, dmr != 0);
		q.off = true;
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OFF && !rf.admit && rf.tx_refused_why == CS7000P_TX_OFF);
		CHECK(rf.tx_refused == 1 && ev_n == 0);
		CHECK(!cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) != CS7000P_TX_OK && !tx_bits_ever());
		// Whatever else is wrong with the request: this reason first
		q.hz = 520u * MHZ;
		q.battery_mv = 0;
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OFF);
		// An admission from before the off state is gone
		q = req(446u * MHZ, 1, dmr != 0);
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit);
		q.off = true;
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OFF && !rf.admit && !cs7000p_rf_tx_begin(&rf));
		// The safe request that came with the power-off stands
		board_rf_safe_fake();
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OFF && rf.safed && rf.restore);
	}
	// A bench drive that is armed is neither used nor dropped, not even one that has waited too long: it is there
	// for the first transmission after the off state, if that comes in time
	bring_up(&cs7000p_cal_model, 446u * MHZ);
	cs7000p_tx_req_t q = req(446u * MHZ, 0, false);
	q.limit_list = true;
	q.off = true;
	CHECK(cs7000p_rf_drive(&rf, 600, T_SETTLED));
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OFF && !rf.admit && rf.ovr_drive == 600);
	q.now_ms = T_SETTLED + CS7000P_OVERRIDE_MS + 1u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OFF && rf.ovr_drive == 600);
	q.now_ms = T_SETTLED + CS7000P_OVERRIDE_MS;
	q.off = false;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit_ovr && rf.admit_drive == 600);
	CHECK(ev_n == 0 && inv_fail == 0 && trips == 0);
}

// The reference DAC in the maker's coding (chip_reg 3:5): every transmission refused, a calibrated one and one on
// the bench drive, FM and DMR, after the off state and before everything else; the bench drive neither used nor
// dropped
static void test_ref_coding(void)
{
	CHECK(CS7000P_TX_REF_CODING == 14);	// the hardware page's number: append only
	for (uint8_t k = CS7000P_REF_MAKER; k <= CS7000P_REF_MAKER_ADC; k++) {
		for (int dmr = 0; dmr < 2; dmr++) {
			bring_up(&cal_ok, 446u * MHZ);
			rf.ref_coding = k;
			cs7000p_tx_req_t q = req(446u * MHZ, 1, dmr != 0);
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_REF_CODING && !rf.admit);
			CHECK(rf.tx_refused == 1 && rf.tx_refused_why == CS7000P_TX_REF_CODING && ev_n == 0);
			CHECK(!cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) != CS7000P_TX_OK && !tx_bits_ever());
			q.hz = 520u * MHZ;	// whatever else is wrong: this reason
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_REF_CODING);
			q.off = true;		// but the off state's first
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OFF);
		}
	}
	// The hold asked for and the modem still holding the maker's: a calibrated transmission refused all the same
	for (int dmr = 0; dmr < 2; dmr++) {
		bring_up(&cal_ok, 446u * MHZ);
		cs7000p_tx_req_t q = req(446u * MHZ, 1, dmr != 0);
		q.ref_other = true;
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_REF_CODING && !rf.admit && ev_n == 0);
		q.ref_other = false;
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit);
	}
	bring_up(&cs7000p_cal_model, 446u * MHZ);
	rf.ref_coding = CS7000P_REF_MAKER;
	cs7000p_tx_req_t q = req(446u * MHZ, 0, false);
	q.limit_list = true;
	CHECK(cs7000p_rf_drive(&rf, 600, T_SETTLED));
	q.now_ms = T_SETTLED + CS7000P_OVERRIDE_MS + 1u;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_REF_CODING && rf.ovr_drive == 600);
	q.now_ms = T_SETTLED;
	rf.ref_coding = CS7000P_REF_HOLD;
	// The hold asked for, the modem still in the maker's coding (before the status poll takes the way back): the same
	q.ref_other = true;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_REF_CODING && !rf.admit && rf.ovr_drive == 600);
	q.ref_other = false;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit_ovr && rf.admit_drive == 600);
	CHECK(ev_n == 0 && inv_fail == 0 && trips == 0);
	// A new chain is the hold
	cs7000p_rf_init(&rf, &io, &cal_ok);
	CHECK(rf.ref_coding == CS7000P_REF_HOLD);
}

// --- FM transmit ---

static void test_fm_tx(void)
{
	bring_up(&cal_ok, 446u * MHZ);
	cs7000p_tx_req_t q = req(446500000u, 1, false);
	uint8_t m2 = 0, m1 = 0;
	drive_cap = 3000;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit_drive == 3000 && !rf.admit_ovr);
	CHECK(ev_n == 0);	// the check itself writes nothing
	CHECK(cs7000p_rf_mod_pair(&rf, q.hz, false, &m2, &m1) && m2 == 40 && m1 == 50);
	// Steps 1 and 2, ahead of the modem's set-up: the DAC to 0, the receive rail off
	CHECK(cs7000p_rf_tx_begin(&rf));
	CHECK(ev_n == 2 && ev[0].type == EV_DAC && ev[0].a == 0 && ev[1].type == EV_EXP && ev[1].b == EXP_RX_RAIL);
	CHECK(dac_now == 0 && (shadow & EXP_RX_RAIL) == 0u);
	clear_log();
	us_total = 0;
	CHECK(cs7000p_rf_tx_key(&rf) == CS7000P_TX_OK);
	CHECK(rf.seq.state == CHAINSEQ_ST_TX && inv_fail == 0 && trips == 0);
	// The VCO line for the carrier (clear over 430 MHz), the synthesizer on it, locked, the rail, 500 us, the
	// floor, the loop, then the ramp to the drive
	int i_vco = find_exp(0, EXP_VCO_LOW, false), i_synth = find_ev(0, EV_SYNTH);
	int i_txr = find_exp(0, EXP_TX_RAIL, true), i_floor = find_dac_over(0, 0);
	int i_loop = find_exp(0, EXP_APC_LOOP, true), i_ramp = find_dac_over(0, cal_ok.floor);
	CHECK(i_vco >= 0 && i_vco < i_synth && i_synth < i_txr && i_txr < i_floor && i_floor < i_loop && i_loop < i_ramp);
	CHECK(ev[i_synth].a == 2 && ev[i_synth].b == 446500000u);
	CHECK(ev[i_floor].a == cal_ok.floor);
	bool lead = false;
	for (int i = i_txr; i < i_loop; i++) {
		lead = lead || (ev[i].type == EV_DELAY && ev[i].a == CS7000P_TX_RAIL_US);
	}
	CHECK(lead && CS7000P_TX_RAIL_US == 500u);
	// The ramp: eight steps, each at or over the one before, never under the floor, ending on the drive
	uint16_t last = cal_ok.floor;
	int steps = 0;
	for (int i = i_loop; i < ev_n; i++) {
		if (ev[i].type == EV_DAC) {
			CHECK(ev[i].a >= last && ev[i].a <= 3000);
			last = (uint16_t)ev[i].a;
			steps++;
		}
	}
	CHECK(steps == (int)CHAINSEQ_RAMP_STEPS && dac_now == 3000 && last == 3000);
	CHECK((shadow & EXP_TX_BITS) == EXP_TX_BITS && (shadow & (EXP_VCO_LOW | EXP_RX_RAIL)) == 0u);
	CHECK(cs7000p_rf_drive_top(&rf) == 3000);

	// The end: down the ramp to the floor, the loop, the rail, the DAC off, then the whole receive set-up
	clear_log();
	cs7000p_rf_tx_off(&rf, false);
	int o_loop = find_exp(0, EXP_APC_LOOP, false), o_txr = find_exp(0, EXP_TX_RAIL, false);
	int o_vco = find_exp(0, EXP_VCO_LOW, true), o_if = find_ev(0, EV_IF);
	CHECK(o_loop > 0 && o_loop < o_txr && o_txr < o_vco && o_vco < o_if);
	last = 3000;
	for (int i = 0; i < o_loop; i++) {
		if (ev[i].type == EV_DAC) {
			CHECK(ev[i].a <= last && ev[i].a >= cal_ok.floor);
			last = (uint16_t)ev[i].a;
		}
	}
	CHECK(last == cal_ok.floor);
	CHECK(rf.seq.state == CHAINSEQ_ST_RX && is_safe() && dac_now == 1500 && !rf.admit);
	CHECK(ev[find_ev(o_vco, EV_SYNTH)].b == 446u * MHZ - CS7000P_IF_HZ);
	CHECK(inv_fail == 0 && trips == 0 && cs7000p_rf_drive_top(&rf) == 0);
	// After the end the plan answers for nothing: the next key-up needs its own check
	CHECK(cs7000p_rf_tx_key(&rf) != CS7000P_TX_OK && !cs7000p_rf_tx_begin(&rf));
	// Nor after a receive set-up that came between the check and the key-up
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit);
	CHECK(cs7000p_rf_rx(&rf, 446u * MHZ, true) && !rf.admit);
	clear_log();
	CHECK(!cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) != CS7000P_TX_OK && ev_n == 0);
	CHECK(chainseq_tx(&rf.seq, q.hz, q.power) == CHAINSEQ_REFUSED_CAL && ev_n == 0);
}

static void test_fm_refusals(void)
{
	cs7000p_tx_req_t q = req(446500000u, 0, false);

	// Not locked on the carrier: no rail, no loop, no drive; receiving again, and counted
	bring_up(&cal_ok, 446u * MHZ);
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && cs7000p_rf_tx_begin(&rf));
	clear_log();
	pll_locked = false;
	CHECK(cs7000p_rf_tx_key(&rf) == CS7000P_TX_UNLOCKED);
	CHECK(!tx_bits_ever() && find_dac_over(0, CS7000P_TUNE_MAX) < 0);
	CHECK(rf.seq.state == CHAINSEQ_ST_RX && is_safe() && dac_now == 1500 && !rf.admit);
	CHECK(rf.tx_refused == 1 && rf.tx_refused_why == CS7000P_TX_UNLOCKED && inv_fail == 0);

	// The expander refuses the transmit rail: safe, and back to receive
	bring_up(&cal_ok, 446u * MHZ);
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && cs7000p_rf_tx_begin(&rf));
	refuse_set = EXP_TX_RAIL;
	CHECK(cs7000p_rf_tx_key(&rf) == CS7000P_TX_BUS && rf.tx_refused_why == CS7000P_TX_BUS);
	CHECK(is_safe() && rf.seq.state == CHAINSEQ_ST_RX && dac_now == 1500 && inv_fail == 0);
	// ... the loop: the rail that was already on comes off again
	bring_up(&cal_ok, 446u * MHZ);
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && cs7000p_rf_tx_begin(&rf));
	refuse_set = EXP_APC_LOOP;
	CHECK(cs7000p_rf_tx_key(&rf) == CS7000P_TX_BUS);
	CHECK(is_safe() && rf.seq.state == CHAINSEQ_ST_RX && rf.dac_refused != 0 && inv_fail == 0);
	// A DAC write refused in the middle of the ramp: safe at once, nothing left keyed
	for (int n = 1; n <= (int)CHAINSEQ_RAMP_STEPS; n++) {
		bring_up(&cal_ok, 446u * MHZ);
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && cs7000p_rf_tx_begin(&rf));
		dac_fail_in = n;
		CHECK(cs7000p_rf_tx_key(&rf) == CS7000P_TX_BUS);
		CHECK(is_safe() && rf.seq.state == CHAINSEQ_ST_RX && dac_now == 1500 && inv_fail == 0 && !rf.admit);
	}
	// The interlock itself, were the table's floor over the one the board's writer knows: the floor's write is
	// turned down and nothing keys
	bring_up(&cal_ok, 446u * MHZ);
	il_floor = 0;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && cs7000p_rf_tx_begin(&rf));
	clear_log();
	CHECK(cs7000p_rf_tx_key(&rf) == CS7000P_TX_BUS && trips != 0 && rf.dac_refused != 0);
	CHECK(find_exp(0, EXP_APC_LOOP, true) < 0 && is_safe() && inv_fail == 0);
}

// --- board_rf_safe() ---

static void check_safed(void)
{
	CHECK(is_safe() && dac_now == 0 && rf.safed && inv_fail == 0);
	CHECK(rf.seq.state == CHAINSEQ_ST_IDLE || rf.seq.state == CHAINSEQ_ST_OFF);
	// Nothing transmits until the radio task's next request: no drive, no burst piece
	CHECK(!chainseq_drive_now(&rf.seq, 2000));
#if CONFIG_DMR
	CHECK(!chainseq_burst_point(&rf.seq, CHAINSEQ_PT_LOOP, 0) && !chainseq_burst_point(&rf.seq, PA_KEYUP_PT_PA, 2000));
	chainseq_burst_off(&rf.seq);
	CHECK(!chainseq_burst_tune(&rf.seq, rf.admit_hz) || (shadow & EXP_APC_LOOP) == 0u);
	chainseq_safe(&rf.seq);
#endif
	CHECK((shadow & EXP_APC_LOOP) == 0u && dac_now == 0);
}

static void test_rf_safe(void)
{
	cs7000p_tx_req_t q = req(446500000u, 1, false);

	// Before anything is set up (first thing in main: the chain's state is all zeroes)
	memset(&rf, 0, sizeof(rf));
	shadow = EXP_RESET_WORD;
	dac_now = 0;
	clear_log();
	cs7000p_rf_safe(&rf);
	CHECK(ev_n == 0 && !rf.safed && !cs7000p_rf_restore_take(&rf));

	// Off (after init), idle, receiving, asleep
	bring_up(&cal_ok, 0);
	board_rf_safe_fake();
	check_safed();
	bring_up(&cal_ok, 446u * MHZ);
	board_rf_safe_fake();
	check_safed();
	// The receiver comes back with the radio task's next request, and transmitting works again after it
	CHECK(cs7000p_rf_rx(&rf, 446u * MHZ, true) && !rf.safed && rf.seq.state == CHAINSEQ_ST_RX && dac_now == 1500);
	cs7000p_rf_sleep(&rf, true);
	board_rf_safe_fake();
	check_safed();
	cs7000p_rf_sleep(&rf, false);	// the sequencer no longer knows it slept: a full set-up
	CHECK(rf.seq.state == CHAINSEQ_ST_RX && (shadow & EXP_RX_RAIL) != 0u && !rf.safed);

	// In an FM transmission: off at once, and the end the core asks for later brings the receiver back
	drive_cap = 3000;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && cs7000p_rf_tx_begin(&rf));
	CHECK(cs7000p_rf_tx_key(&rf) == CS7000P_TX_OK && dac_now == 3000);
	board_rf_safe_fake();
	check_safed();
	cs7000p_rf_tx_off(&rf, false);
	CHECK(rf.seq.state == CHAINSEQ_ST_RX && is_safe() && dac_now == 1500 && inv_fail == 0);

	// Between the check and the key-up: the key-up finds the request standing and sets no transmit bit
	bring_up(&cal_ok, 446u * MHZ);
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && cs7000p_rf_tx_begin(&rf));
	board_rf_safe_fake();
	clear_log();
	CHECK(cs7000p_rf_tx_key(&rf) == CS7000P_TX_BUS);
	CHECK(!tx_bits_ever() && find_dac_over(0, 0) < 0 && is_safe() && dac_now == 0 && inv_fail == 0);

	// In the middle of a key-up, at each of its waits (the lock's, the rail's lead, every ramp step): what the
	// sequence writes afterwards never sets a transmit bit again and never puts a value on the DAC. With a measured
	// floor the write after the rail's lead is the floor's, which the latch refuses first; with none (the shipped
	// 0) the loop line follows the lead straight away, and only the latch's line check stops it
	static cs7000p_cal_t cal_f0;
	cal_f0 = cal_ok;
	cal_f0.floor = 0;
	for (int f = 0; f < 2; f++) {
		const cs7000p_cal_t *cal = f == 0 ? &cal_ok : &cal_f0;
		for (int n = 1; n <= 12; n++) {
			bring_up(cal, 446u * MHZ);
			drive_cap = 3000;
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && cs7000p_rf_tx_begin(&rf));
			clear_log();
			safe_in_delays = n;
			safe_at_ev = -1;
			cs7000p_tx_why_t why = cs7000p_rf_tx_key(&rf);
			if (safe_in_delays != 0) {
				CHECK(why == CS7000P_TX_OK);	// fewer waits than n: the key-up ran through
				break;
			}
			CHECK(n <= 8 || why == CS7000P_TX_OK || why == CS7000P_TX_BUS);
			CHECK(is_safe() && dac_now == 0 && rf.safed && inv_fail == 0);
			CHECK(rf.seq.state == CHAINSEQ_ST_IDLE);
			// Nothing after the request: no transmit line set, no value on the DAC
			CHECK(safe_at_ev >= 0 && find_exp(safe_at_ev, EXP_APC_LOOP, true) < 0);
			CHECK(find_exp(safe_at_ev, EXP_TX_RAIL, true) < 0 && find_dac_over(safe_at_ev, 0) < 0);
			// The receiver is back with the next request
			CHECK(cs7000p_rf_rx(&rf, 446u * MHZ, true) && dac_now == 1500);
		}
		CHECK(safe_in_delays != 0);	// the loop reached past the key-up's last wait
	}
	safe_in_delays = 0;
	CHECK(safe_wrote == 0);		// no call above went through the callbacks

	// In the middle of a receive set-up: it ends safe and not receiving, and the next one works
	bring_up(&cal_ok, 0);
	safe_in_delays = 1;
	CHECK(!cs7000p_rf_rx(&rf, 446u * MHZ, true) && is_safe() && dac_now == 0 && rf.seq.state == CHAINSEQ_ST_IDLE);
	CHECK(cs7000p_rf_rx(&rf, 446u * MHZ, true) && rf.seq.state == CHAINSEQ_ST_RX);

	// The request to set the receiver up again outlives the latch. On a radio without calibration the next
	// request may be a key-up that is refused: it lifts the latch and writes nothing, and the receiver would stay
	// off if the request had gone with it
	bring_up(&cs7000p_cal_model, 446u * MHZ);
	CHECK(!cs7000p_rf_restore_take(&rf));	// nothing asked yet
	board_rf_safe_fake();
	CHECK(rf.safed && rf.seq.state == CHAINSEQ_ST_IDLE && dac_now == 0);
	clear_log();
	q = req(446500000u, 0, false);
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNCAL && !rf.safed && ev_n == 0);
	CHECK(rf.seq.state == CHAINSEQ_ST_IDLE);	// still not receiving
	CHECK(cs7000p_rf_restore_take(&rf));		// ... and the radio task still learns of it, once
	CHECK(!cs7000p_rf_restore_take(&rf));
	CHECK(cs7000p_rf_rx(&rf, 446u * MHZ, true) && rf.seq.state == CHAINSEQ_ST_RX && dac_now == CS7000P_RX_TUNE_NOMINAL);
	// Nor does a receive set-up or a transmission's end take it: what else went off with the request (the
	// speaker's amplifier) is the caller's to put back
	board_rf_safe_fake();
	CHECK(cs7000p_rf_rx(&rf, 446u * MHZ, true) && !rf.safed);
	cs7000p_rf_tx_off(&rf, false);
	cs7000p_rf_sleep(&rf, true);
	cs7000p_rf_sleep(&rf, false);
	CHECK(cs7000p_rf_restore_take(&rf) && !cs7000p_rf_restore_take(&rf));
	// Two requests before the task looks are one
	board_rf_safe_fake();
	board_rf_safe_fake();
	CHECK(cs7000p_rf_restore_take(&rf) && !cs7000p_rf_restore_take(&rf));
	CHECK(inv_fail == 0);
}

// --- Bursts ---

#if CONFIG_DMR
// One burst as platform/stm32/dmr_keyup runs it: the loop point first (drive 0), then the key-up rule's points
static bool burst(uint16_t boost, uint16_t drive)
{
	if (!chainseq_burst_point(&rf.seq, CHAINSEQ_PT_LOOP, 0)) {
		return false;
	}
	if (boost != 0 && !chainseq_burst_point(&rf.seq, PA_KEYUP_PT_BOOST, boost)) {
		return false;
	}
	if (!chainseq_burst_point(&rf.seq, PA_KEYUP_PT_PA, boost != 0 ? boost : drive)) {
		return false;
	}
	return boost == 0 || chainseq_burst_point(&rf.seq, PA_KEYUP_PT_DRIVE, drive);
}

static void test_bursts(void)
{
	// The constants the runner is given: it takes them (room after the interrupt for the loop's lead, the drop
	// inside the timer's range), and with one fixed point the loop closes 1250 us and the PA keys 1750 us after
	// the modem's RF TX interrupt
	static const pa_keyup_cfg_t fixed = PA_KEYUP_FIXED(CS7000P_KEYUP_PA_US);
	CHECK(CS7000P_KEYUP_LOOP_LEAD_US + 30u < CS7000P_KEYUP_INT_AHEAD_US);
	CHECK(CS7000P_KEYUP_INT_AHEAD_US + 27500u <= 65535u);
	CHECK(CS7000P_KEYUP_INT_AHEAD_US - CS7000P_KEYUP_LOOP_LEAD_US - 30u == 2020u);
	CHECK(fixed.pa_us == 800u && fixed.window_us == 800u && fixed.slope_us == 800u && fixed.knee_us == 0u);
	CHECK(fixed.pa_us <= CS7000P_KEYUP_INT_AHEAD_US - CS7000P_KEYUP_LOOP_LEAD_US - 30u);
	CHECK(CS7000P_KEYUP_INT_AHEAD_US - fixed.pa_us == 1750u);
	CHECK(CS7000P_KEYUP_INT_AHEAD_US - fixed.pa_us - CS7000P_KEYUP_LOOP_LEAD_US == 1250u);

	bring_up(&cal_ok, 446u * MHZ);
	cs7000p_tx_req_t q = req(441u * MHZ, 1, true);
	uint16_t d = 0, top = 0, fl = 0;
	drive_cap = 3000;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
	cs7000p_rf_burst_drives(&rf, &d, &top, &fl);
	CHECK(d == 3000 && top == 3000 && fl == 300);
	// The tune: receiver off, the VCO line for the carrier, the synthesizer on it, the rail; the DAC off and the
	// loop open. The sequencer asks the plan for level 0 here, and the plan answers for the admitted transmission
	// all the same
	CHECK(chainseq_burst_tune(&rf.seq, 441u * MHZ));
	int i_rxr = find_exp(0, EXP_RX_RAIL, false), i_vco = find_exp(0, EXP_VCO_LOW, false);
	int i_synth = find_ev(0, EV_SYNTH), i_txr = find_exp(0, EXP_TX_RAIL, true);
	CHECK(ev[0].type == EV_DAC && ev[0].a == 0 && i_rxr > 0 && i_rxr < i_vco && i_vco < i_synth && i_synth < i_txr);
	CHECK(ev[i_synth].b == 441u * MHZ && dac_now == 0 && (shadow & EXP_TX_BITS) == EXP_TX_RAIL);
	CHECK(rf.seq.state == CHAINSEQ_ST_BURST && find_ev(0, EV_DELAY) < 0);	// interrupt level: no waits
	// Another frequency than the admitted one has no plan
	CHECK(!chainseq_burst_tune(&rf.seq, 442u * MHZ));

	// Two bursts: the loop closes on the floor before any drive, then the drive; the end takes all of it away and
	// leaves the synthesizer on the carrier
	for (int n = 0; n < 2; n++) {
		clear_log();
		CHECK(burst(0, d));
		int i_floor = find_dac_over(0, 0), i_loop = find_exp(0, EXP_APC_LOOP, true), i_drive = find_dac_over(0, fl);
		CHECK(i_floor >= 0 && ev[i_floor].a == fl && i_floor < i_loop && i_loop < i_drive && ev[i_drive].a == 3000);
		CHECK((n == 0) == (find_exp(0, EXP_TX_RAIL, true) < 0) || find_exp(0, EXP_TX_RAIL, true) < i_floor);
		CHECK(dac_now == 3000 && (shadow & EXP_TX_BITS) == EXP_TX_BITS && rf.seq.state == CHAINSEQ_ST_BURST_ON);
		chainseq_burst_off(&rf.seq);
		chainseq_burst_off(&rf.seq);	// the timed drop, then the glue's switch back
		CHECK(dac_now == 0 && (shadow & (EXP_TX_BITS | EXP_VCO_LOW)) == 0u && rf.seq.state == CHAINSEQ_ST_BURST);
		CHECK(find_ev(0, EV_SYNTH) < 0 && find_ev(0, EV_DELAY) < 0);
	}
	// With a boost: it comes after the loop point, and the PA's point carries it
	clear_log();
	CHECK(burst(3000, 1000) && dac_now == 1000 && inv_fail == 0 && trips == 0);
	chainseq_burst_off(&rf.seq);

	// Not locked at the PA's point: no drive, the burst is off, counted; the next burst keys
	uint32_t unlocked = rf.seq.unlocked;
	pll_locked = false;
	clear_log();
	CHECK(!burst(0, d) && find_dac_over(0, fl) < 0);
	CHECK(dac_now == 0 && (shadow & EXP_TX_BITS) == 0u && rf.seq.unlocked == unlocked + 1u);
	pll_locked = true;
	CHECK(burst(0, d) && dac_now == 3000);
	chainseq_burst_off(&rf.seq);

	// The way back at the transmission's end: the receive LO, the receive rail, the tuning value, no waits
	clear_log();
	CHECK(chainseq_burst_rx(&rf.seq) && rf.seq.state == CHAINSEQ_ST_RX);
	CHECK(is_safe() && (shadow & EXP_RX_RAIL) != 0u && dac_now == 1500 && find_ev(0, EV_DELAY) < 0);
	CHECK(ev[find_ev(0, EV_SYNTH)].b == 446u * MHZ - CS7000P_IF_HZ);
	// keep: the end leaves a chain that already receives alone
	clear_log();
	cs7000p_rf_tx_off(&rf, true);
	CHECK(ev_n == 0 && !rf.admit && rf.seq.state == CHAINSEQ_ST_RX);
	CHECK(inv_fail == 0 && trips == 0);
	// Without keep the same end runs the whole receive set-up: the way back above had none for the IF chip
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && chainseq_burst_tune(&rf.seq, 441u * MHZ) && burst(0, d));
	chainseq_burst_off(&rf.seq);
	CHECK(chainseq_burst_rx(&rf.seq));
	clear_log();
	cs7000p_rf_tx_off(&rf, false);
	CHECK(find_ev(0, EV_IF) >= 0 && find_ev(0, EV_SYNTH) >= 0 && rf.seq.state == CHAINSEQ_ST_RX && dac_now == 1500);
	// And with no transmission behind it, an end while receiving does nothing either way
	clear_log();
	cs7000p_rf_tx_off(&rf, false);
	cs7000p_rf_tx_off(&rf, true);
	CHECK(ev_n == 0 && inv_fail == 0 && trips == 0);

	// A drive ahead of the loop point is refused, and the transmission's RF ends there
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && chainseq_burst_tune(&rf.seq, 441u * MHZ));
	CHECK(!chainseq_burst_point(&rf.seq, PA_KEYUP_PT_DRIVE, 2000));
	CHECK(is_safe() && dac_now == 0 && inv_fail == 0);
	// A full end from the burst state: the receiver set up again
	CHECK(chainseq_burst_tune(&rf.seq, 441u * MHZ) && burst(0, d));
	chainseq_burst_off(&rf.seq);
	cs7000p_rf_tx_off(&rf, false);
	CHECK(rf.seq.state == CHAINSEQ_ST_RX && is_safe() && dac_now == 1500 && inv_fail == 0);

	// A safe request in a burst: off, and neither that burst nor the next keys until the task asks again
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && chainseq_burst_tune(&rf.seq, 441u * MHZ) && burst(0, d));
	board_rf_safe_fake();
	check_safed();
	CHECK(!burst(0, d) && is_safe() && dac_now == 0);
	clear_log();
	if (chainseq_burst_tune(&rf.seq, 441u * MHZ)) {
		CHECK(!burst(0, d));
	}
	CHECK(find_exp(0, EXP_APC_LOOP, true) < 0 && find_dac_over(0, 0) < 0 && inv_fail == 0);

	// On the bench drive (a build that allows it in DMR): the top drive is the bench drive's own
	bring_up(&cs7000p_cal_model, 446u * MHZ);
	q.limit_list = true;
	q.bench_dmr = true;
	CHECK(cs7000p_rf_drive(&rf, 500, T_SETTLED) && cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
	cs7000p_rf_burst_drives(&rf, &d, &top, &fl);
	CHECK(d == 500 && top == 500 && fl == 0);
	drive_cap = 500;
	CHECK(chainseq_burst_tune(&rf.seq, 441u * MHZ) && burst(0, d) && dac_now == 500);
	chainseq_burst_off(&rf.seq);
	CHECK(chainseq_burst_rx(&rf.seq) && inv_fail == 0 && trips == 0);
}
#endif

// --- The VCO line by frequency ---

#define F_OWNER	433500000u

// A receive set-up from the start, the same channel again at the other width, and a key-up on the bench drive with
// its end; with DMR also a bench burst transmission and its way back
static void owner_run(int part)
{
	cs7000p_tx_req_t q = req(F_OWNER, 0, false);
	q.limit_list = true;
	if (part == 0) {
		bring_up(&cs7000p_cal_model, 0);
		CHECK(cs7000p_rf_rx(&rf, F_OWNER, true));
		CHECK(cs7000p_rf_rx(&rf, F_OWNER, false));
		drive_cap = 600;
		CHECK(cs7000p_rf_drive(&rf, 600, T_SETTLED) && cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
		CHECK(cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) == CS7000P_TX_OK);
		cs7000p_rf_tx_off(&rf, false);
		return;
	}
#if CONFIG_DMR
	bring_up(&cs7000p_cal_model, F_OWNER);
	q.dmr = true;
	q.bench_dmr = true;
	drive_cap = 500;
	CHECK(cs7000p_rf_drive(&rf, 500, T_SETTLED) && cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
	CHECK(chainseq_burst_tune(&rf.seq, F_OWNER) && burst(0, 500));
	chainseq_burst_off(&rf.seq);
	CHECK(chainseq_burst_rx(&rf.seq));
	cs7000p_rf_tx_off(&rf, false);
#endif
}

// What owner_run wrote before the VCO line came from the frequency plan (taken from the code then, by direction):
// the same writes in the same order, each level, value and wait, now that it does. 433.5 MHz receives with the
// line set and transmits with it clear under both rules
typedef struct { uint8_t type; uint32_t a, b; } owner_ev_t;
#define GS(bit)	{ EV_EXP, (bit), 0 }
#define GC(bit)	{ EV_EXP, 0, (bit) }
#define GD(v)	{ EV_DAC, (v), 0 }
#define GW(us)	{ EV_DELAY, (us), 0 }
#define GP(hz)	{ EV_SYNTH, 2, (hz) }
#define GI(w)	{ EV_IF, (w), 0 }
// The receive set-up after its safe writes (the line set), from the words on; and the IF chip's width
#define G_RX_LO	GP(383550000u), GW(100), GW(100), GS(EXP_RX_RAIL), GS(EXP_AK_RUN), GW(1000)
#define G_SAFE	GD(0), GC(EXP_APC_LOOP), GC(EXP_TX_RAIL)

static const owner_ev_t owner_fm[] = {
	// The first receive set-up: the synthesizer rail's settle time
	G_SAFE, GS(EXP_VCO_LOW), GS(EXP_SYNTH_RAIL), GW(1000), G_RX_LO, GI(1), GD(2048), GS(EXP_AUDIO_RAIL),
	// Again, 12.5 kHz
	G_SAFE, GS(EXP_VCO_LOW), GS(EXP_SYNTH_RAIL), G_RX_LO, GI(0), GD(2048), GS(EXP_AUDIO_RAIL),
	// The key-up on the bench drive: tx_begin, then the line clear before the words, the lock, the rail, the loop
	GD(0), GC(EXP_RX_RAIL),
	G_SAFE, GC(EXP_RX_RAIL), GS(EXP_SYNTH_RAIL), GC(EXP_VCO_LOW), GP(433500000u), GW(100), GW(100),
	GS(EXP_TX_RAIL), GW(500), GS(EXP_APC_LOOP),
	GD(26), GW(250), GD(94), GW(250), GD(190), GW(250), GD(300), GW(250), GD(410), GW(250), GD(506), GW(250),
	GD(574), GW(250), GD(600),
	// The end: the ramp down, the PA off, the whole receive set-up
	GD(574), GW(250), GD(506), GW(250), GD(410), GW(250), GD(300), GW(250), GD(190), GW(250), GD(94), GW(250),
	GD(26), GW(250), GD(0),
	G_SAFE, G_SAFE, GS(EXP_VCO_LOW), GS(EXP_SYNTH_RAIL), G_RX_LO, GI(0), GD(2048),
};
#if CONFIG_DMR
static const owner_ev_t owner_dmr[] = {
	// The burst tune: the line clear, the words, the transmit rail; the burst; its end
	G_SAFE, GC(EXP_RX_RAIL), GS(EXP_SYNTH_RAIL), GC(EXP_VCO_LOW), GP(433500000u), GS(EXP_TX_RAIL),
	GS(EXP_TX_RAIL), GS(EXP_APC_LOOP), GD(500), G_SAFE,
	// The way back: the line set, the receive LO, the receive rail, the tuning value; then the whole set-up
	G_SAFE, GS(EXP_VCO_LOW), GP(383550000u), GS(EXP_RX_RAIL), GD(2048),
	G_SAFE, GS(EXP_VCO_LOW), GS(EXP_SYNTH_RAIL), G_RX_LO, GI(1), GD(2048),
};
#endif

static void owner_match(const owner_ev_t *want, int n, int line)
{
	bool ok = ev_n == n;
	for (int i = 0; ok && i < n; i++) {
		ok = ev[i].type == want[i].type && ev[i].a == want[i].a && ev[i].b == want[i].b;
	}
	if (!ok) {
		printf("FAIL %s:%d: the 433.5 MHz log differs (%d events, %d expected)\n", __FILE__, line, ev_n, n);
		for (int i = 0; i < ev_n || i < n; i++) {
			if (i < ev_n && (i >= n || ev[i].type != want[i].type || ev[i].a != want[i].a || ev[i].b != want[i].b)) {
				printf("  %3d got %u 0x%lx 0x%lx\n", i, ev[i].type, (unsigned long)ev[i].a, (unsigned long)ev[i].b);
				break;
			}
		}
		failures++;
	}
}

// The line where a receive set-up leaves it, and where a bench key-up has it with the transmit rail on. Each starts
// a new log
static bool rx_line(uint32_t hz)
{
	clear_log();
	CHECK(cs7000p_rf_rx(&rf, hz, true) && rf.seq.state == CHAINSEQ_ST_RX);
	return (shadow & EXP_VCO_LOW) != 0u;
}

static bool fm_line(uint32_t hz)
{
	cs7000p_tx_req_t q = req(hz, 0, false);
	q.limit_list = true;
	drive_cap = 600;
	clear_log();
	CHECK(cs7000p_rf_drive(&rf, 600, T_SETTLED) && cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
	CHECK(cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) == CS7000P_TX_OK);
	CHECK(rf.seq.state == CHAINSEQ_ST_TX && (shadow & EXP_TX_BITS) == EXP_TX_BITS);
	bool on = (shadow & EXP_VCO_LOW) != 0u;
	cs7000p_rf_tx_off(&rf, false);
	CHECK(rf.seq.state == CHAINSEQ_ST_RX);
	return on;
}

static void test_vco_rule(void)
{
	// The owner's channel, write for write as before
	owner_run(0);
	owner_match(owner_fm, (int)(sizeof(owner_fm) / sizeof(owner_fm[0])), __LINE__);
	CHECK(inv_fail == 0 && trips == 0);
#if CONFIG_DMR
	owner_run(1);
	owner_match(owner_dmr, (int)(sizeof(owner_dmr) / sizeof(owner_dmr[0])), __LINE__);
	CHECK(inv_fail == 0 && trips == 0);
#endif

	// Receive: set up to 479 MHz, clear above. Over the switch the safe writes park the line and the set-up clears
	// it before the synthesizer rail and the words
	bring_up(&cs7000p_cal_model, 0);
	CHECK(rx_line(F_OWNER) && rx_line(479u * MHZ) && !rx_line(480u * MHZ) && !rx_line(527u * MHZ));
	clear_log();
	CHECK(!rx_line(480u * MHZ));
	int i_park = find_exp(0, EXP_VCO_LOW, true), i_clr = find_exp(0, EXP_VCO_LOW, false);
	CHECK(i_park > find_exp(0, EXP_TX_RAIL, false) && i_park < i_clr && i_clr < find_exp(0, EXP_SYNTH_RAIL, true));
	CHECK(i_clr < find_ev(0, EV_SYNTH) && ev[find_ev(0, EV_SYNTH)].b == 480u * MHZ - CS7000P_IF_HZ);
	CHECK(find_exp(i_clr + 1, EXP_VCO_LOW, true) < 0 && find_exp(i_clr + 1, EXP_VCO_LOW, false) < 0);
	// Transmit: set up to 430 MHz, clear above, whatever the receiver had. Each key-up here starts and ends on the
	// receiver over 479 MHz (the line clear): the line moves on the way up and back, with the transmit rail off
	CHECK(fm_line(420u * MHZ) && fm_line(430u * MHZ) && !fm_line(430000010u) && !fm_line(F_OWNER));
	CHECK(!fm_line(446500000u) && !fm_line(512u * MHZ));
	CHECK((shadow & EXP_VCO_LOW) == 0u && rf.seq.rx_hz == 480u * MHZ && inv_fail == 0 && trips == 0);

	// 3:6, receive by direction: set over 479 MHz too. Transmit follows its frequency all the same
	rf.vco_rx_dir = true;
	CHECK(rx_line(480u * MHZ) && rx_line(527u * MHZ));
	CHECK(fm_line(420u * MHZ) && !fm_line(446500000u));
	rf.vco_rx_dir = false;
	CHECK(!rx_line(480u * MHZ));
	// The LO above the signal (3:3): receive by direction, as the bench ran it before; transmit by its frequency
	rf.lo_high = true;
	CHECK(rx_line(F_OWNER) && rx_line(480u * MHZ));
	CHECK(fm_line(420u * MHZ) && !fm_line(F_OWNER));
	rf.lo_high = false;
	CHECK(inv_fail == 0 && trips == 0);

#if CONFIG_DMR
	// Bursts across the switches, at interrupt level: from a receiver at 480 MHz (clear) to bursts at 420 MHz (set),
	// and the way back clears it before the words
	bring_up(&cs7000p_cal_model, 480u * MHZ);
	CHECK((shadow & EXP_VCO_LOW) == 0u);
	cs7000p_tx_req_t q = req(420u * MHZ, 0, true);
	q.limit_list = true;
	q.bench_dmr = true;
	drive_cap = 500;
	CHECK(cs7000p_rf_drive(&rf, 500, T_SETTLED) && cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
	CHECK(chainseq_burst_tune(&rf.seq, 420u * MHZ) && (shadow & EXP_VCO_LOW) != 0u);
	CHECK(burst(0, 500) && (shadow & EXP_VCO_LOW) != 0u && dac_now == 500);
	chainseq_burst_off(&rf.seq);
	clear_log();
	CHECK(chainseq_burst_rx(&rf.seq) && (shadow & EXP_VCO_LOW) == 0u && find_ev(0, EV_DELAY) < 0);
	CHECK(find_exp(0, EXP_VCO_LOW, false) >= 0 && find_exp(0, EXP_VCO_LOW, false) < find_ev(0, EV_SYNTH));
	cs7000p_rf_tx_off(&rf, false);
	CHECK(rf.seq.state == CHAINSEQ_ST_RX && (shadow & EXP_VCO_LOW) == 0u && inv_fail == 0 && trips == 0);
#endif
}

// The maker's table in use, valid (invented: every group passed, a tune row of code 150 at 433.5 MHz): the chain
// receives on the row and transmits as without the table. The owner's 433.5 MHz run writes as before but for the
// receive tuning values, so a bench key-up's own writes are today's (the modem's held level, which carries trim A
// with a table, is not this test's: cs7000pRadioTest); no level or mode is admitted without the bench drive
static void test_maker_table(void)
{
	static cs7000p_maker_t m;
	static const uint32_t pts[8] = { 400500000u, 420000000u, 440000000u, 460000000u, 478950000u, 479050000u,
					 495000000u, 511500000u };
	memset(&m, 0, sizeof(m));
	m.state = CS7000P_MK_OK;
	m.valid = true;
	m.fields = CS7000P_MK_F_ALL;
	m.trim_a = 0x7B;
	m.trim_b = 0x7D;
	m.rssi_lo = 60;
	m.rssi_hi = 160;
	for (unsigned i = 0; i < CS7000P_MAKER_POINTS; i++) {
		m.rx_hz[i] = m.tx_hz[i] = pts[i];
		m.rx_p10[i] = m.tx_p10[i] = pts[i] / 10u;
		for (unsigned r = 0; r < CS7000P_MK_ROWS; r++) {
			m.row[r][i] = 150;
		}
	}
	m.row[CS7000P_MK_POWER_LO][0] = 100;	// high above low at every point
	uint16_t tune = 0;
	CHECK(cs7000p_maker_rx_tune(&m, F_OWNER, &tune, NULL) && tune == 16u * 150u && tune != CS7000P_RX_TUNE_NOMINAL);

	run_maker = &m;
	owner_ev_t want[sizeof(owner_fm) / sizeof(owner_fm[0])];
	const int n = (int)(sizeof(want) / sizeof(want[0]));
	int moved = 0;
	for (int i = 0; i < n; i++) {
		want[i] = owner_fm[i];
		if (want[i].type == EV_DAC && want[i].a == CS7000P_RX_TUNE_NOMINAL) {
			want[i].a = tune;	// the receive set-ups' tuning value; the key-up's ramp is the bench drive's
			moved++;
		}
	}
	CHECK(moved == 3);
	owner_run(0);
	owner_match(want, n, __LINE__);
	CHECK(rf.tune_src == CS7000P_SRC_MAKER && rf.rx_tune == tune && inv_fail == 0 && trips == 0);
#if CONFIG_DMR
	owner_ev_t want_dmr[sizeof(owner_dmr) / sizeof(owner_dmr[0])];
	const int n_dmr = (int)(sizeof(want_dmr) / sizeof(want_dmr[0]));
	for (int i = 0; i < n_dmr; i++) {
		want_dmr[i] = owner_dmr[i];
		if (want_dmr[i].type == EV_DAC && want_dmr[i].a == CS7000P_RX_TUNE_NOMINAL) {
			want_dmr[i].a = tune;
		}
	}
	owner_run(1);
	owner_match(want_dmr, n_dmr, __LINE__);
	CHECK(inv_fail == 0 && trips == 0);
#endif

	// Nothing transmits on it: every level and mode is refused as uncalibrated, even with a list naming the
	// frequency, and nothing is written; no modulation pair comes from it
	for (uint8_t level = 0; level < CS7000P_POWER_LEVELS; level++) {
		for (int dmr = 0; dmr < 2; dmr++) {
			bring_up(&cs7000p_cal_model, F_OWNER);
			cs7000p_tx_req_t q = req(F_OWNER, level, dmr != 0);
			q.limit_list = true;
			q.bench_dmr = true;
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNCAL && !rf.admit && ev_n == 0);
			uint8_t m2 = 9, m1 = 9;
			CHECK(!cs7000p_rf_mod_pair(&rf, F_OWNER, dmr != 0, &m2, &m1));
			CHECK(cs7000p_rf_drive_level(&rf, F_OWNER, level) == 0u);
		}
	}
	run_maker = NULL;
}

// --- The owner's transmit arm ---

static void put32be(uint8_t *t, unsigned off, uint32_t v)
{
	t[off] = (uint8_t)(v >> 24);
	t[off + 1] = (uint8_t)(v >> 16);
	t[off + 2] = (uint8_t)(v >> 8);
	t[off + 3] = (uint8_t)v;
}

static void fill16(uint8_t *t, unsigned off, uint16_t v)
{
	for (unsigned i = 0; i < 8; i++) {
		t[off + 2u * i] = (uint8_t)v;
		t[off + 2u * i + 1u] = (uint8_t)(v >> 8);
	}
}

// An invented table that passes every rule of the reader and holds every transmit value between its points
// (cs7000pCalTest's, calibration-cs7000p.md 3's layout; the values made up)
static void invented(uint8_t t[CS7000P_MAKER_SIZE])
{
	static const uint32_t tx[8] = { 400200000u, 429800000u, 430200000u, 445000000u, 458000000u, 472000000u,
					492000000u, 511800000u };
	static const uint32_t rx[8] = { 400500000u, 420000000u, 440000000u, 460000000u, 478950000u, 479050000u,
					495000000u, 511500000u };
	static const uint8_t high[8] = { 190, 194, 195, 210, 213, 215, 212, 209 };
	static const uint8_t low[8] = { 100, 99, 99, 108, 115, 122, 126, 127 };
	static const uint8_t tune[8] = { 120, 124, 128, 132, 136, 140, 150, 160 };
	static const uint8_t dmr2[8] = { 70, 72, 74, 76, 79, 82, 85, 88 };	// a slope: a split channel's pair differs
	static const uint8_t dmr1[8] = { 96, 97, 98, 100, 102, 104, 106, 108 };
	memset(t, 0, CS7000P_MAKER_SIZE);
	for (unsigned i = 0; i < 8; i++) {
		put32be(t, 0x000u + 4u * i, tx[i]);
		put32be(t, 0x020u + 4u * i, rx[i]);
	}
	t[0x040] = 60;
	t[0x041] = 160;
	memcpy(t + 0x044, tune, 8);
	memset(t + 0x04C, 40, 8);
	memset(t + 0x054, 50, 8);
	memset(t + 0x05C, 120, 8);
	memset(t + 0x064, 130, 8);
	memcpy(t + 0x06C, high, 8);
	memcpy(t + 0x074, low, 8);
	memset(t + 0x084, 40, 8);
	memset(t + 0x08C, 52, 8);
	memset(t + 0x094, 200, 8);
	memcpy(t + 0x0B4, dmr2, 8);
	memcpy(t + 0x0BC, dmr1, 8);
	memset(t + 0x0C4, 120, 8);
	memset(t + 0x0CC, 135, 8);
	t[0x0DC] = 0x7B;
	t[0x0DD] = 0xA0;
	memset(t + 0x0EC, 20, 8);
	memset(t + 0x0F4, 12, 8);
	memset(t + 0x0FC, 19, 8);
	fill16(t, 0x104, 300);
	fill16(t, 0x114, 310);
	memset(t + 0x124, 21, 8);
	memset(t + 0x12C, 12, 8);
	memset(t + 0x134, 20, 8);
	fill16(t, 0x13C, 301);
	fill16(t, 0x14C, 311);
	memset(t + 0x15C, 61, 8);
	memset(t + 0x164, 159, 8);
	t[0x174] = 0x7D;
}

static cs7000p_maker_t armed_table;

// The arm's verdict, written out (not cs7000p_arm_covers): FM at the lowest level from stage 1, FM at both from 2,
// DMR from 3. Covered and listed, the transmission goes ahead on the table
static bool covered(uint8_t stage, uint8_t level, bool dmr)
{
	static const uint8_t from[2][2] = { { 1, 2 }, { 3, 3 } };	// [dmr][level]
	return stage >= from[dmr ? 1 : 0][level];
}

static cs7000p_tx_why_t want_why(uint8_t stage, uint8_t level, bool dmr, bool listed)
{
	if (!covered(stage, level, dmr)) {
		return CS7000P_TX_UNARMED;
	}
	return listed ? CS7000P_TX_OK : CS7000P_TX_ARM_NO_LIST;
}

// The drive tx_drive_level gives for a level at hz on the table m at this stage: the table's FM drive while the stage
// covers FM at that level, else 0 (the model's rows are unset)
static uint16_t level_drive(const cs7000p_maker_t *m, uint8_t stage, uint32_t hz, uint8_t level)
{
	cs7000p_fm_tx_t v;
	return covered(stage, level, false) && cs7000p_tx_band(hz) && cs7000p_cal_maker_fm(m, hz, level, &v) ? v.drive : 0u;
}

// Nothing is live: no admission, no write, no transmit value of the transmission's getters, and the steps behind the
// check refuse. lvl: what tx_drive_level gives for the level (level_drive: the arm may cover FM at it although this
// transmission was refused)
static void nothing_live_at(uint32_t hz, uint8_t level, bool dmr, uint16_t lvl)
{
	uint16_t d = 9, top = 9, fl = 9;
	uint8_t m2 = 9, m1 = 9, n = 9;
	CHECK(!rf.admit && !rf.admit_arm && ev_n == 0 && !tx_bits_ever() && dac_now != 0u);	// the receive tune held
	CHECK(cs7000p_rf_drive_level(&rf, hz, level) == lvl && cs7000p_rf_drive_top(&rf) == 0u);
	CHECK(!cs7000p_rf_mod_pair(&rf, hz, dmr, &m2, &m1) && m2 == 9 && m1 == 9);
	CHECK(!cs7000p_rf_mod_pair(&rf, hz, false, &m2, &m1) && m2 == 9 && m1 == 9);
	CHECK(cs7000p_rf_fm_tx_rows(&rf, hz, false, &n) == NULL && n == 0u);
	CHECK(cs7000p_rf_ref_for(&rf, true) == rf.ref_offset);
	CHECK(cs7000p_rf_r04(&rf, true, true) == rf.ref_offset && cs7000p_rf_r04(&rf, false, true) == rf.ref_offset);
	cs7000p_rf_burst_drives(&rf, &d, &top, &fl);
	CHECK(d == 0u && top == 0u);
	CHECK(!cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) != CS7000P_TX_OK);
	CHECK(chainseq_tx(&rf.seq, hz, level) == CHAINSEQ_REFUSED_CAL);
#if CONFIG_DMR
	CHECK(!chainseq_burst_tune(&rf.seq, hz) && !chainseq_burst_point(&rf.seq, PA_KEYUP_PT_PA, 2000));
#endif
	CHECK(ev_n == 0 && !tx_bits_ever() && rf.seq.state == CHAINSEQ_ST_RX);
}

static void nothing_live(uint32_t hz, uint8_t level, bool dmr)
{
	nothing_live_at(hz, level, dmr, 0u);
}

// The maker's FM transmit registers the modem gets (calibration-cs7000p.md 5.13), narrow and wide
static bool maker_rows_are(const hrc6000_if_row_t *r, uint8_t n, bool wide)
{
	static const uint8_t want[2][5][2] = {
		{ { 0x3F, 0x04 }, { 0x35, 0x20 }, { 0x34, 0xBC }, { 0x3E, 0x06 }, { 0xE4, 0x20 } },
		{ { 0x3F, 0x07 }, { 0x35, 0x20 }, { 0x34, 0xBC }, { 0x3E, 0x06 }, { 0xE4, 0x22 } },
	};
	if (r == NULL || n != 5u) {
		return false;
	}
	for (unsigned i = 0; i < 5u; i++) {
		if (r[i].page != 4u || r[i].reg != want[wide][i][0] || r[i].val != want[wide][i][1]) {
			return false;
		}
	}
	return true;
}

// An FM key-up admitted on the table m at hz for the level: the check wrote nothing, the drive is the table's for
// that level at hz (want, when not 0: pinned), and the modem's callbacks give the table's FM pair, trim A and the
// maker's registers for that frequency only; then it ends, nothing written, and the transmission's getters are
// gone with it
static void admitted_fm(const cs7000p_maker_t *m, uint32_t hz, uint8_t level, uint16_t want)
{
	uint16_t drive = 0;
	uint8_t m2 = 9, m1 = 9, n = 9;
	int16_t d = 99;
	CHECK(cs7000p_maker_drive(m, hz, level, &drive, NULL) && (want == 0u || drive == want));
	CHECK(rf.admit && rf.admit_arm && !rf.admit_ovr && !rf.admit_dmr && rf.admit_hz == hz && rf.admit_power == level);
	CHECK(rf.admit_drive == drive && ev_n == 0 && !tx_bits_ever());
	CHECK(cs7000p_rf_drive_level(&rf, hz, level) == drive);
	CHECK(cs7000p_rf_mod_pair(&rf, hz, false, &m2, &m1) && m2 == m->row[CS7000P_MK_FM_MOD2][0] &&
	      m1 == m->row[CS7000P_MK_FM_MOD1][0]);
	CHECK(cs7000p_maker_trim(m, false, &d) && cs7000p_rf_ref_for(&rf, true) == d);
	const hrc6000_if_row_t *r = cs7000p_rf_fm_tx_rows(&rf, hz, false, &n);
	CHECK(maker_rows_are(r, n, false));
	r = cs7000p_rf_fm_tx_rows(&rf, hz, true, &n);
	CHECK(maker_rows_are(r, n, true));
	// Another frequency than the admitted one, or DMR's pair: none of it
	m2 = m1 = 9;
	CHECK(!cs7000p_rf_mod_pair(&rf, hz + 12500u, false, &m2, &m1) && m2 == 9 && m1 == 9);
	CHECK(!cs7000p_rf_mod_pair(&rf, hz, true, &m2, &m1) && m2 == 9 && m1 == 9);
	CHECK(cs7000p_rf_fm_tx_rows(&rf, hz + 12500u, false, &n) == NULL && n == 0u);
	cs7000p_rf_tx_off(&rf, false);
	CHECK(!rf.admit && !rf.admit_arm && ev_n == 0 && !tx_bits_ever());
	CHECK(!cs7000p_rf_mod_pair(&rf, hz, false, &m2, &m1) && cs7000p_rf_fm_tx_rows(&rf, hz, false, &n) == NULL);
	CHECK(cs7000p_rf_ref_for(&rf, true) == rf.ref_offset);
}

// A DMR key-up admitted on the table m at hz for the level: the check wrote nothing, the drive is the table's for that
// level at hz, the burst plan's top drive the top level's there, and the modem gets the table's DMR pair at hz only
// (not at another frequency, not FM's) and trim B for the key-up's 4:0x04; FM's set-up values none of it. Then (with
// DMR built) two bursts on it under the chain's rules, the line for f_tx, the drive never over the level's; the way
// back, and the end: the transmission's getters gone with it
static void admitted_dmr(const cs7000p_maker_t *m, uint32_t hz, uint8_t level)
{
	uint16_t drive = 0, hi = 0, d = 9, top = 9, fl = 9;
	uint8_t m2 = 9, m1 = 9, p2 = 0, p1 = 0, n = 9;
	int16_t tb = 99;
	cs7000p_dmr_tx_t v;
	CHECK(cs7000p_maker_drive(m, hz, level, &drive, NULL));
	// The top level's drive when the table serves DMR there (its burst fields too), else the level's own
	hi = cs7000p_cal_maker_dmr(m, hz, 1, &v) ? v.drive : drive;
	CHECK(cs7000p_maker_pair(m, hz, true, &p2, &p1, NULL) && cs7000p_maker_trim(m, true, &tb));
	CHECK(rf.admit && rf.admit_arm && !rf.admit_ovr && rf.admit_dmr && rf.admit_hz == hz && rf.admit_power == level);
	CHECK(rf.admit_drive == drive && ev_n == 0 && !tx_bits_ever());
	cs7000p_rf_burst_drives(&rf, &d, &top, &fl);
	CHECK(d == drive && top == hi && top >= d && fl == rf.cal->floor);
	CHECK(cs7000p_rf_mod_pair(&rf, hz, true, &m2, &m1) && m2 == p2 && m1 == p1);
	CHECK(cs7000p_rf_r04(&rf, true, true) == tb && rf.r04 == tb);
	CHECK(cs7000p_rf_r04(&rf, false, true) == rf.ref_offset);	// never in FM
	m2 = m1 = 9;
	CHECK(!cs7000p_rf_mod_pair(&rf, hz + 12500u, true, &m2, &m1) && m2 == 9 && m1 == 9);
	CHECK(!cs7000p_rf_mod_pair(&rf, hz, false, &m2, &m1) && m2 == 9 && m1 == 9);
	CHECK(cs7000p_rf_fm_tx_rows(&rf, hz, false, &n) == NULL && n == 0u);
	CHECK(cs7000p_rf_ref_for(&rf, true) == rf.ref_offset);
#if CONFIG_DMR
	drive_cap = drive;
	CHECK(chainseq_burst_tune(&rf.seq, hz));
	for (int b = 0; b < 2; b++) {
		CHECK(burst(0, d) && dac_now == drive && (shadow & EXP_TX_BITS) == EXP_TX_BITS);
		CHECK(((shadow & EXP_VCO_LOW) != 0u) == cs7000p_vco_low(hz, true, false));
		chainseq_burst_off(&rf.seq);
	}
	CHECK(find_dac_over(0, drive) < 0 && chainseq_burst_rx(&rf.seq));
	cs7000p_rf_tx_off(&rf, true);
	CHECK(rf.seq.state == CHAINSEQ_ST_RX && is_safe() && inv_fail == 0 && trips == 0);
	drive_cap = CS7000P_DAC_MAX;
#else
	cs7000p_rf_tx_off(&rf, false);
#endif
	clear_log();
	CHECK(!rf.admit && !rf.admit_arm);
	m2 = m1 = 9;
	CHECK(!cs7000p_rf_mod_pair(&rf, hz, true, &m2, &m1) && m2 == 9 && m1 == 9);
	cs7000p_rf_burst_drives(&rf, &d, &top, &fl);
	CHECK(d == 0u && top == 0u && cs7000p_rf_r04(&rf, true, true) == rf.ref_offset);
}

// The rule and its numbers; a start and a safe request clear the arm
static void test_arm_rule(void)
{
	CHECK(CS7000P_TX_UNARMED == 13 && CS7000P_TX_ARM_NO_LIST == 15);	// the hardware page's: append only
	CHECK(CS7000P_TX_ARM_REQUIRED == 1);
	for (uint8_t stage = 0; stage <= CS7000P_ARM_ALL; stage++) {
		for (uint8_t level = 0; level < CS7000P_POWER_LEVELS; level++) {
			for (int dmr = 0; dmr < 2; dmr++) {
				const bool c = want_why(stage, level, dmr != 0, true) != CS7000P_TX_UNARMED;
				CHECK(cs7000p_arm_covers(stage, level, dmr != 0) == c);
				CHECK(cs7000p_arm_why(stage, level, dmr != 0, false) ==
				      (c ? CS7000P_TX_ARM_NO_LIST : CS7000P_TX_UNARMED));
				CHECK(cs7000p_arm_why(stage, level, dmr != 0, true) == (c ? CS7000P_TX_OK : CS7000P_TX_UNARMED));
			}
		}
	}
	// arm_set: the stage and its id; off forgets the id
	bring_up(&cs7000p_cal_model, 446u * MHZ);
	cs7000p_rf_arm_set(&rf, CS7000P_ARM_FM, 0xCAFEF00Du);
	CHECK(rf.arm == CS7000P_ARM_FM && rf.arm_id == 0xCAFEF00Du && ev_n == 0);
	cs7000p_rf_arm_set(&rf, CS7000P_ARM_OFF, 0xCAFEF00Du);
	CHECK(rf.arm == CS7000P_ARM_OFF && rf.arm_id == 0u);
	// A safe request (a fault path, the power's off state) disarms; so does a new chain
	cs7000p_rf_arm_set(&rf, CS7000P_ARM_ALL, 0x1234u);
	board_rf_safe_fake();
	CHECK(rf.arm == CS7000P_ARM_OFF && rf.arm_id == 0u && safe_wrote == 0);
	cs7000p_rf_arm_set(&rf, CS7000P_ARM_ALL, 0x1234u);
	cs7000p_rf_init(&rf, &io, &cs7000p_cal_model);
	CHECK(rf.arm == CS7000P_ARM_OFF && rf.arm_id == 0u);
}

// The table holds every value at these frequencies (both VCOs, a point, inside the 0.4 MHz switch segment, past the
// last point): every level and mode is refused with the arm's reason at every stage where the arm or the list is
// missing, and nothing is admitted, written or given by the transmission's getters; covered and listed, the key-up is
// admitted on the table's drive at that frequency for its level, FM's or DMR's
static void test_arm_admit(void)
{
	static uint8_t t[CS7000P_MAKER_SIZE];
	static const uint32_t freqs[] = { 420000000u, 430000000u, 433500000u, 445000000u, 446u * MHZ, 511900000u };
	invented(t);
	CHECK(cs7000p_maker_parse(t, 0x600DCAFEu, 0, &armed_table) && armed_table.fields == CS7000P_MK_F_ALL);
	run_maker = &armed_table;
	for (unsigned f = 0; f < sizeof(freqs) / sizeof(freqs[0]); f++) {
		const uint32_t hz = freqs[f];
		for (uint8_t stage = 0; stage <= CS7000P_ARM_ALL; stage++) {
			run_arm = stage;
			bring_up(&cs7000p_cal_model, 446u * MHZ);
			uint32_t n = 0;
			for (uint8_t level = 0; level < CS7000P_POWER_LEVELS; level++) {
				for (int dmr = 0; dmr < 2; dmr++) {
					CHECK(cs7000p_cal_maker_tx(&armed_table, hz, level, dmr != 0));
					for (int listed = 0; listed < 2; listed++) {
						cs7000p_tx_req_t q = req(hz, level, dmr != 0);
						q.limit_list = listed != 0;
						q.bench_dmr = true;
						const cs7000p_tx_why_t w = want_why(stage, level, dmr != 0, listed != 0);
						const uint32_t refused = rf.tx_refused;
						CHECK(cs7000p_rf_tx_admit(&rf, &q) == w);
						if (w == CS7000P_TX_OK) {
							CHECK(rf.tx_refused == refused);
							if (dmr) {
								admitted_dmr(&armed_table, hz, level);
							} else {
								admitted_fm(&armed_table, hz, level, 0);
							}
						} else {
							CHECK(rf.tx_refused_why == w);
							nothing_live_at(hz, level, dmr != 0,
									level_drive(&armed_table, stage, hz, level));
							n++;
						}
					}
					// The bench's reference coding first, whatever the arm; the off state before it
					cs7000p_tx_req_t q = req(hz, level, dmr != 0);
					q.limit_list = true;
					rf.ref_coding = CS7000P_REF_MAKER;
					CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_REF_CODING);
					rf.ref_coding = CS7000P_REF_MAKER_ADC;
					CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_REF_CODING);
					q.off = true;
					CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OFF);
					rf.ref_coding = CS7000P_REF_HOLD;
					nothing_live_at(hz, level, dmr != 0, level_drive(&armed_table, stage, hz, level));
					n += 3;
				}
			}
			CHECK(rf.tx_refused == n && rf.arm == stage);
		}
	}
	// Outside the transmit ranges the band's reason, armed or not
	run_arm = CS7000P_ARM_ALL;
	bring_up(&cs7000p_cal_model, 446u * MHZ);
	cs7000p_tx_req_t q = req(406050000u, 0, false);
	q.limit_list = true;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
	q.hz = 520u * MHZ;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
	// A list cleared under an arm that covers the transmission: 15 again, and the admission before it gone
	q = req(446u * MHZ, 1, false);
	q.limit_list = true;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit_arm);
	q.limit_list = false;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_ARM_NO_LIST);
	nothing_live_at(446u * MHZ, 1, false, level_drive(&armed_table, CS7000P_ARM_ALL, 446u * MHZ, 1));
	// A stage lowered under it: 13 again, and the high row is no level's drive any more
	cs7000p_rf_arm_set(&rf, CS7000P_ARM_FM_LOW, 0x5EEDu);
	q.limit_list = true;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNARMED);
	nothing_live_at(446u * MHZ, 1, false, 0);

	// Another correction of the reference than trim A: a bench PC8 idle, a stored trim, the bench's reference offset
	// (3:4) or the level it left in the modem refuses what the arm covers and a list names (16), FM and DMR, after
	// the arm's own reasons; the model's values and a bench carrier keep theirs
	CHECK(CS7000P_TX_ARM_REF == 16);	// the hardware page's: append only
	cs7000p_rf_arm_set(&rf, CS7000P_ARM_ALL, 0x5EEDu);
	const uint8_t src0 = rf.ref_src;
	for (int k = 0; k < 4; k++) {
		for (int dmr = 0; dmr < 2; dmr++) {
			q = req(446u * MHZ, 1, dmr != 0);
			q.limit_list = true;
			q.bench_dmr = true;
			q.pc8_bench = k == 0;
			rf.trim_ppb = k == 1 ? -450 : 0;
			rf.ref_override = k == 2;
			rf.ref_offset = k == 2 ? 12 : CS7000P_REF_OFFSET;
			rf.ref_src = k == 3 ? CS7000P_SRC_BENCH : src0;
			CHECK(cs7000p_rf_ref_bench(&rf) == (k >= 2));
			const uint32_t refused = rf.tx_refused;
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_ARM_REF && rf.tx_refused == refused + 1u);
			CHECK(rf.tx_refused_why == CS7000P_TX_ARM_REF);
			nothing_live_at(446u * MHZ, 1, dmr != 0, level_drive(&armed_table, CS7000P_ARM_ALL, 446u * MHZ, 1));
			q.limit_list = false;
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_ARM_NO_LIST);
		}
	}
	cs7000p_rf_arm_set(&rf, CS7000P_ARM_FM_LOW, 0x5EEDu);
	q = req(446u * MHZ, 1, false);
	q.limit_list = true;
	q.pc8_bench = true;
	rf.ref_override = true;
	rf.ref_offset = 12;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNARMED);
	// A bench carrier: its own rules, the trim, PC8 and the bench's 3:4 as they are (the offset in its FM set-up)
	CHECK(cs7000p_rf_drive(&rf, 600, T_SETTLED));
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit_ovr && !rf.admit_arm);
	CHECK(cs7000p_rf_ref_for(&rf, true) == 12 && cs7000p_rf_r04(&rf, false, true) == 12);
	cs7000p_rf_tx_off(&rf, false);
	rf.trim_ppb = 0;
	q.pc8_bench = false;
	rf.ref_override = false;
	rf.ref_offset = CS7000P_REF_OFFSET;
	rf.ref_src = src0;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNARMED);
	cs7000p_rf_arm_set(&rf, CS7000P_ARM_FM, 0x5EEDu);
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit_arm);
	cs7000p_rf_tx_off(&rf, false);

	// A table without what a mode needs answers 2 for that mode, armed or not, as a table without values; the other
	// mode keeps the arm's reasons. Each group alone, as a refusal would clear it
	static const struct { uint16_t group; bool fm, dmr0, dmr1; } need[] = {
		{ CS7000P_MK_F_POWER, false, false, false },
		{ CS7000P_MK_F_FM_PAIR, false, true, true },
		{ CS7000P_MK_F_DMR_PAIR, true, false, false },
		{ CS7000P_MK_F_TRIM_A, false, false, false },
		{ CS7000P_MK_F_TRIM_B, true, false, false },
		{ CS7000P_MK_F_BURST_LO, true, false, true },
		{ CS7000P_MK_F_BURST_HI, true, true, false },
		{ CS7000P_MK_F_TUNE | CS7000P_MK_F_SQUELCH | CS7000P_MK_F_RSSI | CS7000P_MK_F_CTCSS | CS7000P_MK_F_DCS |
		  CS7000P_MK_F_BATTERY, true, true, true },
	};
	static cs7000p_maker_t m;
	run_maker = &m;
	for (unsigned i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
		m = armed_table;
		m.fields = (uint16_t)(CS7000P_MK_F_ALL & ~need[i].group);
		for (uint8_t stage = 0; stage <= CS7000P_ARM_ALL; stage += CS7000P_ARM_ALL) {
			run_arm = stage;
			bring_up(&cs7000p_cal_model, 446u * MHZ);
			for (uint8_t level = 0; level < CS7000P_POWER_LEVELS; level++) {
				for (int dmr = 0; dmr < 2; dmr++) {
					const bool has = dmr ? (level ? need[i].dmr1 : need[i].dmr0) : need[i].fm;
					q = req(446u * MHZ, level, dmr != 0);
					q.limit_list = true;
					const cs7000p_tx_why_t w = has ? want_why(stage, level, dmr != 0, true) : CS7000P_TX_UNCAL;
					CHECK(cs7000p_rf_tx_admit(&rf, &q) == w);
					if (w == CS7000P_TX_OK && dmr) {
						admitted_dmr(&m, 446u * MHZ, level);
					} else if (w == CS7000P_TX_OK) {
						admitted_fm(&m, 446u * MHZ, level, 0);
					} else {
						nothing_live_at(446u * MHZ, level, dmr != 0, level_drive(&m, stage, 446u * MHZ, level));
					}
				}
			}
		}
	}
	// No table, or one not valid: 2 everywhere, at every stage
	static uint8_t blank[CS7000P_MAKER_SIZE];
	memset(blank, 0xFF, sizeof(blank));
	CHECK(!cs7000p_maker_parse(blank, 0, 0, &m));
	for (int k = 0; k < 2; k++) {
		run_maker = k == 0 ? NULL : &m;
		for (uint8_t stage = 0; stage <= CS7000P_ARM_ALL; stage++) {
			run_arm = stage;
			bring_up(&cs7000p_cal_model, 446u * MHZ);
			for (uint8_t level = 0; level < CS7000P_POWER_LEVELS; level++) {
				for (int dmr = 0; dmr < 2; dmr++) {
					q = req(446u * MHZ, level, dmr != 0);
					q.limit_list = true;
					CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNCAL);
					nothing_live(446u * MHZ, level, dmr != 0);
				}
			}
		}
	}
	run_maker = NULL;
	run_arm = CS7000P_ARM_OFF;
}

// The bench drive with a valid table: admitted unarmed under its own four conditions, with its own reasons (the arm's
// never replace them), at every stage the same; its key-up's writes are today's but for the receive tuning values
// (the modem's held level, which carries trim A, is cs7000pRadioTest's)
static void test_arm_bench(void)
{
	static uint8_t t[CS7000P_MAKER_SIZE];
	invented(t);
	CHECK(cs7000p_maker_parse(t, 0x600DCAFEu, 0, &armed_table));
	run_maker = &armed_table;
	for (uint8_t stage = 0; stage <= CS7000P_ARM_ALL; stage++) {
		run_arm = stage;
		bring_up(&cs7000p_cal_model, F_OWNER);
		CHECK(cs7000p_rf_drive(&rf, 600, T_SETTLED));
		cs7000p_tx_req_t q = req(F_OWNER, 1, false);
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OVR_NO_LIST);
		q.dmr = true;
		q.limit_list = true;
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OVR_DMR);
		q.debuglink = false;
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OVR_NO_LINK);
		q = req(F_OWNER, 1, false);
		q.limit_list = true;
		q.now_ms = T_SETTLED + CS7000P_OVERRIDE_MS + 1u;
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OVR_EXPIRED);
		CHECK(ev_n == 0 && !rf.admit);
		CHECK(cs7000p_rf_drive(&rf, 600, T_SETTLED));
		q.now_ms = T_SETTLED;
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit_ovr && !rf.admit_arm && rf.admit_drive == 600);
		uint8_t m2 = 9, m1 = 9;
		CHECK(cs7000p_rf_mod_pair(&rf, F_OWNER, false, &m2, &m1) && m2 == 0 && m1 == 0);	// no pair: a carrier
		cs7000p_rf_tx_off(&rf, false);
	}
	// The owner's run, write for write, at every stage: today's log with the table's receive tune in place
	uint16_t tune = 0;
	CHECK(cs7000p_maker_rx_tune(&armed_table, F_OWNER, &tune, NULL) && tune != CS7000P_RX_TUNE_NOMINAL);
	owner_ev_t want[sizeof(owner_fm) / sizeof(owner_fm[0])];
	const int n = (int)(sizeof(want) / sizeof(want[0]));
	for (int i = 0; i < n; i++) {
		want[i] = owner_fm[i];
		if (want[i].type == EV_DAC && want[i].a == CS7000P_RX_TUNE_NOMINAL) {
			want[i].a = tune;
		}
	}
	for (uint8_t stage = 0; stage <= CS7000P_ARM_ALL; stage++) {
		run_arm = stage;
		owner_run(0);
		owner_match(want, n, __LINE__);
		CHECK(inv_fail == 0 && trips == 0 && rf.arm == stage);
	}
	run_maker = NULL;
	run_arm = CS7000P_ARM_OFF;
}

// The invented table's TX points with others in their place (every other row as it is: still valid)
static void tx_points(uint8_t t[CS7000P_MAKER_SIZE], const uint32_t p[8])
{
	for (unsigned i = 0; i < 8; i++) {
		put32be(t, 0x000u + 4u * i, p[i]);
	}
}

// A key-up on the table at stage stage, FM at level: admitted, then keyed through the whole sequence. The ramp ends on
// the table's drive at f_tx for that level, the VCO line is the carrier's (rules() checks it at every write), and
// nothing over that drive reaches the loop (drive_cap)
static void keyup_on_table(uint8_t stage, uint32_t hz, uint8_t level, uint16_t want)
{
	run_arm = stage;
	bring_up(&cs7000p_cal_model, 446u * MHZ);
	drive_cap = want;
	cs7000p_tx_req_t q = req(hz, level, false);
	q.limit_list = true;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit_arm && rf.admit_drive == want);
	CHECK(cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) == CS7000P_TX_OK);
	CHECK(rf.seq.state == CHAINSEQ_ST_TX && dac_now == want && (shadow & EXP_TX_BITS) == EXP_TX_BITS);
	CHECK(((shadow & EXP_VCO_LOW) != 0u) == cs7000p_vco_low(hz, true, false));
	CHECK(find_dac_over(0, want) < 0 && inv_fail == 0 && trips == 0);
	int synth = find_ev(0, EV_SYNTH);
	CHECK(synth >= 0 && ev[synth].b == hz);
}

// The drive at f_tx, pinned on the invented table (calibration-cs7000p.md 4.3-4.4 by hand: 16 c of the row's value):
// each VCO, a point's segment, inside the 0.4 MHz switch segment (the same VCO's end value), past the last point
// (extended inside the band). The low stage keys the low row and no more: the top level's drive is 0 to the sweep,
// the bench's drive now goes no higher than the admitted drive, and the high row is refused (13); stage 2 gives the
// top level's
static void test_arm_drive(void)
{
	static const struct { uint32_t hz; uint16_t lo, hi; } pin[] = {
		{ 420000000u, 1600, 3088 },	// segment 0 (400.2-429.8), the low VCO: 100 and 193
		{ 430000000u, 1584, 3104 },	// the switch segment: 429.8's 99 and 194
		{ 433500000u, 1616, 3168 },	// segment 2 (430.2-445.0), the high VCO: 101 and 198
		{ 446000000u, 1744, 3360 },	// segment 3: 109 (rounded up) and 210
		{ 511900000u, 2032, 3344 },	// past 511.8: 127 and 209
	};
	static uint8_t t[CS7000P_MAKER_SIZE];
	invented(t);
	CHECK(cs7000p_maker_parse(t, 0x600DCAFEu, 0, &armed_table) && armed_table.fields == CS7000P_MK_F_ALL);
	run_maker = &armed_table;
	for (unsigned i = 0; i < sizeof(pin) / sizeof(pin[0]); i++) {
		const uint32_t hz = pin[i].hz;
		// Unarmed: nothing from any getter at either level
		run_arm = CS7000P_ARM_OFF;
		bring_up(&cs7000p_cal_model, 446u * MHZ);
		CHECK(cs7000p_rf_drive_level(&rf, hz, 0) == 0u && cs7000p_rf_drive_level(&rf, hz, 1) == 0u);
		// Stage 1: the low row only
		keyup_on_table(CS7000P_ARM_FM_LOW, hz, 0, pin[i].lo);
		CHECK(cs7000p_rf_drive_level(&rf, hz, 0) == pin[i].lo && cs7000p_rf_drive_level(&rf, hz, 1) == 0u);
		CHECK(cs7000p_rf_drive_top(&rf) == 0u);
		CHECK(cs7000p_rf_drive_now_ok(&rf, pin[i].lo) && cs7000p_rf_drive_now_ok(&rf, 0) &&
		      !cs7000p_rf_drive_now_ok(&rf, (uint16_t)(pin[i].lo + 1u)) && !cs7000p_rf_drive_now_ok(&rf, pin[i].hi));
		cs7000p_rf_tx_off(&rf, false);
		CHECK(rf.seq.state == CHAINSEQ_ST_RX && is_safe() && inv_fail == 0);
		cs7000p_tx_req_t q = req(hz, 1, false);
		q.limit_list = true;
		clear_log();
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNARMED);
		nothing_live_at(hz, 1, false, 0);
		// Stage 2: either row, and the top level's drive for the sweep while the low one keys
		keyup_on_table(CS7000P_ARM_FM, hz, 0, pin[i].lo);
		CHECK(cs7000p_rf_drive_top(&rf) == pin[i].hi);
		cs7000p_rf_tx_off(&rf, false);
		keyup_on_table(CS7000P_ARM_FM, hz, 1, pin[i].hi);
		CHECK(cs7000p_rf_drive_top(&rf) == pin[i].hi && cs7000p_rf_drive_level(&rf, hz, 0) == pin[i].lo);
		cs7000p_rf_tx_off(&rf, false);
		CHECK(rf.seq.state == CHAINSEQ_ST_RX && is_safe() && inv_fail == 0 && trips == 0);
	}
	// Not a bench drive: the drive now has no limit of the arm's there (cs7000pRadioTest keys one)
	run_maker = NULL;
	run_arm = CS7000P_ARM_OFF;
	bring_up(&cs7000p_cal_model, 446u * MHZ);
	CHECK(cs7000p_rf_drive_now_ok(&rf, CS7000P_DAC_MAX));
}

// A table whose transmit switch segment is 20 MHz wide: refused inside it (2, the values of two VCOs), used on either
// side. A table whose TX points all lie above the switch: refused below it, where the extension would cross it, and
// used above, past the last point too while inside the band. FM and DMR alike (the DMR pair on the same points).
// Armed at the top and listed: every refusal is the table's, never the arm's
static void test_arm_windows(void)
{
	static const uint32_t wide_seg[8] = { 400200000u, 420000000u, 440000000u, 445000000u, 458000000u, 472000000u,
					      492000000u, 511800000u };
	static const uint32_t above[8] = { 431000000u, 440000000u, 450000000u, 460000000u, 470000000u, 480000000u,
					   490000000u, 511000000u };
	static const struct { const uint32_t *pts; uint32_t hz; bool ok; } c[] = {
		{ wide_seg, 419000000u, true }, { wide_seg, 425000000u, false }, { wide_seg, 430000000u, false },
		{ wide_seg, 435000000u, false }, { wide_seg, 441000000u, true },
		{ above, 405000000u, false }, { above, 420000000u, false }, { above, 430000000u, false },
		{ above, 431500000u, true }, { above, 511900000u, true },
	};
	static uint8_t t[CS7000P_MAKER_SIZE];
	static cs7000p_maker_t m;
	for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		invented(t);
		tx_points(t, c[i].pts);
		CHECK(cs7000p_maker_parse(t, 0x600DCAFEu, 0, &m) && m.valid);
		run_maker = &m;
		run_arm = CS7000P_ARM_ALL;
		for (uint8_t level = 0; level < CS7000P_POWER_LEVELS; level++) {
			for (int dmr = 0; dmr < 2; dmr++) {
				bring_up(&cs7000p_cal_model, 446u * MHZ);
				cs7000p_tx_req_t q = req(c[i].hz, level, dmr != 0);
				q.limit_list = true;
				if (c[i].ok) {
					CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
					if (dmr) {
						admitted_dmr(&m, c[i].hz, level);
					} else {
						admitted_fm(&m, c[i].hz, level, 0);
					}
				} else {
					CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNCAL);
					nothing_live(c[i].hz, level, dmr != 0);
				}
			}
		}
	}
	// Out of the transmit ranges, either mode: the band's reason first
	for (int dmr = 0; dmr < 2; dmr++) {
		static const uint32_t out[] = { 399990000u, 406000000u, 512100000u };
		for (unsigned i = 0; i < sizeof(out) / sizeof(out[0]); i++) {
			bring_up(&cs7000p_cal_model, 446u * MHZ);
			cs7000p_tx_req_t q = req(out[i], 0, dmr != 0);
			q.limit_list = true;
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
			nothing_live(out[i], 0, dmr != 0);
		}
	}
	run_maker = NULL;
	run_arm = CS7000P_ARM_OFF;
}

// A tone channel (CTCSS or DCS) on the table: the arm's reasons first, as without a tone; covered and listed it is
// admitted where the table gives its tone's level at f_tx, on the same drive as without a tone, and its depth is that
// level's (pinned), twice the level on 25 kHz; only for the admitted frequency and while the arm covers it, never the
// build's depth. A tone the table holds no level for (a CTCSS tone past the two it was set at, a refused group) is
// refused as without values (2), nothing written. DMR is not a tone channel: its reasons are the arm's, and the top
// stage admits it. The bench drive keys a tone channel as before (its own conditions), at the build's depth on both
// bandwidths
static void test_arm_tone(void)
{
	static uint8_t t[CS7000P_MAKER_SIZE];
	static const subtone_t tones[] = { { TONE_CTCSS, 885 }, { TONE_DCS, 023 }, { TONE_DCS_INV, 0754 } };
	// On the invented table (CTCSS 40 at 67.0 Hz and 52 at 254.1 Hz, DCS 200): the levels narrow and wide, and their
	// depths, (4000 L + 525) / 1050
	static const uint8_t lvl[3][2] = { { 41, 82 }, { 200, 254 }, { 200, 254 } };
	static const uint16_t pm[3][2] = { { 156, 312 }, { 762, 968 }, { 762, 968 } };
	const uint32_t hz = 446u * MHZ;
	invented(t);
	CHECK(cs7000p_maker_parse(t, 0x600DCAFEu, 0, &armed_table));
	run_maker = &armed_table;
	for (uint8_t stage = 0; stage <= CS7000P_ARM_ALL; stage++) {
		run_arm = stage;
		bring_up(&cs7000p_cal_model, hz);
		for (uint8_t level = 0; level < CS7000P_POWER_LEVELS; level++) {
			for (int listed = 0; listed < 2; listed++) {
				for (unsigned k = 0; k < sizeof(tones) / sizeof(tones[0]); k++) {
					cs7000p_tx_req_t q = req(hz, level, false);
					q.limit_list = listed != 0;
					q.tone = true;
					q.tone_sel = tones[k];
					const cs7000p_tx_why_t w = want_why(stage, level, false, listed != 0);
					CHECK(cs7000p_rf_tx_admit(&rf, &q) == w);
					if (w == CS7000P_TX_OK) {
						uint16_t d = 9;
						uint8_t l = 9;
						for (int wide = 0; wide < 2; wide++) {
							CHECK(cs7000p_cal_maker_tone(&armed_table, hz, &tones[k], wide != 0, &l));
							CHECK(l == lvl[k][wide]);
							CHECK(cs7000p_rf_tone_depth(&rf, hz, wide != 0, &tones[k], &d));
							CHECK(d == pm[k][wide] && d == cs7000p_subpwm_depth_pm(l));
						}
						// Another frequency than the admitted one, or the arm gone: no depth at all
						d = 9;
						CHECK(!cs7000p_rf_tone_depth(&rf, hz + 12500u, false, &tones[k], &d) && d == 9);
						rf.arm = CS7000P_ARM_OFF;
						CHECK(!cs7000p_rf_tone_depth(&rf, hz, false, &tones[k], &d) && d == 9);
						rf.arm = stage;
						admitted_fm(&armed_table, hz, level, 0);	// as without a tone; ends it
						CHECK(!cs7000p_rf_tone_depth(&rf, hz, false, &tones[k], &d) && d == 9);
					} else {
						nothing_live_at(hz, level, false, level_drive(&armed_table, stage, hz, level));
					}
					// DMR carries no tone: the arm's reasons alone, and the top stage admits it
					q.dmr = true;
					if (cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK) {
						CHECK(want_why(stage, level, true, listed != 0) == CS7000P_TX_OK);
						admitted_dmr(&armed_table, hz, level);
					} else {
						CHECK(rf.tx_refused_why == want_why(stage, level, true, listed != 0));
						CHECK(!rf.admit && !rf.admit_arm);
					}
				}
			}
		}
		CHECK(cs7000p_rf_drive(&rf, 600, T_SETTLED));
		cs7000p_tx_req_t q = req(hz, 0, false);
		q.limit_list = true;
		q.tone = true;
		q.tone_sel = tones[0];
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit_ovr && !rf.admit_arm);
		for (int wide = 0; wide < 2; wide++) {
			uint16_t d = 9;
			CHECK(cs7000p_rf_tone_depth(&rf, hz, wide != 0, &tones[0], &d) && d == CS7000P_CTCSS_DEPTH_PM);
		}
		cs7000p_rf_tx_off(&rf, false);
	}
	// Armed for everything and listed: a tone the table holds no level for is 2, nothing written, whatever else the
	// table holds; the other kind of tone still goes ahead
	run_arm = CS7000P_ARM_ALL;
	static const subtone_t past[] = { { TONE_CTCSS, 669 }, { TONE_CTCSS, 2542 }, { TONE_CTCSS, 0 }, { 9, 885 } };
	bring_up(&cs7000p_cal_model, hz);
	for (unsigned k = 0; k < sizeof(past) / sizeof(past[0]); k++) {
		cs7000p_tx_req_t q = req(hz, 0, false);
		q.limit_list = true;
		q.tone = true;
		q.tone_sel = past[k];
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNCAL && rf.tx_refused_why == CS7000P_TX_UNCAL);
		nothing_live_at(hz, 0, false, level_drive(&armed_table, CS7000P_ARM_ALL, hz, 0));
	}
	static cs7000p_maker_t part;
	static const uint16_t groups[2] = { CS7000P_MK_F_CTCSS, CS7000P_MK_F_DCS };
	for (unsigned g = 0; g < 2; g++) {
		part = armed_table;
		part.fields = (uint16_t)(part.fields & ~groups[g]);
		run_maker = &part;
		bring_up(&cs7000p_cal_model, hz);
		for (unsigned k = 0; k < sizeof(tones) / sizeof(tones[0]); k++) {
			const bool has = (k == 0u) != (g == 0u);	// CTCSS without its group, DCS without its own: none
			cs7000p_tx_req_t q = req(hz, 0, false);
			q.limit_list = true;
			q.tone = true;
			q.tone_sel = tones[k];
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == (has ? CS7000P_TX_OK : CS7000P_TX_UNCAL));
			if (has) {
				uint16_t d = 9;
				CHECK(cs7000p_rf_tone_depth(&rf, hz, false, &tones[k], &d) && d == pm[k][0]);
				admitted_fm(&part, hz, 0, 0);
			} else {
				nothing_live_at(hz, 0, false, level_drive(&part, CS7000P_ARM_ALL, hz, 0));
			}
		}
		// Without a tone the group asks nothing
		cs7000p_tx_req_t q = req(hz, 0, false);
		q.limit_list = true;
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
		admitted_fm(&part, hz, 0, 0);
	}
	run_maker = NULL;
	run_arm = CS7000P_ARM_OFF;
}

// The tone's level is the one at f_tx: split channels on a table whose 67.0 Hz and DCS rows move with the frequency,
// keyed inside the switch segment (P[1]'s value), between points and past the last one, receiving at 480 MHz (another
// level) and at 515 MHz (outside the transmit band: no level there at all); just past the band's top nothing is
// admitted
static void test_arm_tone_freq(void)
{
	static uint8_t t[CS7000P_MAKER_SIZE];
	static const uint8_t l67[8] = { 30, 34, 38, 42, 46, 50, 54, 58 };
	static const uint8_t dcs_row[8] = { 100, 110, 120, 130, 140, 150, 160, 170 };
	static const subtone_t tones[] = { { TONE_CTCSS, 885 }, { TONE_DCS, 023 } };
	static const uint32_t tx_hz[] = { 429900000u, 445000000u, 465000000u, 500000000u, 511900000u };
	static const uint32_t rx_list[2] = { 480u * MHZ, 515u * MHZ };
	static cs7000p_maker_t m;
	invented(t);
	memcpy(t + 0x084, l67, 8);
	memcpy(t + 0x094, dcs_row, 8);
	CHECK(cs7000p_maker_parse(t, 0x600DCAFEu, 0, &m) && m.fields == CS7000P_MK_F_ALL);
	run_maker = &m;
	run_arm = CS7000P_ARM_FM;
	const unsigned n_tx = sizeof(tx_hz) / sizeof(tx_hz[0]), n_tones = sizeof(tones) / sizeof(tones[0]);
	for (unsigned r = 0; r < 2u; r++) {
		const uint32_t rx_hz = rx_list[r];
		bring_up(&cs7000p_cal_model, rx_hz);
		for (unsigned n = 0; n < n_tx * n_tones; n++) {
			const uint32_t hz = tx_hz[n / n_tones];
			const subtone_t *tone = &tones[n % n_tones];
			uint8_t want = 0, at_rx = 0, wide = 0;
			CHECK(cs7000p_cal_maker_tone(&m, hz, tone, false, &want));
			CHECK(cs7000p_cal_maker_tone(&m, hz, tone, true, &wide));
			CHECK(r == 0u ? cs7000p_cal_maker_tone(&m, rx_hz, tone, false, &at_rx) && at_rx != want
				      : !cs7000p_cal_maker_tone(&m, rx_hz, tone, false, &at_rx));
			cs7000p_tx_req_t q = req(hz, 0, false);
			q.limit_list = true;
			q.tone = true;
			q.tone_sel = *tone;
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
			uint16_t d = 9;
			CHECK(cs7000p_rf_tone_depth(&rf, hz, false, tone, &d) && d == cs7000p_subpwm_depth_pm(want));
			CHECK(cs7000p_rf_tone_depth(&rf, hz, true, tone, &d) && d == cs7000p_subpwm_depth_pm(wide));
			d = 9;
			CHECK(!cs7000p_rf_tone_depth(&rf, rx_hz, false, tone, &d) && d == 9);
			admitted_fm(&m, hz, 0, 0);
		}
	}
	// Inside the switch segment: P[1]'s DCS level, 110, not the line's mix toward P[2]'s 120
	uint8_t l = 0;
	CHECK(cs7000p_cal_maker_tone(&m, tx_hz[0], &tones[1], false, &l) && l == 110u);
	cs7000p_tx_req_t q = req(512100000u, 0, false);
	q.limit_list = true;
	q.tone = true;
	q.tone_sel = tones[1];
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_BAND);
	nothing_live_at(512100000u, 0, false, 0u);
	run_maker = NULL;
	run_arm = CS7000P_ARM_OFF;
}

// A source whose model rows are set (cal_ok) under a table that holds the transmission's values: the table wins at
// every stage. An uncovered one, with a tone or without, is the arm's reason, never the model's drive; a covered one
// keys the table's drive, not the model's
static void test_arm_over_model(void)
{
	static uint8_t t[CS7000P_MAKER_SIZE];
	invented(t);
	CHECK(cs7000p_maker_parse(t, 0x600DCAFEu, 0, &armed_table));
	run_maker = &armed_table;
	for (uint8_t stage = 0; stage <= CS7000P_ARM_ALL; stage++) {
		run_arm = stage;
		for (uint8_t level = 0; level < CS7000P_POWER_LEVELS; level++) {
			for (int tone = 0; tone < 2; tone++) {
				for (int listed = 0; listed < 2; listed++) {
					bring_up(&cal_ok, 446u * MHZ);
					cs7000p_tx_req_t q = req(446u * MHZ, level, false);
					q.limit_list = listed != 0;
					q.tone = tone != 0;
					q.tone_sel = (subtone_t){ tone ? TONE_CTCSS : TONE_NONE, tone ? 885 : 0 };
					const cs7000p_tx_why_t w = want_why(stage, level, false, listed != 0);
					CHECK(cs7000p_rf_tx_admit(&rf, &q) == w);
					if (w == CS7000P_TX_OK) {
						uint8_t m2 = 9, m1 = 9;
						CHECK(rf.admit_arm && rf.admit_drive == (level ? 3360u : 1744u));
						CHECK(cs7000p_rf_mod_pair(&rf, 446u * MHZ, false, &m2, &m1) &&
						      m2 == armed_table.row[CS7000P_MK_FM_MOD2][0]);
						// The arm gone before the modem's set-up: no pair at all, not the model's
						cs7000p_rf_arm_set(&rf, CS7000P_ARM_OFF, 0);
						m2 = m1 = 9;
						CHECK(!cs7000p_rf_mod_pair(&rf, 446u * MHZ, false, &m2, &m1) && m2 == 9 && m1 == 9);
						CHECK(cs7000p_rf_ref_for(&rf, true) == rf.ref_offset);
						CHECK(cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) != CS7000P_TX_OK);
						CHECK(!tx_bits_ever());
						clear_log();
					} else {
						CHECK(!rf.admit && !rf.admit_arm && rf.tx_refused_why == w);
						CHECK(!cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) != CS7000P_TX_OK);
					}
					CHECK(ev_n == 0 && !tx_bits_ever());
				}
			}
		}
	}
	run_maker = NULL;
	run_arm = CS7000P_ARM_OFF;
}

// The source's PA floor over the table's drive: a drive at or under it keys nothing (2), one above it keys from the
// floor. And a table whose power row reaches the floor is not valid at all (the reader's integrity rule): nothing
static void test_arm_floor(void)
{
	static cs7000p_cal_t cal_floor;
	static uint8_t t[CS7000P_MAKER_SIZE];
	static cs7000p_maker_t m;
	static const struct { uint32_t hz; bool ok; } c[] = {
		{ 420000000u, false }, { 430000000u, false }, { 433500000u, true },
	};
	cal_floor = cs7000p_cal_model;
	cal_floor.floor = 1600;		// the table's low drive at 420 MHz exactly, over its 1584 at 430 MHz
	invented(t);
	CHECK(cs7000p_maker_parse(t, 0x600DCAFEu, 0, &armed_table));
	run_maker = &armed_table;
	run_arm = CS7000P_ARM_FM_LOW;
	for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		bring_up(&cal_floor, 446u * MHZ);
		cs7000p_tx_req_t q = req(c[i].hz, 0, false);
		q.limit_list = true;
		if (c[i].ok) {
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit_drive == 1616u);
			CHECK(cs7000p_rf_tx_begin(&rf) && cs7000p_rf_tx_key(&rf) == CS7000P_TX_OK && dac_now == 1616u);
			CHECK(inv_fail == 0 && trips == 0);
			cs7000p_rf_tx_off(&rf, false);
		} else {
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNCAL && !rf.admit && ev_n == 0 && !tx_bits_ever());
		}
	}
	// DMR on the same rows at the top stage: the same verdicts, and the burst plan's floor the board's
	run_arm = CS7000P_ARM_ALL;
	for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		bring_up(&cal_floor, 446u * MHZ);
		cs7000p_tx_req_t q = req(c[i].hz, 0, true);
		q.limit_list = true;
		if (c[i].ok) {
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit_drive == 1616u);
			admitted_dmr(&armed_table, c[i].hz, 0);
		} else {
			CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNCAL && !rf.admit && ev_n == 0 && !tx_bits_ever());
		}
	}
	CHECK(!cs7000p_maker_parse(t, 0x600DCAFEu, 1600, &m) && !m.valid);
	run_maker = &m;
	run_arm = CS7000P_ARM_ALL;
	bring_up(&cs7000p_cal_model, 446u * MHZ);
	cs7000p_tx_req_t q = req(433500000u, 0, false);
	q.limit_list = true;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNCAL);
	nothing_live(433500000u, 0, false);
	run_maker = NULL;
	run_arm = CS7000P_ARM_OFF;
}

// The arm lowered between the check and the key-up (another task, without the safe request the board makes): the
// modem gets no pair, no trim A and no registers, and the transmit plan refuses, so nothing keys. And the bench's 3:4
// over trim A in the transmit set-up, as it is over the receive level
static void test_arm_lowered(void)
{
	static uint8_t t[CS7000P_MAKER_SIZE];
	uint8_t m2 = 9, m1 = 9, n = 9;
	invented(t);
	CHECK(cs7000p_maker_parse(t, 0x600DCAFEu, 0, &armed_table));
	run_maker = &armed_table;
	for (uint8_t lower = CS7000P_ARM_OFF; lower <= CS7000P_ARM_FM_LOW; lower++) {
		run_arm = CS7000P_ARM_FM;
		bring_up(&cs7000p_cal_model, 446u * MHZ);
		cs7000p_tx_req_t q = req(446u * MHZ, 1, false);
		q.limit_list = true;
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && rf.admit_arm);
		CHECK(cs7000p_rf_ref_for(&rf, true) == -5);
		cs7000p_rf_arm_set(&rf, lower, 0x5EEDu);
		CHECK(!cs7000p_rf_mod_pair(&rf, 446u * MHZ, false, &m2, &m1) && m2 == 9 && m1 == 9);
		CHECK(cs7000p_rf_fm_tx_rows(&rf, 446u * MHZ, false, &n) == NULL && n == 0u);
		CHECK(cs7000p_rf_ref_for(&rf, true) == rf.ref_offset);
		CHECK(cs7000p_rf_drive_level(&rf, 446u * MHZ, 1) == 0u);
		CHECK(cs7000p_rf_tx_begin(&rf));	// the DAC to 0 and the receive rail off: no transmit line
		CHECK(cs7000p_rf_tx_key(&rf) == CS7000P_TX_UNCAL && !rf.admit && !tx_bits_ever());
		CHECK(rf.seq.state != CHAINSEQ_ST_TX && (shadow & EXP_TX_BITS) == 0u);
	}
	run_arm = CS7000P_ARM_FM_LOW;
	bring_up(&cs7000p_cal_model, 446u * MHZ);
	cs7000p_tx_req_t q = req(446u * MHZ, 0, false);
	q.limit_list = true;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK && cs7000p_rf_ref_for(&rf, true) == -5);
	rf.ref_override = true;
	rf.ref_offset = 12;
	CHECK(cs7000p_rf_ref_for(&rf, true) == 12);
	rf.ref_override = false;
	rf.ref_offset = CS7000P_REF_OFFSET;
	CHECK(cs7000p_rf_ref_for(&rf, true) == -5);
	run_maker = NULL;
	run_arm = CS7000P_ARM_OFF;
}

// A lower stage ends a transmission it no longer covers: first on admissions marked as one (rf.admit_arm) for each
// mode and level, then on a real one, the board's sequence after it: arm_ends, the safe request, then the new stage
static void test_arm_ends(void)
{
	static const struct { uint8_t level; bool dmr; uint8_t covered_from; } tx[] = {
		{ 0, false, CS7000P_ARM_FM_LOW }, { 1, false, CS7000P_ARM_FM }, { 0, true, CS7000P_ARM_ALL },
		{ 1, true, CS7000P_ARM_ALL },
	};
	for (unsigned i = 0; i < sizeof(tx) / sizeof(tx[0]); i++) {
		bring_up(&cs7000p_cal_model, 446u * MHZ);
		rf.admit = true;
		rf.admit_arm = true;
		rf.admit_power = tx[i].level;
		rf.admit_dmr = tx[i].dmr;
		rf.admit_hz = 446u * MHZ;
		for (uint8_t stage = 0; stage <= CS7000P_ARM_ALL; stage++) {
			CHECK(cs7000p_rf_arm_ends(&rf, stage) == (stage < tx[i].covered_from));
		}
		// Not on the maker's values (the bench drive, the model's): no stage ends it
		rf.admit_arm = false;
		for (uint8_t stage = 0; stage <= CS7000P_ARM_ALL; stage++) {
			CHECK(!cs7000p_rf_arm_ends(&rf, stage));
		}
		// Nothing admitted: nothing to end
		rf.admit_arm = true;
		rf.admit = false;
		CHECK(!cs7000p_rf_arm_ends(&rf, CS7000P_ARM_OFF));
	}
	// A carrier up on the table's values at stage 2, FM level 1 (446 MHz: 3360). Stage 2 again keeps it; stage 1 ends
	// it: the safe request, the chain safe and nothing keys again until the radio task's next request, and the new
	// stage set after it
	static uint8_t t[CS7000P_MAKER_SIZE];
	invented(t);
	CHECK(cs7000p_maker_parse(t, 0x600DCAFEu, 0, &armed_table));
	run_maker = &armed_table;
	keyup_on_table(CS7000P_ARM_FM, 446u * MHZ, 1, 3360);
	run_maker = NULL;
	run_arm = CS7000P_ARM_OFF;
	cs7000p_rf_arm_set(&rf, CS7000P_ARM_FM, 0x77u);
	CHECK(!cs7000p_rf_arm_ends(&rf, CS7000P_ARM_FM) && !cs7000p_rf_arm_ends(&rf, CS7000P_ARM_ALL));
	CHECK(rf.seq.state == CHAINSEQ_ST_TX && dac_now == 3360);
	CHECK(cs7000p_rf_arm_ends(&rf, CS7000P_ARM_FM_LOW));
	const uint32_t kept = rf.arm_id;	// the board takes the id it keeps before the safe request clears it
	board_rf_safe_fake();
	CHECK(rf.arm == CS7000P_ARM_OFF && rf.arm_id == 0u);
	cs7000p_rf_arm_set(&rf, CS7000P_ARM_FM_LOW, kept);
	check_safed();
	CHECK(rf.arm == CS7000P_ARM_FM_LOW && rf.arm_id == 0x77u && rf.restore);
	CHECK(inv_fail == 0 && trips == 0);
}

// DMR on a split channel (receive 446 MHz, transmit 441 MHz): the table's DMR pair at f_tx, never at f_rx, and the
// burst plan's drives at f_tx. 4:0x04 in DMR, trim B (-3) only on the top stage: at a receive entry while the arm
// covers DMR, at the key-up only for the transmission admitted on the table; FM's today's at every stage; the bench's
// 3:4 over all of it. The arm lowered after the admission (another task, without the safe request the board makes):
// no pair, the burst plan's drives 0, today's 4:0x04, and the bursts' tune refused, so nothing keys
static void test_arm_dmr(void)
{
	static uint8_t t[CS7000P_MAKER_SIZE];
	const uint32_t f_rx = 446u * MHZ, f_tx = 441u * MHZ;
	uint8_t m2 = 9, m1 = 9, r2 = 0, r1 = 0, t2 = 0, t1 = 0;
	uint16_t d = 9, top = 9, fl = 9;
	invented(t);
	CHECK(cs7000p_maker_parse(t, 0x600DCAFEu, 0, &armed_table) && armed_table.fields == CS7000P_MK_F_ALL);
	CHECK(cs7000p_maker_pair(&armed_table, f_rx, true, &r2, &r1, NULL));
	CHECK(cs7000p_maker_pair(&armed_table, f_tx, true, &t2, &t1, NULL));
	CHECK(r2 != t2 && r1 != t1);	// the invented rows' slope
	const int8_t today = (int8_t)CS7000P_REF_OFFSET;
	CHECK(today != -3);
	run_maker = &armed_table;
	for (uint8_t stage = 0; stage <= CS7000P_ARM_ALL; stage++) {
		run_arm = stage;
		bring_up(&cs7000p_cal_model, f_rx);
		const bool top_stage = stage == CS7000P_ARM_ALL;
		CHECK(cs7000p_rf_r04(&rf, false, false) == today && rf.r04 == today);
		CHECK(cs7000p_rf_r04(&rf, true, false) == (top_stage ? -3 : today) && rf.r04 == (top_stage ? -3 : today));
		CHECK(cs7000p_rf_r04(&rf, true, true) == today);	// no transmission admitted
		// The receive entry's pair: none from the table (receive admits nothing), so the IF host's safe gain
		CHECK(!cs7000p_rf_mod_pair(&rf, f_rx, true, &m2, &m1));
		cs7000p_tx_req_t q = req(f_tx, 1, true);
		q.limit_list = true;
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == (top_stage ? CS7000P_TX_OK : CS7000P_TX_UNARMED));
		if (!top_stage) {
			nothing_live_at(f_tx, 1, true, level_drive(&armed_table, stage, f_tx, 1));
			continue;
		}
		CHECK(cs7000p_rf_mod_pair(&rf, f_tx, true, &m2, &m1) && m2 == t2 && m1 == t1);
		m2 = m1 = 9;
		CHECK(!cs7000p_rf_mod_pair(&rf, f_rx, true, &m2, &m1) && m2 == 9 && m1 == 9);
		CHECK(cs7000p_rf_r04(&rf, true, true) == -3 && cs7000p_rf_r04(&rf, false, false) == today);
		admitted_dmr(&armed_table, f_tx, 1);
		// The getter: the bench's 3:4 over trim B, in receive and at the key-up (the radio never gets here: 3:4 is
		// refused while armed, and an admission with it, 16)
		CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_OK);
		rf.ref_override = true;
		rf.ref_offset = 12;
		CHECK(cs7000p_rf_r04(&rf, true, true) == 12 && cs7000p_rf_r04(&rf, true, false) == 12);
		rf.ref_override = false;
		rf.ref_offset = CS7000P_REF_OFFSET;
		// The arm lowered after the admission: nothing of the table, and nothing keys
		CHECK(cs7000p_rf_r04(&rf, true, true) == -3);
		cs7000p_rf_arm_set(&rf, CS7000P_ARM_FM, 0x5EEDu);
		m2 = m1 = 9;
		CHECK(!cs7000p_rf_mod_pair(&rf, f_tx, true, &m2, &m1) && m2 == 9 && m1 == 9);
		cs7000p_rf_burst_drives(&rf, &d, &top, &fl);
		CHECK(d == 0u && top == 0u);
		CHECK(cs7000p_rf_r04(&rf, true, true) == today && cs7000p_rf_r04(&rf, true, false) == today);
#if CONFIG_DMR
		clear_log();
		CHECK(!chainseq_burst_tune(&rf.seq, f_tx) && !tx_bits_ever());
#endif
		cs7000p_rf_tx_off(&rf, false);
	}
	// A table whose TRIM_B group was refused: no DMR transmission (2), and today's 4:0x04 in DMR at the top stage too
	static cs7000p_maker_t m;
	m = armed_table;
	m.fields = (uint16_t)(CS7000P_MK_F_ALL & ~CS7000P_MK_F_TRIM_B);
	run_maker = &m;
	run_arm = CS7000P_ARM_ALL;
	bring_up(&cs7000p_cal_model, f_rx);
	CHECK(cs7000p_rf_r04(&rf, true, false) == today);
	cs7000p_tx_req_t q = req(f_tx, 0, true);
	q.limit_list = true;
	CHECK(cs7000p_rf_tx_admit(&rf, &q) == CS7000P_TX_UNCAL && cs7000p_rf_r04(&rf, true, true) == today);
	// No table: today's everywhere
	run_maker = NULL;
	bring_up(&cs7000p_cal_model, f_rx);
	CHECK(cs7000p_rf_r04(&rf, true, false) == today && cs7000p_rf_r04(&rf, true, true) == today);
	run_arm = CS7000P_ARM_OFF;
}

// The modem's callbacks as the board's glue answers them (c6000_cs7000p.c, one call each): with a valid table (trim A
// -5) a receive entry's level is trim A and 4:0x04 today's, and the FM transmit set-up takes today's value for 4:0x04
// and its bias, never trim A; the bench's 3:4 is both. The coding's form for the IF host, and back
static void test_ref_choice(void)
{
	uint8_t t[CS7000P_MAKER_SIZE];
	invented(t);
	CHECK(cs7000p_maker_parse(t, 0x600DCAFEu, 0, &armed_table) && armed_table.valid);
	run_maker = &armed_table;
	bring_up(&cs7000p_cal_model, F_OWNER);
	CHECK(cs7000p_rf_ref_for(&rf, false) == -5 && rf.ref_src == CS7000P_SRC_MAKER);
	CHECK(cs7000p_rf_ref_for(&rf, true) == CS7000P_REF_OFFSET && CS7000P_REF_OFFSET != -5);
	CHECK(cs7000p_rf_r04(&rf, false, false) == CS7000P_REF_OFFSET &&
	      cs7000p_rf_r04(&rf, true, false) == CS7000P_REF_OFFSET);
	CHECK(rf.rx_ref == -5);	// the transmit set-up's question notes nothing for the view
	rf.ref_override = true;
	rf.ref_offset = 12;
	CHECK(cs7000p_rf_ref_for(&rf, false) == 12 && cs7000p_rf_ref_for(&rf, true) == 12 &&
	      cs7000p_rf_r04(&rf, false, false) == 12);
	run_maker = NULL;
	bring_up(&cs7000p_cal_model, F_OWNER);
	CHECK(cs7000p_rf_ref_for(&rf, false) == CS7000P_REF_OFFSET && cs7000p_rf_ref_for(&rf, true) == CS7000P_REF_OFFSET);
	CHECK(rf.ref_why == CS7000P_RXREF_WHY_TABLE);

	static const struct {
		uint8_t coding;
		bool sgn;
		uint8_t adc;
	} form[] = {
		{ CS7000P_REF_HOLD, false, 0 },
		{ CS7000P_REF_MAKER, true, 0 },
		{ CS7000P_REF_MAKER_ADC, true, 0x7D },
	};
	for (unsigned i = 0; i < sizeof(form) / sizeof(form[0]); i++) {
		uint8_t adc = 0x55;
		CHECK(cs7000p_ref_signed(form[i].coding, &adc) == form[i].sgn && adc == form[i].adc);
		CHECK(cs7000p_ref_coding_of(form[i].sgn, form[i].adc) == form[i].coding);
	}
	uint8_t adc = 0x55;
	CHECK(!cs7000p_ref_signed(3, &adc) && adc == 0u);	// not a coding: the hold
	CHECK(cs7000p_ref_coding_of(false, 0x7D) == CS7000P_REF_HOLD);

	// The RSSI: the table's points (code 110 on 60 / 160: -95 dBm), the data sheet's line with the bench's 3:8
	run_maker = &armed_table;
	bring_up(&cs7000p_cal_model, F_OWNER);
	uint8_t src = 9;
	CHECK(cs7000p_rf_rssi(&rf, 110u << 4, 500u, &src) == -95 && src == CS7000P_SRC_MAKER);
	rf.rssi_sheet = true;
	CHECK(cs7000p_rf_rssi(&rf, 110u << 4, 500u, &src) == cs7000p_rssi_dbm(500u) && src == CS7000P_SRC_BENCH);
	CHECK(cs7000p_rf_rssi(&rf, 110u << 4, 500u, NULL) == cs7000p_rssi_dbm(500u));
	run_maker = NULL;
	bring_up(&cs7000p_cal_model, F_OWNER);
	CHECK(!rf.rssi_sheet);	// a new chain: the table's
	CHECK(cs7000p_rf_rssi(&rf, 110u << 4, 500u, &src) == cs7000p_rssi_dbm(500u) && src == CS7000P_SRC_DEFAULT);
}

int main(void)
{
	test_start();
	test_receive();
	test_uncalibrated();
	test_override();
	test_partial_cal();
	test_settle_battery_band();
	test_off_state();
	test_ref_coding();
	test_fm_tx();
	test_fm_refusals();
	test_rf_safe();
#if CONFIG_DMR
	test_bursts();
#endif
	test_vco_rule();
	test_maker_table();
	test_arm_rule();
	test_arm_admit();
	test_arm_bench();
	test_arm_drive();
	test_arm_windows();
	test_arm_tone();
	test_arm_tone_freq();
	test_arm_over_model();
	test_arm_floor();
	test_arm_lowered();
	test_arm_ends();
	test_arm_dmr();
	test_ref_choice();
	printf("# cs7000pRadioSeqTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
