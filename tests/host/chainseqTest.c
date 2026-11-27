// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The RF chain sequencer (drivers/rf/chainseq) against a fake board. Every line, DAC, synthesizer and IF write and
// every delay is logged, and a watcher checks the driver's three rules at each write, whatever the test: the DAC at
// or under the floor and the loop open whenever the transmit rail changes, the VCO select line moving only with the
// transmit rail off and the loop open (and by direction, the transmit rail off whenever the receive VCO comes on),
// no tuning value on the DAC while the loop is closed. The fake also has a board's interlock on
// the shared DAC (a transmit line only with the DAC at or under the floor, a value over the floor only with both
// transmit lines on or both off): no sequence may run into it. Nothing here drives hardware: the "PA" is a log.
//
// The tests: each sequence's order and waits, the lock indication (three reads, the timeout, none at all), the
// refusals with nothing written, the burst pieces, an unlocked synthesizer at a burst's points, a refused write at
// every position of every three-call sequence (once, and from there on; no drive reaches a closed loop in a call
// after its refused write), and the rules and the state under every sequence of four calls and a long random walk.
// All of it with a floor of 0 and with one above 0, and without the burst pieces as chainseqTestFm (CONFIG_DMR=0).
// Without a vco_line op every expected log is the one the line by direction gives; with one (a plan whose receive and
// transmit switch points differ) the same tests run with the plan's levels, plus its own cases.
#include <stdio.h>
#include <string.h>
#include "rf/chainseq/chainseq.h"
#include "radio/apc_ramp.h"

static int failures;

// A broken rule shows at thousands of sequences: the first reports are enough
#define CHECK(cond) do { \
	if (!(cond)) { \
		if (failures < 40) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
		failures++; \
	} \
} while (0)

// --- The fake board ---

// Its frequency plan: receive 400-480 MHz with the LO an IF below, transmit calibrated up to 470 MHz with the VCO on
// the carrier. Tuning values are all at or over TUNE_BASE and drives all under it, so the watcher can tell them apart
#define RX_LO_HZ	400000000u
#define RX_HI_HZ	480000000u
#define TX_HI_HZ	470000000u
#define IF_HZ		45000000u
#define TUNE_BASE	3000u
#define RXVCO(hz)	((hz) - IF_HZ)
#define TUNE(hz)	((uint16_t)(TUNE_BASE + ((hz) - RX_LO_HZ) / 1000000u))
#define F_A		440000000u
#define F_B		446500000u
#define F_NOCAL		475000000u	// receives, no transmit calibration
#define F_C		460000000u	// over the plan's receive switch
// The plan's VCO rule (ops_plan): the line on (the lower VCO) below each switch. F_A is under both, F_B between them
// (on to receive, off to transmit), F_C over both
#define PLAN_RX_SW_HZ	450000000u
#define PLAN_TX_SW_HZ	443000000u
#define F_OUT		500000000u	// out of band
#define DRIVE_LOW	900u
#define DRIVE_HIGH	1800u
#define DRIVE_BOOST	2400u
#define SETTLE_US	2000u
#define LOCK_US		5000u
#define LEAD_US		500u

enum { EV_LINE, EV_DAC, EV_SYNTH, EV_IF, EV_DELAY };

typedef struct {
	uint8_t kind, a;	// a: the line
	uint32_t v;		// level / value / Hz / wide / microseconds
} ev_t;

#define EV_MAX	200
static ev_t ev[EV_MAX];
static int nev;

static struct {
	bool level[CHAINSEQ_LINE_COUNT];
	uint16_t dac;
	uint32_t synth_hz;
	bool if_wide;
	int if_setups;
	// The lock line: pat's reads first ('1' locked), then lock. run: reads in a row that said locked since the last
	// synthesizer write
	bool lock;
	const char *pat;
	int lock_reads, lock_run;
	bool rx_gone;		// rx_plan refuses everything (a plan that changed under a transmission)
	// Calibration: tx_plan's answer
	bool cal;
	uint16_t drive[2], floor;
	int plan_power;
	uint32_t delay_total;
	int calls;		// every write and delay the driver asked for, refused ones too
	// The plan's VCO rule: how often it was asked, and the last question
	int plan_asks;
	uint32_t plan_hz;
	bool plan_tx;
	// The watcher
	bool pa_allowed;	// the PA may come on air
	bool no_wait;		// an interrupt level call: no delay
	int rises;		// times the PA came on air
	int trips;		// writes the interlock refused
	// Refused writes. A board's interlock never refuses a line going off or a DAC of 0, so only the others count:
	// the write number fail_at once, or every one from fail_from on; all: every write, the harmless ones too
	int nwrites, fail_at, fail_from;
	bool fail_hit, fail_all;
	bool hit_call;		// a write was refused in the call that is running (begin_call)
} f;

static chainseq_t q;
static uint16_t g_floor;
static const chainseq_ops_t *g_ops;
static bool g_log = true;

static bool has_lock(void)
{
	return g_ops->locked != NULL;
}

static bool has_plan(void)
{
	return g_ops->vco_line != NULL;
}

// The VCO line's level the driver must give a channel at hz: the plan's, or by direction
static bool want_vco(uint32_t hz, bool tx)
{
	if (!has_plan()) {
		return !tx;
	}
	return hz < (tx ? PLAN_TX_SW_HZ : PLAN_RX_SW_HZ);
}

static void log_ev(uint8_t kind, uint8_t a, uint32_t v)
{
	if (!g_log) {
		return;
	}
	if (nev < EV_MAX) {
		ev[nev] = (ev_t){ kind, a, v };
	}
	nev++;
}

static bool on_air(void)
{
	return f.level[CHAINSEQ_LINE_APC_LOOP] && f.level[CHAINSEQ_LINE_TX_RAIL] && f.dac > f.floor;
}

static bool refused(void)
{
	int n = f.nwrites++;
	if (n == f.fail_at || (f.fail_from >= 0 && n >= f.fail_from)) {
		f.fail_hit = true;
		f.hit_call = true;
		return true;
	}
	return false;
}

static bool f_line(void *ctx, uint8_t line, bool on)
{
	CHECK(ctx == &f);
	CHECK(line < CHAINSEQ_LINE_COUNT);
	f.calls++;
	if (f.fail_all || (on && refused())) {
		return false;
	}
	bool tx_line = line == CHAINSEQ_LINE_TX_RAIL || line == CHAINSEQ_LINE_APC_LOOP;
	if (line == CHAINSEQ_LINE_TX_RAIL && f.level[line] != on) {
		// Rule 1
		CHECK(f.dac <= f.floor);
		CHECK(!f.level[CHAINSEQ_LINE_APC_LOOP]);
	}
	if (line == CHAINSEQ_LINE_VCO_RX && f.level[line] != on) {
		CHECK(!f.level[CHAINSEQ_LINE_TX_RAIL] && !f.level[CHAINSEQ_LINE_APC_LOOP]);	// rule 2
	}
	if (on && line == CHAINSEQ_LINE_VCO_RX && !has_plan()) {
		CHECK(!f.level[CHAINSEQ_LINE_TX_RAIL]);	// rule 2 by direction
	}
	if (on && line == CHAINSEQ_LINE_TX_RAIL) {
		// Rule 2, from the other side: by direction the transmit VCO, with a plan its level for the carrier
		CHECK(f.level[CHAINSEQ_LINE_VCO_RX] == want_vco(f.synth_hz, true));
		CHECK(!f.level[CHAINSEQ_LINE_RX_RAIL]);
	}
	if (on && line == CHAINSEQ_LINE_APC_LOOP) {
		// Rule 3, and the order of a key-up: the loop closes on the floor, with the transmit rail up
		CHECK(f.dac <= f.floor);
		CHECK(f.level[CHAINSEQ_LINE_TX_RAIL]);
	}
	if (on && line == CHAINSEQ_LINE_RX_RAIL) {
		CHECK(!f.level[CHAINSEQ_LINE_TX_RAIL] && !f.level[CHAINSEQ_LINE_APC_LOOP]);
	}
	if (on && tx_line && f.dac > f.floor) {
		f.trips++;	// the interlock
		return false;
	}
	f.level[line] = on;
	log_ev(EV_LINE, line, on);
	return true;
}

static bool f_dac(void *ctx, uint16_t v)
{
	CHECK(ctx == &f);
	f.calls++;
	if (f.fail_all || (v != 0u && refused())) {
		return false;
	}
	bool loop = f.level[CHAINSEQ_LINE_APC_LOOP], rail = f.level[CHAINSEQ_LINE_TX_RAIL];
	if (loop) {
		CHECK(v < TUNE_BASE);	// rule 3
		// After a refused write the call is on its way to safe: no drive follows it onto a closed loop
		CHECK(!f.hit_call || v <= f.floor);
	}
	if (v > f.floor && loop != rail) {
		f.trips++;	// the interlock
		return false;
	}
	if (loop && rail && v > f.floor && f.dac <= f.floor) {
		// The PA comes on air: only where a test expects it, on a calibrated carrier from its own VCO, with the
		// receiver off, and with the lock read since the synthesizer's last words
		f.rises++;
		CHECK(f.pa_allowed);
		CHECK(f.cal && f.synth_hz >= RX_LO_HZ && f.synth_hz <= TX_HI_HZ);
		CHECK(f.level[CHAINSEQ_LINE_VCO_RX] == want_vco(f.synth_hz, true) && !f.level[CHAINSEQ_LINE_RX_RAIL]);
		CHECK(f.level[CHAINSEQ_LINE_SYNTH_RAIL]);
		CHECK(!has_lock() || f.lock_run >= (int)CHAINSEQ_LOCK_READS);
	}
	f.dac = v;
	log_ev(EV_DAC, 0, v);
	return true;
}

