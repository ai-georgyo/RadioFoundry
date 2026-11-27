// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS's key tables (keys_cs7000p.h) under the shared decoders (drivers/input/keymatrix, chswitch):
// every crossing of the matrix gives the key the LCD board's drawing has there, written out below by line name; no
// key is in the matrix twice; the six function switches follow exactly the documented, measured order; the
// channel selector's table is the one read on a radio, a Gray code that steps once per detent both ways, and the
// rules that turn a table into steps are right on a synthetic one too; the accessory PTT's stuck-line rule.
#include <stdio.h>
#include <string.h>
#include "cs7000p.h"
#include "keys_cs7000p.h"
#include "input/keymatrix/keymatrix.h"
#include "input/chswitch/chswitch.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// ---- The matrix ----

// The drawing's table, by the connector's line names: the sense line (K1 to K4), the driven line (K5 to K9), and
// what sits at the crossing: a key's legend, or the number of an unlabeled function switch
typedef struct { uint8_t sense_k, drive_k; char legend; uint8_t fn; } crossing_t;

static const crossing_t drawing[] = {
	{ 1, 8, '1', 0 }, { 1, 7, '2', 0 }, { 1, 6, '3', 0 }, { 1, 5, 0, 1 }, { 1, 9, 0, 5 },
	{ 2, 8, '4', 0 }, { 2, 7, '5', 0 }, { 2, 6, '6', 0 }, { 2, 5, 0, 2 }, { 2, 9, 0, 6 },
	{ 3, 8, '7', 0 }, { 3, 7, '8', 0 }, { 3, 6, '9', 0 }, { 3, 5, 0, 3 }, { 3, 9, 0, 0 },
	{ 4, 8, '*', 0 }, { 4, 7, '0', 0 }, { 4, 6, '#', 0 }, { 4, 5, 0, 4 }, { 4, 9, 0, 0 },
};

// The order of the function switches as measured on a radio and documented (hardware-cs7000p.md 9.4)
static const uint8_t documented_fn[6] = { KEY_OK, KEY_BACK, KEY_F1, KEY_F2, KEY_UP, KEY_DOWN };

static uint8_t key_of_legend(char c)
{
	if (c >= '0' && c <= '9') {
		return (uint8_t)(KEY_0 + (c - '0'));
	}
	return c == '*' ? KEY_STAR : c == '#' ? KEY_HASH : KEYMATRIX_NONE;
}

