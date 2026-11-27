// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR burst key-up runner (dmr_keyup.h). Everything here runs with the modem's and the timer's interrupts held
// off: in their handlers, under the glue's lock (tx_prepare, tx_done), or under the mask the task level calls take
// themselves. So a burst's state is never seen half written, and the board's ops never run inside one another.
#include "config.h"

#if CONFIG_DMR

#include <stddef.h>
#include "dmr_keyup.h"
#include "oneshot.h"
#include "hrc/dmr_glue.h"
#include "FreeRTOS.h"

// The PA drop comes this long after the burst's first symbol: at its last symbol's end, so the ramp down is inside
// the slot's guard time. The modem's own interrupt for the burst's end comes later than that (dmr_keyup_rf_switch)
#define BURST_US	27500u
#define TIMER_MAX_US	0xFFFFu		// the one-shot's longest wait (oneshot.h)

enum {
	PHASE_IDLE,	// no burst up: the timer is stopped
	PHASE_KEY,	// the timer runs to the burst's next step
	PHASE_HOLD,	// the points are done and the PA is up: the timer runs to the drop
};

static struct {
	const dmr_keyup_ops_t *ops;	// NULL: none, or ops that were not taken. Nothing arms
	volatile bool armed;		// dmr_keyup_arm: the plan below is the transmission's
	volatile bool tuned;		// the RF is on the transmit side, on hz
	volatile bool settle_due;	// tuned since the last settle wait (dmr_keyup_tx_wait_lock)
	uint32_t hz;
	pa_keyup_t ku;
	pa_keyup_point_t pt[PA_KEYUP_POINTS_MAX];	// the burst's points, earliest first, the loop's ahead of the rule's
	pa_keyup_step_t st[PA_KEYUP_POINTS_MAX];	// and as the timer's steps: the points at one time together
	uint8_t ns;
	volatile uint8_t phase;
	uint8_t step;			// PHASE_KEY: the step the timer runs to
	uint16_t base_us;		// and its time before the first symbol: the timer's wait ends there
	uint16_t drive;			// the reference of this moment in the burst: the PA's point takes it
	uint32_t since_us;		// the timer's waits since the RF TX interrupt
	uint32_t skipped, unarmed, rx_back;
	uint32_t keyed[2];		// bursts keyed on the prepared transmit side, and tuned at their interrupt
	bool last_direct;
} k;

static void timer_wait(uint32_t us)
{
	k.since_us += us;
	stm32_oneshot_start(us);
}

// No burst up and the PA off. Also with none up: the op takes that, and a timer that failed leaves no other trace
static void burst_end(void)
{
	stm32_oneshot_stop();
	k.phase = PHASE_IDLE;
	k.ops->pa_off(k.ops->ctx);
}

static bool key_point(uint8_t kind)
{
	const dmr_keyup_ops_t *o = k.ops;
	uint16_t drive;

	switch (kind) {
	case DMR_KEYUP_PT_LOOP:
		drive = 0;	// the loop closes on no reference: the drives come after it
		break;
	case PA_KEYUP_PT_BOOST:
		drive = k.drive = k.ku.boost;
		break;
	case PA_KEYUP_PT_DRIVE:
		drive = k.drive = k.ku.drive;
		break;
	default:
		drive = k.drive;	// the PA's point: on the boost when one came before it, else on the level's drive
		break;
	}
	return o->point(o->ctx, kind, drive);
}

// The step the timer ran to, with the steps that are due sooner than the timer's shortest wait after it (those run
// now, that much early). The timer is started for what comes next before the points run: it counts while they do, so
// the time the board's points take (pin and register writes, an expander's shift) does not add up over the steps and
// into the drop. A wait that runs out meanwhile is called right after this handler (the same priority, so in order);
// that step, and what follows it, is then late by the overrun, as after a handler that had to wait for a modem
// interrupt: the timer gives no count to correct by
static void key_steps(void)
{
	uint8_t first = k.step, last = first;

	while (last + 1u < k.ns && (uint32_t)k.base_us - k.st[last + 1u].us < DMR_KEYUP_MIN_US) {
		last++;
	}
	if (last + 1u < k.ns) {
		uint32_t wait = (uint32_t)k.base_us - k.st[last + 1u].us;
		k.base_us = k.st[last + 1u].us;
		k.step = (uint8_t)(last + 1u);
		timer_wait(wait);
	} else {
		// since_us is at most int_ahead_us (the points are all before the first symbol): the drop comes BURST_US
		// after the first symbol whenever the last point ran
		k.phase = PHASE_HOLD;
		timer_wait((uint32_t)k.ops->int_ahead_us + BURST_US - k.since_us);
	}
	for (uint8_t p = first != 0u ? k.st[first - 1u].end : 0u; p < k.st[last].end; p++) {
		if (!key_point(k.pt[p].kind)) {
			// The board left its PA safe. The rest of this burst would find the chain in another state than
			// the one its points were planned for: none of them runs
			burst_end();
			k.skipped++;
			return;
		}
	}
}

