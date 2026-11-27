// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The 74HC595 output expander (drivers/io/hc595) against a model of a chain of the chips: bit order, the outputs
// moving only on the one latch edge of a write, the shadow, set and clear, the critical section around a task's and
// an interrupt's updates, and a fault handler's stateless shift that cuts into a write
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "io/hc595/hc595.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- The chain: SER into chip 0, each QH' into the next chip's SER, SRCLK and RCLK common ---

#define MAX_CHIPS	4
#define LOG_MAX		16

static struct {
	int chips;
	bool clk, ser, rclk;
	uint8_t sr[MAX_CHIPS];		// bit 0 = QA's stage (the newest bit), bit 7 = QH's
	uint8_t out[MAX_CHIPS];		// the storage register: the pins
	uint32_t t;			// half periods waited
	uint32_t t_ser, t_clk, t_rise;	// when SER and SRCLK last changed, when SRCLK last rose
	uint32_t t_rclk;		// when RCLK last changed
	bool timed;			// the ops have a delay: check the chip's set-up times and pulse widths
	int timing_errors;
	int clk_rises;			// since the last latch
	int latches;
	int rises_at_latch;		// clk_rises when the last latch edge came
	uint32_t log[LOG_MAX];		// every word the pins have shown, in order
	int logged;
} chain;

// What happens on the next SRCLK rising edges: the test's interrupt or fault
static void (*on_rise)(int n);

static uint32_t pins(void)
{
	uint32_t w = 0;
	for (int k = 0; k < chain.chips; k++) {
		w |= (uint32_t)chain.out[k] << (8 * k);
	}
	return w;
}

static void chain_reset(int chips)
{
	memset(&chain, 0, sizeof(chain));
	chain.chips = chips;
	// The chips have no reset wired: whatever they held
	for (int k = 0; k < chips; k++) {
		chain.sr[k] = (uint8_t)(0x5Au + 0x31u * (unsigned)k);
		chain.out[k] = (uint8_t)(0xC3u - 0x17u * (unsigned)k);
	}
	on_rise = NULL;
}

// --- The interrupt mask the critical section hooks drive ---

static struct {
	bool masked;
	bool pending;			// an interrupt asked while masked
	int enters, exits;
	int bad_tokens;			// crit_exit got something else than its crit_enter returned
	int unmasked_edges;		// a line moved outside the section
	void (*isr)(void);
	void (*before_enter)(void);	// runs as crit_enter is called, before it masks: an interrupt in that gap
} irq;

static bool hooks;			// the ops under test have the critical section hooks

static void irq_raise(void)
{
	if (irq.masked) {
		irq.pending = true;
	} else if (irq.isr != NULL) {
		irq.isr();
	}
}

#define TOKEN	0x51A70000u

static uint32_t op_crit_enter(void)
{
	if (irq.before_enter != NULL) {
		void (*f)(void) = irq.before_enter;
		irq.before_enter = NULL;
		f();
	}
	uint32_t saved = TOKEN | (irq.masked ? 1u : 0u);
	irq.masked = true;
	irq.enters++;
	return saved;
}

static void op_crit_exit(uint32_t saved)
{
	if ((saved & ~1u) != TOKEN) {
		irq.bad_tokens++;
	}
	irq.exits++;
	irq.masked = (saved & 1u) != 0u;
	if (!irq.masked && irq.pending) {
		irq.pending = false;
		if (irq.isr != NULL) {
			irq.isr();
		}
	}
}

// --- The three lines ---

static void line_moved(void)
{
	if (hooks && !irq.masked) {
		irq.unmasked_edges++;
	}
}

static void op_clk(bool level)
{
	if (level == chain.clk) {
		return;
	}
	if (chain.timed && chain.t - chain.t_clk < 1u) {
		chain.timing_errors++;		// SRCLK high or low for less than a half period
	}
	chain.clk = level;
	chain.t_clk = chain.t;
	line_moved();
	if (!level) {
		return;
	}
	if (chain.timed && chain.t - chain.t_ser < 1u) {
		chain.timing_errors++;		// SER before SRCLK rising
	}
	chain.t_rise = chain.t;
	bool carry = chain.ser;
	for (int k = 0; k < chain.chips; k++) {
		bool next = (chain.sr[k] & 0x80u) != 0u;
		chain.sr[k] = (uint8_t)((chain.sr[k] << 1) | (carry ? 1u : 0u));
		carry = next;
	}
	chain.clk_rises++;
	if (on_rise != NULL) {
		on_rise(chain.clk_rises);
	}
}

static void op_data(bool level)
{
	if (level != chain.ser) {
		chain.ser = level;
		chain.t_ser = chain.t;
		line_moved();
	}
}

