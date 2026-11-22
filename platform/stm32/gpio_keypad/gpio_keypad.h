// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A key matrix on GPIO lines, any pins of any ports, which may also be a bus between scans (a display's data lines,
// lcd8080_gpio/lcd8080_gpio.h): the scan runs under the bus's lock and leaves every line an input. Open-drain style:
// a driven line is low or released, never high, so two keys held together never join two outputs, and a line that is
// someone else's output at another time is never driven against. The scan gives the raw rows; the keys come from
// drivers/input/keymatrix (keymatrix_decode). A board adds platform/stm32/gpio_keypad to BOARD_DIRS.
//
// A matrix whose rows are driven high into a memory controller's data lines is another circuit:
// platform/stm32f4/fsmc_keypad.
#ifndef STM32_GPIO_KEYPAD_H
#define STM32_GPIO_KEYPAD_H

#include <stdint.h>
#include <stdbool.h>
#include "platform.h"

typedef struct { GPIO_TypeDef *port; uint16_t pin; } stm32_kp_line_t;
typedef struct {
	const stm32_kp_line_t *drive;	// driven low one at a time, released (input, pull-up) otherwise
	const stm32_kp_line_t *sense;	// inputs with pull-ups; low = the key at (drive, sense) is down
	uint8_t n_drive, n_sense;	// at most 8 each
	uint16_t settle_us;
	void (*lock)(void);		// optional: the bus lock when the lines are shared
	void (*unlock)(void);
} stm32_gpio_keypad_cfg_t;
// Every line an input with its pull-up, and its port's clock on. Not under the lock: before the lines' other user
// starts, or with the bus idle
void stm32_gpio_keypad_init(const stm32_gpio_keypad_cfg_t *c);
// settle_us is waited (busy, under the lock) between a line going low and the read: long enough for the line before
// it to rise again on its pull-up, the slowest edge of a scan. Lines past the eighth are left alone
// row_low[d] = the sense lines read low with drive line d low (bit s). All lines are inputs with pull-ups on return
void stm32_gpio_keypad_scan(const stm32_gpio_keypad_cfg_t *c, uint8_t *row_low);

#endif
