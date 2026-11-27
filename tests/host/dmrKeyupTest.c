// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR burst key-up runner (platform/stm32/dmr_keyup) on a fake one-shot timer with its own clock, a fake RF chain
// that logs every op with its time and context, and a fake modem glue; the key-up rule is the real one
// (core/radio/pa_keyup.c). Each burst's points against the rule's, in order and on time, the loop's point first; the
// PA drop at the burst's end and the glue's switch back behind it; a refused point, a second RF TX interrupt, a
// refused plan and every disarm: none of them leaves the PA keyed or a timer running
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "FreeRTOS.h"
#include "dmr_keyup/dmr_keyup.h"
#include "oneshot.h"
#include "hrc/dmr_glue.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define INT_AHEAD	2550u	// the RF TX interrupt's lead on the first symbol
#define LOOP_LEAD	500u
#define BURST		27500u	// first symbol to the PA drop
#define DROP		(INT_AHEAD + BURST)

// ---- Context: a task, a task under the interrupt mask, or the modem's and the timer's interrupt level ----

int fake_irq_depth;
static bool in_isr;
static uint32_t ctx_errors;

void fake_assert(unsigned line)
{
	printf("FAIL: assertion at line %u\n", line);
	failures++;
}

static bool masked(void)
{
	return in_isr || fake_irq_depth > 0;
}

// ---- The one-shot timer: a clock in microseconds the test moves ----

static uint64_t now;
static void (*timer_fn)(void);
static bool timer_due;
static uint64_t timer_at;
static uint32_t timer_inits, timer_starts, timer_stops, timer_bad;

void stm32_oneshot_init(void (*fn)(void))
{
	timer_fn = fn;
	timer_due = false;
	timer_inits++;
}

void stm32_oneshot_start(uint32_t us)
{
	if (us < DMR_KEYUP_MIN_US || us > 0xFFFFu || !masked()) {
		timer_bad++;	// a wait the timer can't count or might miss, or a start a handler could fall into
	}
	timer_due = true;
	timer_at = now + us;
	timer_starts++;
}

void stm32_oneshot_stop(void)
{
	if (!masked()) {
		timer_bad++;
	}
	timer_due = false;
	timer_stops++;
}

// Time goes on to t (after the reference the test set): every wait that runs out on the way calls the handler
static uint64_t t0;

static void run_to(uint32_t t)
{
	while (timer_due && timer_at <= t0 + t) {
		if (now < timer_at) {
			now = timer_at;		// else it ran out while the handler before it was still at work: called now
		}
		timer_due = false;
		in_isr = true;
		timer_fn();
		in_isr = false;
	}
	if (now < t0 + t) {
		now = t0 + t;
	}
}

// ---- The RF chain: every op logged with its time and context ----

enum { EV_CFG, EV_PLAN, EV_TUNE, EV_SETTLE, EV_POINT, EV_OFF, EV_RX };
typedef struct {
	uint32_t t;	// after the reference
	uint8_t ev, kind;
	uint16_t drive;
	uint32_t hz;
} ev_t;

#define LOG_MAX	64u
static ev_t log_[LOG_MAX];
static uint32_t n_log;
static int ctx_tag;

static uint32_t unwaited_points;	// points that ran with no wait started for what follows them

static struct {
	bool plan_ok, tune_ok, rx_ok;
	uint32_t point_us, tune_us;	// the time the chain's writes take
	int refuse_kind;	// the point kind the chain refuses, -1: none
	bool plan_rule;		// the plan from the real rule (pa_keyup_plan) on the drives below, else ku as it is
	uint16_t d, top, ref;
	pa_keyup_t ku;
	const pa_keyup_cfg_t *cfg;
} chain;

static void put(uint8_t ev, uint8_t kind, uint16_t drive, uint32_t hz, bool want_masked)
{
	if (masked() != want_masked) {
		ctx_errors++;
	}
	if (n_log < LOG_MAX) {
		log_[n_log] = (ev_t){ .t = (uint32_t)(now - t0), .ev = ev, .kind = kind, .drive = drive, .hz = hz };
	}
	n_log++;
}

static bool op_tune(void *ctx, uint32_t hz)
{
	CHECK(ctx == &ctx_tag);
	put(EV_TUNE, 0, 0, hz, true);
	now += chain.tune_us;
	return chain.tune_ok;
}

static uint32_t op_settle(void *ctx)
{
	CHECK(ctx == &ctx_tag);
	put(EV_SETTLE, 0, 0, 0, false);
	return 1234;
}

static bool op_point(void *ctx, uint8_t kind, uint16_t drive)
{
	CHECK(ctx == &ctx_tag);
	put(EV_POINT, kind, drive, 0, true);
	if (!timer_due) {
		unwaited_points++;
	}
	now += chain.point_us;
	return (int)kind != chain.refuse_kind;
}

static void op_off(void *ctx)
{
	CHECK(ctx == &ctx_tag);
	put(EV_OFF, 0, 0, 0, true);
}

static bool op_rx(void *ctx)
{
	CHECK(ctx == &ctx_tag);
	put(EV_RX, 0, 0, 0, true);
	return chain.rx_ok;
}