static void test_matrix(void)
{
	uint8_t map[CS7000P_KEY_DRIVES * CS7000P_KEY_SENSES];
	const keymatrix_t m = { .rows = CS7000P_KEY_DRIVES, .cols = CS7000P_KEY_SENSES, .map = map, .direct = NULL };
	uint32_t seen = 0;
	unsigned keys = 0;

	CHECK(CS7000P_KEY_DRIVES == 5 && CS7000P_KEY_SENSES == 4);
	CHECK(sizeof(drawing) / sizeof(drawing[0]) == CS7000P_KEY_DRIVES * CS7000P_KEY_SENSES);
	memset(map, 0xA5, sizeof(map));
	cs7000p_keys_map(map);

	for (unsigned i = 0; i < sizeof(drawing) / sizeof(drawing[0]); i++) {
		const crossing_t *x = &drawing[i];
		// The scan's order: drive index 0..4 = K5..K9, sense bit 0..3 = K1..K4
		uint8_t rows[CS7000P_KEY_DRIVES] = { 0 };
		rows[x->drive_k - 5] = (uint8_t)(1u << (x->sense_k - 1));
		uint32_t got = keymatrix_decode(&m, 0, rows);

		uint8_t want = x->fn != 0 ? documented_fn[x->fn - 1] : key_of_legend(x->legend);
		if (want == KEYMATRIX_NONE) {
			CHECK(got == 0);	// no switch at this crossing
			continue;
		}
		CHECK(got == (1u << want));
		CHECK((seen & got) == 0);	// no key at two crossings
		seen |= got;
		keys++;
	}
	// The 18 switches of the drawing: twelve with a legend, six without
	CHECK(keys == 18);
	const uint32_t digits = (1u << KEY_0) | (1u << KEY_1) | (1u << KEY_2) | (1u << KEY_3) | (1u << KEY_4) |
				(1u << KEY_5) | (1u << KEY_6) | (1u << KEY_7) | (1u << KEY_8) | (1u << KEY_9) |
				(1u << KEY_STAR) | (1u << KEY_HASH);
	const uint32_t fn = (1u << KEY_F1) | (1u << KEY_F2) | (1u << KEY_OK) | (1u << KEY_BACK) | (1u << KEY_UP) |
			    (1u << KEY_DOWN);
	CHECK(seen == (digits | fn));
	// The keys with a line of their own are not in the matrix
	CHECK((seen & ((1u << KEY_SK1) | (1u << KEY_SK2) | (1u << KEY_SK3) | (1u << KEY_TOP) | (1u << KEY_PTT))) == 0);
	// With them every key bit of the board has exactly one source
	CHECK((seen | (1u << KEY_SK1) | (1u << KEY_SK2) | (1u << KEY_SK3) | (1u << KEY_TOP) | (1u << KEY_PTT)) ==
	      (1u << KEY_COUNT) - 1u);

	// Nothing down, two keys down, and a whole driven line's worth
	uint8_t rows[CS7000P_KEY_DRIVES] = { 0 };
	CHECK(keymatrix_decode(&m, 0, rows) == 0);
	rows[3] = 0x01;		// K8 x K1: 1
	rows[1] = 0x08;		// K6 x K4: #
	CHECK(keymatrix_decode(&m, 0, rows) == ((1u << KEY_1) | (1u << KEY_HASH)));
	memset(rows, 0, sizeof(rows));
	rows[2] = 0x0F;		// K7: 2, 5, 8, 0
	CHECK(keymatrix_decode(&m, 0, rows) == ((1u << KEY_2) | (1u << KEY_5) | (1u << KEY_8) | (1u << KEY_0)));
	// Sense bits past the fourth line are not keys (the scan reads four lines)
	memset(rows, 0, sizeof(rows));
	rows[0] = 0xF0;
	CHECK(keymatrix_decode(&m, 0, rows) == 0);
	// The two empty crossings stay empty with everything else read low too
	memset(rows, 0x0F, sizeof(rows));
	CHECK(keymatrix_decode(&m, 0, rows) == (digits | fn));
}

static void test_function_order(void)
{
	// Exactly the documented order, switch 1 to 6
	CHECK(sizeof(CS7000P_FN_KEYS) == 6 && CS7000P_FN_SWITCHES == 6);
	CHECK(memcmp(CS7000P_FN_KEYS, documented_fn, sizeof(documented_fn)) == 0);
	CHECK(CS7000P_FN_KEYS[0] == KEY_OK && CS7000P_FN_KEYS[1] == KEY_BACK && CS7000P_FN_KEYS[2] == KEY_F1);
	CHECK(CS7000P_FN_KEYS[3] == KEY_F2 && CS7000P_FN_KEYS[4] == KEY_UP && CS7000P_FN_KEYS[5] == KEY_DOWN);
	// Each of the radio's six such keys once, whatever order a measurement puts them in
	uint32_t seen = 0;
	for (unsigned i = 0; i < 6; i++) {
		CHECK(CS7000P_FN_KEYS[i] < KEY_COUNT && (seen & (1u << CS7000P_FN_KEYS[i])) == 0);
		seen |= 1u << CS7000P_FN_KEYS[i];
	}
	// Where the switches are: four on K5, two on K9, numbered down the sense lines
	for (unsigned s = 0; s < 4; s++) {
		CHECK(cs7000p_matrix[0][s] == CS7000P_FN_SW(s + 1));
	}
	CHECK(cs7000p_matrix[4][0] == CS7000P_FN_SW(5) && cs7000p_matrix[4][1] == CS7000P_FN_SW(6));
	CHECK(cs7000p_matrix[4][2] == KEYMATRIX_NONE && cs7000p_matrix[4][3] == KEYMATRIX_NONE);
}

static void test_settle(void)
{
	uint32_t last = 0, settled = 0;

	// One scan is not a press; two that agree are; a one-scan glitch changes nothing; release the same way
	CHECK(cs7000p_keys_settle(0x10, &last, &settled) == 0);
	CHECK(cs7000p_keys_settle(0x10, &last, &settled) == 0x10);
	CHECK(cs7000p_keys_settle(0x30, &last, &settled) == 0x10);
	CHECK(cs7000p_keys_settle(0x10, &last, &settled) == 0x10);
	CHECK(cs7000p_keys_settle(0x10, &last, &settled) == 0x10);
	CHECK(cs7000p_keys_settle(0, &last, &settled) == 0x10);
	CHECK(cs7000p_keys_settle(0, &last, &settled) == 0);
}

