// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS key tables and input rules, with no hardware access so that tests/cs7000pKeysTest.c runs them: the
// key matrix as the LCD board's drawing has it, the order of its six unlabeled function switches, the channel
// selector's code table, and the small rules input_cs7000p.c applies to them (docs/spec/hardware-cs7000p.md 9.4).
//
// The matrix is on the LCD board, which is not in the maker's schematic: the OEM's 2018 LCD board is the working
// assumption. Which function switch is which key is in no document: CS7000P_FN_KEYS is the order read on a radio
// (board.keys_raw, one key at a time), and so is the channel selector's code table (board.chsw_raw, one detent at a
// time; clockwise counts up, confirmed on the radio).
#ifndef CS7000P_KEYS_H
#define CS7000P_KEYS_H

#include <stdint.h>
#include <stdbool.h>
#include "cs7000p.h"
#include "input/keymatrix/keymatrix.h"

// 1: cs7000p_chsw_position below holds a measured table and the selector makes steps. 0 (CS7000P_DEFS in board.mk)
// turns the selector off again. board.c reads it too, for CAP_ENCODER
#ifndef CS7000P_CHSW_VERIFIED
#define CS7000P_CHSW_VERIFIED	1
#endif

// ---- The key matrix ----

// Five lines driven low one at a time, four sensed with pull-ups. In scan order (the order of
// cs7000p_input_state_t.rows and of board.keys_raw's five bytes): drive K5 (PD4), K6 (PD5), K7 (PD6), K8 (PD7),
// K9 (PB7); bit n of a row is sense line K(n+1): K1 (PD0), K2 (PD1), K3 (PD2), K4 (PD3)
#define CS7000P_KEY_DRIVES	5u
#define CS7000P_KEY_SENSES	4u

// A function switch of the drawing, 1 to 6, in the table below: the drawing gives them no legend
#define CS7000P_FN_SW(n)	((uint8_t)(0x80u | (n)))
#define CS7000P_FN_SWITCHES	6u

// [drive][sense], from the 2018 LCD board drawing
static const uint8_t cs7000p_matrix[CS7000P_KEY_DRIVES][CS7000P_KEY_SENSES] = {
	//  K1 (PD0)         K2 (PD1)          K3 (PD2)          K4 (PD3)
	{ CS7000P_FN_SW(1), CS7000P_FN_SW(2), CS7000P_FN_SW(3), CS7000P_FN_SW(4) },	// K5 (PD4)
	{ KEY_3,            KEY_6,            KEY_9,            KEY_HASH },		// K6 (PD5)
	{ KEY_2,            KEY_5,            KEY_8,            KEY_0 },		// K7 (PD6)
	{ KEY_1,            KEY_4,            KEY_7,            KEY_STAR },		// K8 (PD7)
	{ CS7000P_FN_SW(5), CS7000P_FN_SW(6), KEYMATRIX_NONE,   KEYMATRIX_NONE },	// K9 (PB7)
};

// The key behind function switch 1 to 6. The radio's six keys are F1, Up, F2 over OK, Down, BACK, and the drawing
// does not say which switch each one is. Measured on a radio, each key held alone (board.keys_raw, 2026-11-22):
// OK 01 00 00 00 00, BACK 02 .., F1 04 .., F2 08 .. (drive K5, senses K1 to K4); Up .. 01, Down .. 02 (drive K9)
static const uint8_t CS7000P_FN_KEYS[CS7000P_FN_SWITCHES] = { KEY_OK, KEY_BACK, KEY_F1, KEY_F2, KEY_UP, KEY_DOWN };

// The table for keymatrix_decode (row major: drive x sense), the function switches resolved through the order above
static inline void cs7000p_keys_map(uint8_t map[CS7000P_KEY_DRIVES * CS7000P_KEY_SENSES])
{
	for (uint8_t d = 0; d < CS7000P_KEY_DRIVES; d++) {
		for (uint8_t s = 0; s < CS7000P_KEY_SENSES; s++) {
			uint8_t k = cs7000p_matrix[d][s];
			if (k != KEYMATRIX_NONE && (k & 0x80u) != 0) {
				uint8_t sw = (uint8_t)(k & 0x7Fu);
				k = sw >= 1u && sw <= CS7000P_FN_SWITCHES ? CS7000P_FN_KEYS[sw - 1u] : KEYMATRIX_NONE;
			}
			map[d * CS7000P_KEY_SENSES + s] = k;
		}
	}
}

