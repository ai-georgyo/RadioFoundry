// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Receive / transmit ordering of a discrete RF chain (chainseq.h)
#include <stddef.h>
#include "chainseq.h"
#include "radio/apc_ramp.h"

// --- The ops ---

static bool set(const chainseq_t *s, uint8_t line, bool on)
{
	return s->ops->line(s->ops->ctx, line, on);
}

static bool dac(const chainseq_t *s, uint16_t v)
{
	return s->ops->dac(s->ops->ctx, v);
}

static void wait_us(const chainseq_t *s, uint32_t us)
{
	if (us != 0u) {
		s->ops->delay_us(s->ops->ctx, us);
	}
}

// --- Results ---

static chainseq_err_t fail(chainseq_t *s, chainseq_err_t e)
{
	s->last_err = (uint8_t)e;
	if (e == CHAINSEQ_BUS) {
		s->errors++;
	} else if (e == CHAINSEQ_UNLOCKED) {
		s->unlocked++;
	} else {
		s->refused++;
	}
	return e;
}

// A write was refused or failed: nothing more of the sequence runs, and the chain is safe
static chainseq_err_t bus_fail(chainseq_t *s)
{
	chainseq_safe(s);
	return fail(s, CHAINSEQ_BUS);
}

// --- The PA ---

// The loop is closed: the DAC is the PA's reference
static bool keyed(const chainseq_t *s)
{
	return s->state == CHAINSEQ_ST_TX || s->state == CHAINSEQ_ST_BURST_LOOP || s->state == CHAINSEQ_ST_BURST_ON;
}

// The PA off in order, no waits. With the loop closed the reference goes to the floor first: under it a loop may
// lose control, so it is opened there, and the DAC goes to 0 after the transmit rail. With the loop open the DAC may
// hold the receiver's tuning value: it goes to 0 before anything else, so it can never become a power request
static bool pa_off(const chainseq_t *s, bool loop_closed)
{
	uint16_t rest = loop_closed ? s->floor : 0u;
	if (!dac(s, rest) || !set(s, CHAINSEQ_LINE_APC_LOOP, false) || !set(s, CHAINSEQ_LINE_TX_RAIL, false)) {
		return false;
	}
	return rest == 0u || dac(s, 0u);
}

// The FM ramp between the floor and the drive, up or down: the shared smoothstep over the span, so the output has no
// step at either end. Down ends on the floor
static bool ramp(const chainseq_t *s, bool up)
{
	uint16_t span = (uint16_t)(s->drive - s->floor);
	for (uint16_t i = 1; i <= CHAINSEQ_RAMP_STEPS; i++) {
		uint16_t step = up ? i : (uint16_t)(CHAINSEQ_RAMP_STEPS - i);
		if (!dac(s, (uint16_t)(s->floor + apc_ramp_level(span, step, CHAINSEQ_RAMP_STEPS)))) {
			return false;
		}
		if (i < CHAINSEQ_RAMP_STEPS) {
			wait_us(s, CHAINSEQ_RAMP_STEP_US);
		}
	}
	return true;
}

// A transmission ended in order: FM ramped down (task level), a burst at once. False: a write was refused, and the
// caller's chainseq_safe is what turns the PA off
static bool tx_down(chainseq_t *s)
{
	bool closed = keyed(s);
	if ((s->state == CHAINSEQ_ST_TX && !ramp(s, false)) || !pa_off(s, closed)) {
		return false;
	}
	s->state = CHAINSEQ_ST_IDLE;	// down once: what follows must not ramp from the drive again
	return true;
}

// The four writes of chainseq_safe. The DAC first: it acts at once, the loop takes its own time to open. Every step
// runs whatever the one before answered, and none waits or reads the state, so this is good from a fault handler in
// the middle of anything
static bool safe_writes(chainseq_t *s)
{
	bool ok = dac(s, 0u);
	ok = set(s, CHAINSEQ_LINE_APC_LOOP, false) && ok;
	ok = set(s, CHAINSEQ_LINE_TX_RAIL, false) && ok;
	ok = set(s, CHAINSEQ_LINE_VCO_RX, true) && ok;
	chainseq_safed(s);
	return ok;
}

void chainseq_safed(chainseq_t *s)
{
	if (s->state != CHAINSEQ_ST_OFF) {
		s->state = CHAINSEQ_ST_IDLE;
	}
}

void chainseq_safe(chainseq_t *s)
{
	if (!safe_writes(s)) {
		(void)fail(s, CHAINSEQ_BUS);
	}
}

void chainseq_init(chainseq_t *s, const chainseq_ops_t *ops)
{
	*s = (chainseq_t){ .ops = ops, .state = CHAINSEQ_ST_OFF };
	chainseq_safe(s);
}

