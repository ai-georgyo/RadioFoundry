// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS display: a 160x128 color panel on an 8-bit 8080 bus bit-banged on GPIO
// (platform/stm32/lcd8080_gpio: data on PD0-PD7, which are also the key matrix), driven by drivers/display/st7735;
// the backlight is TIM8 CH4 PWM on PC9 (docs/spec/hardware-cs7000p.md 9), set up dark in board_early_init. Nothing
// here has run on a radio.
//
// The LCD board is not in the maker's schematic. The OEM's 2018 LCD board is the working assumption, so init()
// starts with a check: the connector's five control lines are read as inputs with pull-ups, and only the pattern of
// that board lets anything below drive a line (panel_cs7000p.h has the rule). On any other pattern the connector's
// lines stay inputs for good: no bus, no ID read, no key scan (input_cs7000p.c asks the state here), the backlight
// and the key lights stay off, and every function of this file does nothing. The radio then runs by the debug
// link, the side keys and PTT. CS7000P_LCD_FORCE=1 skips the check: a bench knob, for a board that has been traced.
//
// No document names the panel's controller. Its ID is read at every start (read display ID, 0x04, and ID1-ID3,
// 0xDA-0xDC) and picks the driver's panel type; an ID the driver does not know gets type 0, reported as
// "unknown" / "default". CS7000P_PANEL_TYPE overrides the type and CS7000P_PANEL_MADCTL_XOR the orientation. The
// size, the offsets (0) and the color order (the driver's own for the type) are assumptions until a radio shows a
// picture. The panel is powered by the switched battery rail, so it gets a full init at every start.
#include "cs7000p.h"
#include "config.h"
#include "pins.h"
#include "hal_time.h"
#include "debuglink/dlog.h"
#include "exp_cs7000p.h"
#include "panel_cs7000p.h"
#include "keys_cs7000p.h"
#include "lcd8080_gpio/lcd8080_gpio.h"
#include "periph/pwm.h"
#include "display/st7735/st7735.h"
#include "FreeRTOS.h"

#define VISIBLE_W	160
#define VISIBLE_H	128

// ---- The bus ----

_Static_assert(PIN_MASK(PIN_LCD_D0) == GPIO_PIN_0, "D0 is pin 0 of its port: data_shift below");

// Every line passes 1 k on the LCD board into the pin and trace capacitance, so the strobes are held far longer
// than a controller of this class asks: 100 ns each way for a write, 1000 ns for a read (only the ID is read, once).
// Between sessions the data lines are inputs on their pull-ups: the state the key scan wants. Kept by pointer
static const stm32_lcd8080_cfg_t bus_cfg = {
	.data_port = PIN_PORT(PIN_LCD_D0),
	.data_shift = 0,
	.cs_port = PIN_PORT(PIN_LCD_CS), .cs_pin = PIN_MASK(PIN_LCD_CS),
	.dc_port = PIN_PORT(PIN_LCD_DC), .dc_pin = PIN_MASK(PIN_LCD_DC),
	.wr_port = PIN_PORT(PIN_LCD_WR), .wr_pin = PIN_MASK(PIN_LCD_WR),
	.rd_port = PIN_PORT(PIN_LCD_RD), .rd_pin = PIN_MASK(PIN_LCD_RD),
	.res_port = PIN_PORT(PIN_LCD_RES), .res_pin = PIN_MASK(PIN_LCD_RES),
	.wr_low_ns = 100, .wr_high_ns = 100,
	.rd_low_ns = 1000, .rd_high_ns = 1000,
	.idle_pull = GPIO_PULLUP,
	.lock = cs7000p_lcdbus_lock,
	.unlock = cs7000p_lcdbus_unlock,
};

// Until stm32_lcd8080_init() has run, which only init() below calls and only behind the board check, every one of
// these does nothing and touches no pin
static const st7735_bus_t bus = {
	.begin = stm32_lcd8080_begin,
	.end = stm32_lcd8080_end,
	.cmd = stm32_lcd8080_cmd,
	.data = stm32_lcd8080_data,
	.pixels = stm32_lcd8080_pixels,
	.reset = stm32_lcd8080_reset,
	.delay_ms = hal_delay_ms,
};

static st7735_t panel = {
	.bus = &bus,
	.width = VISIBLE_W,
	.height = VISIBLE_H,
	.col_offset = 0,
	.row_offset = 0,
	.madctl_xor = CS7000P_PANEL_MADCTL_XOR,
};