static bool f_synth(void *ctx, uint32_t vco_hz)
{
	CHECK(ctx == &f);
	f.calls++;
	if (f.fail_all || refused()) {
		return false;
	}
	CHECK(!on_air());	// no carrier is moved under a keyed PA
	f.synth_hz = vco_hz;
	f.lock_run = 0;
	log_ev(EV_SYNTH, 0, vco_hz);
	return true;
}

static bool f_locked(void *ctx)
{
	CHECK(ctx == &f);
	f.lock_reads++;
	bool v = f.pat != NULL && *f.pat != '\0' ? *f.pat++ == '1' : f.lock;
	f.lock_run = v ? f.lock_run + 1 : 0;
	return v;
}

static bool f_if_setup(void *ctx, bool wide)
{
	CHECK(ctx == &f);
	f.calls++;
	if (f.fail_all || refused()) {
		return false;
	}
	// The IF chip is set up with its supplies on and nothing transmitting
	CHECK(f.level[CHAINSEQ_LINE_RX_RAIL] && f.level[CHAINSEQ_LINE_IF_RUN] && f.level[CHAINSEQ_LINE_SYNTH_RAIL]);
	CHECK(!f.level[CHAINSEQ_LINE_TX_RAIL] && !f.level[CHAINSEQ_LINE_APC_LOOP]);
	f.if_wide = wide;
	f.if_setups++;
	log_ev(EV_IF, 0, wide);
	return true;
}

static void f_delay(void *ctx, uint32_t us)
{
	CHECK(ctx == &f);
	CHECK(!f.no_wait);
	CHECK(us != 0u);
	f.calls++;
	f.delay_total += us;
	log_ev(EV_DELAY, 0, us);
}

static bool f_rx_plan(void *ctx, uint32_t hz, uint32_t *vco_hz, uint16_t *tune_dac)
{
	CHECK(ctx == &f);
	if (f.rx_gone || hz < RX_LO_HZ || hz > RX_HI_HZ) {
		return false;
	}
	*vco_hz = RXVCO(hz);
	*tune_dac = TUNE(hz);
	return true;
}

static bool f_tx_plan(void *ctx, uint32_t hz, uint8_t power, uint32_t *vco_hz, uint16_t *drive, uint16_t *floor)
{
	CHECK(ctx == &f);
	f.plan_power = power;
	if (!f.cal || hz < RX_LO_HZ || hz > TX_HI_HZ) {
		return false;
	}
	*vco_hz = hz;
	*drive = f.drive[power != 0u];
	*floor = f.floor;
	return true;
}

// The plan's VCO rule. Pure: it writes nothing and waits for nothing (the burst pieces ask it at interrupt level)
static bool f_vco_line(void *ctx, uint32_t hz, bool tx)
{
	CHECK(ctx == &f);
	f.plan_asks++;
	f.plan_hz = hz;
	f.plan_tx = tx;
	return want_vco(hz, tx);
}

static const chainseq_ops_t ops_lock = {
	.line = f_line, .synth = f_synth, .locked = f_locked, .if_setup = f_if_setup, .dac = f_dac, .delay_us = f_delay,
	.rx_plan = f_rx_plan, .tx_plan = f_tx_plan,
	.rail_settle_us = SETTLE_US, .lock_timeout_us = LOCK_US, .tx_rail_lead_us = LEAD_US, .ctx = &f,
};

// A board without a lock indication
static const chainseq_ops_t ops_nolock = {
	.line = f_line, .synth = f_synth, .locked = NULL, .if_setup = f_if_setup, .dac = f_dac, .delay_us = f_delay,
	.rx_plan = f_rx_plan, .tx_plan = f_tx_plan,
	.rail_settle_us = SETTLE_US, .lock_timeout_us = LOCK_US, .tx_rail_lead_us = LEAD_US, .ctx = &f,
};

// A board whose VCOs are chosen by frequency, with a lock indication
static const chainseq_ops_t ops_plan = {
	.line = f_line, .synth = f_synth, .locked = f_locked, .if_setup = f_if_setup, .dac = f_dac, .delay_us = f_delay,
	.rx_plan = f_rx_plan, .tx_plan = f_tx_plan, .vco_line = f_vco_line,
	.rail_settle_us = SETTLE_US, .lock_timeout_us = LOCK_US, .tx_rail_lead_us = LEAD_US, .ctx = &f,
};

static void clear_log(void)
{
	nev = 0;
}

// Before a call of the driver: the refusals the watcher holds against it are this call's
static void begin_call(void)
{
	f.hit_call = false;
}

// A board after its reset, and the sequencer started on it
static void fresh(void)
{
	memset(&f, 0, sizeof f);
	f.lock = true;
	f.cal = true;
	f.drive[0] = DRIVE_LOW;
	f.drive[1] = DRIVE_HIGH;
	f.floor = g_floor;
	f.pa_allowed = true;
	f.fail_at = -1;
	f.fail_from = -1;
	chainseq_init(&q, g_ops);
	clear_log();
}

// --- The expected log ---

static ev_t want[EV_MAX];
static int nwant;

static void w(uint8_t kind, uint8_t a, uint32_t v)
{
	if (nwant < EV_MAX) {
		want[nwant] = (ev_t){ kind, a, v };
	}
	nwant++;
}

#define WL(line, on)	w(EV_LINE, CHAINSEQ_LINE_##line, (on))
#define WD(v)		w(EV_DAC, 0, (v))
#define WS(hz)		w(EV_SYNTH, 0, (hz))
#define WI(wide)	w(EV_IF, 0, (wide))
#define WW(us)		w(EV_DELAY, 0, (us))

static const char *const kind_name[] = { "line", "dac", "synth", "if", "delay" };

// The log is exactly what was expected; both start over
static void match(int line)
{
	bool ok = nev == nwant && nev <= EV_MAX;
	for (int i = 0; ok && i < nev; i++) {
		ok = ev[i].kind == want[i].kind && ev[i].a == want[i].a && ev[i].v == want[i].v;
	}
	if (!ok) {
		printf("FAIL %s:%d: the log differs (floor %u)\n", __FILE__, line, g_floor);
		for (int i = 0; i < nev || i < nwant; i++) {
			printf("  %2d  ", i);
			if (i < nev && i < EV_MAX) {
				printf("got %-5s %u %-10lu", kind_name[ev[i].kind], ev[i].a, (unsigned long)ev[i].v);
			} else {
				printf("got %-18s", "-");
			}
			if (i < nwant && i < EV_MAX) {
				printf("  want %-5s %u %lu", kind_name[want[i].kind], want[i].a, (unsigned long)want[i].v);
			}
			printf("\n");
		}
		failures++;
	}
	nev = 0;
	nwant = 0;
}

#define MATCH()	match(__LINE__)

static void w_safe(void)
{
	WD(0);
	WL(APC_LOOP, 0);
	WL(TX_RAIL, 0);
	WL(VCO_RX, 1);
}

// The PA off in order: from a closed loop through the floor
static void w_pa_off(bool closed)
{
	uint16_t rest = closed ? g_floor : 0u;
	WD(rest);
	WL(APC_LOOP, 0);
	WL(TX_RAIL, 0);
	if (rest != 0u) {
		WD(0);
	}
}

// The wait for a lock that is there at once: three reads, a poll time between them; without the line the fixed wait
static void w_lock(void)
{
	if (has_lock()) {
		WW(CHAINSEQ_LOCK_POLL_US);
		WW(CHAINSEQ_LOCK_POLL_US);
	} else {
		WW(LOCK_US);
	}
}

static void w_rx(uint32_t hz, bool wide, bool cold)
{
	w_safe();
	if (!want_vco(hz, false)) {
		WL(VCO_RX, 0);	// a plan that wants the other level than the parked one
	}
	WL(SYNTH_RAIL, 1);
	if (cold) {
		WW(SETTLE_US);
	}
	WS(RXVCO(hz));
	w_lock();
	WL(RX_RAIL, 1);
	WL(IF_RUN, 1);
	WW(SETTLE_US);
	WI(wide);
	WD(TUNE(hz));
}

static void w_ramp(uint16_t drive, bool up)
{
	uint16_t span = (uint16_t)(drive - g_floor);
	for (uint16_t i = 1; i <= CHAINSEQ_RAMP_STEPS; i++) {
		uint16_t step = up ? i : (uint16_t)(CHAINSEQ_RAMP_STEPS - i);
		WD((uint32_t)g_floor + apc_ramp_level(span, step, CHAINSEQ_RAMP_STEPS));
		if (i < CHAINSEQ_RAMP_STEPS) {
			WW(CHAINSEQ_RAMP_STEP_US);
		}
	}
}