// --- The synthesizer ---

// After the words, task level: the reads that agree, CHAINSEQ_LOCK_POLL_US apart, within the board's timeout. Without
// a lock indication the timeout is the settle time
static bool wait_lock(const chainseq_t *s)
{
	const chainseq_ops_t *o = s->ops;
	if (o->locked == NULL) {
		wait_us(s, o->lock_timeout_us);
		return true;
	}
	uint8_t run = 0;
	for (uint32_t t = 0;; t += CHAINSEQ_LOCK_POLL_US) {
		run = o->locked(o->ctx) ? (uint8_t)(run + 1u) : 0u;
		if (run >= CHAINSEQ_LOCK_READS) {
			return true;
		}
		if (t >= o->lock_timeout_us) {
			return false;
		}
		wait_us(s, CHAINSEQ_LOCK_POLL_US);
	}
}

// The synthesizer rail on, with its settle time when it was not known to be on (cold: nothing set up since
// chainseq_init)
static bool synth_rail(chainseq_t *s, bool cold)
{
	if (!set(s, CHAINSEQ_LINE_SYNTH_RAIL, true)) {
		return false;
	}
	if (cold) {
		wait_us(s, s->ops->rail_settle_us);
	}
	// From here a stray burst point finds no burst, and a failure leaves a rail that has settled
	s->state = CHAINSEQ_ST_IDLE;
	return true;
}

// --- Receive ---

chainseq_err_t chainseq_rx(chainseq_t *s, uint32_t hz, bool wide)
{
	const chainseq_ops_t *o = s->ops;
	uint32_t vco_hz = 0;
	uint16_t tune = 0;
	if (!o->rx_plan(o->ctx, hz, &vco_hz, &tune)) {
		return fail(s, CHAINSEQ_REFUSED_BAND);
	}
	bool cold = s->state == CHAINSEQ_ST_OFF;
	// A transmission comes down in order first. Then the chain is made safe whatever that answered: the DAC is about
	// to become a tuning voltage, and the loop must be open before it does
	bool down = !keyed(s) || tx_down(s);
	if (!safe_writes(s) || !down) {
		return fail(s, CHAINSEQ_BUS);
	}
	s->rx_hz = hz;
	s->wide = wide;
	// The rail and the receive VCO before the words, so the loop locks onto the VCO it will use
	if (!synth_rail(s, cold) || !o->synth(o->ctx, vco_hz)) {
		return bus_fail(s);
	}
	if (!wait_lock(s)) {
		// An unlocked receive LO radiates nothing: counted, and receive goes on
		(void)fail(s, CHAINSEQ_UNLOCKED);
	}
	// The IF chip's calibration needs the receive rail up (its LO may come from it)
	if (!set(s, CHAINSEQ_LINE_RX_RAIL, true) || !set(s, CHAINSEQ_LINE_IF_RUN, true)) {
		return bus_fail(s);
	}
	wait_us(s, o->rail_settle_us);
	if (!o->if_setup(o->ctx, wide)) {
		return bus_fail(s);
	}
	// Safe on the PA's reference line: the loop is open and the transmit rail off
	if (!dac(s, tune)) {
		return bus_fail(s);
	}
	s->state = CHAINSEQ_ST_RX;
	return CHAINSEQ_OK;
}

void chainseq_sleep(chainseq_t *s, bool sleep)
{
	if (sleep && s->state == CHAINSEQ_ST_RX) {
		if (!set(s, CHAINSEQ_LINE_RX_RAIL, false)) {
			(void)bus_fail(s);
			return;
		}
		s->state = CHAINSEQ_ST_SLEEP;
	} else if (!sleep && s->state == CHAINSEQ_ST_SLEEP) {
		(void)chainseq_rx(s, s->rx_hz, s->wide);
	}
}

// --- FM transmit ---

// A key-up that did not happen: safe at once, then the receiver back where it was, and the transmit failure reported
// (not what the way back ran into)
static chainseq_err_t tx_fail(chainseq_t *s, chainseq_err_t e)
{
	chainseq_safe(s);
	if (s->rx_hz != 0u) {
		(void)chainseq_rx(s, s->rx_hz, s->wide);
	}
	return fail(s, e);
}