// Unexpected until the check has passed: the state in which nothing is driven
static uint8_t lcd_board = CS7000P_LCD_UNEXPECTED;
static uint8_t lcd_levels;
static bool ready;		// the bus is up and the panel initialized
static hal_display_panel_t panel_id;

// ---- The LCD board check ----

static uint8_t read_levels(void)
{
	return cs7000p_lcd_levels(PIN_READ(PIN_LCD_RES), PIN_READ(PIN_LCD_CS), PIN_READ(PIN_LCD_DC),
				  PIN_READ(PIN_LCD_WR), PIN_READ(PIN_LCD_RD));
}

// The five control lines as inputs with pull-ups, read twice. They have been in that state since the pin set-up
// (pinsetup_cs7000p.c); it is set again here so that the check does not rest on another file. No line is driven
static uint8_t board_check(void)
{
	stm32_pin_in(PIN_LCD_RES, GPIO_PULLUP);
	stm32_pin_in(PIN_LCD_CS, GPIO_PULLUP);
	stm32_pin_in(PIN_LCD_DC, GPIO_PULLUP);
	stm32_pin_in(PIN_LCD_WR, GPIO_PULLUP);
	stm32_pin_in(PIN_LCD_RD, GPIO_PULLUP);
	hal_delay_ms(1);	// a pull-up into the line's capacitance takes microseconds
	uint8_t first = read_levels();
	hal_delay_ms(1);
	uint8_t second = read_levels();
	// What the view shows: the reading that fails the rule, if one does
	lcd_levels = cs7000p_lcd_board_ok(first) ? second : first;
	return cs7000p_lcd_board(first, second, CS7000P_LCD_FORCE != 0);
}

// ---- Panel ID: read display ID (0x04) and ID1-ID3 (0xDA-0xDC), each answered after a dummy byte ----

// A key that is down joins two data lines through 2 k while the panel drives each through 1 k: two bits that differ
// then sit near the input thresholds. So the matrix is scanned first, and a held key is given this long to come up
#define KEYS_UP_WAIT_MS		500u
#define KEYS_UP_POLL_MS		10u

static bool wait_keys_up(void)
{
	for (uint32_t t = 0; cs7000p_matrix_key_down(); t += KEYS_UP_POLL_MS) {
		if (t >= KEYS_UP_WAIT_MS) {
			return false;
		}
		hal_delay_ms(KEYS_UP_POLL_MS);
	}
	return true;
}

static void read_panel_id(void)
{
	uint8_t b[4];

	// One session a command: the bus is not recursive, and a new command ends the read before it
	stm32_lcd8080_begin();
	stm32_lcd8080_read(0x04, b, 4);
	stm32_lcd8080_end();
	for (uint32_t i = 0; i < 3; i++) {
		panel_id.id[i] = b[1 + i];
	}
	for (uint32_t i = 0; i < 3; i++) {
		stm32_lcd8080_begin();
		stm32_lcd8080_read((uint8_t)(0xDA + i), b, 2);
		stm32_lcd8080_end();
		panel_id.id_reg[i] = b[1];
	}
	panel_id.id_read = true;
}

// ---- Backlight: TIM8 CH4 PWM on PC9 (AF3), 40 kHz: far above the two 72 Hz filter stages behind the pin ----

// The LCD board turns the filtered level into the base current of the backlight's transistor, so the light follows
// the duty only inside a window (panel_cs7000p.h, CS7000P_BL_MIN / _MAX). The key lights (EXP_LAMP, on or off) follow
// the backlight being on
#define BL_TIMER	TIM8
#define BL_CHANNEL	4u
#define BL_PWM_HZ	40000u

static uint16_t bl_period;	// timer clocks in a PWM period; 0: the PWM is not set up
static uint16_t bl_duty;
static bool lamp_on;

// Once, from board_early_init(), before the scheduler. Giving the pin to the timer is a mode write that is not
// atomic against another task's on the same port, and the GPS receiver's and the audio stream's pins are on it:
// before any task exists nothing can write port C's modes in between, whatever the tasks' order later. The channel
// starts at duty 0, a constant low, which is what the pin was as an output: nothing on the connector changes, and on
// a board that fails the check (init below) the level stays 0 for good
void cs7000p_backlight_init(void)
{
	// From the timer's clock as it is: on the platform's fallback clock tree it is not the usual 96 MHz
	uint32_t period = stm32_pwm_timer_hz(BL_TIMER) / BL_PWM_HZ;
	bl_period = 0;
	bl_duty = 0;
	if (period < 2u || period > 0xFFFFu) {
		return;		// no clock tree of the platform gives this: the pin stays a low output
	}
	bl_period = (uint16_t)period;
	stm32_pwm_init(BL_TIMER, BL_CHANNEL, PIN_LCD_BACKLIGHT, GPIO_AF3_TIM8, bl_period, 0);
}

