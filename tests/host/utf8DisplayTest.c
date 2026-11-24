// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// utf8_to_display (core/ui/gfx.c): UTF-8 from the air as the display's one-byte text. ASCII stays, U+00A0-U+00FF
// becomes its Latin-1 byte, a character the CJK face has stays as its three bytes in a build with it (CONFIG_LANG_ZH:
// utf8DisplayTestZh) and is '?' without, anything else is '?', control characters go, and a cut never splits a
// character.
#include <stdio.h>
#include <string.h>
#include "ui/gfx.h"
#include "ui/cjk_font.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void hex(const char *s)
{
	for (; *s; s++)
		printf("%02X", (unsigned)(unsigned char)*s);
}

// in (to its NUL) into a buffer of cap bytes: want, and whether all of it fit
static void one(int line, const char *in, size_t cap, const char *want, bool whole)
{
	char out[160];
	memset(out, 'x', sizeof out);
	bool got = utf8_to_display(out, cap, in, strlen(in));
	if (strcmp(out, want) != 0 || got != whole || strlen(out) >= cap) {
		printf("FAIL %s:%d: ", __FILE__, line);
		hex(in);
		printf(" cap %zu gave ", cap);
		hex(out);
		printf(" (%s) not ", got ? "whole" : "cut");
		hex(want);
		printf(" (%s)\n", whole ? "whole" : "cut");
		failures++;
	}
}
#define ONE(in, cap, want, whole)	one(__LINE__, in, cap, want, whole)

#define ZHONG	"\xE4\xB8\xAD"	// U+4E2D, in the CJK face (zh.po uses it)
#define DING	"\xE4\xB8\x81"	// U+4E01, not in it
#if CONFIG_LANG_ZH
#define ZHONG_SHOWN	ZHONG
#else
#define ZHONG_SHOWN	"?"
#endif

int main(void)
{
	char out[16];

#if CONFIG_LANG_ZH
	// The face this test counts on: one character it has, one it hasn't
	CHECK(cjk_font_glyph(0x4E2D) != NULL && cjk_font_glyph(0x4E01) == NULL);
#endif

	// ASCII as it is; empty stays empty
	ONE("", 8, "", true);
	ONE("Meet at the north lot", 64, "Meet at the north lot", true);
	ONE("N0CALL/P 73!", 64, "N0CALL/P 73!", true);

	// U+00A0-U+00FF: the Latin-1 byte ("Café", "Señor", the degree sign, a no-break space)
	ONE("Caf\xC3\xA9", 16, "Caf\xE9", true);
	ONE("Se\xC3\xB1or 20\xC2\xB0", 16, "Se\xF1or 20\xB0", true);
	ONE("\xC2\xA0x\xC3\xBF", 16, "\xA0x\xFF", true);

	// Chinese: kept where the face has the character (a CJK build), else '?'; one the face lacks is '?' everywhere
	ONE(ZHONG "A", 16, ZHONG_SHOWN "A", true);
	ONE(DING, 16, "?", true);
	ONE("\xE2\x80\x9C" "hi" "\xE2\x80\x9D", 16,	// the curly quotes U+201C / U+201D: the face has them
#if CONFIG_LANG_ZH
	    "\xE2\x80\x9C" "hi" "\xE2\x80\x9D",
#else
	    "?hi?",
#endif
	    true);

	// Outside Latin-1 and the face: one '?' a character, whatever its length (U+0100, U+20AC, an emoji)
	ONE("\xC4\x80" "a\xE2\x82\xAC" "b\xF0\x9F\x98\x80" "c", 16, "?a?b?c", true);

	// Mixed: ASCII, Latin-1, CJK and unknown in one text
	ONE("Ol\xC3\xA1 " ZHONG " \xE2\x82\xAC" "5", 32, "Ol\xE1 " ZHONG_SHOWN " ?5", true);

	// Not UTF-8: one '?' a byte (a stray continuation, a lone Latin-1 byte, overlong forms, a surrogate, a sequence
	// cut short by the end or by a byte that doesn't continue it, past U+10FFFF)
	ONE("a\x80" "b", 16, "a?b", true);
	ONE("caf\xE9", 16, "caf?", true);
	ONE("\xC0\xAF", 16, "??", true);
	ONE("\xE0\x80\xAF", 16, "???", true);
	ONE("\xED\xA0\x80", 16, "???", true);
	ONE("\xE4\xB8", 16, "??", true);
	ONE("\xE4" "A", 16, "?A", true);
	ONE("\xF4\x90\x80\x80", 16, "????", true);

	// Control characters go: the text layer's own bytes (the glyphs 0x80.., the text alternates 0x1d-0x1f) are
	// among them, and so are C1 controls sent as UTF-8 (U+0082 would draw an arrow)
	ONE("a\nb\tc\x1d" "d\x7F" "e", 16, "abcde", true);
	ONE("x\xC2\x82y\xC2\x9Fz", 16, "xyz", true);

	// The cut: at most cap - 1 bytes, between characters, and false
	ONE("ABCDEF", 4, "ABC", false);
	ONE("ABC", 4, "ABC", true);
	ONE("AB\xC3\xA9", 4, "AB\xE9", true);		// a 2-byte character that becomes one byte fits
	ONE("ABC\xC3\xA9", 4, "ABC", false);
#if CONFIG_LANG_ZH
	ONE("A" ZHONG "B", 4, "A", false);		// the 3-byte character doesn't fit in what's left: not split
	ONE("A" ZHONG, 5, "A" ZHONG, true);
	ONE("AB" ZHONG ZHONG, 8, "AB" ZHONG, false);
#else
	ONE("A" ZHONG "B", 4, "A?B", true);
	ONE("AB" ZHONG ZHONG, 4, "AB?", false);
#endif
	ONE("a\nb", 2, "a", false);			// a dropped control before the cut doesn't save it
	ONE("abc\n", 4, "abc", true);			// only dropped controls left: nothing was cut

	// in_len ends the text before its NUL, also in the middle of a character (which is then not UTF-8)
	CHECK(utf8_to_display(out, sizeof out, "ABCDEF", 3) && strcmp(out, "ABC") == 0);
	CHECK(utf8_to_display(out, sizeof out, "A\xC3\xA9", 2) && strcmp(out, "A?") == 0);
	CHECK(utf8_to_display(out, sizeof out, "A" ZHONG, 3) && strcmp(out, "A??") == 0);
	CHECK(utf8_to_display(out, sizeof out, "AB\0CD", 5) && strcmp(out, "AB") == 0);

	// No room at all: nothing written, and whole only for an empty text
	CHECK(utf8_to_display(out, 0, "", 0));
	CHECK(!utf8_to_display(out, 0, "A", 1));
	CHECK(!utf8_to_display(out, 1, "A", 1) && out[0] == '\0');

	const char *name = CONFIG_LANG_ZH ? "utf8DisplayTestZh" : "utf8DisplayTest";
	if (failures)
		printf("# %s FAILED (%d)\n", name, failures);
	else
		printf("# %s PASSED\n", name);
	return failures ? 1 : 0;
}