static void op_latch(bool level)
{
	if (level == chain.rclk) {
		return;
	}
	if (chain.timed && chain.t - chain.t_rclk < 1u) {
		chain.timing_errors++;		// RCLK high or low for less than a half period
	}
	chain.rclk = level;
	chain.t_rclk = chain.t;
	line_moved();
	if (!level) {
		return;
	}
	if (chain.timed && chain.t - chain.t_rise < 1u) {
		chain.timing_errors++;		// SRCLK rising before RCLK rising
	}
	for (int k = 0; k < chain.chips; k++) {
		chain.out[k] = chain.sr[k];
	}
	chain.rises_at_latch = chain.clk_rises;
	chain.clk_rises = 0;
	chain.latches++;
	if (chain.logged < LOG_MAX) {
		chain.log[chain.logged] = pins();
	}
	chain.logged++;
}

static void op_delay(void)
{
	chain.t++;
}

static const hc595_ops_t ops_plain = { op_clk, op_data, op_latch, op_delay, NULL, NULL };
static const hc595_ops_t ops_fast = { op_clk, op_data, op_latch, NULL, NULL, NULL };
static const hc595_ops_t ops_crit = { op_clk, op_data, op_latch, op_delay, op_crit_enter, op_crit_exit };

static void start(int chips, const hc595_ops_t *ops)
{
	chain_reset(chips);
	chain.timed = ops->delay != NULL;
	memset(&irq, 0, sizeof(irq));
	hooks = ops->crit_enter != NULL;
}

// --- Tests ---

// Word bit n is output n: chip n / 8, QA + n % 8. The first bit out (the top one) ends in the last chip's QH
static void test_bit_order(void)
{
	hc595_t x;
	start(3, &ops_plain);
	uint32_t undefined = pins();
	hc595_init(&x, &ops_plain, 24, 0x9C3A65u);
	CHECK(undefined != 0x9C3A65u);
	CHECK(pins() == 0x9C3A65u);
	CHECK(chain.out[0] == 0x65u && chain.out[1] == 0x3Au && chain.out[2] == 0x9Cu);
	CHECK(hc595_get(&x) == 0x9C3A65u);
	CHECK(x.bits == 24 && x.writes == 1);
	CHECK(chain.latches == 1 && chain.rises_at_latch == 24);

	for (int n = 0; n < 24; n++) {
		hc595_write(&x, 1u << n);
		CHECK(chain.out[n / 8] == (uint8_t)(1u << (n % 8)));
		CHECK(pins() == 1u << n);
		CHECK(chain.rises_at_latch == 24);
	}
	hc595_write(&x, 0x800000u);
	CHECK(chain.out[2] == 0x80u && chain.out[1] == 0 && chain.out[0] == 0);	// the last chip's QH
	hc595_write(&x, 0x000001u);
	CHECK(chain.out[0] == 0x01u && chain.out[1] == 0 && chain.out[2] == 0);	// the first chip's QA
	CHECK(chain.latches == 27 && x.writes == 27);		// one latch a write
	CHECK(chain.timing_errors == 0);
	CHECK(!chain.clk && !chain.rclk);			// both clocks left low

	// Bits above the chain are not outputs: dropped from the word and from the shadow
	hc595_write(&x, 0xFFFFFFFFu);
	CHECK(hc595_get(&x) == 0x00FFFFFFu && pins() == 0x00FFFFFFu);
	hc595_update(&x, 0x01000000u, 0x0000000Fu);
	CHECK(hc595_get(&x) == 0x00FFFFF0u && pins() == 0x00FFFFF0u);
}

// Nothing shows on the pins while a word is shifted: every pin moves on the latch edge, after the last clock
static uint32_t held;
static int seen_rises;

static void expect_held(int n)
{
	seen_rises = n;
	CHECK(pins() == held);
	CHECK(chain.latches == 0);
}

static void test_outputs_move_on_latch(void)
{
	hc595_t x;
	start(3, &ops_plain);
	hc595_init(&x, &ops_plain, 24, 0x00F00Fu);
	held = 0x00F00Fu;
	chain.latches = 0;
	seen_rises = 0;
	on_rise = expect_held;
	hc595_write(&x, 0xABCDEFu);
	on_rise = NULL;
	CHECK(seen_rises == 24);
	CHECK(chain.latches == 1);
	CHECK(pins() == 0xABCDEFu);
	CHECK(chain.timing_errors == 0);
}