static void w_tx(uint32_t hz, uint16_t drive, bool cold)
{
	w_pa_off(false);
	WL(RX_RAIL, 0);
	WL(SYNTH_RAIL, 1);
	if (cold) {
		WW(SETTLE_US);
	}
	WL(VCO_RX, want_vco(hz, true));
	WS(hz);
	w_lock();
	WL(TX_RAIL, 1);
	WW(LEAD_US);
	if (g_floor != 0u) {
		WD(g_floor);
	}
	WL(APC_LOOP, 1);
	w_ramp(drive, true);
}

// --- The state against the lines ---

// What each state promises, checked wherever no write was refused
static void consistent(void)
{
	const bool *lv = f.level;
	bool loop = lv[CHAINSEQ_LINE_APC_LOOP], txr = lv[CHAINSEQ_LINE_TX_RAIL], vco_rx = lv[CHAINSEQ_LINE_VCO_RX];
	bool rxr = lv[CHAINSEQ_LINE_RX_RAIL], syn = lv[CHAINSEQ_LINE_SYNTH_RAIL];
	// The line as the state's frequency wants it (by direction: on to receive, off to transmit); parked when idle
	bool vco_r = vco_rx == want_vco(q.rx_hz, false), vco_t = vco_rx == want_vco(q.tx_hz, true);
	bool ok = false;
	switch (q.state) {
	case CHAINSEQ_ST_OFF:
	case CHAINSEQ_ST_IDLE:
		ok = !loop && !txr && vco_rx && f.dac == 0u;
		break;
	case CHAINSEQ_ST_RX:
		ok = !loop && !txr && vco_r && rxr && syn && lv[CHAINSEQ_LINE_IF_RUN] && f.dac == TUNE(q.rx_hz) &&
		     f.synth_hz == RXVCO(q.rx_hz) && f.if_wide == q.wide;
		break;
	case CHAINSEQ_ST_SLEEP:
		ok = !loop && !txr && vco_r && !rxr && syn && f.dac == TUNE(q.rx_hz);
		break;
	case CHAINSEQ_ST_TX:
		ok = loop && txr && vco_t && !rxr && syn && f.dac == q.drive && q.drive >= q.floor &&
		     f.synth_hz == q.tx_hz;
		break;
	case CHAINSEQ_ST_BURST:
		ok = !loop && vco_t && !rxr && syn && f.dac == 0u && f.synth_hz == q.tx_hz;
		break;
	case CHAINSEQ_ST_BURST_LOOP:
		ok = loop && txr && vco_t && !rxr && syn && f.dac == q.floor && f.synth_hz == q.tx_hz;
		break;
	case CHAINSEQ_ST_BURST_ON:
		ok = loop && txr && vco_t && !rxr && syn && f.dac == q.drive && q.drive > q.floor &&
		     f.synth_hz == q.tx_hz;
		break;
	default:
		break;
	}
	if (!ok && failures < 40) {
		printf("  state %u: loop %d txr %d vco_rx %d rxr %d synth_rail %d dac %u synth %lu\n", q.state, loop, txr,
		       vco_rx, rxr, syn, f.dac, (unsigned long)f.synth_hz);
	}
	CHECK(ok);
	CHECK(!on_air() || q.state == CHAINSEQ_ST_TX || q.state == CHAINSEQ_ST_BURST_ON);
	CHECK(q.floor == 0u || q.floor == g_floor);
}

// The PA is off and the DAC is no power request: where every failed path has to end
static void pa_safe(void)
{
	bool receiving = q.state == CHAINSEQ_ST_RX || q.state == CHAINSEQ_ST_SLEEP;
	CHECK(!f.level[CHAINSEQ_LINE_APC_LOOP]);
	CHECK(!f.level[CHAINSEQ_LINE_TX_RAIL]);
	CHECK(f.dac == 0u || (receiving && f.dac == TUNE(q.rx_hz)));
	CHECK(receiving || q.state == CHAINSEQ_ST_OFF || q.state == CHAINSEQ_ST_IDLE);
}

static bool reach(uint8_t state);

// --- Start, receive ---

static void test_init(void)
{
	// The lines as a power-up may leave them: everything on, a value on the DAC
	memset(&f, 0, sizeof f);
	f.floor = g_floor;
	f.fail_at = -1;
	f.fail_from = -1;
	for (int i = 0; i < CHAINSEQ_LINE_COUNT; i++) {
		f.level[i] = true;
	}
	f.dac = 2000;
	clear_log();
	memset(&q, 0xA5, sizeof q);
	chainseq_init(&q, g_ops);
	w_safe();
	MATCH();
	CHECK(q.ops == g_ops && q.state == CHAINSEQ_ST_OFF);
	CHECK(q.rx_hz == 0u && q.tx_hz == 0u && q.drive == 0u && q.floor == 0u);
	CHECK(q.refused == 0u && q.unlocked == 0u && q.errors == 0u && q.last_err == CHAINSEQ_OK);
	CHECK(f.delay_total == 0u && f.lock_reads == 0);
	pa_safe();
}

static void test_rx(void)
{
	fresh();
	CHECK(chainseq_rx(&q, F_A, true) == CHAINSEQ_OK);
	w_rx(F_A, true, true);	// the first set-up waits for the synthesizer rail
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_RX && q.rx_hz == F_A && q.wide);
	CHECK(!has_lock() || f.lock_reads == 3);
	consistent();

	// A retune is the whole set-up again, without the rail's wait
	CHECK(chainseq_rx(&q, F_B, false) == CHAINSEQ_OK);
	w_rx(F_B, false, false);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_RX && q.rx_hz == F_B && !q.wide && f.if_setups == 2);
	consistent();

	// The same channel again: still the whole set-up
	CHECK(chainseq_rx(&q, F_B, false) == CHAINSEQ_OK);
	w_rx(F_B, false, false);
	MATCH();

	// Out of band: nothing written, the receiver stays where it was
	CHECK(chainseq_rx(&q, F_OUT, true) == CHAINSEQ_REFUSED_BAND);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_RX && q.rx_hz == F_B && !q.wide);
	CHECK(q.refused == 1u && q.last_err == CHAINSEQ_REFUSED_BAND && q.errors == 0u);
	consistent();
	CHECK(f.rises == 0 && f.trips == 0);
}

static void test_rx_lock(void)
{
	if (!has_lock()) {
		// The fixed wait is in every expected log (w_lock)
		return;
	}
	// Never locked: the whole timeout, counted, and receive goes on
	fresh();
	f.lock = false;
	CHECK(chainseq_rx(&q, F_A, false) == CHAINSEQ_OK);
	CHECK(q.state == CHAINSEQ_ST_RX && q.unlocked == 1u && q.last_err == CHAINSEQ_UNLOCKED && q.errors == 0u);
	CHECK(f.lock_reads == (int)(LOCK_US / CHAINSEQ_LOCK_POLL_US) + 1);
	CHECK(f.delay_total == SETTLE_US + LOCK_US + SETTLE_US);
	consistent();

	// A line that pulses: three reads in a row have to agree
	fresh();
	f.pat = "1101011";
	CHECK(chainseq_rx(&q, F_A, false) == CHAINSEQ_OK);
	CHECK(q.unlocked == 0u && f.lock_reads == 8);
	CHECK(f.delay_total == SETTLE_US + 7u * CHAINSEQ_LOCK_POLL_US + SETTLE_US);

	// Locked on the last reads the timeout allows
	fresh();
	static char late[64];
	int n = (int)(LOCK_US / CHAINSEQ_LOCK_POLL_US) + 1 - (int)CHAINSEQ_LOCK_READS;
	memset(late, '0', (size_t)n);
	late[n] = '\0';
	f.pat = late;
	CHECK(chainseq_rx(&q, F_A, false) == CHAINSEQ_OK);
	CHECK(q.unlocked == 0u && f.lock_reads == n + 3);
}

static void test_sleep(void)
{
	fresh();
	chainseq_sleep(&q, true);	// nothing to put to sleep yet
	chainseq_sleep(&q, false);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_OFF);

	(void)chainseq_rx(&q, F_A, true);
	clear_log();
	chainseq_sleep(&q, false);	// not asleep
	MATCH();
	chainseq_sleep(&q, true);
	WL(RX_RAIL, 0);		// the synthesizer rail and the IF chip stay on
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_SLEEP);
	CHECK(f.level[CHAINSEQ_LINE_SYNTH_RAIL] && f.level[CHAINSEQ_LINE_IF_RUN]);
	consistent();
	chainseq_sleep(&q, true);
	MATCH();
	chainseq_sleep(&q, false);
	w_rx(F_A, true, false);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_RX);
	consistent();

	// Never under a transmission
	CHECK(chainseq_tx(&q, F_A, 0) == CHAINSEQ_OK);
	clear_log();
	chainseq_sleep(&q, true);
	chainseq_sleep(&q, false);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_TX);

	// A board that refuses the rail's write: not asleep. Counted, the four safe writes tried, and the next receive
	// set-up starts from idle
	CHECK(reach(CHAINSEQ_ST_RX));
	f.fail_all = true;
	int calls = f.calls;
	chainseq_sleep(&q, true);
	CHECK(f.calls == calls + 5);
	CHECK(q.state == CHAINSEQ_ST_IDLE && q.errors >= 1u && q.last_err == CHAINSEQ_BUS);
	f.fail_all = false;
	chainseq_sleep(&q, false);	// not asleep: nothing to wake
	CHECK(q.state == CHAINSEQ_ST_IDLE);
	CHECK(chainseq_rx(&q, F_A, false) == CHAINSEQ_OK);
	consistent();
	clear_log();
}

