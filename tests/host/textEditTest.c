// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The text editor's Back: one press removes one character, a Chinese name by its three UTF-8 bytes, an ASCII or
// Latin-1 name one byte at a time (core/ui/text_edit.h). The text layer's rule (gfx_utf8_next) is the same, so
// what Back removes is what the screen drew. Then the character sets: the keys' cycles, Up / Down's steps and what
// of a default text each set keeps.
#include <stdio.h>
#include <string.h>
#include "ui/text_edit.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// Back once over a text of len bytes; the result is NUL-terminated in buf, its length returned
static uint8_t back(const char *text, uint8_t len, char *buf)
{
	memcpy(buf, text, len);
	uint8_t n = text_edit_backspace(buf, len);
	buf[n] = '\0';
	return n;
}

static void one(const char *text, uint8_t len, const char *want, uint8_t want_len)
{
	char buf[64];
	uint8_t n = back(text, len, buf);
	if (n != want_len || memcmp(buf, want, n) != 0) {
		printf("FAIL %s:%d: Back over %u bytes from \"%s\" gave (%u) not (%u)\n",
		       __FILE__, __LINE__, len, text, n, want_len);
		failures++;
	}
}

// A key pressed n times within the window in a set and mode: the characters it typed, one per press
static void cycle(int line, uint8_t set, uint8_t key, uint8_t mode, const char *want)
{
	const char *keys = text_set_keys(set, key);
	uint8_t len = text_set_cycle_len(set, keys);
	char got[32];
	uint8_t n = (uint8_t)strlen(want);
	for (uint8_t i = 0; i < n; i++)
		got[i] = text_set_char(set, keys, (uint8_t)(i % len), mode);
	got[n] = '\0';
	if (strcmp(got, want) != 0) {
		printf("FAIL %s:%d: set %u key %u mode %u cycled \"%s\" not \"%s\"\n", __FILE__, line, set, key, mode, got,
		       want);
		failures++;
	}
}
#define CYCLE(set, key, mode, want)	cycle(__LINE__, set, key, mode, want)

// Up (dir 1) or Down (-1) from c, n times: the characters it went through
static void steps(int line, uint8_t set, char c, int dir, int n, const char *want)
{
	char got[128];
	int k = 0;
	for (int i = 0; i < n && c != 0; i++) {
		c = text_set_step(set, c, dir);
		got[k++] = c != 0 ? c : '|';	// '|': Down past the space deleted it
	}
	got[k] = '\0';
	if (strcmp(got, want) != 0) {
		printf("FAIL %s:%d: set %u stepped \"%s\" not \"%s\"\n", __FILE__, line, set, got, want);
		failures++;
	}
}
#define STEPS(set, c, dir, n, want)	steps(__LINE__, set, c, dir, n, want)

// A default text filtered into a set
static void paste(int line, uint8_t set, const char *text, const char *want)
{
	char buf[64];
	strcpy(buf, text);
	uint8_t n = text_set_filter(set, buf, (uint8_t)strlen(buf));
	if (n != strlen(want) || strcmp(buf, want) != 0) {
		printf("FAIL %s:%d: set %u kept \"%s\" (%u) of \"%s\", not \"%s\"\n", __FILE__, line, set, buf, n, text,
		       want);
		failures++;
	}
}
#define PASTE(set, text, want)	paste(__LINE__, set, text, want)