static bool op_plan(void *ctx, uint32_t hz, uint8_t power, pa_keyup_t *ku)
{
	CHECK(ctx == &ctx_tag);
	put(EV_PLAN, power, 0, hz, false);
	*ku = chain.plan_rule ? pa_keyup_plan(chain.d, chain.top, chain.ref) : chain.ku;
	return chain.plan_ok;
}

static const pa_keyup_cfg_t *op_cfg(void *ctx, uint32_t hz)
{
	CHECK(ctx == &ctx_tag);
	put(EV_CFG, 0, 0, hz, false);
	return chain.cfg;
}

static const dmr_keyup_ops_t ops = {
	.tx_tune = op_tune, .settle_us = op_settle, .point = op_point, .pa_off = op_off, .rx_back = op_rx,
	.plan = op_plan, .cfg = op_cfg, .loop_lead_us = LOOP_LEAD, .int_ahead_us = INT_AHEAD, .ctx = &ctx_tag,
};

// ---- The modem glue ----

static struct {
	bool continuous, bs, active;
	bool rf_due;		// a transmission has begun: stm32_hrc_dmr_tx_prepare has the transmit side made ready
	uint32_t prepares, drops;
} glue;

bool stm32_hrc_dmr_tx_continuous(void)
{
	return glue.continuous;
}

bool stm32_hrc_dmr_bs_boundary_us(uint32_t *us)
{
	*us = 1000;
	return glue.bs;
}

bool stm32_hrc_dmr_tx_active(void)
{
	return glue.active;
}

// As the glue: the engine drops its transmission under the lock, and its PA with the board's switch
void stm32_hrc_dmr_tx_drop(void)
{
	CHECK(!masked());
	glue.drops++;
	glue.active = false;
	fake_irq_depth++;
	(void)dmr_keyup_rf_switch(false);
	fake_irq_depth--;
}

// As the glue: the board's tx_prepare under the lock, then its wait outside
void stm32_hrc_dmr_tx_prepare(void)
{
	CHECK(!masked());
	glue.prepares++;
	if (!glue.rf_due) {
		return;
	}
	fake_irq_depth++;
	dmr_keyup_tx_prepare();
	fake_irq_depth--;
	dmr_keyup_tx_wait_lock();
}

// The modem's interrupts and the engine's task, as the glue calls the hooks
static bool isr_rftx(void)
{
	in_isr = true;
	bool ok = dmr_keyup_rf_switch(true);
	in_isr = false;
	return ok;
}

static void isr_pa_drop(void)
{
	in_isr = true;
	CHECK(dmr_keyup_rf_switch(false));
	in_isr = false;
}

static bool task_tx_done(void)
{
	fake_irq_depth++;
	bool ok = dmr_keyup_tx_done();
	fake_irq_depth--;
	return ok;
}

// ---- Helpers ----

static const pa_keyup_cfg_t fixed800 = PA_KEYUP_FIXED(800);
static const pa_keyup_cfg_t fixed600 = PA_KEYUP_FIXED(600);

static uint32_t skipped0, unarmed0, rx0;
static hal_dmr_keying_t keying0;

static uint32_t skipped(void)
{
	uint32_t s, u, r;
	dmr_keyup_rf_counters(&s, &u, &r);
	return s - skipped0;
}

static uint32_t unarmed(void)
{
	uint32_t s, u, r;
	dmr_keyup_rf_counters(&s, &u, &r);
	return u - unarmed0;
}

static uint32_t rx_backs(void)
{
	uint32_t s, u, r;
	dmr_keyup_rf_counters(&s, &u, &r);
	return r - rx0;
}

static uint32_t keyed_idle(void)
{
	hal_dmr_keying_t kg;
	dmr_keyup_keying(&kg);
	return kg.idle - keying0.idle;
}

static uint32_t keyed_direct(void)
{
	hal_dmr_keying_t kg;
	dmr_keyup_keying(&kg);
	return kg.direct - keying0.direct;
}

static void log_clear(void)
{
	n_log = 0;
}

// A new case: the runner disarmed, the fakes as a calibrated radio with the glue preparing each transmission, the
// level's key-up from the real rule with fixed constants (the PA's point 800 us ahead), the clock at a new reference
static void fresh(void)
{
	glue = (typeof(glue)){ .rf_due = true };
	chain = (typeof(chain)){ .plan_ok = true, .tune_ok = true, .rx_ok = true, .refuse_kind = -1, .plan_rule = true,
				 .d = 1000, .top = 3000, .ref = 0, .cfg = &fixed800 };
	dmr_keyup_init(&ops);
	dmr_keyup_disarm();
	CHECK(!timer_due);
	dmr_keyup_rf_counters(&skipped0, &unarmed0, &rx0);
	dmr_keyup_keying(&keying0);
	now += 1000000u;
	t0 = now;
	timer_starts = timer_stops = 0;
	log_clear();
}

static bool is(uint32_t i, uint8_t ev, uint32_t t)
{
	return i < n_log && log_[i].ev == ev && log_[i].t == t;
}

static bool is_point(uint32_t i, uint8_t kind, uint16_t drive, uint32_t t)
{
	return is(i, EV_POINT, t) && log_[i].kind == kind && log_[i].drive == drive;
}

// ---- The cases ----