// --- FM transmit ---

static void test_tx(void)
{
	fresh();
	(void)chainseq_rx(&q, F_A, false);
	clear_log();
	CHECK(chainseq_tx(&q, F_B, 1) == CHAINSEQ_OK);
	w_tx(F_B, DRIVE_HIGH, false);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_TX && q.tx_hz == F_B && q.drive == DRIVE_HIGH && q.floor == g_floor);
	CHECK(f.plan_power == 1 && f.dac == DRIVE_HIGH && f.rises == 1);
	consistent();

	// The end: the ramp down to the floor, the loop, the rail, then the whole receive set-up
	chainseq_tx_off(&q);
	w_ramp(DRIVE_HIGH, false);
	w_pa_off(true);
	w_rx(F_A, false, false);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_RX && q.rx_hz == F_A);
	consistent();

	// Not transmitting: nothing to end
	chainseq_tx_off(&q);
	MATCH();
	chainseq_sleep(&q, true);
	clear_log();
	chainseq_tx_off(&q);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_SLEEP);

	// Before any receive set-up: the rail's wait, and the end has nowhere to go back to
	fresh();
	chainseq_tx_off(&q);
	MATCH();
	CHECK(chainseq_tx(&q, F_A, 0) == CHAINSEQ_OK);
	w_tx(F_A, DRIVE_LOW, true);
	MATCH();
	CHECK(f.plan_power == 0);
	consistent();
	chainseq_tx_off(&q);
	w_ramp(DRIVE_LOW, false);
	w_pa_off(true);
	w_safe();
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_IDLE);
	consistent();
	CHECK(f.trips == 0 && q.errors == 0u);

	// The same when the plan no longer has the receive frequency: the transmission ends, the chain is safe
	fresh();
	(void)chainseq_rx(&q, F_A, false);
	(void)chainseq_tx(&q, F_A, 1);
	clear_log();
	f.rx_gone = true;
	chainseq_tx_off(&q);
	w_ramp(DRIVE_HIGH, false);
	w_pa_off(true);
	w_safe();
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_IDLE && q.refused == 1u && q.last_err == CHAINSEQ_REFUSED_BAND);
	consistent();
}

// A write refused on the way down: the chain is safe at once and the receiver comes back. The ramp does not start
// over from the drive, with the loop still closed or open. n: the refused write, counted from the key-down's first
static void tx_off_refused(int n)
{
	fresh();
	(void)chainseq_rx(&q, F_A, false);
	(void)chainseq_tx(&q, F_A, 1);
	clear_log();
	f.fail_at = f.nwrites + n;
	begin_call();
	chainseq_tx_off(&q);
	// The ramp's steps up to the refused one (each with its wait), or all of it when the floor's write after it is
	// the one refused
	uint16_t span = (uint16_t)(DRIVE_HIGH - g_floor);
	for (uint16_t i = 1; i <= CHAINSEQ_RAMP_STEPS && i <= n; i++) {
		WD((uint32_t)g_floor + apc_ramp_level(span, (uint16_t)(CHAINSEQ_RAMP_STEPS - i), CHAINSEQ_RAMP_STEPS));
		if (i < CHAINSEQ_RAMP_STEPS) {
			WW(CHAINSEQ_RAMP_STEP_US);
		}
	}
	w_safe();
	w_rx(F_A, false, false);
	MATCH();
	CHECK(f.fail_hit && q.errors == 1u && q.last_err == CHAINSEQ_BUS);
	CHECK(q.state == CHAINSEQ_ST_RX && f.rises == 1 && f.trips == 0);
	consistent();
}

static void test_tx_off_refused(void)
{
	tx_off_refused(0);	// the first step
	tx_off_refused(3);	// one in the middle
	if (g_floor != 0u) {
		// The ramp ran to the floor, and the floor's write before the loop opens is refused: the loop is still
		// closed, and a second ramp would key the PA again
		tx_off_refused((int)CHAINSEQ_RAMP_STEPS);
	}
}

// The ramp has the shared shape: from the floor to the drive, never down on the way up
static void test_tx_ramp(void)
{
	fresh();
	(void)chainseq_rx(&q, F_A, false);
	clear_log();
	(void)chainseq_tx(&q, F_A, 1);
	uint32_t last = g_floor;
	int steps = 0;
	bool closed = false;
	for (int i = 0; i < nev; i++) {
		if (ev[i].kind == EV_LINE && ev[i].a == CHAINSEQ_LINE_APC_LOOP) {
			closed = ev[i].v != 0u;
		} else if (ev[i].kind == EV_DAC && closed) {
			CHECK(ev[i].v >= last && ev[i].v >= g_floor && ev[i].v <= DRIVE_HIGH);
			last = ev[i].v;
			steps++;
		}
	}
	CHECK(steps == (int)CHAINSEQ_RAMP_STEPS && last == DRIVE_HIGH);
	clear_log();
}

static void test_tx_refused(void)
{
	fresh();
	(void)chainseq_rx(&q, F_A, false);
	clear_log();
	f.pa_allowed = false;
	int calls = f.calls;

	CHECK(chainseq_tx(&q, F_NOCAL, 1) == CHAINSEQ_REFUSED_CAL);	// no calibration at the frequency
	f.cal = false;
	CHECK(chainseq_tx(&q, F_A, 1) == CHAINSEQ_REFUSED_CAL);		// none at all
	f.cal = true;
	f.drive[1] = 0;
	CHECK(chainseq_tx(&q, F_A, 1) == CHAINSEQ_REFUSED_CAL);		// a table nobody filled
	CHECK(q.refused == 3u);
	if (g_floor != 0u) {
		f.drive[1] = (uint16_t)(g_floor - 1u);
		CHECK(chainseq_tx(&q, F_A, 1) == CHAINSEQ_REFUSED_CAL);	// a drive under its own floor
		CHECK(q.refused == 4u);
	}
	// Nothing was written, no delay either: the receiver never noticed
	MATCH();
	CHECK(f.calls == calls);
	CHECK(q.state == CHAINSEQ_ST_RX && q.last_err == CHAINSEQ_REFUSED_CAL && q.errors == 0u && q.tx_hz == 0u);
	consistent();

	// A drive at the floor is a calibration that asks for no output: allowed, and nothing comes on air
	if (g_floor != 0u) {
		f.drive[1] = g_floor;
		CHECK(chainseq_tx(&q, F_A, 1) == CHAINSEQ_OK);
		CHECK(q.state == CHAINSEQ_ST_TX && f.rises == 0);
		consistent();
		chainseq_tx_off(&q);
	}
	clear_log();
}

static void test_tx_unlocked(void)
{
	if (!has_lock()) {
		return;
	}
	// The transmit VCO does not lock within the timeout, the receive LO does afterwards
	fresh();
	(void)chainseq_rx(&q, F_A, true);
	clear_log();
	static char never[64];
	int n = (int)(LOCK_US / CHAINSEQ_LOCK_POLL_US) + 1;
	memset(never, '0', (size_t)n);
	never[n] = '\0';
	f.pat = never;
	f.pa_allowed = false;
	CHECK(chainseq_tx(&q, F_B, 1) == CHAINSEQ_UNLOCKED);
	// Up to the lock wait, then safe, then the receiver again. The transmit rail never came on
	w_pa_off(false);
	WL(RX_RAIL, 0);
	WL(SYNTH_RAIL, 1);
	WL(VCO_RX, want_vco(F_B, true));
	WS(F_B);
	for (int i = 0; i < n - 1; i++) {
		WW(CHAINSEQ_LOCK_POLL_US);
	}
	w_safe();
	w_rx(F_A, true, false);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_RX && q.unlocked == 1u && q.last_err == CHAINSEQ_UNLOCKED && q.errors == 0u);
	CHECK(f.rises == 0);
	consistent();

	// Unlocked for good: the way back to receive goes on unlocked, and the failure reported is the transmission's
	fresh();
	(void)chainseq_rx(&q, F_A, true);
	f.lock = false;
	f.pa_allowed = false;
	CHECK(chainseq_tx(&q, F_B, 1) == CHAINSEQ_UNLOCKED);
	CHECK(q.state == CHAINSEQ_ST_RX && q.unlocked == 2u && q.last_err == CHAINSEQ_UNLOCKED);
	consistent();

	// With no receive set-up to go back to: safe, and that is all
	fresh();
	f.lock = false;
	f.pa_allowed = false;
	CHECK(chainseq_tx(&q, F_B, 1) == CHAINSEQ_UNLOCKED);
	CHECK(q.state == CHAINSEQ_ST_IDLE && q.unlocked == 1u);
	consistent();
	pa_safe();
	clear_log();
}