// The timer's interrupt
static void on_timer(void)
{
	switch (k.phase) {
	case PHASE_KEY:
		key_steps();
		break;
	case PHASE_HOLD:
		k.phase = PHASE_IDLE;
		k.ops->pa_off(k.ops->ctx);
		break;
	default:
		break;	// stopped with its interrupt already on the way: nothing is due
	}
}

void dmr_keyup_init(const dmr_keyup_ops_t *ops)
{
	k.ops = NULL;
	if (ops == NULL || ops->tx_tune == NULL || ops->point == NULL || ops->pa_off == NULL || ops->rx_back == NULL ||
	    ops->plan == NULL) {
		return;
	}
	// The loop's point has to fit between the interrupt and the first drive, and the drop on the timer
	if ((uint32_t)ops->loop_lead_us + DMR_KEYUP_MIN_US >= ops->int_ahead_us ||
	    (uint32_t)ops->int_ahead_us + BURST_US > TIMER_MAX_US) {
		return;
	}
	stm32_oneshot_init(on_timer);
	k.ops = ops;
}

bool dmr_keyup_arm(uint32_t hz, uint8_t power)
{
	const dmr_keyup_ops_t *o = k.ops;
	pa_keyup_t ku = { .drive = 0 };
	// The physical layer mode keys once and holds, with no burst to end it: not this runner's keying
	bool ok = o != NULL && !stm32_hrc_dmr_tx_continuous();

	if (ok) {
		const pa_keyup_cfg_t *c = o->cfg != NULL ? o->cfg(o->ctx, hz) : NULL;
		if (c != NULL) {
			pa_keyup_select(c);
		}
		// The board's refusal (no calibration for hz and the level) comes before anything is written
		ok = o->plan(o->ctx, hz, power, &ku) && ku.drive != 0u;
	}
	if (!ok) {
		if (k.armed) {
			dmr_keyup_disarm();	// an earlier plan does not stay armed behind a refusal
		}
		return false;
	}
	// The loop's point is the burst's first, its lead ahead of the first drive (the PA's point, or the boost). Ahead
	// of the earliest of the plan's times, not of the first in the rule's order: the rule gives them in that order
	// (pa_keyup_drives), but a board's plan that did not would put a drive before the loop
	pa_keyup_point_t pt[PA_KEYUP_POINTS_MAX];
	pa_keyup_step_t st[PA_KEYUP_POINTS_MAX];
	uint8_t n = pa_keyup_points(&ku, pt);
	uint32_t loop_us = pt[0].us;
	for (uint8_t i = 1; i < n; i++) {
		if (pt[i].us > loop_us) {
			loop_us = pt[i].us;
		}
	}
	loop_us += o->loop_lead_us;
	n = pa_keyup_insert(pt, n, (uint16_t)(loop_us > 0xFFFFu ? 0xFFFFu : loop_us), DMR_KEYUP_PT_LOOP);
	// A point earlier than the timer can run after the interrupt goes with the first step, late, in its order
	uint8_t ns = pa_keyup_steps(pt, n, o->int_ahead_us, DMR_KEYUP_MIN_US, st);

	UBaseType_t m = portSET_INTERRUPT_MASK_FROM_ISR();
	if (k.phase != PHASE_IDLE) {
		burst_end();	// a burst that is up ends before its plan changes under it
	}
	if (hz != k.hz) {
		k.tuned = false;	// tx_prepare tunes again
	}
	k.hz = hz;
	k.ku = ku;
	for (uint8_t i = 0; i < PA_KEYUP_POINTS_MAX; i++) {
		k.pt[i] = pt[i];
		k.st[i] = st[i];
	}
	k.ns = ns;
	k.armed = true;
	portCLEAR_INTERRUPT_MASK_FROM_ISR(m);
	stm32_hrc_dmr_tx_prepare();	// a transmission the stack has already begun gets its first burst too
	return true;
}