chainseq_err_t chainseq_tx(chainseq_t *s, uint32_t hz, uint8_t power)
{
	const chainseq_ops_t *o = s->ops;
	uint32_t vco_hz = 0;
	uint16_t drive = 0, floor = 0;
	// A drive of 0 is a table nobody filled, one under its floor a table that contradicts itself: neither keys
	if (!o->tx_plan(o->ctx, hz, power, &vco_hz, &drive, &floor) || drive == 0u || drive < floor) {
		return fail(s, CHAINSEQ_REFUSED_CAL);
	}
	bool cold = s->state == CHAINSEQ_ST_OFF;
	// A transmission still up (a retune) comes down first. From receive the DAC goes to 0: it held the tuning
	// voltage and must not become a power request
	if (!(keyed(s) ? tx_down(s) : pa_off(s, false))) {
		return tx_fail(s, CHAINSEQ_BUS);
	}
	if (!set(s, CHAINSEQ_LINE_RX_RAIL, false) || !synth_rail(s, cold)) {
		return tx_fail(s, CHAINSEQ_BUS);
	}
	if (!set(s, CHAINSEQ_LINE_VCO_RX, false) || !o->synth(o->ctx, vco_hz)) {
		return tx_fail(s, CHAINSEQ_BUS);
	}
	if (!wait_lock(s)) {
		return tx_fail(s, CHAINSEQ_UNLOCKED);	// no carrier off frequency
	}
	if (!set(s, CHAINSEQ_LINE_TX_RAIL, true)) {
		return tx_fail(s, CHAINSEQ_BUS);
	}
	wait_us(s, o->tx_rail_lead_us);
	s->tx_hz = hz;
	s->drive = drive;
	s->floor = floor;
	// The loop is closed on a reference that gives no output, then the reference is raised
	if ((floor != 0u && !dac(s, floor)) || !set(s, CHAINSEQ_LINE_APC_LOOP, true)) {
		return tx_fail(s, CHAINSEQ_BUS);
	}
	s->state = CHAINSEQ_ST_TX;
	if (!ramp(s, true)) {
		return tx_fail(s, CHAINSEQ_BUS);
	}
	return CHAINSEQ_OK;
}

void chainseq_tx_off(chainseq_t *s)
{
	// Receiving (a burst transmission's last switch may have left it there: gotchas 3.14), asleep or never set up:
	// nothing to end. Idle is a transmission that failed on its way: the receiver comes back as after any other
	if (s->state == CHAINSEQ_ST_OFF || s->state == CHAINSEQ_ST_RX || s->state == CHAINSEQ_ST_SLEEP) {
		return;
	}
	if (!tx_down(s)) {
		// Safe at once, and the transmission is over: the way back to receive must not find one to bring down
		// again, or a refused write on the way down would be followed by a second ramp from the drive
		(void)bus_fail(s);
	}
	// Nowhere to go back to (no receive set-up yet, or the plan no longer has it): safe, and the next chainseq_rx
	// sets the receiver up
	if (s->rx_hz == 0u || chainseq_rx(s, s->rx_hz, s->wide) == CHAINSEQ_REFUSED_BAND) {
		chainseq_safe(s);
	}
}

bool chainseq_drive_now(chainseq_t *s, uint16_t drive)
{
	if (s->state != CHAINSEQ_ST_TX) {
		return false;
	}
	uint16_t v = drive < s->floor ? s->floor : drive;
	if (!dac(s, v)) {
		(void)bus_fail(s);
		return false;
	}
	s->drive = v;	// key-down ramps from here
	return true;
}

// --- Bursts: interrupt level, no waits ---

#if CONFIG_DMR

// A burst transmission: tuned, with or without a burst up
static bool bursting(const chainseq_t *s)
{
	return s->state == CHAINSEQ_ST_BURST || s->state == CHAINSEQ_ST_BURST_LOOP || s->state == CHAINSEQ_ST_BURST_ON;
}

// CHAINSEQ_LOCK_READS reads in a row, back to back. No lock indication: taken as locked
static bool locked_now(const chainseq_t *s)
{
	const chainseq_ops_t *o = s->ops;
	if (o->locked == NULL) {
		return true;
	}
	for (uint8_t i = 0; i < CHAINSEQ_LOCK_READS; i++) {
		if (!o->locked(o->ctx)) {
			return false;
		}
	}
	return true;
}

