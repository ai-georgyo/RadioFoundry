// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The text editor's rules that need no screen (ui_menus.c draws and drives it), tested by tests/host/textEditTest.c:
// Back drops the last character whole, and a character set says what the keys type, what Up / Down step through and
// what of a default text stays.
//
// A name field is ASCII / Latin-1 (one byte each) and 3-byte UTF-8 sequences for Chinese; there is no 2-byte form (the
// same rule as gfx_utf8_next()).
#ifndef CORE_UI_TEXT_EDIT_H
#define CORE_UI_TEXT_EDIT_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Back: the new length; the buffer is unchanged up to it. Walk from the start so the last character's boundary is the
// one the text layer drew. Stepping back over trailing continuation bytes instead would eat a Latin-1 letter in
// 0xA0-0xBF (a degree sign, 0xB0, is one character too), which must be deleted on its own.
static inline uint8_t text_edit_backspace(const char *buf, uint8_t len)
{
	const uint8_t *b = (const uint8_t *)buf;
	uint8_t start = 0;
	for (uint8_t i = 0; i < len; ) {
		start = i;
		uint8_t n = 1;
		if (i + 2 < len && (b[i] & 0xF0u) == 0xE0u &&
				(b[i + 1] & 0xC0u) == 0x80u && (b[i + 2] & 0xC0u) == 0x80u) {
			uint32_t cp = ((uint32_t)(b[i] & 0x0Fu) << 12) | ((uint32_t)(b[i + 1] & 0x3Fu) << 6) |
					(uint32_t)(b[i + 2] & 0x3Fu);
			if (cp >= 0x800u)		// not an overlong form (gfx_utf8_next)
				n = 3;
		}
		i = (uint8_t)(i + n);
	}
	return start;
}

// --- Character sets (text_open's set) ---
enum {
	TEXT_SET_ANY,		// names and messages: the keys type ASCII in both cases; a default text stays as it is
	TEXT_SET_CALLSIGN,	// an M17 callsign (base-40, m17.md 10): A-Z, 0-9, '-', '/', '.' and space, upper case only
	TEXT_SET_ASCII,		// printable ASCII (M17 call text): the keys as ANY, no Latin-1 or Chinese
};
// The keypad modes '#' cycles through, in order: ABC, 123, abc (TEXT_SET_CALLSIGN: ABC and 123 only)
enum { TEXT_UPPER, TEXT_DIGITS, TEXT_LOWER, TEXT_MODES };

static inline uint8_t text_set_modes(uint8_t set)
{
	return set == TEXT_SET_CALLSIGN ? 2u : (uint8_t)TEXT_MODES;
}

// The characters key 0-9 cycles through in a set, before case: 2-9 their letters, then their digit
static inline const char *text_set_keys(uint8_t set, uint8_t key)
{
	static const char *const any[10] = { " 0+#*", ".-/,'&()@_!?1", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7",
		"TUV8", "WXYZ9" };
	if (key > 9)
		return "";
	if (set == TEXT_SET_CALLSIGN && key <= 1)
		return key == 0 ? " 0" : "-/.1";
	return any[key];
}

// How many characters a key's cycle has. Letters cycle letters, digit, then the letters in the other case (ABC: A B C
// 2 a b c); a callsign has no other case
static inline uint8_t text_set_cycle_len(uint8_t set, const char *keys)
{
	uint8_t n = (uint8_t)strlen(keys);
	return set != TEXT_SET_CALLSIGN && keys[0] >= 'A' && keys[0] <= 'Z' ? (uint8_t)(2u * n - 1u) : n;
}

// Character idx of a key's cycle in a mode (TEXT_UPPER or TEXT_LOWER)
static inline char text_set_char(uint8_t set, const char *keys, uint8_t idx, uint8_t mode)
{
	uint8_t n = (uint8_t)strlen(keys);
	if (n == 0)
		return 0;
	if (!(keys[0] >= 'A' && keys[0] <= 'Z') || set == TEXT_SET_CALLSIGN)
		return keys[idx < n ? idx : 0];
	bool lower = mode == TEXT_LOWER;
	if (idx < n) {
		char c = keys[idx];
		return idx == n - 1u || !lower ? c : (char)(c - 'A' + 'a');	// the letters in the mode's case, the digit
	}
	char c = keys[(uint8_t)(idx - n) < n ? idx - n : 0];
	return lower ? c : (char)(c - 'A' + 'a');	// then the other case
}

// Whether a set holds a character (one byte; a Chinese character's bytes are in TEXT_SET_ANY only)
static inline bool text_set_has(uint8_t set, char c)
{
	uint8_t u = (uint8_t)c;
	switch (set) {
	case TEXT_SET_CALLSIGN:
		return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '/' || c == '.' || c == ' ';
	case TEXT_SET_ASCII:
		return u >= 0x20u && u <= 0x7Eu;
	default:
		return u != 0;
	}
}

// Up / Down on a radio without a keypad: the character after (dir > 0) or before c, wrapping; 0 for Down from a space,
// which deletes it. ANY and ASCII step the printable ASCII range in code order; a callsign space, A-Z, 0-9, '-', '/', '.'
static inline char text_set_step(uint8_t set, char c, int dir)
{
	if (dir < 0 && c == ' ')
		return 0;
	if (set != TEXT_SET_CALLSIGN)
		return (char)(' ' + (c - ' ' + dir + 95) % 95);
	static const char order[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
	const int n = (int)sizeof order - 1;
	const char *p = c != '\0' ? strchr(order, c) : NULL;
	int at = p != NULL ? (int)(p - order) : 0;
	return order[(at + (dir > 0 ? 1 : n - 1)) % n];
}

// A default text in a set, in place: TEXT_SET_ANY keeps it; a callsign takes letters in upper case and drops what it
// can't hold; ASCII drops what isn't printable ASCII (a Latin-1 byte, a Chinese character's three). Returns the length
static inline uint8_t text_set_filter(uint8_t set, char *buf, uint8_t len)
{
	if (set == TEXT_SET_ANY)
		return len;
	uint8_t n = 0;
	for (uint8_t i = 0; i < len; i++) {
		char c = buf[i];
		if (set == TEXT_SET_CALLSIGN && c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		if (text_set_has(set, c))
			buf[n++] = c;
	}
	buf[n] = '\0';
	return n;
}

#endif
