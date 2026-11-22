// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A chain of 74HC595 shift registers as an output expander: up to 32 outputs on three lines (TI SN74HC595 data
// sheet SCLS041J). Generic: the board supplies the three lines and knows what each output does.
//
// The chip: SER is taken into the shift register on the rising edge of SRCLK, QH' feeds the next chip's SER, and the
// rising edge of RCLK copies every shift register into its outputs at once. So the outputs never show a half-shifted
// word: they move only on the latch edge, all together.
//
// Rules:
// - One shadow, one writer path. Whoever changes an output goes through hc595_write / hc595_update of the one
//   hc595_t, so the shadow is always what the outputs hold.
// - hc595_update reads the shadow, changes it and shifts it out inside the critical section (crit_enter / crit_exit),
//   so a task and an interrupt can both use it. Give the hooks wherever an interrupt writes the chain too.
// - A fault handler may call hc595_shift with a word built from hc595_get, hooks or not: it cuts into an update at
//   any point, shifts a whole word and latches it. The update it cut is never resumed (the handler ends in a reset).
#ifndef HC595_H
#define HC595_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	void (*clk)(bool level);	// SRCLK: a bit is taken on its rising edge
	void (*data)(bool level);	// SER
	void (*latch)(bool level);	// RCLK: the outputs change on its rising edge, all together
	void (*delay)(void);		// one half period (at least 100 ns at 3.3 V). NULL only where the caller's own code is
					// slower than that at every clock it can run at
	uint32_t (*crit_enter)(void);	// optional: keeps the interrupts that also write the chain out
	void (*crit_exit)(uint32_t saved);
} hc595_ops_t;

typedef struct {
	const hc595_ops_t *ops;
	uint8_t bits;			// outputs in the chain, 1..32
	volatile uint32_t shadow;	// what the outputs hold (from the start of the write that puts it there)
	uint32_t writes;		// words shifted out (diagnostics)
} hc595_t;

static inline void hc595_half(const hc595_ops_t *ops)
{
	if (ops->delay != NULL) {
		ops->delay();
	}
}

// Shift the low `bits` of word, MSB first (the first bit ends in the last chip's QH), then one latch pulse. No state:
// usable before .data and .bss exist when ops is const and its functions only write registers
static inline void hc595_shift(const hc595_ops_t *ops, uint32_t word, uint8_t bits)
{
	if (bits > 32u) {
		bits = 32u;
	}
	// Both clocks low first, whatever they were: a caller that cut into another shift (a fault handler), or the first
	// write after a reset, still gets its rising edges. A falling edge does nothing in the chip
	ops->latch(false);
	ops->clk(false);
	for (uint8_t i = bits; i-- > 0u;) {
		ops->data(((word >> i) & 1u) != 0u);
		hc595_half(ops);
		ops->clk(true);
		hc595_half(ops);
		ops->clk(false);
	}
	// The last clock's high half was the set-up the latch edge needs after a shift
	ops->latch(true);
	hc595_half(ops);
	ops->latch(false);
}

void hc595_init(hc595_t *x, const hc595_ops_t *ops, uint8_t bits, uint32_t initial);	// writes initial
void hc595_write(hc595_t *x, uint32_t word);			// the whole word
void hc595_update(hc595_t *x, uint32_t set, uint32_t clear);	// shadow = (shadow & ~clear) | set, one latch
static inline uint32_t hc595_get(const hc595_t *x) { return x->shadow; }

#endif
