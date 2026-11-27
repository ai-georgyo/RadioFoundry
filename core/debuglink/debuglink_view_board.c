// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The board's own view keys (docs/spec/debuglink.md 3.7): the `board` section, and `board2` for a board whose lines
// outgrow one reply (board.view_split). Which sections a radio has: every one but `board2`, which only such a board
// has, so a host that follows the next links never asks another radio for it. Apart from debuglink_view.c, so the
// host tests run it on a board of their own.
#include <string.h>
#include "config.h"
#include "board.h"
#include "debuglink/debuglink.h"

// The longest line board.view_line is given room for, its NUL included (its len is a uint8_t)
#define LINE_MAX	255u

bool debuglink_view_present(uint8_t section)
{
	return section < DL_VIEW_COUNT && (section != DL_VIEW_BOARD2 || board.view_split != 0u);
}

uint8_t debuglink_view_next(uint8_t section)
{
	for (unsigned s = (unsigned)section + 1u; s < DL_VIEW_COUNT; s++) {
		if (debuglink_view_present((uint8_t)s)) {
			return (uint8_t)s;
		}
	}
	return 0xFFu;
}

// One line "board.<line>\n", formatted in place in the reply: no line buffer on the debug link task's stack, and a
// line as long as the reply's room allows. As dl_text() does with a piece, a line that filled its room may have been
// cut, so it counts as not fitting: nothing is appended, full is set. False when the board has no line i
static bool board_line(uint8_t i, dl_text_t *t)
{
	static const char prefix[] = "board.";
	const size_t pre = sizeof(prefix) - 1u, room = t->cap - t->len;

	if (room < pre + 3u) {	// not even a one-character line and its '\n'
		t->full = true;
		return true;
	}
	char *p = t->buf + t->len;
	size_t cap = room - pre - 1u;	// the '\n' after it
	if (cap > LINE_MAX) {
		cap = LINE_MAX;
	}
	p[pre] = '\0';
	if (!board.view_line(i, p + pre, (uint8_t)cap)) {
		return false;
	}
	size_t n = strnlen(p + pre, cap);
	if (n + 1u >= cap) {
		t->full = true;
		return true;
	}
	memcpy(p, prefix, pre);
	p[pre + n] = '\n';
	t->len += pre + n + 1u;
	return true;
}

// Part 0: the name, the variant warning and the board's lines up to view_split (all of them without it). Part 1: the
// lines from view_split on. Each "board.<key>"; a line that would not fit ends the section (the caller marks it
// truncated)
void debuglink_view_board(uint8_t part, dl_text_t *t)
{
	uint8_t i = 0, end = 0xFFu;

	if (part == 0u) {
		dl_text_kv_str(t, "board.name", board.name);
		dl_text(t, "board.variant_mismatch=%u\n", board_variant_mismatch() ? 1u : 0u);
		if (board.view_split != 0u) {
			end = board.view_split;
		}
	} else if (board.view_split != 0u) {
		i = board.view_split;
	} else {
		return;
	}
	for (; board.view_line != NULL && i < end && !t->full; i++) {
		if (!board_line(i, t)) {
			break;
		}
	}
}