static void test_tx_retune(void)
{
	// tx() during a transmission: the PA comes down in order before the carrier moves
	fresh();
	(void)chainseq_rx(&q, F_A, false);
	(void)chainseq_tx(&q, F_A, 1);
	clear_log();
	CHECK(chainseq_tx(&q, F_B, 0) == CHAINSEQ_OK);
	w_ramp(DRIVE_HIGH, false);
	w_pa_off(true);
	WL(RX_RAIL, 0);
	WL(SYNTH_RAIL, 1);
	WL(VCO_RX, want_vco(F_B, true));
	WS(F_B);
	w_lock();
	WL(TX_RAIL, 1);
	WW(LEAD_US);
	if (g_floor != 0u) {
		WD(g_floor);
	}
	WL(APC_LOOP, 1);
	w_ramp(DRIVE_LOW, true);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_TX && q.tx_hz == F_B && q.drive == DRIVE_LOW && f.rises == 2);
	consistent();

	// rx() during a transmission: the same way down, then the receive set-up
	CHECK(chainseq_rx(&q, F_B, true) == CHAINSEQ_OK);
	w_ramp(DRIVE_LOW, false);
	w_pa_off(true);
	w_rx(F_B, true, false);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_RX);
	consistent();

	// An rx() out of band is refused before anything moves: the transmission goes on
	(void)chainseq_tx(&q, F_A, 1);
	clear_log();
	CHECK(chainseq_rx(&q, F_OUT, true) == CHAINSEQ_REFUSED_BAND);
	// So does a tx() without calibration
	CHECK(chainseq_tx(&q, F_NOCAL, 1) == CHAINSEQ_REFUSED_CAL);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_TX && q.tx_hz == F_A);
	consistent();
	chainseq_tx_off(&q);
	CHECK(q.state == CHAINSEQ_ST_RX && q.rx_hz == F_B);
	CHECK(f.trips == 0 && q.errors == 0u);
	clear_log();
}

static void test_drive_now(void)
{
	fresh();
	CHECK(!chainseq_drive_now(&q, 1200));	// only during an FM transmission
	(void)chainseq_rx(&q, F_A, false);
	clear_log();
	CHECK(!chainseq_drive_now(&q, 1200));
	MATCH();
	CHECK(f.dac == TUNE(F_A) && q.refused == 0u && q.errors == 0u);

	(void)chainseq_tx(&q, F_A, 1);
	clear_log();
	CHECK(chainseq_drive_now(&q, 1200));
	WD(1200);
	MATCH();
	CHECK(q.drive == 1200u);
	consistent();
	// 0: the PA off with the exciter on. The reference waits at the floor, never under it
	CHECK(chainseq_drive_now(&q, 0));
	WD(g_floor);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_TX && q.drive == g_floor);
	CHECK(f.level[CHAINSEQ_LINE_TX_RAIL] && f.level[CHAINSEQ_LINE_APC_LOOP]);
	consistent();
	if (g_floor != 0u) {
		CHECK(chainseq_drive_now(&q, (uint16_t)(g_floor - 1u)));
		WD(g_floor);
		MATCH();
	}
	CHECK(chainseq_drive_now(&q, 1500));
	WD(1500);
	MATCH();
	// Key-down ramps from where the drive is now
	chainseq_tx_off(&q);
	w_ramp(1500, false);
	w_pa_off(true);
	w_rx(F_A, false, false);
	MATCH();
	CHECK(f.trips == 0 && q.errors == 0u);
}

// --- Every state, and chainseq_safe from each ---

#if CONFIG_DMR
static bool point(uint8_t kind, uint16_t drive)
{
	f.no_wait = true;
	bool ok = chainseq_burst_point(&q, kind, drive);
	f.no_wait = false;
	return ok;
}

static bool burst_tune(uint32_t hz)
{
	f.no_wait = true;
	bool ok = chainseq_burst_tune(&q, hz);
	f.no_wait = false;
	return ok;
}

static void burst_off(void)
{
	f.no_wait = true;
	chainseq_burst_off(&q);
	f.no_wait = false;
}

static bool burst_rx(void)
{
	f.no_wait = true;
	bool ok = chainseq_burst_rx(&q);
	f.no_wait = false;
	return ok;
}
#endif

// The chain in each state in turn
static bool reach(uint8_t state)
{
	fresh();
	if (state == CHAINSEQ_ST_OFF) {
		return true;
	}
	(void)chainseq_rx(&q, F_A, false);
	switch (state) {
	case CHAINSEQ_ST_IDLE:
		chainseq_safe(&q);
		break;
	case CHAINSEQ_ST_SLEEP:
		chainseq_sleep(&q, true);
		break;
	case CHAINSEQ_ST_TX:
		(void)chainseq_tx(&q, F_B, 1);
		break;
#if CONFIG_DMR
	case CHAINSEQ_ST_BURST:
		(void)burst_tune(F_B);
		break;
	case CHAINSEQ_ST_BURST_LOOP:
		(void)burst_tune(F_B);
		(void)point(CHAINSEQ_PT_LOOP, 0);
		break;
	case CHAINSEQ_ST_BURST_ON:
		(void)burst_tune(F_B);
		(void)point(CHAINSEQ_PT_LOOP, 0);
		(void)point(PA_KEYUP_PT_PA, DRIVE_HIGH);
		break;
#endif
	default:
		break;
	}
	clear_log();
	return q.state == state;
}

static void test_safe(void)
{
	for (uint8_t st = CHAINSEQ_ST_OFF; st <= CHAINSEQ_ST_BURST_ON; st++) {
		if (!CONFIG_DMR && st >= CHAINSEQ_ST_BURST) {
			break;
		}
		CHECK(reach(st));
		consistent();
		// Any context: no wait, no lock read, the same four writes whatever the state
		f.no_wait = true;
		int reads = f.lock_reads;
		chainseq_safe(&q);
		f.no_wait = false;
		w_safe();
		MATCH();
		CHECK(f.lock_reads == reads);
		CHECK(q.state == (st == CHAINSEQ_ST_OFF ? CHAINSEQ_ST_OFF : CHAINSEQ_ST_IDLE));
		CHECK(q.errors == 0u && q.refused == 0u);
		consistent();
		pa_safe();
		// And the chain is usable afterwards
		CHECK(chainseq_rx(&q, F_B, true) == CHAINSEQ_OK);
		CHECK(q.state == CHAINSEQ_ST_RX);
		consistent();
	}
	clear_log();

	// A board that refuses everything: every step of the four is still tried, and it is counted
	CHECK(reach(CHAINSEQ_ST_TX));
	f.fail_all = true;
	int calls = f.calls;
	chainseq_safe(&q);
	CHECK(f.calls == calls + 4);
	CHECK(q.errors == 1u && q.last_err == CHAINSEQ_BUS && q.state == CHAINSEQ_ST_IDLE);
	clear_log();
}

// chainseq_safed: the board wrote the four lines itself, in one piece and past the ops (a fault path), and the
// sequencer follows without a write of its own
static void test_safed(void)
{
	for (uint8_t st = CHAINSEQ_ST_OFF; st <= CHAINSEQ_ST_BURST_ON; st++) {
		if (!CONFIG_DMR && st >= CHAINSEQ_ST_BURST) {
			break;
		}
		CHECK(reach(st));
		// The board's own safe write
		f.dac = 0;
		f.level[CHAINSEQ_LINE_APC_LOOP] = false;
		f.level[CHAINSEQ_LINE_TX_RAIL] = false;
		f.level[CHAINSEQ_LINE_VCO_RX] = true;
		int calls = f.calls, reads = f.lock_reads;
		chainseq_safed(&q);
		CHECK(f.calls == calls && f.lock_reads == reads && nev == 0);	// no write, no delay, no read
		CHECK(q.state == (st == CHAINSEQ_ST_OFF ? CHAINSEQ_ST_OFF : CHAINSEQ_ST_IDLE));
		CHECK(q.errors == 0u && q.refused == 0u);
		consistent();
		pa_safe();
		// Nothing of a transmission that was up goes on: no drive, no burst piece
		CHECK(!chainseq_drive_now(&q, DRIVE_HIGH));
#if CONFIG_DMR
		CHECK(!point(CHAINSEQ_PT_LOOP, 0) && !point(PA_KEYUP_PT_PA, DRIVE_HIGH));
		burst_off();
#endif
		CHECK(f.calls == calls && !on_air());
		pa_safe();
		// And the chain is usable afterwards, as after chainseq_safe
		CHECK(chainseq_rx(&q, F_B, true) == CHAINSEQ_OK);
		CHECK(q.state == CHAINSEQ_ST_RX);
		consistent();
	}
	clear_log();
}

// --- Bursts ---

#if CONFIG_DMR

static void w_loop(void)
{
	WL(TX_RAIL, 1);
	if (g_floor != 0u) {
		WD(g_floor);
	}
	WL(APC_LOOP, 1);
}

