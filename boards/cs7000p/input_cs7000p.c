// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS keys: the front key matrix, the three side keys, the top key, PTT, the accessory connector's PTT
// and the channel selector (docs/spec/hardware-cs7000p.md 9.4). Nothing here has run on a radio.
//
// The matrix is on the LCD board, on the display's data lines PD0-PD7 plus PB7 (platform/stm32/gpio_keypad: a line
// is driven low or released, never high; drivers/input/keymatrix decodes). It is scanned under the LCD bus lock and
// only while no display session is open. That board is not in the maker's schematic: the matrix is scanned only if
// the display's board check passed (display_cs7000p.c), because a scan drives the connector's lines. On any other
// board the scan never runs and the keys below are all the radio has. Which of the matrix's six unlabeled function
// switches is which key was read on a radio (keys_cs7000p.h).
//
// The side keys, the top key and PTT have a pin each, low = pressed, with a pull-up on the main board. The
// accessory connector's PTT counts only once its line has read high (drivers/input/plugptt): the jack may be empty
// or hold a plug that keeps the line low.
//
// The channel selector is a 16-code switch on four lines whose code is in no document. Its lines are read and shown
// (board.chsw_raw); it makes steps only in a build with the measured table and CS7000P_CHSW_VERIFIED=1.
#include <string.h>
#include "cs7000p.h"
#include "pins.h"
#include "hal_time.h"
#include "keys_cs7000p.h"
#include "panel_cs7000p.h"
#include "lcd8080_gpio/lcd8080_gpio.h"
#include "gpio_keypad/gpio_keypad.h"
#include "input/keymatrix/keymatrix.h"
#include "input/chswitch/chswitch.h"
#include "input/plugptt/plug_ptt.h"

// ---- The matrix ----

// Between a line going low and the read: the driven line's fall, and the rise of the line released before it on
// its pull-up (about 40 k into the line's capacitance, a few microseconds). Unmeasured
#define KEY_SETTLE_US	20u
// Scans in a row that may be skipped for an open display session before one waits for the bus
#define MATRIX_SKIP_MAX	3u

// The display's data line n: D0-D7 are eight neighboring pins from PIN_LCD_D0 up
#define DATA_LINE(n)	{ PIN_PORT(PIN_LCD_D0), (uint16_t)(PIN_MASK(PIN_LCD_D0) << (n)) }

// K5 to K9, driven low one at a time, and K1 to K4, sensed on their pull-ups: the order keys_cs7000p.h's table has
static const stm32_kp_line_t drive_lines[CS7000P_KEY_DRIVES] = {
	DATA_LINE(4), DATA_LINE(5), DATA_LINE(6), DATA_LINE(7), { PIN_KEY_K9 },
};
static const stm32_kp_line_t sense_lines[CS7000P_KEY_SENSES] = {
	DATA_LINE(0), DATA_LINE(1), DATA_LINE(2), DATA_LINE(3),
};

static const stm32_gpio_keypad_cfg_t keypad = {
	.drive = drive_lines,
	.sense = sense_lines,
	.n_drive = CS7000P_KEY_DRIVES,
	.n_sense = CS7000P_KEY_SENSES,
	.settle_us = KEY_SETTLE_US,
	.lock = cs7000p_lcdbus_lock,
	.unlock = cs7000p_lcdbus_unlock,
};

static uint8_t matrix_map[CS7000P_KEY_DRIVES * CS7000P_KEY_SENSES];
static const keymatrix_t matrix = {
	.rows = CS7000P_KEY_DRIVES, .cols = CS7000P_KEY_SENSES, .map = matrix_map, .direct = NULL,
};

static bool matrix_on;		// the LCD board passed its check: the scan runs
static uint8_t rows[CS7000P_KEY_DRIVES];	// the last scan, for the views
static uint32_t matrix_last, matrix_keys;
static uint8_t matrix_skipped;
static uint32_t direct_last, direct_keys;
static plug_ptt_t ext_ptt;
static cs7000p_ext_stuck_t ext_stuck;
static bool ext_is_stuck;
static chswitch_t chsw;
static uint8_t chsw_raw;
static int8_t chsw_pos = -1;	// -1: no position (no table, or not settled yet)

