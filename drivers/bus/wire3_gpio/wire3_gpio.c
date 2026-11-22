// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Three-wire bus on pin callbacks (wire3_gpio.h)
#include <stddef.h>
#include "wire3_gpio.h"

static bool usable(const wire3_gpio_t *b)
{
	return b != NULL && b->cs != NULL && b->clk != NULL && b->data != NULL && b->delay != NULL;
}

// The clock at its idle level before the select falls, and half a period before it: if the clock was found
// elsewhere, that edge must be over when the chip starts counting
static void begin(const wire3_gpio_t *b, uint8_t cs)
{
	b->clk(b->ctx, b->clk_idle_high);
	b->delay(b->ctx);
	b->cs(b->ctx, cs, false);
	b->delay(b->ctx);
}

// Leaves the clock high, half a period after the last bit was taken
static void put(const wire3_gpio_t *b, uint32_t word, uint8_t bits)
{
	for (uint8_t i = bits; i-- > 0u;) {
		b->clk(b->ctx, false);
		b->data(b->ctx, ((word >> i) & 1u) != 0u);
		b->delay(b->ctx);
		b->clk(b->ctx, true);
		b->delay(b->ctx);
	}
}

// The clock back to idle while the chip is still selected, then the select: no clock edge with nothing selected
static void end(const wire3_gpio_t *b, uint8_t cs)
{
	b->clk(b->ctx, b->clk_idle_high);
	b->delay(b->ctx);
	b->cs(b->ctx, cs, true);
	b->delay(b->ctx);
}

bool wire3_gpio_write(void *bus, uint8_t cs, uint32_t word, uint8_t bits)
{
	const wire3_gpio_t *b = bus;
	if (!usable(b) || bits == 0u || bits > 32u) {
		return false;
	}
	begin(b, cs);
	put(b, word, bits);
	end(b, cs);
	return true;
}

bool wire3_gpio_read(const wire3_gpio_t *b, uint8_t cs, uint32_t head, uint8_t head_bits, bool pull_up,
		     uint8_t n, uint32_t *out)
{
	if (!usable(b) || b->data_release == NULL || b->data_in == NULL || out == NULL || head_bits > 32u || n == 0u ||
	    n > 32u) {
		return false;
	}
	begin(b, cs);
	put(b, head, head_bits);
	// Turn the line around before the clock falls: a chip drives its first bit from a falling edge, and the line
	// must be free by then
	b->data_release(b->ctx, pull_up);
	b->delay(b->ctx);
	uint32_t v = 0;
	for (uint8_t i = 0; i < n; i++) {
		b->clk(b->ctx, false);
		b->delay(b->ctx);
		v = (v << 1) | (b->data_in(b->ctx) ? 1u : 0u);
		if (i + 1u < n) {
			b->clk(b->ctx, true);
			b->delay(b->ctx);
		}
	}
	end(b, cs);
	*out = v;
	return true;
}