static void test_burst(void)
{
	fresh();
	(void)chainseq_rx(&q, F_A, true);
	clear_log();
	int reads = f.lock_reads;

	// The tune: the receiver off, the transmit VCO on the carrier, the transmit rail. No lock read, no wait
	CHECK(burst_tune(F_B));
	w_pa_off(false);
	WL(RX_RAIL, 0);
	WL(SYNTH_RAIL, 1);
	WL(VCO_RX, want_vco(F_B, true));
	WS(F_B);
	WL(TX_RAIL, 1);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST && q.tx_hz == F_B && q.floor == g_floor && f.lock_reads == reads);
	CHECK(f.plan_power == (int)CHAINSEQ_BURST_PLAN_POWER);
	consistent();

	// The first burst: the loop on the floor, then the PA's point with its lock check
	CHECK(point(CHAINSEQ_PT_LOOP, DRIVE_LOW));
	w_loop();
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST_LOOP && f.lock_reads == reads && f.rises == 0);
	consistent();
	CHECK(point(CHAINSEQ_PT_LOOP, DRIVE_LOW));	// again: nothing
	MATCH();
	CHECK(point(PA_KEYUP_PT_PA, DRIVE_LOW));
	WD(DRIVE_LOW);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST_ON && q.drive == DRIVE_LOW && f.rises == 1);
	CHECK(!has_lock() || f.lock_reads == reads + 3);
	consistent();
	burst_off();
	w_pa_off(true);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST);
	consistent();
	burst_off();	// the fallback drop after the timed one: the same writes, nothing new
	w_pa_off(false);
	MATCH();

	// The next burst, with a boost: the loop point brings the transmit rail back. The boost raises the reference, so
	// it asks for the lock; the PA's point asks again; the drive to the level does not
	reads = f.lock_reads;
	CHECK(point(CHAINSEQ_PT_LOOP, DRIVE_BOOST));
	w_loop();
	MATCH();
	CHECK(point(PA_KEYUP_PT_BOOST, DRIVE_BOOST));
	WD(DRIVE_BOOST);
	MATCH();
	CHECK(!has_lock() || f.lock_reads == reads + 3);
	CHECK(point(PA_KEYUP_PT_PA, DRIVE_BOOST));
	WD(DRIVE_BOOST);
	MATCH();
	CHECK(!has_lock() || f.lock_reads == reads + 6);
	CHECK(point(PA_KEYUP_PT_DRIVE, DRIVE_LOW));
	WD(DRIVE_LOW);
	MATCH();
	CHECK(!has_lock() || f.lock_reads == reads + 6);
	CHECK(q.state == CHAINSEQ_ST_BURST_ON && q.drive == DRIVE_LOW && f.rises == 2);
	consistent();
	burst_off();
	w_pa_off(true);
	MATCH();

	// Back to the receive LO: no wait, no lock read, no IF set-up
	int setups = f.if_setups;
	reads = f.lock_reads;
	CHECK(burst_rx());
	w_pa_off(false);
	WL(VCO_RX, want_vco(F_A, false));
	WS(RXVCO(F_A));
	WL(RX_RAIL, 1);
	WD(TUNE(F_A));
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_RX && f.if_setups == setups && f.lock_reads == reads);
	consistent();
	CHECK(burst_rx());	// already there
	MATCH();
	// The transmission's end finds the chain receiving and leaves it alone (gotchas 3.14)
	chainseq_tx_off(&q);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_RX && q.errors == 0u && q.refused == 0u && f.trips == 0);
}

static void test_burst_orders(void)
{
	// A PA point without a loop point closes the loop itself, on the floor, before the drive
	CHECK(reach(CHAINSEQ_ST_BURST));
	CHECK(point(PA_KEYUP_PT_PA, DRIVE_HIGH));
	w_loop();
	WD(DRIVE_HIGH);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST_ON);
	consistent();

	// A drive while the loop is open is refused and ends safe: the loop point comes first
	for (uint8_t kind = PA_KEYUP_PT_BOOST; kind <= PA_KEYUP_PT_DRIVE; kind += PA_KEYUP_PT_DRIVE - PA_KEYUP_PT_BOOST) {
		CHECK(reach(CHAINSEQ_ST_BURST));
		f.pa_allowed = false;
		CHECK(!point(kind, DRIVE_HIGH));
		w_safe();
		MATCH();
		CHECK(q.state == CHAINSEQ_ST_IDLE && q.errors == 1u && q.last_err == CHAINSEQ_BUS);
		consistent();
		// The rest of that transmission's points find no burst: nothing written
		CHECK(!point(CHAINSEQ_PT_LOOP, DRIVE_HIGH));
		CHECK(!point(PA_KEYUP_PT_PA, DRIVE_HIGH));
		burst_off();
		MATCH();
		CHECK(q.refused == 2u && q.state == CHAINSEQ_ST_IDLE);
		// The receiver comes back without waits all the same, or with the transmission's end
		CHECK(burst_rx());
		CHECK(q.state == CHAINSEQ_ST_RX);
		consistent();
		clear_log();
	}
	CHECK(reach(CHAINSEQ_ST_BURST));
	(void)point(PA_KEYUP_PT_DRIVE, DRIVE_HIGH);
	clear_log();
	chainseq_tx_off(&q);
	w_pa_off(false);
	w_rx(F_A, false, false);
	MATCH();

	// The reference never goes under the floor while the loop is closed, and at the floor nothing is on air yet
	CHECK(reach(CHAINSEQ_ST_BURST_LOOP));
	CHECK(point(PA_KEYUP_PT_DRIVE, 0));
	WD(g_floor);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST_LOOP && f.rises == 0);
	consistent();

	// A retune while a burst is up: the PA off first
	CHECK(reach(CHAINSEQ_ST_BURST_ON));
	CHECK(burst_tune(F_A));
	w_pa_off(true);
	WL(RX_RAIL, 0);
	WL(SYNTH_RAIL, 1);
	WL(VCO_RX, want_vco(F_A, true));
	WS(F_A);
	WL(TX_RAIL, 1);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST && q.tx_hz == F_A);
	consistent();

	// From sleep: the receive rail is off already
	CHECK(reach(CHAINSEQ_ST_SLEEP));
	CHECK(burst_tune(F_B));
	CHECK(q.state == CHAINSEQ_ST_BURST);
	consistent();
	clear_log();

	// The task level calls from inside a burst transmission
	CHECK(reach(CHAINSEQ_ST_BURST_ON));
	chainseq_tx_off(&q);
	w_pa_off(true);
	w_rx(F_A, false, false);
	MATCH();
	CHECK(reach(CHAINSEQ_ST_BURST_ON));
	CHECK(chainseq_rx(&q, F_B, true) == CHAINSEQ_OK);
	w_pa_off(true);
	w_rx(F_B, true, false);
	MATCH();
	CHECK(reach(CHAINSEQ_ST_BURST_ON));
	CHECK(chainseq_tx(&q, F_A, 1) == CHAINSEQ_OK);	// FM over a burst transmission: the burst ends first
	CHECK(q.state == CHAINSEQ_ST_TX);
	consistent();
	CHECK(reach(CHAINSEQ_ST_BURST_ON));
	CHECK(!chainseq_drive_now(&q, 1200));
	chainseq_sleep(&q, true);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST_ON);
	CHECK(f.trips == 0);
	clear_log();
}

static void test_burst_refused(void)
{
	// Before the first receive set-up
	fresh();
	CHECK(!burst_tune(F_A));
	CHECK(!burst_rx());
	CHECK(!point(PA_KEYUP_PT_PA, DRIVE_HIGH));
	burst_off();
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_OFF && q.refused == 3u && q.errors == 0u);

	// Tuned for bursts without ever having received (after an FM transmission from the start): there is no receive
	// set-up to go back to, and the plan is not even asked
	(void)chainseq_tx(&q, F_A, 0);
	chainseq_tx_off(&q);
	CHECK(q.state == CHAINSEQ_ST_IDLE && burst_tune(F_A));
	clear_log();
	CHECK(!burst_rx());
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST && q.refused == 4u && q.last_err == CHAINSEQ_OK);
	fresh();

	// No calibration: nothing written, the receiver stays as it was
	(void)chainseq_rx(&q, F_A, false);
	clear_log();
	f.pa_allowed = false;
	int calls = f.calls;
	CHECK(!burst_tune(F_NOCAL));
	f.cal = false;
	CHECK(!burst_tune(F_A));
	f.cal = true;
	MATCH();
	CHECK(f.calls == calls && q.state == CHAINSEQ_ST_RX && q.last_err == CHAINSEQ_REFUSED_CAL && q.refused == 2u);
	consistent();

	// Points and the burst's end while receiving (a timer that outlived its transmission): the DAC is the
	// receiver's, and it keeps its value
	CHECK(!point(CHAINSEQ_PT_LOOP, DRIVE_HIGH));
	CHECK(!point(PA_KEYUP_PT_BOOST, DRIVE_HIGH));
	CHECK(!point(PA_KEYUP_PT_PA, DRIVE_HIGH));
	CHECK(!point(PA_KEYUP_PT_DRIVE, DRIVE_HIGH));
	burst_off();
	MATCH();
	CHECK(f.calls == calls && f.dac == TUNE(F_A) && q.state == CHAINSEQ_ST_RX && q.refused == 6u);
	consistent();
	chainseq_sleep(&q, true);
	clear_log();
	CHECK(!point(PA_KEYUP_PT_PA, DRIVE_HIGH));
	CHECK(!burst_rx());	// asleep is not receiving, and waking is the task's
	burst_off();
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_SLEEP);

	// Under an FM transmission: no tune, no point, no way back; the transmission goes on
	f.pa_allowed = true;
	CHECK(reach(CHAINSEQ_ST_TX));
	CHECK(!burst_tune(F_A));
	CHECK(!point(PA_KEYUP_PT_PA, DRIVE_LOW));
	CHECK(!burst_rx());
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_TX && q.refused == 3u && f.dac == DRIVE_HIGH);
	consistent();
	// The burst's end there drops the PA at once, in order; the transmission's end then finds the receiver
	burst_off();
	w_pa_off(true);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST);
	consistent();
	chainseq_tx_off(&q);
	CHECK(q.state == CHAINSEQ_ST_RX);
	consistent();
	CHECK(f.trips == 0 && q.errors == 0u);
	clear_log();
}