static void test_update(void)
{
	hc595_t x;
	start(3, &ops_plain);
	hc595_init(&x, &ops_plain, 24, 0x000F00u);
	int latches = chain.latches;

	hc595_update(&x, 0x000003u, 0);				// set
	CHECK(hc595_get(&x) == 0x000F03u && pins() == 0x000F03u);
	hc595_update(&x, 0, 0x000301u);				// clear
	CHECK(hc595_get(&x) == 0x000C02u && pins() == 0x000C02u);
	hc595_update(&x, 0x800000u, 0x000002u);			// both
	CHECK(hc595_get(&x) == 0x800C00u && pins() == 0x800C00u);
	hc595_update(&x, 0x000010u, 0x000010u);			// a bit in both: set
	CHECK(hc595_get(&x) == 0x800C10u && pins() == 0x800C10u);
	hc595_update(&x, 0x000010u, 0x000001u);			// no change: the word is written again all the same
	CHECK(hc595_get(&x) == 0x800C10u && pins() == 0x800C10u);
	hc595_update(&x, 0, 0);
	CHECK(pins() == 0x800C10u);
	CHECK(chain.latches == latches + 6);			// one latch each
	CHECK(x.writes == 7);
	CHECK(chain.timing_errors == 0);
}

// Other chain lengths; no delay where the caller is slow enough; clocks found high
static void test_lengths(void)
{
	hc595_t x;
	start(1, &ops_fast);
	hc595_init(&x, &ops_fast, 8, 0xA5u);
	CHECK(pins() == 0xA5u && chain.rises_at_latch == 8);
	hc595_update(&x, 0x100u | 0x02u, 0x80u);
	CHECK(pins() == 0x27u && hc595_get(&x) == 0x27u);

	start(4, &ops_plain);
	hc595_init(&x, &ops_plain, 32, 0x80000001u);
	CHECK(pins() == 0x80000001u && chain.rises_at_latch == 32);
	CHECK(chain.out[3] == 0x80u && chain.out[0] == 0x01u);
	hc595_update(&x, 0x40000000u, 0x80000000u);
	CHECK(pins() == 0x40000001u && hc595_get(&x) == 0x40000001u);
	CHECK(chain.timing_errors == 0);

	// More than the 32 a word holds: 32
	start(4, &ops_plain);
	hc595_init(&x, &ops_plain, 40, 0xDEADBEEFu);
	CHECK(x.bits == 32 && pins() == 0xDEADBEEFu && chain.rises_at_latch == 32);
	start(4, &ops_plain);
	hc595_shift(&ops_plain, 0x12345678u, 200);
	CHECK(pins() == 0x12345678u && chain.rises_at_latch == 32);

	// Both clocks high when the shift starts (it cut into another, or nothing set the lines up): the word still
	// goes out whole and is latched once
	start(3, &ops_plain);
	chain.timed = false;		// the model has no time at which these went high
	chain.clk = true;
	chain.rclk = true;
	hc595_shift(&ops_plain, 0x9C3A65u, 24);
	CHECK(pins() == 0x9C3A65u);
	CHECK(chain.latches == 1 && chain.rises_at_latch == 24);
}

// The stateless shift with const ops and no hc595_t: what a reset path uses before any variable exists
static void test_stateless(void)
{
	start(3, &ops_plain);
	hc595_shift(&ops_plain, 0x9C3A65u, 24);
	CHECK(pins() == 0x9C3A65u);
	CHECK(chain.latches == 1 && chain.rises_at_latch == 24);
	CHECK(chain.timing_errors == 0);
	CHECK(irq.enters == 0 && irq.exits == 0);
}

// --- The critical section ---

static hc595_t *shared;
static int isr_runs;
#define TASK_A		0x000008u
#define TASK_B		0x000200u
#define ISR_1		0x002000u
#define ISR_2		0x100000u
static uint32_t isr_set, isr_clear;

static void isr_update(void)
{
	isr_runs++;
	hc595_update(shared, isr_set, isr_clear);
}

static void raise_at_5(int n)
{
	if (n == 5) {
		on_rise = NULL;		// once: the interrupt's own shift must not ask again
		irq_raise();
	}
}