// The display's verdict on the LCD board. Before its check has run it is "unexpected": nothing is scanned
static bool lcd_board_usable(void)
{
	cs7000p_display_state_t d;
	cs7000p_display_state(&d);
	return cs7000p_lcd_may_drive(d.lcd_board);
}

// One scan, under the bus lock and only with no display session open: the lines are the display's bus then. The
// lock is recursive, so the scan's own lock nests. False: not scanned
static bool matrix_scan(uint8_t *row_low)
{
	cs7000p_lcdbus_lock();
	bool idle = stm32_lcd8080_idle();
	if (idle) {
		stm32_gpio_keypad_scan(&keypad, row_low);
	}
	cs7000p_lcdbus_unlock();
	return idle;
}

// The display's init calls this before init() below has run. A scan needs nothing from it: it sets every line's mode
// and pull itself, and the ports' clocks are on since the pin set-up
bool cs7000p_matrix_key_down(void)
{
	uint8_t r[CS7000P_KEY_DRIVES];

	if (!lcd_board_usable()) {
		return false;	// no matrix this firmware knows, and no scan
	}
	if (!matrix_scan(r)) {
		return true;	// not known: the caller must not take the lines for free
	}
	for (uint8_t d = 0; d < CS7000P_KEY_DRIVES; d++) {
		if (r[d] != 0) {
			return true;
		}
	}
	return false;
}

static uint32_t matrix_poll(void)
{
	uint8_t r[CS7000P_KEY_DRIVES];

	if (!matrix_on) {
		return 0;
	}
	// While the display is being written, waiting for the bus would hold the side keys and PTT up for a whole
	// flush: the matrix keeps its last state and is read at the next scan. Not for long: after a few skipped
	// scans this one waits its turn
	if (!stm32_lcd8080_idle() && matrix_skipped < MATRIX_SKIP_MAX) {
		matrix_skipped++;
		return matrix_keys;
	}
	if (!matrix_scan(r)) {
		return matrix_keys;
	}
	matrix_skipped = 0;
	memcpy(rows, r, sizeof(rows));
	return cs7000p_keys_settle(keymatrix_decode(&matrix, 0, r), &matrix_last, &matrix_keys);
}

// ---- Keys with a line of their own ----

static uint32_t direct_poll(uint32_t now)
{
	uint32_t raw = 0;

	if (!PIN_READ(PIN_KEY_SK1)) {
		raw |= 1u << KEY_SK1;
	}
	if (!PIN_READ(PIN_KEY_SK2)) {
		raw |= 1u << KEY_SK2;
	}
	if (!PIN_READ(PIN_KEY_SK3)) {
		raw |= 1u << KEY_SK3;
	}
	if (!PIN_READ(PIN_KEY_TOP)) {
		raw |= 1u << KEY_TOP;
	}
	bool ext_low = !PIN_READ(PIN_EXT_PTT);
	bool ext = plug_ptt_pressed(&ext_ptt, ext_low, now);
	ext_is_stuck = cs7000p_ext_ptt_stuck(&ext_stuck, ext_low, ext_ptt.armed, now);
	if (!PIN_READ(PIN_PTT) || ext) {
		raw |= 1u << KEY_PTT;
	}
	return cs7000p_keys_settle(raw, &direct_last, &direct_keys);
}

// ---- The channel selector ----

// A code counts once it has read the same for this long: the contacts do not all change at once between two
// detents (each line has 0.47 ms of RC behind it; the switch's own bounce is not measured)
#define CHSW_SETTLE_MS	30u

#if CS7000P_CHSW_VERIFIED
static bool chsw_steps_on;	// the table is one a position can be trusted from
static int8_t chsw_last = -1;
static int32_t enc_count;
#endif