// Nothing armed: an RF TX interrupt keys nothing and no op but the PA's off is ever called
static void test_unarmed(void)
{
	fresh();
	CHECK(!isr_rftx());
	CHECK(unarmed() == 1 && n_log == 0 && !timer_due && timer_starts == 0);
	fake_irq_depth++;
	dmr_keyup_tx_prepare();
	fake_irq_depth--;
	dmr_keyup_tx_wait_lock();
	CHECK(!task_tx_done());
	CHECK(n_log == 0);
	isr_pa_drop();
	CHECK(n_log == 1 && is(0, EV_OFF, 0));
	run_to(100000);
	CHECK(n_log == 1 && unarmed() == 1 && skipped() == 0 && rx_backs() == 0);
	CHECK(keyed_idle() == 0 && keyed_direct() == 0);
}

// A transmission as the first board runs it: the plan, the transmit side prepared and settled, then each burst's
// loop point, PA point and drop on time, and the receiver back at the end
static void test_transmission(void)
{
	fresh();
	CHECK(dmr_keyup_arm(446000000u, 1));
	// The band's constants, the plan, and only then the chain: tuned under the mask, settled outside it
	CHECK(n_log == 4 && is(0, EV_CFG, 0) && is(1, EV_PLAN, 0) && is(2, EV_TUNE, 0) && is(3, EV_SETTLE, 0));
	CHECK(log_[0].hz == 446000000u && log_[1].hz == 446000000u && log_[1].kind == 1 && log_[2].hz == 446000000u);
	CHECK(glue.prepares == 1 && !timer_due);
	for (uint32_t burst = 0; burst < 3; burst++) {
		t0 = now;
		log_clear();
		CHECK(isr_rftx());
		CHECK(n_log == 0 && timer_due);		// nothing at the interrupt itself: the transmit side was ready
		run_to(1249);
		CHECK(n_log == 0);
		run_to(1250);
		// The loop closes on no reference, its lead ahead of the PA's point
		CHECK(n_log == 1 && is_point(0, DMR_KEYUP_PT_LOOP, 0, INT_AHEAD - 800u - LOOP_LEAD));
		run_to(1749);
		CHECK(n_log == 1);
		run_to(1750);
		CHECK(n_log == 2 && is_point(1, PA_KEYUP_PT_PA, 1000, INT_AHEAD - 800u));
		run_to(DROP - 1u);
		CHECK(n_log == 2 && timer_due);
		run_to(DROP);
		CHECK(n_log == 3 && is(2, EV_OFF, DROP) && !timer_due);
		// The glue's switch back at the timeslot interrupt after the burst: the PA off once more, nothing else
		run_to(DROP + 1850u);
		isr_pa_drop();
		CHECK(n_log == 4 && is(3, EV_OFF, DROP + 1850u) && !timer_due);
		run_to(60000);
	}
	CHECK(keyed_idle() == 3 && keyed_direct() == 0 && skipped() == 0 && unarmed() == 0);
	hal_dmr_keying_t kg;
	dmr_keyup_keying(&kg);
	CHECK(!kg.last_direct);
	// TX done: the PA off, the receiver back, counted; the transmit side is not ready any more
	t0 = now;
	log_clear();
	CHECK(task_tx_done());
	CHECK(n_log == 2 && is(0, EV_OFF, 0) && is(1, EV_RX, 0) && rx_backs() == 1);
	CHECK(!task_tx_done());
	CHECK(n_log == 2);
	// The next transmission of the same key-down is prepared again
	stm32_hrc_dmr_tx_prepare();
	CHECK(n_log == 4 && is(2, EV_TUNE, 0) && is(3, EV_SETTLE, 0));
	stm32_hrc_dmr_tx_prepare();
	CHECK(n_log == 4);	// once
	dmr_keyup_disarm();
	CHECK(ctx_errors == 0 && timer_bad == 0);
}

// A level with a boost: the loop, the boost, the PA on the boost (the reference of that moment), then the level's
// drive, each at the rule's time
static void test_boost(void)
{
	fresh();
	chain.plan_rule = false;
	chain.ku = (pa_keyup_t){ .boost = 2400, .drive = 1000, .boost_us = 1500, .pa_us = 1200, .drive_us = 400 };
	CHECK(dmr_keyup_arm(446000000u, 0));
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(40000);
	CHECK(n_log == 5);
	CHECK(is_point(0, DMR_KEYUP_PT_LOOP, 0, INT_AHEAD - 1500u - LOOP_LEAD));	// ahead of the first drive, the boost
	CHECK(is_point(1, PA_KEYUP_PT_BOOST, 2400, INT_AHEAD - 1500u));
	CHECK(is_point(2, PA_KEYUP_PT_PA, 2400, INT_AHEAD - 1200u));
	CHECK(is_point(3, PA_KEYUP_PT_DRIVE, 1000, INT_AHEAD - 400u));
	CHECK(is(4, EV_OFF, DROP));
	// The boost and the PA at one time: one step, the boost first
	chain.ku = (pa_keyup_t){ .boost = 2400, .drive = 1000, .boost_us = 1500, .pa_us = 1500, .drive_us = 400 };
	CHECK(dmr_keyup_arm(446000000u, 0));
	run_to(60000);
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(40000);
	CHECK(n_log == 5);
	CHECK(is_point(0, DMR_KEYUP_PT_LOOP, 0, 550) && is_point(1, PA_KEYUP_PT_BOOST, 2400, 1050));
	CHECK(is_point(2, PA_KEYUP_PT_PA, 2400, 1050) && is_point(3, PA_KEYUP_PT_DRIVE, 1000, 2150));
	CHECK(is(4, EV_OFF, DROP));
	dmr_keyup_disarm();
	CHECK(ctx_errors == 0 && timer_bad == 0 && skipped() == 0);
}