void dmr_keyup_disarm(void)
{
	k.armed = false;	// first: an RF TX interrupt from here on keys nothing
	if (stm32_hrc_dmr_tx_active()) {
		stm32_hrc_dmr_tx_drop();
	}
	if (k.ops == NULL) {
		return;
	}
	UBaseType_t m = portSET_INTERRUPT_MASK_FROM_ISR();
	burst_end();
	k.tuned = false;
	portCLEAR_INTERRUPT_MASK_FROM_ISR(m);
}

// The glue's rf_switch, from the modem's interrupts. tx: the RF TX interrupt of one of our bursts. Else the PA off:
// the timeslot interrupt after a burst (the timer has dropped the PA by then, at the last symbol's end; this is what
// ends a burst whose timer did not), and a transmission that is finished, dropped or stalled, at once
bool dmr_keyup_rf_switch(bool tx)
{
	const dmr_keyup_ops_t *o = k.ops;

	if (!tx) {
		if (o != NULL) {
			burst_end();
		}
		return true;
	}
	if (o == NULL || !k.armed) {
		k.unarmed++;
		return false;
	}
	uint32_t until;
	if (k.phase != PHASE_IDLE || stm32_hrc_dmr_tx_continuous() || stm32_hrc_dmr_bs_boundary_us(&until)) {
		// A burst is keyed already: its drop stays where its own interrupt put it, so interrupts in a row
		// can't hold the PA up. Or a transmission that keys once and holds, which is not written here
		k.skipped++;
		return false;
	}
	// The timer first, so that it counts from the interrupt whatever follows here takes
	k.step = 0;
	k.since_us = 0;
	k.base_us = k.st[0].us;
	k.drive = k.ku.drive;
	k.phase = PHASE_KEY;
	timer_wait((uint32_t)o->int_ahead_us - k.st[0].us);	// at least DMR_KEYUP_MIN_US (pa_keyup_steps)
	bool direct = !k.tuned;
	if (direct) {
		// The glue normally had the transmit side ready (tx_prepare). Tuned here the synthesizer has had no
		// time to lock: the board's PA point lets the burst through or not
		if (!o->tx_tune(o->ctx, k.hz)) {
			stm32_oneshot_stop();	// nothing was keyed: no burst
			k.phase = PHASE_IDLE;
			k.skipped++;
			return false;
		}
		k.tuned = true;
		k.settle_due = true;
	}
	k.keyed[direct]++;
	k.last_direct = direct;
	return true;
}

// The glue's tx_prepare (under its lock, or in the timeslot interrupt for an aligned start): the transmit side ready
// ahead of the first burst, without the waits
void dmr_keyup_tx_prepare(void)
{
	const dmr_keyup_ops_t *o = k.ops;

	if (o == NULL || !k.armed || k.tuned || stm32_hrc_dmr_tx_continuous()) {
		return;
	}
	if (!o->tx_tune(o->ctx, k.hz)) {
		k.skipped++;
		return;
	}
	k.tuned = true;
	k.settle_due = true;
}

// The glue's tx_wait_lock (task level, after tx_prepare, outside its lock). The glue calls it at every prepare, also
// when the transmit side was ready already: the board's wait runs once for each tune
void dmr_keyup_tx_wait_lock(void)
{
	const dmr_keyup_ops_t *o = k.ops;

	if (o == NULL || !k.tuned || !k.settle_due) {
		return;
	}
	k.settle_due = false;	// before the wait: a tune that falls into it asks for the next one
	if (o->settle_us != NULL) {
		(void)o->settle_us(o->ctx);
	}
}

// The glue's tx_done (task level under its lock, at the engine's TX done): the receiver back, once for the
// transmission and not per burst
bool dmr_keyup_tx_done(void)
{
	const dmr_keyup_ops_t *o = k.ops;

	if (o == NULL || !k.armed || !k.tuned) {
		return false;
	}
	burst_end();
	if (!o->rx_back(o->ctx)) {
		return false;	// still on the transmit side: tx_off() sets receive up
	}
	k.tuned = false;
	k.rx_back++;
	return true;
}

void dmr_keyup_rf_counters(uint32_t *skipped, uint32_t *unarmed, uint32_t *rx)
{
	*skipped = k.skipped;
	*unarmed = k.unarmed;
	*rx = k.rx_back;
}

void dmr_keyup_keying(hal_dmr_keying_t *out)
{
	out->last_direct = k.last_direct;
	out->direct = k.keyed[1];
	out->idle = k.keyed[0];
}

#endif