// Two tasks set the level: the UI, and the start task's power-off. The level, the key lights' state and the lamp
// bit change in one masked section, so the last caller wins for all three and the lights never stay on under a dark
// panel. Short: a register write and, on a change, one expander update
static void bl_apply(uint16_t duty)
{
	if (!ready || bl_period == 0) {
		return;
	}
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	bl_duty = duty;
	stm32_pwm_set(BL_TIMER, BL_CHANNEL, duty);
	bool on = duty != 0;
	if (on != lamp_on) {
		lamp_on = on;
		// Never refused: no transmit bit is in it
		(void)cs7000p_exp_set(on ? EXP_LAMP : 0u, on ? 0u : EXP_LAMP);
	}
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
}

// 0 = off (the pin low), 1 to 100: across the duty window
static void set_backlight(uint8_t percent)
{
	bl_apply(cs7000p_backlight_duty(percent, bl_period));
}

#if CONFIG_BACKLIGHT_TEST
// The backlight tests (docs/testing/backlight-curve.md): the duty straight, in steps of CS7000P_BL_RAW_MAX a period
static void set_backlight_raw(uint16_t value)
{
	bl_apply(cs7000p_backlight_raw_duty(value, bl_period));
}

static uint16_t backlight_raw(void)
{
	if (bl_period == 0) {
		return 0;
	}
	return bl_duty >= bl_period ? (uint16_t)CS7000P_BL_RAW_MAX :
				      (uint16_t)((uint32_t)bl_duty * CS7000P_BL_RAW_MAX / bl_period);
}
#endif

// ---- hal_display_t ----

static void init(void)
{
	// 1. The board check, before anything on the connector is driven
	ready = false;
	panel_id = (hal_display_panel_t){ 0 };
	lcd_board = board_check();
	if (!cs7000p_lcd_may_drive(lcd_board)) {
		// Not the assumed board: the bus and matrix lines stay inputs, the backlight and LAMP stay low, and
		// ready stays false
		dlog("display: LCD board unexpected (levels %02x), not driven", lcd_levels);
		return;
	}

	// 2. The bus: reset low first, then the strobes and the select at rest. The backlight's PWM has run dark since
	// board_early_init (cs7000p_backlight_init); from here on set_backlight moves its level
	stm32_lcd8080_init(&bus_cfg);

	// 3. The IDs straight after a hardware reset, before any controller's init has run, with no key down
	hal_delay_ms(20);
	stm32_lcd8080_reset(true);
	hal_delay_ms(120);
	if (!wait_keys_up()) {
		dlog("display: a key is held, the panel ID may read wrong");
	}
	read_panel_id();

	cs7000p_panel_t found = cs7000p_panel_choose(panel_id.id, CS7000P_PANEL_BUILD_TYPE);
	panel.type = found.type;
	panel_id.controller = found.controller;
	panel_id.source = found.source;

	// 4. The driver's init for that type: an unknown controller gets the standard one
	st7735_init(&panel);
	ready = true;
}

static void flush(const void *fb, const uint16_t *palette, uint16_t y0, uint16_t y1)
{
	if (!ready || fb == NULL) {
		return;
	}
	st7735_write_rows_indexed(&panel, (const uint8_t *)fb, palette, y0, y1);
}

static void panel_info(hal_display_panel_t *p)
{
	*p = panel_id;
	p->factory = -1;	// no panel type is stored on this radio
	p->type = panel.type;
}

void cs7000p_display_state(cs7000p_display_state_t *s)
{
	s->lcd_levels = lcd_levels;
	s->lcd_board = lcd_board;
	panel_info(&s->panel);
}

const hal_display_t cs7000p_display = {
	.inset_top = CONFIG_DISPLAY_INSET_TOP, .inset_bottom = CONFIG_DISPLAY_INSET_BOTTOM,
	.inset_left = CONFIG_DISPLAY_INSET_LEFT, .inset_right = CONFIG_DISPLAY_INSET_RIGHT,
	.width = VISIBLE_W,
	.height = VISIBLE_H,
	.bpp = 4,
	.init = init,
	.flush_indexed = flush,
	.set_backlight = set_backlight,
#if CONFIG_BACKLIGHT_TEST
	.backlight_raw_max = CS7000P_BL_RAW_MAX,
	.set_backlight_raw = set_backlight_raw,
	.backlight_raw = backlight_raw,
#endif
	.panel = panel_info,
};
