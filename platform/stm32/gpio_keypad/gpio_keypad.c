// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A key matrix on GPIO lines that may be a bus between scans (gpio_keypad.h)
#include <stddef.h>
#include "gpio_keypad.h"
#include "hal_time.h"

#define KEYPAD_MAX_LINES	8u	// a row of the scan is a byte
#define MODER_INPUT		0u
#define MODER_OUTPUT		1u
#define PUPDR_UP		1u

// A two-bit field (MODER, PUPDR) of the line's pin
static uint32_t field2(uint32_t reg, uint16_t pin, uint32_t value)
{
	for (uint32_t i = 0; i < 16u; i++) {
		if (pin & (1u << i)) {
			reg = (reg & ~(3u << (2u * i))) | (value << (2u * i));
		}
	}
	return reg;
}

// Released: an input on its pull-up. Interrupts off: a read-modify-write of the same port's registers elsewhere
// (another pin's set-up) can't fall in between. The pull first, so the line has it the moment it stops driving
static void release(const stm32_kp_line_t *ln)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	ln->port->PUPDR = field2(ln->port->PUPDR, ln->pin, PUPDR_UP);
	ln->port->MODER = field2(ln->port->MODER, ln->pin, MODER_INPUT);
	__set_PRIMASK(primask);
}

// Driven low. The level goes into the output register before the pin becomes an output: the register may hold a
// one from the lines' other user, and a line driven high could meet another one's low through two keys
static void drive_low(const stm32_kp_line_t *ln)
{
	stm32_pin_write(ln->port, ln->pin, false);
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	ln->port->MODER = field2(ln->port->MODER, ln->pin, MODER_OUTPUT);
	__set_PRIMASK(primask);
}

static uint8_t lines(uint8_t n)
{
	return n < KEYPAD_MAX_LINES ? n : (uint8_t)KEYPAD_MAX_LINES;
}

void stm32_gpio_keypad_init(const stm32_gpio_keypad_cfg_t *c)
{
	for (uint8_t d = 0; d < lines(c->n_drive); d++) {
		stm32_pin_in(c->drive[d].port, c->drive[d].pin, GPIO_PULLUP);
	}
	for (uint8_t s = 0; s < lines(c->n_sense); s++) {
		stm32_pin_in(c->sense[s].port, c->sense[s].pin, GPIO_PULLUP);
	}
}

void stm32_gpio_keypad_scan(const stm32_gpio_keypad_cfg_t *c, uint8_t *row_low)
{
	uint8_t n_drive = lines(c->n_drive), n_sense = lines(c->n_sense);

	if (c->lock != NULL) {
		c->lock();
	}
	// Whatever the lines' other user left on them (its pull, an output's mode): inputs with pull-ups before the
	// first line drives
	for (uint8_t s = 0; s < n_sense; s++) {
		release(&c->sense[s]);
	}
	for (uint8_t d = 0; d < n_drive; d++) {
		release(&c->drive[d]);
	}
	for (uint8_t d = 0; d < n_drive; d++) {
		uint8_t low = 0;
		drive_low(&c->drive[d]);
		hal_delay_us(c->settle_us);
		for (uint8_t s = 0; s < n_sense; s++) {
			if (!stm32_pin_read(c->sense[s].port, c->sense[s].pin)) {
				low |= (uint8_t)(1u << s);
			}
		}
		release(&c->drive[d]);	// before the next line drives: one output at a time
		row_low[d] = low;
	}
	if (c->unlock != NULL) {
		c->unlock();
	}
}
