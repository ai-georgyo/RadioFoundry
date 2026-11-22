// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// An 8080 display bus on GPIO pins (lcd8080_gpio.h). A byte is one write of the data port's set / reset register and
// a write strobe; the lines change direction with one write of the port's mode register.
#include <stddef.h>
#include "lcd8080_gpio.h"
#include "FreeRTOS.h"

#define LINES_IN	0x0000u		// MODER, two bits a pin, for eight pins
#define LINES_OUT	0x5555u
#define PULL_ALL	0x5555u		// PUPDR: a pull's two bits (the GPIO_* value) in every pin's field

static struct {
	const stm32_lcd8080_cfg_t *c;	// NULL until stm32_lcd8080_init: nothing here touches a pin
	uint32_t fields;		// the data lines' two-bit fields in MODER and PUPDR
	bool open;			// a session: between begin and end, under the board's lock
} l;

// The strobe times are the board's, measured on the cycle counter: a counted loop is 1 to 3 cycles a pass depending
// on the core
static inline void wait(uint16_t ns)
{
	if (ns != 0u) {
		stm32_wait_ns(ns);
	}
}

// The data lines' direction and pull. Interrupts off: a read-modify-write of the same port's registers elsewhere
// (another pin's set-up) can't fall in between. The pull first, so a line that becomes an input has it already
static void data_lines(uint32_t mode, uint32_t pull)
{
	GPIO_TypeDef *port = l.c->data_port;
	uint32_t shift = 2u * l.c->data_shift;
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	port->PUPDR = (port->PUPDR & ~l.fields) | ((pull * PULL_ALL) << shift);
	port->MODER = (port->MODER & ~l.fields) | (mode << shift);
	__set_PRIMASK(primask);
}

// One byte: the set half of the register takes the ones and the reset half the zeros in one write, and the port's
// other pins keep their levels. The panel takes the byte on the strobe's rising edge
static inline void put(uint8_t b)
{
	const stm32_lcd8080_cfg_t *c = l.c;
	c->data_port->BSRR = ((uint32_t)b << c->data_shift) | ((uint32_t)(uint8_t)~b << (c->data_shift + 16u));
	stm32_pin_write(c->wr_port, c->wr_pin, false);
	wait(c->wr_low_ns);
	stm32_pin_write(c->wr_port, c->wr_pin, true);
	wait(c->wr_high_ns);
}

void stm32_lcd8080_init(const stm32_lcd8080_cfg_t *c)
{
	uint16_t pins = (uint16_t)(0xFFu << c->data_shift);

	// RES first, and low: the panel is in reset before a strobe line is driven. The strobes and the select go
	// straight to their idle level (stm32_pin_out writes the level before the mode), so none of them sees an edge
	stm32_pin_out(c->res_port, c->res_pin, false, false);
	stm32_pin_out(c->cs_port, c->cs_pin, true, false);
	stm32_pin_out(c->wr_port, c->wr_pin, true, false);
	stm32_pin_out(c->rd_port, c->rd_pin, true, false);
	stm32_pin_out(c->dc_port, c->dc_pin, true, false);
	stm32_pin_in(c->data_port, pins, c->idle_pull);
	// What a session's mode write alone does not set: push-pull, and the slope the other outputs have
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	c->data_port->OTYPER &= ~(uint32_t)pins;
	__set_PRIMASK(primask);
	stm32_pin_speed(c->data_port, pins, STM32_GPIO_SPEED);
	l.fields = 0xFFFFu << (2u * c->data_shift);
	l.open = false;
	l.c = c;
}

void stm32_lcd8080_begin(void)
{
	const stm32_lcd8080_cfg_t *c = l.c;
	if (c == NULL) {
		return;
	}
	c->lock();
	// The lock is recursive for the key scan's sake; a session inside a session would end with the outer one's
	// lines turned round under it
	configASSERT(!l.open);
	l.open = true;
	data_lines(LINES_OUT, c->idle_pull);
	stm32_pin_write(c->cs_port, c->cs_pin, false);
}

void stm32_lcd8080_end(void)
{
	const stm32_lcd8080_cfg_t *c = l.c;
	if (c == NULL) {
		return;
	}
	configASSERT(l.open);
	stm32_pin_write(c->cs_port, c->cs_pin, true);
	// The pull again as well: the lines' other user (a key scan) sets its own
	data_lines(LINES_IN, c->idle_pull);
	l.open = false;
	c->unlock();
}

void stm32_lcd8080_cmd(uint8_t cmd)
{
	const stm32_lcd8080_cfg_t *c = l.c;
	if (c == NULL) {
		return;
	}
	configASSERT(l.open);
	stm32_pin_write(c->dc_port, c->dc_pin, false);
	put(cmd);
	stm32_pin_write(c->dc_port, c->dc_pin, true);	// D/C rests high: data and pixels find it there
}

void stm32_lcd8080_data(const uint8_t *d, uint32_t n)
{
	if (l.c == NULL) {
		return;
	}
	configASSERT(l.open);
	for (uint32_t i = 0; i < n; i++) {
		put(d[i]);
	}
}

void stm32_lcd8080_pixels(const uint16_t *px, uint32_t n, bool swap_rb)
{
	if (l.c == NULL) {
		return;
	}
	configASSERT(l.open);
	for (uint32_t i = 0; i < n; i++) {
		uint16_t v = px[i];
		if (swap_rb) {
			v = (uint16_t)((v & 0x07E0u) | (v >> 11) | (v << 11));
		}
		put((uint8_t)(v >> 8));
		put((uint8_t)v);
	}
}

void stm32_lcd8080_reset(bool level)
{
	const stm32_lcd8080_cfg_t *c = l.c;
	if (c == NULL) {
		return;
	}
	stm32_pin_write(c->res_port, c->res_pin, level);
}

void stm32_lcd8080_read(uint8_t cmd, uint8_t *buf, uint8_t n)
{
	const stm32_lcd8080_cfg_t *c = l.c;
	if (c == NULL) {
		for (uint8_t i = 0; i < n; i++) {
			buf[i] = 0;
		}
		return;
	}
	configASSERT(l.open);
	stm32_lcd8080_cmd(cmd);
	// The lines low before they let go, with no strobe (the panel takes nothing): with no pull a line keeps its
	// charge for far longer than a read, so a panel that does not answer reads 00, not the command byte again
	stm32_pin_write(c->data_port, (uint16_t)(0xFFu << c->data_shift), false);
	// The lines are inputs before the first read strobe, which is what makes the panel drive them, and outputs
	// again only after the last strobe's high time, when it has let go. No pulls: the panel's drivers alone
	data_lines(LINES_IN, GPIO_NOPULL);
	for (uint8_t i = 0; i < n; i++) {
		stm32_pin_write(c->rd_port, c->rd_pin, false);
		wait(c->rd_low_ns);
		buf[i] = (uint8_t)(c->data_port->IDR >> c->data_shift);
		stm32_pin_write(c->rd_port, c->rd_pin, true);
		wait(c->rd_high_ns);
	}
	data_lines(LINES_OUT, c->idle_pull);
}

bool stm32_lcd8080_idle(void)
{
	return !l.open;
}
