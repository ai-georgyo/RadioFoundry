// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// An 8-bit 8080 display bus on GPIO pins, for a board whose panel is not on a memory controller's pins: eight data
// lines next to each other on one port, and chip select, D/C, write strobe, read strobe and reset wherever the board
// has them. The functions are a display driver's bus callbacks as they are (drivers/display/st7735/st7735.h,
// st7735_bus_t); the board adds its own delay_ms.
//
// Between sessions the data lines are inputs, so a key matrix may share them (gpio_keypad/gpio_keypad.h, under the
// same lock). One bus per image. Until stm32_lcd8080_init() every function here does nothing and touches no pin: a
// board that found no panel it knows leaves it at that. A board adds platform/stm32/lcd8080_gpio to BOARD_DIRS.
#ifndef STM32_LCD8080_GPIO_H
#define STM32_LCD8080_GPIO_H

#include <stdint.h>
#include <stdbool.h>
#include "platform.h"

// lcd8080_gpio.h (STM32 GPIO registers: the same on both families)
typedef struct {
	GPIO_TypeDef *data_port;	// D0..D7 on eight neighboring pins
	uint8_t data_shift;		// pin number of D0
	GPIO_TypeDef *cs_port, *dc_port, *wr_port, *rd_port, *res_port;
	uint16_t cs_pin, dc_pin, wr_pin, rd_pin, res_pin;
	uint16_t wr_low_ns, wr_high_ns;	// write strobe (series resistors make the lines slow)
	uint16_t rd_low_ns, rd_high_ns;
	uint32_t idle_pull;		// GPIO_PULLUP / GPIO_PULLDOWN / GPIO_NOPULL on the data lines between sessions
	void (*lock)(void);		// the board's bus lock (recursive), shared with the key scan
	void (*unlock)(void);
} stm32_lcd8080_cfg_t;
// c stays valid for the bus's life. The strobe times are waited on the cycle counter (stm32_wait_ns), so the first
// session comes after stm32_time_init(). None of the strobes or the select sees an edge here
// The first call that drives anything: CS, WR, RD outputs high, D/C an output, RES an output LOW (the panel in
// reset), data lines inputs with idle_pull. A board that must look at the lines before driving them does so first
void stm32_lcd8080_init(const stm32_lcd8080_cfg_t *c);
// A transfer (cmd, data, pixels, read) with no session open is a failed assertion and reaches no pin, as is a
// session opened inside another: the lock is recursive, the lines are not
// The next five are a display driver's bus callbacks as they are (st7735_bus_t: begin, end, cmd, data, pixels):
void stm32_lcd8080_begin(void);		// lock, data lines to push-pull outputs, CS low
void stm32_lcd8080_end(void);		// CS high, data lines back to inputs with idle_pull, unlock
void stm32_lcd8080_cmd(uint8_t c);	// one byte with D/C low
void stm32_lcd8080_data(const uint8_t *d, uint32_t n);	// bytes with D/C high
void stm32_lcd8080_pixels(const uint16_t *px, uint32_t n, bool swap_rb);	// RGB565, high byte first
// and its reset callback: the RES pin's LEVEL (false = low = the panel in reset), the driver's meaning, not "asserted"
void stm32_lcd8080_reset(bool level);
// Each byte of a read is taken at the end of its strobe's low time. Without stm32_lcd8080_init: buf is all 0
// A read inside a session: cmd out, the data lines driven low and turned to inputs without pulls, n bytes each on a
// RD strobe (the first is the controller's dummy byte), lines back to outputs. A panel that does not drive reads 00
void stm32_lcd8080_read(uint8_t cmd, uint8_t *buf, uint8_t n);
bool stm32_lcd8080_idle(void);		// no session open (the key scan asserts it)

#endif