// The character sets text_open takes (text_edit.h): ANY as the editor always was, a callsign, printable ASCII
static void sets(void)
{
	// TEXT_SET_ANY: the keys as before. 2-9: the letters in the mode's case, the digit, the other case; 1 and 0 their
	// signs; # three modes
	CYCLE(TEXT_SET_ANY, 2, TEXT_UPPER, "ABC2abcABC2");
	CYCLE(TEXT_SET_ANY, 7, TEXT_UPPER, "PQRS7pqrsP");
	CYCLE(TEXT_SET_ANY, 9, TEXT_LOWER, "wxyz9WXYZw");
	CYCLE(TEXT_SET_ANY, 1, TEXT_UPPER, ".-/,'&()@_!?1.");
	CYCLE(TEXT_SET_ANY, 0, TEXT_LOWER, " 0+#* ");
	CHECK(text_set_modes(TEXT_SET_ANY) == 3);
	// TEXT_SET_ASCII types the same
	CYCLE(TEXT_SET_ASCII, 3, TEXT_UPPER, "DEF3defD");
	CYCLE(TEXT_SET_ASCII, 1, TEXT_UPPER, ".-/,'&()@_!?1");
	CHECK(text_set_modes(TEXT_SET_ASCII) == 3);

	// TEXT_SET_CALLSIGN: upper case only, in either mode; 1 is - / . 1, 0 is space 0; # is ABC / 123 only
	CYCLE(TEXT_SET_CALLSIGN, 2, TEXT_UPPER, "ABC2ABC2");
	CYCLE(TEXT_SET_CALLSIGN, 7, TEXT_UPPER, "PQRS7PQ");
	CYCLE(TEXT_SET_CALLSIGN, 9, TEXT_LOWER, "WXYZ9W");	// no lower case, even if a mode said so
	CYCLE(TEXT_SET_CALLSIGN, 1, TEXT_UPPER, "-/.1-/");
	CYCLE(TEXT_SET_CALLSIGN, 0, TEXT_UPPER, " 0 0");
	CHECK(text_set_modes(TEXT_SET_CALLSIGN) == 2);
	CHECK(text_set_cycle_len(TEXT_SET_CALLSIGN, text_set_keys(TEXT_SET_CALLSIGN, 8)) == 4);
	CHECK(text_set_cycle_len(TEXT_SET_ANY, text_set_keys(TEXT_SET_ANY, 8)) == 7);
	CHECK(text_set_keys(TEXT_SET_CALLSIGN, 10)[0] == '\0');
	// Every character any callsign key types is in the set (base-40)
	for (uint8_t k = 0; k <= 9; k++) {
		const char *keys = text_set_keys(TEXT_SET_CALLSIGN, k);
		for (uint8_t i = 0; i < text_set_cycle_len(TEXT_SET_CALLSIGN, keys); i++)
			CHECK(text_set_has(TEXT_SET_CALLSIGN, text_set_char(TEXT_SET_CALLSIGN, keys, i, TEXT_UPPER)));
	}
	CHECK(text_set_has(TEXT_SET_CALLSIGN, 'Q') && text_set_has(TEXT_SET_CALLSIGN, '7') &&
	      text_set_has(TEXT_SET_CALLSIGN, '/') && text_set_has(TEXT_SET_CALLSIGN, ' '));
	CHECK(!text_set_has(TEXT_SET_CALLSIGN, 'q') && !text_set_has(TEXT_SET_CALLSIGN, '_') &&
	      !text_set_has(TEXT_SET_CALLSIGN, '#') && !text_set_has(TEXT_SET_CALLSIGN, (char)0xC9));
	CHECK(text_set_has(TEXT_SET_ASCII, '~') && text_set_has(TEXT_SET_ASCII, ' ') &&
	      !text_set_has(TEXT_SET_ASCII, (char)0xE9) && !text_set_has(TEXT_SET_ASCII, '\n') &&
	      !text_set_has(TEXT_SET_ASCII, 0x7F));
	CHECK(text_set_has(TEXT_SET_ANY, (char)0xE9) && text_set_has(TEXT_SET_ANY, (char)0xE4));

	// Up / Down without a keypad. ANY and ASCII: the printable ASCII range, wrapping; Down past the space deletes
	STEPS(TEXT_SET_ANY, 'A', 1, 3, "BCD");
	STEPS(TEXT_SET_ANY, '}', 1, 3, "~ !");
	STEPS(TEXT_SET_ANY, 'B', -1, 2, "A@");
	STEPS(TEXT_SET_ANY, '!', -1, 2, " |");
	STEPS(TEXT_SET_ASCII, 'z', 1, 2, "{|");
	// A callsign: space, A-Z, 0-9, -, /, ., then space again; Down from A is the space, from the space deletes
	STEPS(TEXT_SET_CALLSIGN, ' ', 1, 40, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/. ");
	STEPS(TEXT_SET_CALLSIGN, 'Z', 1, 2, "01");
	STEPS(TEXT_SET_CALLSIGN, '9', 1, 4, "-/. ");
	STEPS(TEXT_SET_CALLSIGN, '0', -1, 2, "ZY");
	STEPS(TEXT_SET_CALLSIGN, 'B', -1, 3, "A |");
	STEPS(TEXT_SET_CALLSIGN, '-', -1, 2, "98");
	// A character outside the set (none should be there after the filter) starts from the space
	STEPS(TEXT_SET_CALLSIGN, 'q', 1, 1, "A");

	// The default text: ANY keeps it whole (Latin-1, Chinese); a callsign upper-cases and drops what it can't hold;
	// ASCII drops the bytes outside printable ASCII, a Chinese character's three with them
	PASTE(TEXT_SET_ANY, "Caf\xE9 \xE4\xB8\xAD", "Caf\xE9 \xE4\xB8\xAD");
	PASTE(TEXT_SET_CALLSIGN, "n0call", "N0CALL");
	PASTE(TEXT_SET_CALLSIGN, "w1aw-7/p", "W1AW-7/P");
	PASTE(TEXT_SET_CALLSIGN, "M17-M17 C", "M17-M17 C");
	PASTE(TEXT_SET_CALLSIGN, "ab_c#d\xC9!e.f", "ABCDE.F");
	PASTE(TEXT_SET_CALLSIGN, "", "");
	PASTE(TEXT_SET_ASCII, "Caf\xE9 au lait", "Caf au lait");
	PASTE(TEXT_SET_ASCII, "a\xE4\xB8\xAD" "b\tc~", "abc~");
	PASTE(TEXT_SET_ASCII, "Plain text, 73!", "Plain text, 73!");
}

int main(void)
{
	char buf[64];

	// Empty, and a single ASCII character
	one("", 0, "", 0);
	one("A", 1, "", 0);
	CHECK(back("", 0, buf) == 0 && buf[0] == '\0');

	// An ASCII name shortens by one byte on every press
	one("COUNTY_DMRGM", 12, "COUNTY_DMRG", 11);
	one("ABCD", 4, "ABC", 3);
	one("AB C", 4, "AB ", 3);

	// A Latin-1 byte is one character on its own, even in the continuation-byte range (a degree sign, 0xB0)
	one("A\xB0", 2, "A", 1);
	one("\xB0\xB1", 2, "\xB0", 1);
	one("A\xB7" "B", 3, "A\xB7", 2);
	// A font glyph byte (0x80 = ellipsis) is one character too
	one("A\x80", 2, "A", 1);
	// "Café": 0xE9 has no continuations, so it is one Latin-1 byte
	one("Caf\xE9", 4, "Caf", 3);
	// "à la": 0xE0 followed by a space, one byte at a time
	one("\xE0 la", 4, "\xE0 l", 3);
	// A 3-byte lead with no continuations (a cut-off name) is one byte
	one("AB\xE4", 3, "AB", 2);

	// A Chinese name goes in one press, by all three UTF-8 bytes
	one("\xE4\xB8\xAD", 3, "", 0);				// "中"
	one("A\xE4\xB8\xAD", 4, "A", 1);			// "A中"
	one("\xE4\xB8\xAD" "A", 4, "\xE4\xB8\xAD", 3);		// "中A"
	one("\xE4\xB8\xAD\xE9\x81\x93", 6, "\xE4\xB8\xAD", 3);	// "中道" -> "中"
	one("\xE4\xB8\xAD\xB0", 4, "\xE4\xB8\xAD", 3);		// "中°" -> "中"
	one("A\xE4\xB8\xAD" "B\xB0", 6, "A\xE4\xB8\xAD" "B", 5);

	// An overlong form isn't UTF-8 (gfx_utf8_next reads it byte by byte): Back removes one byte
	one("\xE0\x80\x80", 3, "\xE0\x80", 2);
	// A trailing run of continuation-looking Latin-1 bytes after a lead is decoded as one sequence, exactly as
	// the text layer draws it: all three bytes go together
	one("A\xE9\xB0\xB1", 4, "A", 1);

	sets();

	printf(failures ? "# textEditTest FAILED (%d)\n" : "# textEditTest PASSED\n", failures);
	return failures ? 1 : 0;
}