static uint32_t rnd = 2463534242u;
static uint32_t next_rand(uint32_t n)
{
	rnd ^= rnd << 13;
	rnd ^= rnd >> 17;
	rnd ^= rnd << 5;
	return rnd % n;
}

// The chain's writes take their time (an expander's shift, a synthesizer's words): the steps and the drop stay where
// the plan put them, because each wait is started before the points of its step run. Only a point that takes longer
// than the gap to the next step makes that step late, and what follows it by as much (the timer has no count to read)
static void test_point_time(void)
{
	fresh();
	chain.plan_rule = false;
	chain.ku = (pa_keyup_t){ .boost = 2400, .drive = 1000, .boost_us = 1500, .pa_us = 1200, .drive_us = 400 };
	chain.point_us = 40;
	CHECK(dmr_keyup_arm(446000000u, 0));
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(40000);
	CHECK(n_log == 5 && is_point(0, DMR_KEYUP_PT_LOOP, 0, 550) && is_point(1, PA_KEYUP_PT_BOOST, 2400, 1050));
	CHECK(is_point(2, PA_KEYUP_PT_PA, 2400, 1350) && is_point(3, PA_KEYUP_PT_DRIVE, 1000, 2150) && is(4, EV_OFF, DROP));
	// Longer than the boost's 300 us: the PA's point right behind the boost's, 100 us late, and so the rest
	chain.point_us = 400;
	run_to(60000);
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(40000);
	CHECK(n_log == 5 && is_point(0, DMR_KEYUP_PT_LOOP, 0, 550) && is_point(1, PA_KEYUP_PT_BOOST, 2400, 1050));
	CHECK(is_point(2, PA_KEYUP_PT_PA, 2400, 1450) && is_point(3, PA_KEYUP_PT_DRIVE, 1000, 2250));
	CHECK(is(4, EV_OFF, DROP + 100u));
	dmr_keyup_disarm();
	// A tune at the interrupt takes its time too: the points count from the interrupt all the same
	fresh();
	glue.rf_due = false;
	chain.tune_us = 300;
	chain.point_us = 40;
	CHECK(dmr_keyup_arm(446000000u, 1));
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(40000);
	CHECK(n_log == 4 && is(0, EV_TUNE, 0) && is_point(1, DMR_KEYUP_PT_LOOP, 0, 1250));
	CHECK(is_point(2, PA_KEYUP_PT_PA, 1000, 1750) && is(3, EV_OFF, DROP));
	dmr_keyup_disarm();
	CHECK(ctx_errors == 0 && timer_bad == 0 && skipped() == 0);
}

// Any key-up the rule can give, the ones that start before the timer can run included: the loop's point first with
// no reference, the rule's points in the rule's order with the rule's drives, none late that could be on time, none
// more than the timer's shortest wait early, and the drop where it belongs
static void test_rule(void)
{
	for (uint32_t run = 0; run < 4000; run++) {
		pa_keyup_cfg_t c = {
			.dead_pm = (uint16_t)next_rand(950), .knee_us = (uint16_t)next_rand(400),
			.slope_us = (uint16_t)next_rand(3000), .window_us = (uint16_t)(1u + next_rand(2600)),
			.pa_us = (uint16_t)next_rand(3000), .ease_pm = (uint16_t)(next_rand(3) == 0 ? next_rand(600) : 0),
		};
		c.settle_us = (uint16_t)next_rand(c.window_us);
		uint16_t top = (uint16_t)(500u + next_rand(3500)), d = (uint16_t)(1u + next_rand(top));
		pa_keyup_t ku = pa_keyup_drives(&c, d, top, (uint16_t)(next_rand(2) ? next_rand(d + 1u) : 0), 4095);
		pa_keyup_point_t want[PA_KEYUP_POINTS];
		uint8_t n_want = pa_keyup_points(&ku, want);

		fresh();
		chain.plan_rule = false;
		chain.ku = ku;
		CHECK(dmr_keyup_arm(446000000u, 0));
		t0 = now;
		log_clear();
		CHECK(isr_rftx());
		run_to(40000);
		if (n_log != n_want + 2u) {
			CHECK(n_log == n_want + 2u);
			continue;
		}
		CHECK(log_[0].ev == EV_POINT && log_[0].kind == DMR_KEYUP_PT_LOOP && log_[0].drive == 0);
		uint32_t first = log_[0].t;
		CHECK(first >= DMR_KEYUP_MIN_US);
		for (uint32_t i = 0; i < n_want; i++) {
			const ev_t *e = &log_[i + 1u];
			uint16_t drive = want[i].kind == PA_KEYUP_PT_DRIVE ? ku.drive : ku.boost != 0u ? ku.boost : ku.drive;
			CHECK(e->ev == EV_POINT && e->kind == want[i].kind && e->drive == drive);
			CHECK(e->t >= log_[i].t);
			if (want[i].us + DMR_KEYUP_MIN_US > INT_AHEAD) {
				CHECK(e->t == first);	// due before the timer could run: with the first step
			} else {
				uint32_t due = INT_AHEAD - want[i].us;
				CHECK(e->t <= due && due - e->t < DMR_KEYUP_MIN_US);
			}
		}
		// The loop's own time: its lead ahead of the first drive, when the interrupt leaves that much
		if ((uint32_t)want[0].us + LOOP_LEAD + DMR_KEYUP_MIN_US <= INT_AHEAD) {
			CHECK(first == INT_AHEAD - want[0].us - LOOP_LEAD);
		}
		CHECK(is(n_want + 1u, EV_OFF, DROP));
		CHECK(!timer_due);
	}
	dmr_keyup_disarm();
	CHECK(ctx_errors == 0 && timer_bad == 0 && skipped() == 0);
}

