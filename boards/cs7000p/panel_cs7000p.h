// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS display rules, with no hardware access so that tests/cs7000pPanelTest.c runs them: the LCD board
// check, the panel type from the controller's ID with its build overrides, and the backlight's duty
// (docs/spec/hardware-cs7000p.md 9.2, 9.3). display_cs7000p.c is the one user.
//
// The LCD board is not in the maker's schematic. The OEM's 2018 LCD board is the working assumption, and the check
// below is what the firmware can see of it without driving a line. What has run on a radio is the bring-up page's
// First radio (docs/testing/cs7000p-bringup.md).
#ifndef CS7000P_PANEL_H
#define CS7000P_PANEL_H

#include <stdint.h>
#include <stdbool.h>
#include "display/st7735/id/st7735_id.h"

// Bench knobs (CS7000P_DEFS in board.mk; each a row of docs/testing/cs7000p-bringup.md)
// 1: skip the LCD board check and drive the connector whatever it reads. Only after the board has been traced
#ifndef CS7000P_LCD_FORCE
#define CS7000P_LCD_FORCE		0
#endif
// CS7000P_PANEL_TYPE (not set by default): a panel type of drivers/display/st7735, 0 to 3, instead of the ID's choice
#ifdef CS7000P_PANEL_TYPE
_Static_assert((CS7000P_PANEL_TYPE) >= 0 && (CS7000P_PANEL_TYPE) <= 3, "the display driver has panel types 0 to 3");
#define CS7000P_PANEL_BUILD_TYPE	(CS7000P_PANEL_TYPE)
#else
#define CS7000P_PANEL_BUILD_TYPE	(-1)
#endif
// Flips the controller's orientation bits on top of the type's own (0x40 columns, 0x80 rows, 0xC0 both)
#ifndef CS7000P_PANEL_MADCTL_XOR
#define CS7000P_PANEL_MADCTL_XOR	0
#endif
// The PWM duty, in percent, that 1 % and 100 % backlight map to. The LCD board turns the filtered PWM into a base
// current: dark below roughly 15 to 20 % duty and saturated well before 100 % (estimates from its drawing, not
// measured: docs/testing/backlight-curve.md)
#ifndef CS7000P_BL_MIN
#define CS7000P_BL_MIN			20
#endif
#ifndef CS7000P_BL_MAX
#define CS7000P_BL_MAX			70
#endif
_Static_assert((CS7000P_BL_MIN) >= 0 && (CS7000P_BL_MIN) <= (CS7000P_BL_MAX) && (CS7000P_BL_MAX) <= 100,
	       "the backlight's duty window is 0 <= min <= max <= 100 percent");

// ---- The LCD board check ----

// The connector's five control lines as read with the pins' pull-ups, before any of them is driven: one bit a
// line, 1 = high. Port E's two, then port D's three (board.lcd_board shows the byte as "levels")
#define CS7000P_LCD_LEVEL_RES		0x01u	// PE0: reset, 10k to ground on the assumed board
#define CS7000P_LCD_LEVEL_CS		0x02u	// PE1: chip select
#define CS7000P_LCD_LEVEL_DC		0x04u	// PD12: data / command
#define CS7000P_LCD_LEVEL_WR		0x08u	// PD13: write strobe
#define CS7000P_LCD_LEVEL_RD		0x10u	// PD14: read strobe
#define CS7000P_LCD_LEVELS_ALL		0x1Fu
// The assumed board: only the reset line reads low, held by its 10k against the pin's 40k pull-up (about 0.66 V)
#define CS7000P_LCD_LEVELS_ASSUMED	(CS7000P_LCD_LEVELS_ALL & ~CS7000P_LCD_LEVEL_RES)

static inline uint8_t cs7000p_lcd_levels(bool res, bool cs, bool dc, bool wr, bool rd)
{
	return (uint8_t)((res ? CS7000P_LCD_LEVEL_RES : 0u) | (cs ? CS7000P_LCD_LEVEL_CS : 0u) |
			 (dc ? CS7000P_LCD_LEVEL_DC : 0u) | (wr ? CS7000P_LCD_LEVEL_WR : 0u) |
			 (rd ? CS7000P_LCD_LEVEL_RD : 0u));
}