// ---- The channel selector ----

static void test_selector_table(void)
{
	// The measured table: verified, sixteen entries, every position once
	CHECK(CS7000P_CHSW_VERIFIED == 1);
	CHECK(CS7000P_CHSW_CODES == 16 && sizeof(cs7000p_chsw_position) == 16);
	// The knob and the table go together: a build that says "verified" over an unfilled table fails here
	CHECK(cs7000p_chsw_table_ok(cs7000p_chsw_position) == (CS7000P_CHSW_VERIFIED != 0));

	// The codes as read on a radio, one detent at a time over a whole turn (hardware-cs7000p.md 9.4)
	static const uint8_t read[16] = { 0xF, 0xD, 0xC, 0x8, 0x9, 0x1, 0x0, 0x4, 0x5, 0x7, 0x6, 0x2, 0x3, 0xB, 0xA, 0xE };
	for (unsigned n = 0; n < 16; n++) {
		CHECK(cs7000p_chsw_position[read[n]] == (int8_t)n);
		// a Gray code: one line changes per detent, round the whole turn
		uint8_t x = (uint8_t)(read[n] ^ read[(n + 1) & 15]);
		CHECK(x != 0 && (x & (x - 1)) == 0);
	}

	// The decoder on that table: each code settles on its position, one step per detent both ways round
	chswitch_t s;
	uint32_t now = 1000;
	chswitch_init(&s, cs7000p_chsw_position, CS7000P_CHSW_CODES, 30, read[0], now);
	int8_t last = -1;
	int steps = 0;
	for (unsigned n = 0; n <= 32; n++) {
		int8_t pos = -1;
		for (unsigned i = 0; i < 6; i++) {
			now += 10;
			pos = chswitch_poll(&s, read[n & 15], now);
		}
		CHECK(pos == (int8_t)(n & 15));
		if (last >= 0) {
			CHECK(cs7000p_chsw_steps(last, pos) == 1);
			steps += cs7000p_chsw_steps(last, pos);
		}
		last = pos;
	}
	CHECK(steps == 32);
	for (unsigned n = 32; n-- > 16;) {
		int8_t pos = -1;
		for (unsigned i = 0; i < 6; i++) {
			now += 10;
			pos = chswitch_poll(&s, read[n & 15], now);
		}
		CHECK(cs7000p_chsw_steps(last, pos) == -1);
		last = pos;
	}
	// A code no four lines can give is no position: the last settled one stands
	for (unsigned i = 0; i < 20; i++) {
		now += 10;
		CHECK(chswitch_poll(&s, 0x1F, now) == last);
	}
	// The software entry to the ROM bootloader needs the PB10 line (bit 2) high on a loader other than 9.1
	// (romentry_cs7000p.h): low on two runs of four detents, so four detents either way always reach a high one
	for (unsigned n = 0; n < 16; n++) {
		if ((read[n] & 4u) == 0) {
			CHECK((read[(n + 4) & 15] & 4u) != 0 && (read[(n + 12) & 15] & 4u) != 0);
		}
	}
}

