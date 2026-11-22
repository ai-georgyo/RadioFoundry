// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A wire3_bus_t (drivers/bus/wire3.h) bit-banged through callbacks, for a board whose lines are not all plain pins
// of one kind: a chip select behind an output expander, a data line that is an input outside a frame. The board
// supplies the lines and the half period and keeps any locking; nothing here touches a register.
//
// A frame: the chip select falls, then per bit the clock goes low, the data line is set, half a period, the clock
// goes high (the chip takes the bit on that edge), half a period. The chip select rises after the last clock, which
// is where a chip of this kind latches the word. Data changes only while the clock is low, and the clock never moves
// while no chip is selected. Between frames the clock rests at its idle level and nothing drives the data line
// anew: it keeps the last bit's level until the board parks it (in cs(), or after the call).
//
// boards/md9600/wire3_md9600.c is the register-level twin of this file (the same frames on one port's pins) and
// boards/md380/rfbus_md380.c has another for its synthesizer; each goes when its board moves to this driver.
#ifndef WIRE3_GPIO_H
#define WIRE3_GPIO_H

#include <stdint.h>
#include <stdbool.h>
#include "bus/wire3.h"

typedef struct {
	void (*cs)(void *ctx, uint8_t cs, bool level);	// chip select n; false = selected. May be slow (an expander bit)
	void (*clk)(void *ctx, bool level);
	void (*data)(void *ctx, bool level);		// drive the data line (makes it an output)
	void (*data_release)(void *ctx, bool pull_up);	// data line to input; optional (read frames only)
	bool (*data_in)(void *ctx);			// the data line's level; optional (read frames only)
	void (*delay)(void *ctx);			// one half period
	bool clk_idle_high;
	void *ctx;
} wire3_gpio_t;

// wire3_bus_t.write with bus = a wire3_gpio_t *: the low bits (1..32) of word, MSB first. False, nothing driven: bits
// out of range or a line missing
bool wire3_gpio_write(void *bus, uint8_t cs, uint32_t word, uint8_t bits);

// A read frame: head_bits (0..32) of head out (taken on rising edges), the data line released, n bits (1..32) in
// after the falling edges, MSB first. The line is released half a period after the head's last rising edge, with the
// clock still high, and stays an input when the frame ends. Sample 1 is taken half a period after the falling edge
// that follows that rising edge, each later one after one more clock; the clock does not rise again after the last
// sample (a clock that idles high returns high before the chip select rises: one more rising edge). With a 7-bit
// head and n = 10 a clock that idles low makes 16 clocks, and the samples follow the falling edges of clocks 7 to 16.
// With no head the first sample is what the chip shows once it is selected, before any clock edge (clock idle low),
// or after the first falling edge (idle high). A line that needs longer than half a period to settle under its pull
// gets the time in data_release().
// False, nothing driven: a count out of range, a line missing or out NULL
bool wire3_gpio_read(const wire3_gpio_t *b, uint8_t cs, uint32_t head, uint8_t head_bits, bool pull_up,
		     uint8_t n, uint32_t *out);

#endif