// A plan whose times are not in the rule's order (a board's own, or one gone wrong): the loop's point is still the
// burst's first, its lead ahead of the earliest of them, and the drives keep the rule's order behind it. A chain with
// an interlock on its reference would refuse a drive that came before the loop
static void test_plan_order(void)
{
	static const pa_keyup_t plans[] = {
		{ .boost = 2400, .drive = 1000, .boost_us = 100, .pa_us = 1500, .drive_us = 50 },	// the PA before its boost
		{ .boost = 2400, .drive = 1000, .boost_us = 300, .pa_us = 200, .drive_us = 1900 },	// the drive before both
		{ .boost = 2400, .drive = 1000, .boost_us = 1500, .pa_us = 400, .drive_us = 1200 },	// the drive before the PA
	};
	static const uint32_t loop_at[] = { 550, 150, 550 };	// INT_AHEAD - the earliest time - LOOP_LEAD
	static const uint32_t at[][3] = { { 2450, 2450, 2500 }, { 2250, 2350, 2350 }, { 1050, 2150, 2150 } };

	for (uint32_t i = 0; i < sizeof(plans) / sizeof(plans[0]); i++) {
		fresh();
		chain.plan_rule = false;
		chain.ku = plans[i];
		CHECK(dmr_keyup_arm(446000000u, 0));
		t0 = now;
		log_clear();
		CHECK(isr_rftx());
		run_to(40000);
		CHECK(n_log == 5 && is_point(0, DMR_KEYUP_PT_LOOP, 0, loop_at[i]));
		CHECK(is_point(1, PA_KEYUP_PT_BOOST, 2400, at[i][0]) && is_point(2, PA_KEYUP_PT_PA, 2400, at[i][1]));
		CHECK(is_point(3, PA_KEYUP_PT_DRIVE, 1000, at[i][2]) && is(4, EV_OFF, DROP));
		dmr_keyup_disarm();
	}
	CHECK(ctx_errors == 0 && timer_bad == 0 && skipped() == 0);
}

// A point the chain refuses: the PA off at once, counted, and the burst's other points never run. The next burst is
// tried again from its loop point
static void test_refused_point(void)
{
	static const uint8_t kinds[] = { DMR_KEYUP_PT_LOOP, PA_KEYUP_PT_BOOST, PA_KEYUP_PT_PA, PA_KEYUP_PT_DRIVE };
	static const uint32_t at[] = { 550, 1050, 1350, 2150 };

	for (uint32_t k = 0; k < 4; k++) {
		fresh();
		chain.plan_rule = false;
		chain.ku = (pa_keyup_t){ .boost = 2400, .drive = 1000, .boost_us = 1500, .pa_us = 1200, .drive_us = 400 };
		CHECK(dmr_keyup_arm(446000000u, 0));
		chain.refuse_kind = kinds[k];
		t0 = now;
		log_clear();
		CHECK(isr_rftx());
		run_to(at[k]);
		CHECK(n_log == k + 2u && !timer_due);	// the timer stops with the burst
		run_to(40000);
		CHECK(n_log == k + 2u);
		CHECK(is(k, EV_POINT, at[k]) && log_[k].kind == kinds[k]);
		CHECK(is(k + 1u, EV_OFF, at[k]));
		CHECK(skipped() == 1 && !timer_due);
		isr_pa_drop();
		CHECK(n_log == k + 3u && is(k + 2u, EV_OFF, 40000));
		// The chain takes the next burst
		chain.refuse_kind = -1;
		run_to(60000);
		t0 = now;
		log_clear();
		CHECK(isr_rftx());
		run_to(40000);
		CHECK(n_log == 5 && is_point(0, DMR_KEYUP_PT_LOOP, 0, 550) && is(4, EV_OFF, DROP));
		CHECK(skipped() == 1);
		dmr_keyup_disarm();
	}
	CHECK(ctx_errors == 0 && timer_bad == 0);
}

