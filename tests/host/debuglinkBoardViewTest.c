// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's board view sections (core/debuglink/debuglink_view_board.c, docs/spec/debuglink.md 3.7), built
// twice: a board without view_split (BOARD_SPLIT 0: one `board` section with every line, and no `board2` section at
// all, so the next links end where they did) and one that splits its lines (BOARD_SPLIT 3: lines 0-2 in `board`, the
// rest in `board2`, all as "board.<key>")
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "debuglink/debuglink.h"

#ifndef BOARD_SPLIT
#error "build with -DBOARD_SPLIT=<the board's view_split>"
#endif

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define LINES	6u

static unsigned asked;		// view_line calls
static unsigned long_len;	// a value of this many characters for line 0 (0: "v0")
static unsigned long_room;	// the room line 0 was given

static bool view_line(uint8_t i, char *buf, uint8_t len)
{
	asked++;
	if (i >= LINES) {
		return false;
	}
	if (i == 0u && long_len != 0u) {
		char v[400];
		memset(v, 'x', long_len);
		v[long_len] = '\0';
		long_room = len;
		dl_fmt(buf, len, "k0=%s", v);
		return true;
	}
	dl_fmt(buf, len, "k%u=v%u", i, i);
	return true;
}

const board_t board = { .name = "test", .view_line = view_line, .view_split = BOARD_SPLIT };

static char out[1024];

static const char *render(uint8_t part, size_t cap)
{
	dl_text_t t;
	dl_text_init(&t, out, cap);
	debuglink_view_board(part, &t);
	out[t.len] = '\0';
	return out;
}

static void test_presence(void)
{
	// Every section but board2 is there on every board; board2 only with a split
	for (uint8_t s = 0; s < DL_VIEW_COUNT; s++) {
		CHECK(debuglink_view_present(s) == (s != DL_VIEW_BOARD2 || BOARD_SPLIT != 0));
	}
	CHECK(!debuglink_view_present(DL_VIEW_COUNT) && !debuglink_view_present(0xFFu));
	// The next links skip what is absent and end with 0xFF
	CHECK(debuglink_view_next(0) == 1u);
	CHECK(debuglink_view_next(DL_VIEW_BOARD) == DL_VIEW_BOARD + 1u);
	CHECK(debuglink_view_next(DL_VIEW_BOARD2 - 1u) == (BOARD_SPLIT != 0 ? DL_VIEW_BOARD2 : 0xFFu));
	CHECK(debuglink_view_next(DL_VIEW_COUNT - 1u) == 0xFFu);
	// Following them from 0 visits each present section once
	unsigned seen = 0;
	for (unsigned s = 0; s != 0xFFu && seen <= DL_VIEW_COUNT; s = debuglink_view_next((uint8_t)s)) {
		CHECK(debuglink_view_present((uint8_t)s));
		seen++;
	}
	CHECK(seen == (BOARD_SPLIT != 0 ? DL_VIEW_COUNT : DL_VIEW_COUNT - 1u));
}

static void test_sections(void)
{
#if BOARD_SPLIT == 0
	CHECK(strcmp(render(0, sizeof(out)), "board.name=test\nboard.variant_mismatch=0\nboard.k0=v0\nboard.k1=v1\n"
		     "board.k2=v2\nboard.k3=v3\nboard.k4=v4\nboard.k5=v5\n") == 0);
	asked = 0;
	CHECK(strcmp(render(1, sizeof(out)), "") == 0 && asked == 0);	// nothing, and no line asked for
#else
	CHECK(strcmp(render(0, sizeof(out)), "board.name=test\nboard.variant_mismatch=0\nboard.k0=v0\nboard.k1=v1\n"
		     "board.k2=v2\n") == 0);
	CHECK(strcmp(render(1, sizeof(out)), "board.k3=v3\nboard.k4=v4\nboard.k5=v5\n") == 0);
#endif
	// A reply too small: whole lines only (k1 had room for part of itself: dropped), and the lines after the first
	// that did not fit are not asked for
	static const char head[] = "board.name=test\nboard.variant_mismatch=0\nboard.k0=v0\n";
	for (size_t cap = sizeof(head) + 1u; cap < sizeof(head) + 13u; cap++) {
		asked = 0;
		memset(out, '#', sizeof(out));
		dl_text_t t;
		dl_text_init(&t, out, cap);
		debuglink_view_board(0, &t);
		CHECK(out[cap] == '#');		// nothing written past the reply's room
		out[t.len] = '\0';
		CHECK(t.full && strcmp(out, head) == 0 && asked <= 2u);
	}
	// One more byte and k1 fits: its 12 and the byte a piece leaves free, as dl_text() does
	dl_text_t t;
	dl_text_init(&t, out, sizeof(head) + 13u);
	debuglink_view_board(0, &t);
	out[t.len] = '\0';
	CHECK(strncmp(out, head, sizeof(head) - 1u) == 0 && strcmp(out + sizeof(head) - 1u, "board.k1=v1\n") == 0);
}

// A line is formatted in the reply itself: no fixed line buffer, so a line longer than 80 characters comes whole
// (the debug link task keeps no copy of it on its stack), up to 253 characters: view_line's uint8_t length gives
// room for 254, and a line that fills its room may have been cut, so it is dropped and ends the section
static void test_long_lines(void)
{
	const uint8_t part = 0;		// line 0 is in `board` with a split and without
	char want[400];

	long_len = 120;
	render(part, sizeof(out));
	memset(want, 'x', long_len);
	want[long_len] = '\0';
	const char *k0 = strstr(out, "board.k0=");
	CHECK(k0 != NULL && strncmp(k0 + 9, want, long_len) == 0 && k0[9 + long_len] == '\n');
	CHECK(strstr(out, "board.k1=v1\n") != NULL && long_room == 255u);
	long_len = 250;		// "k0=" and 250: 253 characters, the most
	render(part, sizeof(out));
	k0 = strstr(out, "board.k0=");
	CHECK(k0 != NULL && k0[9 + long_len] == '\n' && strstr(out, "board.k1=v1\n") != NULL);
	long_len = 251;		// fills the room: dropped, nothing after it
	asked = 0;
	dl_text_t t;
	dl_text_init(&t, out, sizeof(out));
	debuglink_view_board(part, &t);
	out[t.len] = '\0';
	CHECK(t.full && strstr(out, "board.k0") == NULL && strstr(out, "board.k1") == NULL && asked == 1u);
	long_len = 0;
}

int main(void)
{
	test_presence();
	test_sections();
	test_long_lines();
	if (failures != 0) {
		printf("debuglinkBoardViewTest (split %d): %d failure(s)\n", BOARD_SPLIT, failures);
		return 1;
	}
	printf("debuglinkBoardViewTest (split %d): ok\n", BOARD_SPLIT);
	return 0;
}