static void test_burst_unlocked(void)
{
	if (!has_lock()) {
		// No lock indication: the points trust the settle time the caller waited
		CHECK(reach(CHAINSEQ_ST_BURST_LOOP));
		f.lock = false;
		CHECK(point(PA_KEYUP_PT_PA, DRIVE_HIGH));
		CHECK(q.state == CHAINSEQ_ST_BURST_ON && f.lock_reads == 0);
		clear_log();
		return;
	}
	// Not locked at the PA's point: the burst ends there, counted, and nothing came on air
	CHECK(reach(CHAINSEQ_ST_BURST_LOOP));
	f.lock = false;
	f.pa_allowed = false;
	CHECK(!point(PA_KEYUP_PT_PA, DRIVE_HIGH));
	w_pa_off(true);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST && q.unlocked == 1u && q.last_err == CHAINSEQ_UNLOCKED && q.errors == 0u);
	CHECK(f.rises == 0);
	consistent();
	// The transmission is still tuned: the next burst, locked by then, goes out
	f.lock = true;
	f.pa_allowed = true;
	CHECK(point(CHAINSEQ_PT_LOOP, DRIVE_HIGH));
	CHECK(point(PA_KEYUP_PT_PA, DRIVE_HIGH));
	CHECK(q.state == CHAINSEQ_ST_BURST_ON && f.rises == 1 && q.unlocked == 1u);
	consistent();
	clear_log();

	// Without a loop point: the lock is asked before the loop closes
	CHECK(reach(CHAINSEQ_ST_BURST));
	f.lock = false;
	f.pa_allowed = false;
	CHECK(!point(PA_KEYUP_PT_PA, DRIVE_HIGH));
	w_pa_off(false);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST && q.unlocked == 1u);

	// A boost is the first to raise the reference: it gets the same refusal
	CHECK(reach(CHAINSEQ_ST_BURST_LOOP));
	f.lock = false;
	f.pa_allowed = false;
	CHECK(!point(PA_KEYUP_PT_BOOST, DRIVE_BOOST));
	w_pa_off(true);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST && q.unlocked == 1u && f.rises == 0);

	// The lock lost between the boost and the PA's point: the burst ends at the PA's point
	CHECK(reach(CHAINSEQ_ST_BURST_LOOP));
	CHECK(point(PA_KEYUP_PT_BOOST, DRIVE_BOOST));
	f.lock = false;
	clear_log();
	CHECK(!point(PA_KEYUP_PT_PA, DRIVE_BOOST));
	w_pa_off(true);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_BURST && q.unlocked == 1u);
	consistent();

	// Three reads in a row, back to back: two are not a lock
	CHECK(reach(CHAINSEQ_ST_BURST_LOOP));
	f.lock = true;
	f.pat = "110";
	f.pa_allowed = false;
	int reads = f.lock_reads;
	CHECK(!point(PA_KEYUP_PT_PA, DRIVE_HIGH));
	CHECK(f.lock_reads == reads + 3 && q.unlocked == 1u && f.rises == 0);
	clear_log();
}

#endif

// --- The VCO line from the plan ---

// A plan whose levels differ by frequency: receive and transmit on both sides of its switch points, bursts across
// them. The line is the plan's wherever the direction's would be written, and in a receive set-up the one new write
// comes only for the level the safe state did not park
static void test_plan(void)
{
	if (!has_plan()) {
		return;
	}
	CHECK(want_vco(F_A, false) && want_vco(F_A, true));
	CHECK(want_vco(F_B, false) && !want_vco(F_B, true));
	CHECK(!want_vco(F_C, false) && !want_vco(F_C, true));

	// Receive under the switch: the parked level is the plan's, nothing more is written
	fresh();
	CHECK(chainseq_rx(&q, F_A, true) == CHAINSEQ_OK);
	w_rx(F_A, true, true);
	MATCH();
	CHECK(f.plan_hz == F_A && !f.plan_tx);
	consistent();
	// Over it: the line off after the park, before the synthesizer rail and the words
	CHECK(chainseq_rx(&q, F_C, false) == CHAINSEQ_OK);
	w_safe();
	WL(VCO_RX, 0);
	WL(SYNTH_RAIL, 1);
	WS(RXVCO(F_C));
	w_lock();
	WL(RX_RAIL, 1);
	WL(IF_RUN, 1);
	WW(SETTLE_US);
	WI(0);
	WD(TUNE(F_C));
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_RX && !f.level[CHAINSEQ_LINE_VCO_RX] && f.plan_hz == F_C && !f.plan_tx);
	consistent();
	// Asleep and awake: the same level again
	chainseq_sleep(&q, true);
	consistent();
	chainseq_sleep(&q, false);
	CHECK(!f.level[CHAINSEQ_LINE_VCO_RX]);
	consistent();

	// FM under the transmit switch from a receiver over the receive one: the line on with the transmit rail off and
	// the loop open, then the transmit rail with it on
	clear_log();
	CHECK(chainseq_tx(&q, F_A, 1) == CHAINSEQ_OK);
	w_tx(F_A, DRIVE_HIGH, false);
	MATCH();
	CHECK(f.level[CHAINSEQ_LINE_VCO_RX] && f.level[CHAINSEQ_LINE_TX_RAIL] && f.rises == 1);
	CHECK(f.plan_hz == F_A && f.plan_tx);
	consistent();
	// The end: down in order, the safe state parks the line on, the receive set-up moves it off again
	chainseq_tx_off(&q);
	w_ramp(DRIVE_HIGH, false);
	w_pa_off(true);
	w_rx(F_C, false, false);
	MATCH();
	CHECK(q.state == CHAINSEQ_ST_RX && !f.level[CHAINSEQ_LINE_VCO_RX]);
	consistent();
	// Between the switches the direction decides it: on to receive, off to transmit
	CHECK(chainseq_rx(&q, F_B, true) == CHAINSEQ_OK && f.level[CHAINSEQ_LINE_VCO_RX]);
	CHECK(chainseq_tx(&q, F_B, 0) == CHAINSEQ_OK && !f.level[CHAINSEQ_LINE_VCO_RX] && f.plan_tx);
	consistent();
	chainseq_tx_off(&q);
	CHECK(f.level[CHAINSEQ_LINE_VCO_RX] && q.state == CHAINSEQ_ST_RX);
	// Not locked on the carrier: safe (the line parked) and back to the receive set-up, which moves it again
	fresh();
	(void)chainseq_rx(&q, F_C, true);
	f.lock = false;
	f.pa_allowed = false;
	CHECK(chainseq_tx(&q, F_A, 1) == CHAINSEQ_UNLOCKED);
	CHECK(q.state == CHAINSEQ_ST_RX && !f.level[CHAINSEQ_LINE_VCO_RX] && f.rises == 0);
	consistent();
	f.lock = true;
	f.pa_allowed = true;

#if CONFIG_DMR
	// Bursts across the switches: from a receiver with the line off to a carrier with it on, at interrupt level
	fresh();
	(void)chainseq_rx(&q, F_C, true);
	clear_log();
	int asks = f.plan_asks;
	CHECK(burst_tune(F_A));
	w_pa_off(false);
	WL(RX_RAIL, 0);
	WL(SYNTH_RAIL, 1);
	WL(VCO_RX, 1);
	WS(F_A);
	WL(TX_RAIL, 1);
	MATCH();
	CHECK(f.plan_asks == asks + 1 && f.plan_hz == F_A && f.plan_tx);
	consistent();
	CHECK(point(CHAINSEQ_PT_LOOP, DRIVE_LOW) && point(PA_KEYUP_PT_PA, DRIVE_LOW));
	CHECK(q.state == CHAINSEQ_ST_BURST_ON && f.level[CHAINSEQ_LINE_VCO_RX] && f.rises == 1);
	consistent();
	burst_off();
	// The points and the burst's end leave the line alone: it moves only with a tune or a way back
	CHECK(f.plan_asks == asks + 1);
	clear_log();
	CHECK(burst_rx());
	w_pa_off(false);
	WL(VCO_RX, 0);
	WS(RXVCO(F_C));
	WL(RX_RAIL, 1);
	WD(TUNE(F_C));
	MATCH();
	CHECK(f.plan_hz == F_C && !f.plan_tx);
	consistent();
	// And the other way: a receiver with the line on, bursts with it off
	CHECK(chainseq_rx(&q, F_A, true) == CHAINSEQ_OK);
	CHECK(burst_tune(F_B) && !f.level[CHAINSEQ_LINE_VCO_RX]);
	consistent();
	CHECK(point(PA_KEYUP_PT_PA, DRIVE_HIGH) && q.state == CHAINSEQ_ST_BURST_ON);
	// A retune of the bursts while one is up: the PA down first, then the line
	CHECK(burst_tune(F_A) && f.level[CHAINSEQ_LINE_VCO_RX] && q.state == CHAINSEQ_ST_BURST);
	consistent();
	CHECK(burst_rx() && f.level[CHAINSEQ_LINE_VCO_RX]);
	consistent();