// A second RF TX interrupt while a burst is up, during its key-up and with the PA on: refused and counted, and the
// burst's points and its drop stay where the first interrupt put them
static void test_second_interrupt(void)
{
	fresh();
	CHECK(dmr_keyup_arm(446000000u, 1));
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(600);
	CHECK(!isr_rftx());
	run_to(1500);
	CHECK(!isr_rftx());
	run_to(10000);
	CHECK(!isr_rftx());
	run_to(DROP - 1u);
	CHECK(!isr_rftx());
	CHECK(n_log == 2 && timer_due);
	run_to(DROP);
	CHECK(n_log == 3 && is_point(0, DMR_KEYUP_PT_LOOP, 0, 1250) && is_point(1, PA_KEYUP_PT_PA, 1000, 1750));
	CHECK(is(2, EV_OFF, DROP) && !timer_due);
	CHECK(skipped() == 4 && keyed_idle() == 1);
	// With the PA down again the next interrupt keys the next burst
	run_to(60000);
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(40000);
	CHECK(n_log == 3 && is(2, EV_OFF, DROP) && keyed_idle() == 2);
	dmr_keyup_disarm();
	CHECK(ctx_errors == 0 && timer_bad == 0);
}

// The timer never comes (its interrupt lost): the glue's switch back ends the burst, and the next one is keyed
static void test_fallback(void)
{
	fresh();
	CHECK(dmr_keyup_arm(446000000u, 1));
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(2000);
	CHECK(n_log == 2 && timer_due);
	timer_due = false;	// lost
	run_to(DROP + 1850u);
	CHECK(n_log == 2);
	isr_pa_drop();
	CHECK(n_log == 3 && is(2, EV_OFF, DROP + 1850u));
	run_to(60000);
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(40000);
	CHECK(n_log == 3 && is(2, EV_OFF, DROP));
	// The glue ends a burst early (a transmission dropped or finished): the PA off there, and the timer with it
	run_to(60000);
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(1500);
	isr_pa_drop();
	CHECK(n_log == 2 && is(1, EV_OFF, 1500) && !timer_due);
	run_to(40000);
	CHECK(n_log == 2);
	dmr_keyup_disarm();
	CHECK(ctx_errors == 0 && timer_bad == 0 && skipped() == 0);
}

// Disarm in every state: the PA off under the mask, the timer stopped, a running transmission dropped, and from then
// on nothing is keyed, not by an RF TX interrupt and not by a timer interrupt that was already on its way
static void test_disarm(void)
{
	// 0: never armed, 1: armed, 2: prepared, 3: before the first point, 4: between the points, 5: the PA up,
	// 6: after the drop, 7: after TX done
	for (uint32_t state = 0; state < 8; state++) {
		for (uint32_t active = 0; active < 2; active++) {
			fresh();
			glue.rf_due = state >= 2;
			if (state >= 1) {
				CHECK(dmr_keyup_arm(446000000u, 1));
			}
			if (state >= 3) {
				t0 = now;
				CHECK(isr_rftx());
				run_to(state == 3 ? 100 : state == 4 ? 1500 : state == 5 ? 10000 : 40000);
			}
			if (state == 7) {
				CHECK(task_tx_done());
			}
			glue.active = active != 0;
			log_clear();
			uint32_t at = (uint32_t)(now - t0);
			dmr_keyup_disarm();
			CHECK(glue.drops == active && !glue.active);
			// The PA off, by the dropped transmission's switch back as well when there was one
			CHECK(n_log == 1u + active && is(n_log - 1u, EV_OFF, at));
			CHECK(!timer_due);
			if (timer_fn != NULL) {
				in_isr = true;
				timer_fn();	// the timer's interrupt was pending when the mask closed: nothing is due
				in_isr = false;
			}
			run_to(at + 60000u);
			CHECK(n_log == 1u + active);
			CHECK(!isr_rftx());
			CHECK(unarmed() == 1 && n_log == 1u + active && !timer_due);
			CHECK(!task_tx_done());
			fake_irq_depth++;
			dmr_keyup_tx_prepare();
			fake_irq_depth--;
			dmr_keyup_tx_wait_lock();
			CHECK(n_log == 1u + active);
			// Armed again, it is a new transmission: tuned again whatever the last one left
			glue.rf_due = true;
			log_clear();
			CHECK(dmr_keyup_arm(446000000u, 1));
			CHECK(n_log == 4 && is(2, EV_TUNE, at + 60000u));
			dmr_keyup_disarm();
		}
	}
	CHECK(ctx_errors == 0 && timer_bad == 0);
}

// The board's plan refuses (no calibration): nothing is armed and the chain is not touched. A drive of 0 is no plan
// either. A refusal behind an arm leaves nothing armed
static void test_refused_plan(void)
{
	fresh();
	chain.plan_ok = false;
	CHECK(!dmr_keyup_arm(446000000u, 1));
	CHECK(n_log == 2 && is(0, EV_CFG, 0) && is(1, EV_PLAN, 0));
	CHECK(glue.prepares == 0 && timer_starts == 0);
	CHECK(!isr_rftx());
	CHECK(unarmed() == 1 && n_log == 2);

	fresh();
	chain.d = 0;
	chain.plan_rule = false;
	chain.ku = (pa_keyup_t){ .drive = 0, .pa_us = 800 };
	CHECK(!dmr_keyup_arm(446000000u, 1));
	CHECK(n_log == 2 && glue.prepares == 0);
	CHECK(!isr_rftx());
	CHECK(unarmed() == 1 && n_log == 2);

	fresh();
	CHECK(dmr_keyup_arm(446000000u, 1));
	t0 = now;
	CHECK(isr_rftx());
	run_to(10000);		// the PA is up
	log_clear();
	chain.plan_ok = false;
	CHECK(!dmr_keyup_arm(446000000u, 2));
	CHECK(n_log == 3 && is(0, EV_CFG, 10000) && is(1, EV_PLAN, 10000) && is(2, EV_OFF, 10000));
	CHECK(!timer_due);
	run_to(40000);
	CHECK(n_log == 3);
	CHECK(!isr_rftx());
	CHECK(unarmed() == 1 && n_log == 3);
	CHECK(ctx_errors == 0 && timer_bad == 0);
}