// The four lines as a number: bit n is line ECNn's level
static uint8_t chsw_read(void)
{
	return (uint8_t)((PIN_READ(PIN_CHSW_0) ? 1u : 0u) | (PIN_READ(PIN_CHSW_1) ? 2u : 0u) |
			 (PIN_READ(PIN_CHSW_2) ? 4u : 0u) | (PIN_READ(PIN_CHSW_3) ? 8u : 0u));
}

static void chsw_poll(uint32_t now)
{
	chsw_raw = chsw_read();
	// With the table as shipped no code is a position, and this stays -1
	int8_t pos = chswitch_poll(&chsw, chsw_raw, now);
#if CS7000P_CHSW_VERIFIED
	if (chsw_steps_on && pos >= 0) {
		if (chsw_last >= 0) {
			enc_count += cs7000p_chsw_steps(chsw_last, pos);
		}
		chsw_last = pos;
	}
#endif
	chsw_pos = pos;
}

#if CS7000P_CHSW_VERIFIED
static int8_t encoder(void)
{
	int32_t n = enc_count;
	if (n > 127) {
		n = 127;
	} else if (n < -127) {
		n = -127;
	}
	enc_count -= n;
	return (int8_t)n;
}
#endif

// ---- hal_input_t ----

// The keys' and the selector's pins are inputs since the pin set-up (pinsetup_cs7000p.c), their pull-ups on the
// main board: nothing to set up for them
static void init(void)
{
	uint32_t now = hal_millis();

	cs7000p_keys_map(matrix_map);
	memset(rows, 0, sizeof(rows));
	matrix_last = matrix_keys = 0;
	matrix_skipped = 0;
	direct_last = direct_keys = 0;
	ext_ptt = (plug_ptt_t){ 0 };
	ext_stuck = (cs7000p_ext_stuck_t){ 0 };
	ext_is_stuck = false;
	// The matrix only on the LCD board the display's check passed (its init has run: the core's order). The lines'
	// set-up drives nothing; under the lock, because it writes the mode of the port the display's bus is on
	matrix_on = lcd_board_usable();
	if (matrix_on) {
		cs7000p_lcdbus_lock();
		stm32_gpio_keypad_init(&keypad);
		cs7000p_lcdbus_unlock();
	}
	chsw_raw = chsw_read();
	chsw_pos = -1;
	chswitch_init(&chsw, cs7000p_chsw_position, CS7000P_CHSW_CODES, CHSW_SETTLE_MS, chsw_raw, now);
#if CS7000P_CHSW_VERIFIED
	chsw_steps_on = cs7000p_chsw_table_ok(cs7000p_chsw_position);
	chsw_last = -1;
	enc_count = 0;
#endif
}

// Called every 10 ms. The matrix and the keys with their own line settle apart: a matrix scan can be skipped
static uint32_t scan(void)
{
	uint32_t now = hal_millis();
	uint32_t keys = matrix_poll() | direct_poll(now);

	chsw_poll(now);
	return keys;
}

// The accessory's PTT line has been low since before it could count: its PTT does nothing until it is released
static bool ptt_line_stuck(void)
{
	return ext_is_stuck;
}

void cs7000p_input_state(cs7000p_input_state_t *s)
{
	memcpy(s->rows, rows, sizeof(s->rows));
	s->chsw_raw = chsw_raw;
	s->chsw_pos = chsw_pos;
	s->ext_ptt_armed = ext_ptt.armed;
	s->matrix_on = matrix_on;
}

// No .encoder until the selector's table is measured: the radio then has no channel knob as far as the core knows.
// The selector turns endlessly, so encoder_limited stays false
const hal_input_t cs7000p_input = {
	.init = init,
	.scan = scan,
#if CS7000P_CHSW_VERIFIED
	.encoder = encoder,
#endif
	.ptt_line_stuck = ptt_line_stuck,
};