static void test_selector_rules(void)
{
	// What a filled table must be: each position once
	// Synthetic, not this switch's
	static const int8_t gray[16] = { 0, 1, 3, 2, 7, 6, 4, 5, 15, 14, 12, 13, 8, 9, 11, 10 };
	int8_t t[16];
	CHECK(cs7000p_chsw_table_ok(gray));
	memcpy(t, gray, sizeof(t));
	t[5] = t[6];			// a position twice
	CHECK(!cs7000p_chsw_table_ok(t));
	memcpy(t, gray, sizeof(t));
	t[0] = -1;			// a code left out
	CHECK(!cs7000p_chsw_table_ok(t));
	memcpy(t, gray, sizeof(t));
	t[3] = 16;			// past the last position
	CHECK(!cs7000p_chsw_table_ok(t));

	// Steps between settled positions: one detent either way, across the wrap, and the shorter way round
	CHECK(cs7000p_chsw_steps(3, 4) == 1 && cs7000p_chsw_steps(4, 3) == -1);
	CHECK(cs7000p_chsw_steps(15, 0) == 1 && cs7000p_chsw_steps(0, 15) == -1);
	CHECK(cs7000p_chsw_steps(7, 7) == 0);
	CHECK(cs7000p_chsw_steps(0, 7) == 7 && cs7000p_chsw_steps(0, 9) == -7);
	CHECK(cs7000p_chsw_steps(14, 2) == 4 && cs7000p_chsw_steps(2, 14) == -4);
	CHECK(cs7000p_chsw_steps(0, 8) == 8 && cs7000p_chsw_steps(8, 0) == 8);	// half a turn: no shorter way
	// A whole turn, detent by detent, is 16 steps and ends where it began
	int sum = 0;
	for (int p = 0; p < 16; p++) {
		sum += cs7000p_chsw_steps((int8_t)p, (int8_t)((p + 1) & 15));
	}
	CHECK(sum == 16);
	sum = 0;
	for (int p = 16; p > 0; p--) {
		sum += cs7000p_chsw_steps((int8_t)(p & 15), (int8_t)(p - 1));
	}
	CHECK(sum == -16);

	// The decoder on the synthetic table, as the input code will run it: a code settles after its time, and the
	// positions it gives turn into the steps above
	chswitch_t s;
	uint32_t now = 0;
	int8_t last = -1;
	int steps = 0;
	// gray[]'s inverse
	static const uint8_t code_of[16] = { 0, 1, 3, 2, 6, 7, 5, 4, 12, 13, 15, 14, 10, 11, 9, 8 };
	for (unsigned p = 0; p < 16; p++) {
		CHECK(gray[code_of[p]] == (int8_t)p);
	}
	chswitch_init(&s, gray, 16, 30, code_of[0], now);
	for (int p = 0; p <= 16; p++) {
		for (unsigned i = 0; i < 5; i++) {
			now += 10;
			int8_t pos = chswitch_poll(&s, code_of[p & 15], now);
			if (pos >= 0 && last >= 0) {
				steps += cs7000p_chsw_steps(last, pos);
			}
			if (pos >= 0) {
				last = pos;
			}
		}
		CHECK(last == (p & 15));
	}
	CHECK(steps == 16);
}

// ---- The accessory connector's PTT line ----

static void test_ext_ptt_stuck(void)
{
	cs7000p_ext_stuck_t s = { 0 };
	uint32_t now = 0xFFFFFF00u;	// across the clock's wrap

	// An empty jack: the line is high on its pull-up, nothing is stuck
	CHECK(!cs7000p_ext_ptt_stuck(&s, false, false, now));
	// Low from the start and never armed: stuck once it has lasted, not before
	CHECK(!cs7000p_ext_ptt_stuck(&s, true, false, now));
	CHECK(!cs7000p_ext_ptt_stuck(&s, true, false, now + CS7000P_EXT_PTT_STUCK_MS - 1));
	CHECK(cs7000p_ext_ptt_stuck(&s, true, false, now + CS7000P_EXT_PTT_STUCK_MS));
	CHECK(cs7000p_ext_ptt_stuck(&s, true, false, now + 10 * CS7000P_EXT_PTT_STUCK_MS));
	// Released: over, and a later low starts counting again
	CHECK(!cs7000p_ext_ptt_stuck(&s, false, false, now + 30000));
	CHECK(!cs7000p_ext_ptt_stuck(&s, true, false, now + 30010));
	CHECK(!cs7000p_ext_ptt_stuck(&s, true, false, now + 30010 + CS7000P_EXT_PTT_STUCK_MS - 1));
	CHECK(cs7000p_ext_ptt_stuck(&s, true, false, now + 30010 + CS7000P_EXT_PTT_STUCK_MS));
	// An armed line that is low is a press, however long it is held: the transmit timeout's business
	CHECK(!cs7000p_ext_ptt_stuck(&s, true, true, now + 40000));
	CHECK(!cs7000p_ext_ptt_stuck(&s, true, true, now + 400000));
	CHECK(CS7000P_EXT_PTT_STUCK_MS == 2000);
}

int main(void)
{
	test_matrix();
	test_function_order();
	test_settle();
	test_selector_table();
	test_selector_rules();
	test_ext_ptt_stuck();
	printf("# cs7000pKeysTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