// The keyings this runner does not do are refused: the physical layer mode's (keyed once and held), at the arm and at
// the interrupt, and the split duplex outbound's
static void test_not_written(void)
{
	fresh();
	glue.continuous = true;
	CHECK(!dmr_keyup_arm(446000000u, 1));
	CHECK(n_log == 0 && glue.prepares == 0);
	CHECK(!isr_rftx());
	CHECK(unarmed() == 1);

	fresh();
	glue.rf_due = false;
	CHECK(dmr_keyup_arm(446000000u, 1));
	log_clear();
	glue.continuous = true;
	fake_irq_depth++;
	dmr_keyup_tx_prepare();
	fake_irq_depth--;
	CHECK(!isr_rftx());
	CHECK(skipped() == 1 && n_log == 0 && !timer_due);
	isr_pa_drop();		// the session's end still drops the PA
	CHECK(n_log == 1 && is(0, EV_OFF, 0));
	glue.continuous = false;
	glue.bs = true;
	CHECK(!isr_rftx());
	CHECK(skipped() == 2 && n_log == 1 && !timer_due);
	glue.bs = false;
	CHECK(isr_rftx());
	dmr_keyup_disarm();
	CHECK(ctx_errors == 0 && timer_bad == 0);
}

// The glue did not prepare the transmit side (the stack began its transmission after the arm and the interrupt came
// first): tuned at the interrupt, counted as such, and the points run. A tune that fails keys nothing
static void test_direct(void)
{
	fresh();
	glue.rf_due = false;
	CHECK(dmr_keyup_arm(446000000u, 1));
	CHECK(n_log == 2);	// the plan only
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	CHECK(n_log == 1 && is(0, EV_TUNE, 0) && log_[0].hz == 446000000u);
	run_to(40000);
	CHECK(n_log == 4 && is_point(1, DMR_KEYUP_PT_LOOP, 0, 1250) && is_point(2, PA_KEYUP_PT_PA, 1000, 1750));
	CHECK(is(3, EV_OFF, DROP));
	CHECK(keyed_direct() == 1 && keyed_idle() == 0);
	hal_dmr_keying_t kg;
	dmr_keyup_keying(&kg);
	CHECK(kg.last_direct);
	// The next burst finds it tuned
	run_to(60000);
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(40000);
	CHECK(n_log == 3 && keyed_direct() == 1 && keyed_idle() == 1);
	dmr_keyup_keying(&kg);
	CHECK(!kg.last_direct);
	dmr_keyup_disarm();

	fresh();
	glue.rf_due = false;
	chain.tune_ok = false;
	CHECK(dmr_keyup_arm(446000000u, 1));
	log_clear();
	CHECK(!isr_rftx());
	CHECK(n_log == 1 && is(0, EV_TUNE, 0) && skipped() == 1 && !timer_due);
	CHECK(!isr_rftx());	// no burst is up behind it: the next interrupt tries again
	CHECK(n_log == 2 && is(1, EV_TUNE, 0) && skipped() == 2 && !timer_due);
	run_to(40000);
	CHECK(n_log == 2 && keyed_direct() == 0);
	// Nor from the glue's prepare; each is counted, and a later one that works is taken
	glue.rf_due = true;
	stm32_hrc_dmr_tx_prepare();
	CHECK(n_log == 3 && is(2, EV_TUNE, 40000) && skipped() == 3);	// no settle wait behind a tune that failed
	chain.tune_ok = true;
	stm32_hrc_dmr_tx_prepare();
	CHECK(n_log == 5 && is(3, EV_TUNE, 40000) && is(4, EV_SETTLE, 40000));
	dmr_keyup_disarm();
	CHECK(ctx_errors == 0 && timer_bad == 0);
}

// TX done with the receiver not coming back: said so, the transmit side still counts as tuned, and it is tried again
static void test_tx_done(void)
{
	fresh();
	CHECK(dmr_keyup_arm(446000000u, 1));
	t0 = now;
	CHECK(isr_rftx());
	run_to(10000);	// TX done with the PA still up (a transmission cut short): the timer goes with it
	log_clear();
	chain.rx_ok = false;
	CHECK(!task_tx_done());
	CHECK(n_log == 2 && is(0, EV_OFF, 10000) && is(1, EV_RX, 10000) && rx_backs() == 0 && !timer_due);
	run_to(40000);
	CHECK(n_log == 2);
	chain.rx_ok = true;
	CHECK(task_tx_done());
	CHECK(n_log == 4 && is(3, EV_RX, 40000) && rx_backs() == 1);
	dmr_keyup_disarm();
	CHECK(ctx_errors == 0 && timer_bad == 0);
}

