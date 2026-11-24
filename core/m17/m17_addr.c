// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 addresses (m17.h, m17.md 10): base-40 text to a 48-bit address and back. The tree's one base-40 codec: the
// codeplug, the engine and the UI (callsign text) all call it
#include "config.h"
#if CONFIG_M17
#include <string.h>
#include "m17/m17.h"

uint8_t m17_addr_value(char c)
{
	if (c >= 'a' && c <= 'z')
		return (uint8_t)(c - 'a' + 1);
	if (c >= 'A' && c <= 'Z')
		return (uint8_t)(c - 'A' + 1);
	if (c >= '0' && c <= '9')
		return (uint8_t)(c - '0' + 27);
	return c == '-' ? 37u : c == '/' ? 38u : c == '.' ? 39u : 0u;
}

bool m17_addr_from_text(const char *text, uint64_t *addr)
{
	size_t n = strlen(text);
	uint64_t a = 0;

	*addr = 0;
	while (n > 0 && text[n - 1] == ' ')
		n--;			// trailing spaces are value 0: the same address without them
	if (n == 0 || n > M17_CALL_MAX)
		return false;		// all spaces is the reserved address; more than 9 characters doesn't fit 48 bits
	for (size_t i = 0; i < n; i++) {
		if (text[i] != ' ' && m17_addr_value(text[i]) == 0)
			return false;	// outside the alphabet: never mapped to a space, so a typo can't make another call
	}
	// The first character is the least significant digit: from the last character to the first
	while (n-- > 0)
		a = a * 40u + m17_addr_value(text[n]);
	*addr = a;
	return true;
}

uint8_t m17_addr_text(uint64_t addr, char out[M17_ADDR_TEXT])
{
	static const char hex[] = "0123456789ABCDEF";
	uint8_t n = 0;

	addr &= M17_ADDR_BROADCAST;
	if (addr == M17_ADDR_BROADCAST) {
		out[0] = '\0';
		return M17_ADDR_KIND_BROADCAST;
	}
	if (addr == M17_ADDR_RESERVED || addr > M17_ADDR_STD_MAX) {
		// The spec gives these no text: 12 hex digits, so two of them never show alike
		for (int i = 0; i < 12; i++)
			out[i] = hex[(addr >> (44 - 4 * i)) & 0xFu];
		out[12] = '\0';
		return addr == M17_ADDR_RESERVED ? M17_ADDR_KIND_RESERVED : M17_ADDR_KIND_EXTENDED;
	}
	// The characters come out first to last; the last digit is never 0, so no trailing space
	for (; addr != 0 && n < M17_CALL_MAX; addr /= 40u)
		out[n++] = m17_addr_char((uint8_t)(addr % 40u));
	out[n] = '\0';
	return M17_ADDR_KIND_STD;
}

#endif	// CONFIG_M17