// The rule: exactly that pattern. Any other one is a board this firmware does not know, on which a line driven
// from here could meet another output: the 1 k series resistors that make that harmless are parts of the assumed
// board
static inline bool cs7000p_lcd_board_ok(uint8_t levels)
{
	return levels == CS7000P_LCD_LEVELS_ASSUMED;
}

// cs7000p_display_state_t.lcd_board. UNEXPECTED is also the state before the check has run: nothing on the
// connector is driven in it
enum { CS7000P_LCD_ASSUMED, CS7000P_LCD_UNEXPECTED, CS7000P_LCD_FORCED };

// The check's verdict from two readings a moment apart: a line that reads differently the second time is not held
// by a resistor, so the pattern proves nothing. force: the bench knob, which the levels cannot overrule
static inline uint8_t cs7000p_lcd_board(uint8_t first, uint8_t second, bool force)
{
	if (force) {
		return CS7000P_LCD_FORCED;
	}
	return cs7000p_lcd_board_ok(first) && cs7000p_lcd_board_ok(second) ? CS7000P_LCD_ASSUMED :
									     CS7000P_LCD_UNEXPECTED;
}

// Whether the display bus, the ID read, the key matrix scan, the backlight and the key lights may run
static inline bool cs7000p_lcd_may_drive(uint8_t lcd_board)
{
	return lcd_board == CS7000P_LCD_ASSUMED || lcd_board == CS7000P_LCD_FORCED;
}

// ---- The panel type ----

typedef struct {
	uint8_t type;		// the display driver's panel type, 0 to 3
	const char *controller;	// "gc9xxx", "st7735" or "unknown"
	const char *source;	// what chose the type: "id" (the controller recognized), "default" or "build"
} cs7000p_panel_t;

// id: the three bytes of the read display ID command (0x04) after its dummy byte, all zero when it was not read.
// build_type: CS7000P_PANEL_BUILD_TYPE (-1: no override). No controller is named in any document of this radio, and
// no type is stored on it: a recognized ID gets its type, any other ID the driver's type 0, reported as
// "unknown" / "default". The controller's name stays the ID's under an override, so the view shows both
static inline cs7000p_panel_t cs7000p_panel_choose(const uint8_t id[3], int16_t build_type)
{
	cs7000p_panel_t p;

	p.controller = st7735_panel_from_id(id, -1, &p.type, &p.source);
	if (build_type >= 0) {
		p.type = (uint8_t)(build_type & 3);
		p.source = "build";
	}
	return p;
}

// ---- The backlight ----

// The PWM compare value (timer clocks high in a period of `period` clocks) for a brightness of 0 to 100 percent:
// 0 is the pin low, 1 to 100 go evenly from CS7000P_BL_MIN to CS7000P_BL_MAX percent duty
static inline uint16_t cs7000p_backlight_duty(uint8_t percent, uint16_t period)
{
	if (percent == 0) {
		return 0;
	}
	if (percent > 100u) {
		percent = 100;
	}
	// In 1/99 of a percent of duty, so that 1 lands on the minimum and 100 on the maximum exactly
	uint32_t duty99 = (uint32_t)(CS7000P_BL_MIN) * 99u +
			  (uint32_t)(percent - 1u) * (uint32_t)((CS7000P_BL_MAX) - (CS7000P_BL_MIN));
	return (uint16_t)((uint32_t)period * duty99 / 9900u);
}

// The backlight tests' raw value (hal_display_t.set_backlight_raw): 0 to CS7000P_BL_RAW_MAX, the compare value at
// the usual timer clock, where a period is that many clocks. On another clock the same raw value is the same duty
#define CS7000P_BL_RAW_MAX		2400u

static inline uint16_t cs7000p_backlight_raw_duty(uint16_t raw, uint16_t period)
{
	if (raw >= CS7000P_BL_RAW_MAX) {
		return period;	// on through the whole period
	}
	return (uint16_t)((uint32_t)raw * period / CS7000P_BL_RAW_MAX);
}

#endif