// A change counts once two scans in a row agree: the matrix has no hardware debounce. Returns the settled keys
static inline uint32_t cs7000p_keys_settle(uint32_t raw, uint32_t *last, uint32_t *settled)
{
	if (raw == *last) {
		*settled = raw;
	}
	*last = raw;
	return *settled;
}

// ---- The channel selector ----

// A 16-code switch on four lines. The code as read, and as board.chsw_raw shows it: bit n is line ECNn's level
// (PE14, PE15, PB10, PB11), so an open contact is a 1 and a closed one a 0
#define CS7000P_CHSW_CODES	16u

// Position by code, 0 to 15, numbered from any detent (the switch turns endlessly); -1: no position. Which code each
// detent gives is in no document. Read on a radio, one detent at a time over a whole turn (board.chsw_raw,
// 2026-11-22): F D C 8 9 1 0 4 5 7 6 2 3 B A E, a Gray code (one line changes per detent, the last back to the
// first too). Position n is the n-th code of that list, from 0. The list runs clockwise: with it the channel goes
// up when the selector is turned clockwise (confirmed on the radio)
static const int8_t cs7000p_chsw_position[CS7000P_CHSW_CODES] = {
	// code 0  1   2   3  4  5   6  7  8  9   A   B  C  D   E  F
	6, 5, 11, 12, 7, 8, 10, 9, 3, 4, 14, 13, 2, 1, 15, 0,
};

// A usable table: every position 0 to 15 exactly once
static inline bool cs7000p_chsw_table_ok(const int8_t *position)
{
	uint32_t seen = 0;

	for (uint8_t c = 0; c < CS7000P_CHSW_CODES; c++) {
		if (position[c] < 0 || position[c] >= (int8_t)CS7000P_CHSW_CODES || (seen & (1u << position[c])) != 0) {
			return false;
		}
		seen |= 1u << position[c];
	}
	return true;
}

// Detents from one settled position to the next, the shorter way round the 16: clockwise is positive, the core's
// "forward". Half a turn at once has no shorter way and counts as clockwise
static inline int8_t cs7000p_chsw_steps(int8_t from, int8_t to)
{
	int8_t d = (int8_t)((to - from) & 0x0F);
	return d > 8 ? (int8_t)(d - 16) : d;
}

// ---- The accessory connector's PTT line ----

// The line counts as a press only once it has read high (drivers/input/plugptt): the jack may hold a plug that
// keeps it low. A line that has been low this long without ever counting is reported as stuck
// (hal_input_t.ptt_line_stuck): the accessory's PTT does nothing until the line has been released once
#define CS7000P_EXT_PTT_STUCK_MS	2000u

typedef struct {
	uint32_t low_ms;	// when the line was first read low while it could not count; valid while timing
	bool timing;
} cs7000p_ext_stuck_t;

// pin_low: the line as read now. armed: it has read high long enough to count (plug_ptt_t.armed)
static inline bool cs7000p_ext_ptt_stuck(cs7000p_ext_stuck_t *s, bool pin_low, bool armed, uint32_t now_ms)
{
	if (!pin_low || armed) {
		s->timing = false;
		return false;
	}
	if (!s->timing) {
		s->timing = true;
		s->low_ms = now_ms;
	}
	return (uint32_t)(now_ms - s->low_ms) >= CS7000P_EXT_PTT_STUCK_MS;
}

// input_cs7000p.c, for the display's ID read: one scan of the matrix now. True: a key is down, and a read of the
// display's data lines could be wrong (a key joins two of them through 2 k). False without a scan on an LCD board
// that is not the assumed one
bool cs7000p_matrix_key_down(void);

#endif