bool chainseq_burst_tune(chainseq_t *s, uint32_t hz)
{
	const chainseq_ops_t *o = s->ops;
	// Not before the first set-up (the synthesizer rail has not settled), and never under a running FM transmission
	if (s->state == CHAINSEQ_ST_OFF || s->state == CHAINSEQ_ST_TX) {
		s->refused++;
		return false;
	}
	uint32_t vco_hz = 0;
	uint16_t drive = 0, floor = 0;
	if (!o->tx_plan(o->ctx, hz, CHAINSEQ_BURST_PLAN_POWER, &vco_hz, &drive, &floor)) {
		(void)fail(s, CHAINSEQ_REFUSED_CAL);
		return false;
	}
	// A burst still up ends first; from receive the DAC held the tuning value
	if (!pa_off(s, keyed(s))) {
		(void)bus_fail(s);
		return false;
	}
	s->state = CHAINSEQ_ST_IDLE;
	s->tx_hz = hz;
	s->drive = 0;
	s->floor = floor;
	// The transmit rail last: the receive VCO is off by then, and the DAC is 0 with the loop open
	if (!set(s, CHAINSEQ_LINE_RX_RAIL, false) || !set(s, CHAINSEQ_LINE_SYNTH_RAIL, true) ||
	    !set(s, CHAINSEQ_LINE_VCO_RX, false) || !o->synth(o->ctx, vco_hz) || !set(s, CHAINSEQ_LINE_TX_RAIL, true)) {
		(void)bus_fail(s);
		return false;
	}
	s->state = CHAINSEQ_ST_BURST;
	return true;
}

// The loop closed on the floor. The transmit rail first: the burst before turned it off
static bool loop_close(chainseq_t *s)
{
	if (!set(s, CHAINSEQ_LINE_TX_RAIL, true) || (s->floor != 0u && !dac(s, s->floor)) ||
	    !set(s, CHAINSEQ_LINE_APC_LOOP, true)) {
		(void)bus_fail(s);
		return false;
	}
	s->state = CHAINSEQ_ST_BURST_LOOP;
	return true;
}

bool chainseq_burst_point(chainseq_t *s, uint8_t kind, uint16_t drive)
{
	if (!bursting(s)) {
		s->refused++;	// a timer that outlived its transmission: the DAC may be the receiver's now
		return false;
	}
	if (kind == CHAINSEQ_PT_LOOP) {
		return s->state != CHAINSEQ_ST_BURST || loop_close(s);
	}
	if (kind != PA_KEYUP_PT_PA && s->state == CHAINSEQ_ST_BURST) {
		// A drive with the loop open would sit on the DAC as a tuning value does, and close the loop on a step
		(void)bus_fail(s);
		return false;
	}
	// The lock where it matters: at the PA's point, and at whichever point first raises the reference
	if ((kind == PA_KEYUP_PT_PA || s->state != CHAINSEQ_ST_BURST_ON) && !locked_now(s)) {
		chainseq_burst_off(s);
		(void)fail(s, CHAINSEQ_UNLOCKED);
		return false;
	}
	if (s->state == CHAINSEQ_ST_BURST && !loop_close(s)) {
		return false;
	}
	uint16_t v = drive < s->floor ? s->floor : drive;
	if (!dac(s, v)) {
		(void)bus_fail(s);
		return false;
	}
	s->drive = v;
	// At the floor nothing is on air yet: the next drive asks for the lock again
	s->state = v > s->floor ? CHAINSEQ_ST_BURST_ON : CHAINSEQ_ST_BURST_LOOP;
	return true;
}

void chainseq_burst_off(chainseq_t *s)
{
	// Receiving, asleep or idle the PA is off by construction and the DAC is the receiver's: nothing to do. An FM
	// transmission ended this way loses its ramp, not its order
	if (s->state != CHAINSEQ_ST_TX && !bursting(s)) {
		return;
	}
	if (!pa_off(s, keyed(s))) {
		(void)bus_fail(s);
		return;
	}
	s->state = CHAINSEQ_ST_BURST;
}

bool chainseq_burst_rx(chainseq_t *s)
{
	const chainseq_ops_t *o = s->ops;
	if (s->state == CHAINSEQ_ST_RX) {
		return true;
	}
	if ((s->state != CHAINSEQ_ST_IDLE && !bursting(s)) || s->rx_hz == 0u) {
		s->refused++;
		return false;
	}
	uint32_t vco_hz = 0;
	uint16_t tune = 0;
	if (!o->rx_plan(o->ctx, s->rx_hz, &vco_hz, &tune)) {
		(void)fail(s, CHAINSEQ_REFUSED_BAND);
		return false;
	}
	// The transmit rail is off before the receive VCO comes on, and the loop open before the DAC tunes again
	if (!pa_off(s, keyed(s))) {
		(void)bus_fail(s);
		return false;
	}
	s->state = CHAINSEQ_ST_IDLE;
	if (!set(s, CHAINSEQ_LINE_VCO_RX, true) || !o->synth(o->ctx, vco_hz) || !set(s, CHAINSEQ_LINE_RX_RAIL, true) ||
	    !dac(s, tune)) {
		(void)bus_fail(s);
		return false;
	}
	s->state = CHAINSEQ_ST_RX;
	return true;
}

#endif