#endif
	CHECK(f.trips == 0 && q.errors == 0u);
	clear_log();
}

// --- Refused writes, everywhere ---

// The calls a sequence is made of. The lock line going away and coming back is one of them. The last ones only with
// the plan (sym_count()): without it they would add sequences, not cases, and the runs by direction stay what they
// were
enum {
	SYM_RX_A, SYM_RX_B, SYM_RX_OUT, SYM_TX_A, SYM_TX_B, SYM_TX_NOCAL, SYM_TX_OFF, SYM_SLEEP, SYM_WAKE, SYM_SAFE,
	SYM_DRIVE, SYM_DRIVE_0, SYM_UNLOCK, SYM_LOCK,
#if CONFIG_DMR
	SYM_TUNE, SYM_TUNE_NOCAL, SYM_PT_LOOP, SYM_PT_BOOST, SYM_PT_PA, SYM_PT_DRIVE, SYM_BURST_OFF, SYM_BURST_RX,
#endif
	SYM_PLAN_FIRST,
	SYM_RX_C = SYM_PLAN_FIRST,	// receive over the plan's switch: the line off
#if CONFIG_DMR
	SYM_TUNE_A,			// bursts on the line's other level than SYM_TUNE's
#endif
	SYM_COUNT
};

static int sym_count(void)
{
	return has_plan() ? SYM_COUNT : SYM_PLAN_FIRST;
}

static void do_sym(int sym)
{
	int calls = f.calls;
	begin_call();
	bool nothing = false;	// a refusal that must not write
	uint32_t errors = q.errors;
	bool drive_open = false;	// a drive while the loop is open: the one error the driver raises by itself
	switch (sym) {
	case SYM_RX_A: (void)chainseq_rx(&q, F_A, false); break;
	case SYM_RX_B: (void)chainseq_rx(&q, F_B, true); break;
	case SYM_RX_OUT: CHECK(chainseq_rx(&q, F_OUT, true) == CHAINSEQ_REFUSED_BAND); nothing = true; break;
	case SYM_TX_A: (void)chainseq_tx(&q, F_A, 0); break;
	case SYM_TX_B: (void)chainseq_tx(&q, F_B, 1); break;
	case SYM_TX_NOCAL: CHECK(chainseq_tx(&q, F_NOCAL, 1) == CHAINSEQ_REFUSED_CAL); nothing = true; break;
	case SYM_TX_OFF: chainseq_tx_off(&q); break;
	case SYM_SLEEP: chainseq_sleep(&q, true); break;
	case SYM_WAKE: chainseq_sleep(&q, false); break;
	case SYM_SAFE: chainseq_safe(&q); break;
	case SYM_DRIVE: (void)chainseq_drive_now(&q, 1200); break;
	case SYM_DRIVE_0: (void)chainseq_drive_now(&q, 0); break;
	case SYM_UNLOCK: f.lock = false; break;
	case SYM_LOCK: f.lock = true; break;
#if CONFIG_DMR
	case SYM_TUNE: (void)burst_tune(F_B); break;
	case SYM_TUNE_NOCAL: CHECK(!burst_tune(F_NOCAL)); nothing = true; break;
	case SYM_PT_LOOP: (void)point(CHAINSEQ_PT_LOOP, DRIVE_LOW); break;
	case SYM_PT_BOOST:
		drive_open = q.state == CHAINSEQ_ST_BURST;
		(void)point(PA_KEYUP_PT_BOOST, DRIVE_BOOST);
		break;
	case SYM_PT_PA: (void)point(PA_KEYUP_PT_PA, DRIVE_BOOST); break;
	case SYM_PT_DRIVE:
		drive_open = q.state == CHAINSEQ_ST_BURST;
		(void)point(PA_KEYUP_PT_DRIVE, DRIVE_LOW);
		break;
	case SYM_BURST_OFF: burst_off(); break;
	case SYM_BURST_RX: (void)burst_rx(); break;
	case SYM_TUNE_A: (void)burst_tune(F_A); break;
#endif
	case SYM_RX_C: (void)chainseq_rx(&q, F_C, false); break;
	default: break;
	}
	CHECK(!nothing || f.calls == calls);
	if (f.fail_at < 0 && f.fail_from < 0) {
		// No write was refused: no error but that one, and it ends safe
		CHECK(q.errors == errors + (drive_open ? 1u : 0u));
		CHECK(!drive_open || q.state == CHAINSEQ_ST_IDLE);
	}
}

// Every sequence of three calls, with a write refused at each position in turn: once (the write number n), and for
// good (from n on). The call the refusal lands in has to end with the PA off and count an error; with the refusals
// going on, so does every call after it. The watcher's rules hold throughout
static void test_refused_writes(void)
{
	g_log = false;
	long runs = 0;
	const int ns = sym_count();
	for (int n3 = 0; n3 < ns * ns * ns && failures < 40; n3++) {
		int seq[3] = { n3 % ns, n3 / ns % ns, n3 / ns / ns };
		fresh();
		int first = f.nwrites;	// the start's own writes are test_safe's
		for (int i = 0; i < 3; i++) {
			do_sym(seq[i]);
		}
		int total = f.nwrites;
		for (int mode = 0; mode < 2; mode++) {
			for (int n = first; n < total; n++) {
				fresh();
				if (mode == 0) {
					f.fail_at = n;
				} else {
					f.fail_from = n;
				}
				for (int i = 0; i < 3; i++) {
					bool was = f.fail_hit;
					uint32_t errors = q.errors;
					do_sym(seq[i]);
					if (f.fail_hit && !was) {
						CHECK(q.errors > errors);
						CHECK(q.last_err != CHAINSEQ_OK);
					}
					if (f.fail_hit && (!was || mode == 1)) {
						pa_safe();
					}
				}
				CHECK(f.fail_hit);
				CHECK(f.trips == 0);
				runs++;
			}
		}
	}
	g_log = true;
	clear_log();
	printf("  refused writes: %ld runs (floor %u%s%s)\n", runs, g_floor, has_lock() ? "" : ", no lock line",
	       has_plan() ? ", VCO plan" : "");
}

// --- Every interleaving ---

static void step(int sym)
{
	do_sym(sym);
	consistent();
}

// Every sequence of four calls from a fresh start, then one long walk that never starts over. After each call the
// lines are what the state promises; the watcher sees every write
static void test_interleavings(void)
{
	g_log = false;
	long seqs = 0;
	const int ns = sym_count();
	int n4 = ns * ns * ns * ns;
	for (int n = 0; n < n4 && failures < 40; n++) {
		fresh();
		for (int i = 0, d = n; i < 4; i++, d /= ns) {
			step(d % ns);
		}
		CHECK(f.trips == 0);
		seqs++;
	}
	fresh();
	uint32_t lcg = 12345u;
	long rises = 0;
	for (long i = 0; i < 1000000 && failures < 40; i++) {
		lcg = lcg * 1664525u + 1013904223u;
		step((int)((lcg >> 16) % (uint32_t)ns));
		rises = f.rises;
	}
	CHECK(f.trips == 0);
	CHECK(!CONFIG_DMR ? q.errors == 0u : q.errors != 0u);	// the walk met a drive with the loop open (do_sym)
	CHECK(rises > 1000);	// the walk did transmit: the rules were not kept by never keying
	g_log = true;
	clear_log();
	printf("  interleavings: %ld sequences, a walk with %ld key-ups (floor %u%s%s)\n", seqs, rises, g_floor,
	       has_lock() ? "" : ", no lock line", has_plan() ? ", VCO plan" : "");
}

static void run_all(const chainseq_ops_t *o, uint16_t floor)
{
	g_ops = o;
	g_floor = floor;
	test_init();
	test_rx();
	test_rx_lock();
	test_sleep();
	test_tx();
	test_tx_off_refused();
	test_tx_ramp();
	test_tx_refused();
	test_tx_unlocked();
	test_tx_retune();
	test_drive_now();
	test_safe();
	test_safed();
#if CONFIG_DMR
	test_burst();
	test_burst_orders();
	test_burst_refused();
	test_burst_unlocked();
#endif
	test_plan();
	test_refused_writes();
	test_interleavings();
}

int main(void)
{
	run_all(&ops_lock, 0);
	run_all(&ops_lock, 300);
	run_all(&ops_nolock, 0);
	run_all(&ops_nolock, 300);
	run_all(&ops_plan, 0);
	run_all(&ops_plan, 300);
	printf("chainseqTest%s: %s (%d failures)\n", CONFIG_DMR ? "" : " (FM only)", failures == 0 ? "PASSED" : "FAILED",
	       failures);
	return failures == 0 ? 0 : 1;
}