static void test_critical_section(void)
{
	hc595_t x;
	start(3, &ops_crit);
	shared = &x;
	isr_runs = 0;
	irq.isr = isr_update;
	hc595_init(&x, &ops_crit, 24, 0x000001u);
	CHECK(irq.enters == 1 && irq.exits == 1);
	chain.logged = 0;

	// A task's update, an interrupt's write between two of them, the next task update: all three hold
	hc595_update(&x, TASK_A, 0);
	isr_set = ISR_1;
	isr_clear = 0;
	irq_raise();
	CHECK(isr_runs == 1);
	hc595_update(&x, TASK_B, TASK_A);
	CHECK(hc595_get(&x) == (0x000001u | ISR_1 | TASK_B));
	CHECK(pins() == hc595_get(&x));

	// The interrupt asks in the middle of a task's shift: it waits for the end of the section, then adds its bit
	// to the task's word. Neither is lost and no mixed word is ever latched
	isr_set = ISR_2;
	isr_clear = ISR_1;
	on_rise = raise_at_5;
	hc595_update(&x, TASK_A, TASK_B);
	on_rise = NULL;
	CHECK(isr_runs == 2);
	CHECK(!irq.pending && !irq.masked);
	CHECK(hc595_get(&x) == (0x000001u | TASK_A | ISR_2));
	CHECK(pins() == hc595_get(&x));
	CHECK(chain.logged == 5);
	CHECK(chain.log[0] == (0x000001u | TASK_A));
	CHECK(chain.log[1] == (0x000001u | TASK_A | ISR_1));
	CHECK(chain.log[2] == (0x000001u | ISR_1 | TASK_B));
	CHECK(chain.log[3] == (0x000001u | ISR_1 | TASK_A));		// the task's word, whole
	CHECK(chain.log[4] == (0x000001u | TASK_A | ISR_2));		// then the interrupt's on top of it

	// The interrupt comes as the task calls in, before the section closes: the task's update is made on the word
	// the interrupt left (the shadow is read inside the section)
	isr_set = ISR_1;
	isr_clear = ISR_2;
	irq.before_enter = irq_raise;
	hc595_update(&x, TASK_B, 0);
	CHECK(isr_runs == 3);
	CHECK(hc595_get(&x) == (0x000001u | TASK_A | TASK_B | ISR_1));
	CHECK(pins() == hc595_get(&x));

	// The same for a whole-word write: the interrupt's update lands on the new word
	isr_set = ISR_2;
	isr_clear = 0;
	on_rise = raise_at_5;
	hc595_write(&x, 0x000F00u);
	on_rise = NULL;
	CHECK(isr_runs == 4);
	CHECK(hc595_get(&x) == (0x000F00u | ISR_2) && pins() == hc595_get(&x));

	// Every enter has its exit with the value the enter returned, the section was open again after each call, and
	// no line moved outside it
	CHECK(irq.enters == irq.exits);
	CHECK(irq.enters == (int)x.writes);
	CHECK(irq.bad_tokens == 0);
	CHECK(!irq.masked);
	CHECK(irq.unmasked_edges == 0);
	CHECK(chain.timing_errors == 0);

	// Called with the interrupts already masked (from an interrupt, or inside the caller's own section): they stay
	// masked afterwards
	irq.masked = true;
	hc595_update(&x, TASK_A, 0);
	CHECK(irq.masked);
	irq.masked = false;
	irq.isr = NULL;
}

// --- A fault handler cutting into a write ---

static jmp_buf reset_vector;
#define TX_BITS		0x000208u

static void fault_at_7(int n)
{
	if (n != 7) {
		return;
	}
	// Nothing masks a fault: it runs inside the section, on lines left wherever the write had them
	on_rise = NULL;
	CHECK(chain.clk);
	uint32_t safe = hc595_get(shared) & ~TX_BITS;
	int latches = chain.latches;
	hc595_shift(shared->ops, safe, shared->bits);
	CHECK(pins() == safe);
	CHECK(chain.latches == latches + 1);
	CHECK(chain.rises_at_latch == 7 + 24);		// its 24 clocks after the 7 of the write it cut
	longjmp(reset_vector, 1);			// the handler ends in a reset: the write is never resumed
}

static void test_fault_cut_in(void)
{
	static hc595_t x;		// static: it is written between setjmp and longjmp and read after
	start(3, &ops_crit);
	shared = &x;
	hc595_init(&x, &ops_crit, 24, 0x140181u);
	hc595_update(&x, 0x008000u, 0);
	chain.timed = false;		// the cut shortens one clock pulse; a whole word follows it
	chain.latches = 0;
	if (setjmp(reset_vector) == 0) {
		on_rise = fault_at_7;
		hc595_update(&x, TX_BITS, 0);		// the write the fault cuts
		CHECK(0);				// never returns here
	}
	// The pins hold the handler's word: the word being written, without the bits it took out. The word before
	// that write never shows again
	CHECK(chain.latches == 1);
	CHECK(pins() == 0x148181u);
	CHECK(hc595_get(&x) == (0x148181u | TX_BITS));	// the shadow already had the new word: the handler built on it
}

int main(void)
{
	test_bit_order();
	test_outputs_move_on_latch();
	test_update();
	test_lengths();
	test_stateless();
	test_critical_section();
	test_fault_cut_in();
	printf("hc595Test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
