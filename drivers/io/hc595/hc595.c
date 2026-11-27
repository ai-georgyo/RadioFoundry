// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// 74HC595 output expander: the shadow and its one writer path (hc595.h)
#include "hc595.h"

static uint32_t mask_of(uint8_t bits)
{
	return bits >= 32u ? 0xFFFFFFFFu : (1u << bits) - 1u;
}

// Inside the critical section. The shadow first: a fault handler that cuts into the shift builds its word from the
// word being written, not from the one before it
static void put(hc595_t *x, uint32_t word)
{
	word &= mask_of(x->bits);
	x->shadow = word;
	hc595_shift(x->ops, word, x->bits);
	x->writes++;
}

void hc595_init(hc595_t *x, const hc595_ops_t *ops, uint8_t bits, uint32_t initial)
{
	x->ops = ops;
	x->bits = bits > 32u ? 32u : bits;
	x->shadow = 0;
	x->writes = 0;
	hc595_write(x, initial);
}

void hc595_write(hc595_t *x, uint32_t word)
{
	const hc595_ops_t *ops = x->ops;
	uint32_t saved = ops->crit_enter != NULL ? ops->crit_enter() : 0u;
	put(x, word);
	if (ops->crit_exit != NULL) {
		ops->crit_exit(saved);
	}
}

void hc595_update(hc595_t *x, uint32_t set, uint32_t clear)
{
	const hc595_ops_t *ops = x->ops;
	uint32_t saved = ops->crit_enter != NULL ? ops->crit_enter() : 0u;
	// Read inside the section: an interrupt's update between the read and the shift would be lost
	put(x, (x->shadow & ~clear) | set);
	if (ops->crit_exit != NULL) {
		ops->crit_exit(saved);
	}
}