// A new arm while armed: the same frequency keeps the transmit side, another one tunes again; a burst that is up
// ends first. The band's constants are selected before the plan is made
static void test_rearm(void)
{
	fresh();
	pa_keyup_select(&fixed800);
	CHECK(dmr_keyup_arm(446000000u, 1));
	log_clear();
	chain.d = 1500;
	CHECK(dmr_keyup_arm(446000000u, 2));
	CHECK(n_log == 2 && is(0, EV_CFG, 0) && is(1, EV_PLAN, 0));	// no tune: it is on that frequency
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(10000);
	CHECK(n_log == 2 && is_point(1, PA_KEYUP_PT_PA, 1500, 1750));	// the new level's drive
	// Another frequency, in a band with other constants, while the PA is up
	log_clear();
	chain.cfg = &fixed600;
	CHECK(dmr_keyup_arm(435000000u, 2));
	CHECK(n_log == 5 && is(0, EV_CFG, 10000) && is(1, EV_PLAN, 10000) && is(2, EV_OFF, 10000));
	CHECK(is(3, EV_TUNE, 10000) && log_[3].hz == 435000000u && is(4, EV_SETTLE, 10000));
	CHECK(!timer_due);
	run_to(60000);
	CHECK(n_log == 5);	// the old burst's drop does not come: it ended at the arm
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(40000);
	CHECK(n_log == 3 && is_point(0, DMR_KEYUP_PT_LOOP, 0, INT_AHEAD - 600u - LOOP_LEAD));
	CHECK(is_point(1, PA_KEYUP_PT_PA, 1500, INT_AHEAD - 600u) && is(2, EV_OFF, DROP));
	dmr_keyup_disarm();
	pa_keyup_select(&fixed800);
	CHECK(ctx_errors == 0 && timer_bad == 0);
}

// Ops the runner can't work with are not taken: nothing arms, no op is called, the timer is left alone
static void test_ops(void)
{
	dmr_keyup_ops_t bad[8];
	uint32_t inits;

	fresh();
	for (uint32_t i = 0; i < 8; i++) {
		bad[i] = ops;
	}
	bad[0].tx_tune = NULL;
	bad[1].point = NULL;
	bad[2].pa_off = NULL;
	bad[3].rx_back = NULL;
	bad[4].plan = NULL;
	bad[5].int_ahead_us = LOOP_LEAD + DMR_KEYUP_MIN_US;	// no room for the loop's lead after the interrupt
	bad[6].int_ahead_us = 0xFFFFu - BURST + 1u;		// the drop past what the timer counts
	bad[7].loop_lead_us = INT_AHEAD;
	inits = timer_inits;
	for (uint32_t i = 0; i < 9; i++) {
		log_clear();
		dmr_keyup_init(i < 8 ? &bad[i] : NULL);
		CHECK(timer_inits == inits);
		CHECK(!dmr_keyup_arm(446000000u, 1));
		CHECK(!isr_rftx());
		isr_pa_drop();
		fake_irq_depth++;
		dmr_keyup_tx_prepare();
		fake_irq_depth--;
		dmr_keyup_tx_wait_lock();
		CHECK(!task_tx_done());
		dmr_keyup_disarm();
		CHECK(n_log == 0 && timer_starts == 0 && glue.prepares == 0);
	}
	// The limits themselves, and the optional ops left out
	dmr_keyup_ops_t edge = ops;
	edge.int_ahead_us = 0xFFFFu - BURST;
	edge.loop_lead_us = (uint16_t)(edge.int_ahead_us - DMR_KEYUP_MIN_US - 1u);
	edge.settle_us = NULL;
	edge.cfg = NULL;
	dmr_keyup_init(&edge);
	CHECK(timer_inits == inits + 1u);
	CHECK(dmr_keyup_arm(446000000u, 1));
	CHECK(n_log == 2 && is(0, EV_PLAN, 0) && is(1, EV_TUNE, 0));
	t0 = now;
	log_clear();
	CHECK(isr_rftx());
	run_to(0xFFFF);
	// The loop's point as early as the timer runs, the PA's on time, the drop at the timer's longest wait
	CHECK(n_log == 3 && is_point(0, DMR_KEYUP_PT_LOOP, 0, DMR_KEYUP_MIN_US));
	CHECK(is_point(1, PA_KEYUP_PT_PA, 1000, edge.int_ahead_us - 800u) && is(2, EV_OFF, 0xFFFF));
	dmr_keyup_disarm();
	dmr_keyup_init(&ops);
	CHECK(ctx_errors == 0 && timer_bad == 0);
}

int main(void)
{
	// The board's part: its constants and its limits, once (the earliest point is the loop's, after the interrupt)
	pa_keyup_init(&fixed800, INT_AHEAD - LOOP_LEAD - DMR_KEYUP_MIN_US, 4095);
	dmr_keyup_init(&ops);
	CHECK(timer_inits == 1 && timer_fn != NULL);
	test_unarmed();
	test_transmission();
	test_boost();
	test_point_time();
	test_rule();
	test_plan_order();
	test_refused_point();
	test_second_interrupt();
	test_fallback();
	test_disarm();
	test_refused_plan();
	test_not_written();
	test_direct();
	test_tx_done();
	test_rearm();
	test_ops();
	CHECK(unwaited_points == 0);
	if (failures == 0) {
		printf("dmrKeyupTest: all tests passed\n");
	}
	return failures != 0;
}
