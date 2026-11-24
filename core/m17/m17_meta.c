// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 META (m17_meta.h; docs/spec/m17.md 4): text blocks, the GNSS fields, and their conversion to the tree's units.
// The extended callsign is two addresses (the header's helpers).
#include "config.h"

#if CONFIG_M17

#include <string.h>

#include "m17/m17_meta.h"

// --- Text (m17.md 4.1) ---
void m17_text_clear(m17_text_t *t)
{
	memset(t, 0, sizeof *t);
}

// The high nibble is 1, 3, 7 or 15 (the blocks the message has), the low one a single block among them; any other
// control byte is not taken. A block again with other bytes replaces the old ones and counts as new: the text is not
// expected to change, but a receiver that shows the latest is never stuck with a corrupted copy
bool m17_text_add(m17_text_t *t, const uint8_t meta[M17_META_BYTES])
{
	uint8_t hi = (uint8_t)(meta[0] >> 4), lo = (uint8_t)(meta[0] & 0xFu), i = 0;
	uint8_t *at;

	if ((hi != 0x1u && hi != 0x3u && hi != 0x7u && hi != 0xFu) || lo == 0 || (lo & (lo - 1u)) || (lo & ~hi))
		return false;
	while (!((lo >> i) & 1u))
		i++;
	at = t->text + M17_TEXT_BLOCK * i;
	t->want |= hi;
	if ((t->have & lo) && !memcmp(at, meta + 1, M17_TEXT_BLOCK))
		return false;
	memcpy(at, meta + 1, M17_TEXT_BLOCK);
	t->have |= lo;
	return true;
}

// Trailing spaces are the last block's padding; zero bytes too, which a sender that pads wrongly leaves
size_t m17_text_get(const m17_text_t *t, char *out)
{
	size_t n = 0;

	for (uint8_t i = 0; i < M17_TEXT_BLOCKS; i++) {
		if ((t->have >> i) & 1u) {
			memcpy(out + n, t->text + M17_TEXT_BLOCK * i, M17_TEXT_BLOCK);
			n += M17_TEXT_BLOCK;
		}
	}
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0'))
		n--;
	out[n] = '\0';
	return n;
}

// Text past 52 bytes cut back to the start of the character it would split (UTF-8 continuation bytes are 10xxxxxx)
static size_t text_len(const uint8_t *utf8, size_t n)
{
	if (n <= M17_TEXT_MAX)
		return n;
	n = M17_TEXT_MAX;
	while (n > 0 && (utf8[n] & 0xC0u) == 0x80u)
		n--;
	return n;
}

uint8_t m17_text_blocks(const uint8_t *utf8, size_t n)
{
	return (uint8_t)((text_len(utf8, n) + M17_TEXT_BLOCK - 1u) / M17_TEXT_BLOCK);
}

// i past the last block is taken modulo the block count, so a caller can count superframes; no text is control byte
// 00 and zeros
void m17_text_meta(const uint8_t *utf8, size_t n, uint8_t i, uint8_t meta[M17_META_BYTES])
{
	uint8_t blocks = m17_text_blocks(utf8, n);
	size_t at, len;

	memset(meta, 0, M17_META_BYTES);
	if (!blocks)
		return;
	n = text_len(utf8, n);
	i = (uint8_t)(i % blocks);
	meta[0] = (uint8_t)(((1u << blocks) - 1u) << 4 | 1u << i);
	at = (size_t)M17_TEXT_BLOCK * i;
	len = n - at < M17_TEXT_BLOCK ? n - at : M17_TEXT_BLOCK;
	memcpy(meta + 1, utf8 + at, len);
	memset(meta + 1 + len, ' ', M17_TEXT_BLOCK - len);
}

// --- GNSS (m17.md 4.2) ---
#define LAT_MAX		8388607	// 2^23 - 1: 90 degrees (latitude), 180 (longitude)
#define BEARING_MAX	359u
#define SPEED_MAX	0xFFFu
#define E7_90		900000000LL

static int32_t s24_get(const uint8_t *p)
{
	int32_t v = (int32_t)((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);

	return v >= 0x800000 ? v - 0x1000000 : v;
}

static void s24_put(uint8_t *p, int32_t v)
{
	uint32_t u = (uint32_t)v & 0xFFFFFFu;

	p[0] = (uint8_t)(u >> 16);
	p[1] = (uint8_t)(u >> 8);
	p[2] = (uint8_t)u;
}

static int32_t clamp24(int32_t v)
{
	return v > LAT_MAX ? LAT_MAX : v < -LAT_MAX ? -LAT_MAX : v;
}

// A field whose validity bit is clear goes out as zeros; out-of-range values are clamped (-8388608 is never sent)
void m17_gnss_encode(const m17_gnss_t *g, uint8_t meta[M17_META_BYTES])
{
	uint8_t v = (uint8_t)(g->valid & 0xFu);
	uint16_t bearing = 0, speed = 0;

	memset(meta, 0, M17_META_BYTES);
	meta[0] = (uint8_t)((g->source & 0xFu) << 4 | (g->station & 0xFu));
	if (v & M17_GNSS_VALID_VEL) {
		bearing = g->bearing > BEARING_MAX ? (uint16_t)(g->bearing % 360u) : g->bearing;
		speed = g->speed > SPEED_MAX ? (uint16_t)SPEED_MAX : g->speed;
	}
	meta[1] = (uint8_t)(v << 4 | ((v & M17_GNSS_VALID_RADIUS) ? (g->radius & 7u) << 1 : 0u) | bearing >> 8);
	meta[2] = (uint8_t)bearing;
	if (v & M17_GNSS_VALID_POS) {
		s24_put(meta + 3, clamp24(g->lat));
		s24_put(meta + 6, clamp24(g->lon));
	}
	if (v & M17_GNSS_VALID_ALT) {
		meta[9] = (uint8_t)(g->alt >> 8);
		meta[10] = (uint8_t)g->alt;
	}
	meta[11] = (uint8_t)(speed >> 4);
	meta[12] = (uint8_t)((speed & 0xFu) << 4);
}

// Fields without their validity bit come out as 0. A bearing over 359 (never sent) makes the velocity invalid, and
// -8388608 reads as -8388607
bool m17_gnss_decode(const uint8_t meta[M17_META_BYTES], m17_gnss_t *g)
{
	memset(g, 0, sizeof *g);
	g->source = (uint8_t)(meta[0] >> 4);
	g->station = (uint8_t)(meta[0] & 0xFu);
	g->valid = (uint8_t)(meta[1] >> 4);
	if (g->valid & M17_GNSS_VALID_RADIUS)
		g->radius = (uint8_t)((meta[1] >> 1) & 7u);
	if (g->valid & M17_GNSS_VALID_VEL) {
		g->bearing = (uint16_t)((meta[1] & 1u) << 8 | meta[2]);
		g->speed = (uint16_t)(meta[11] << 4 | meta[12] >> 4);
		if (g->bearing > BEARING_MAX) {
			g->valid = (uint8_t)(g->valid & ~M17_GNSS_VALID_VEL);
			g->bearing = 0;
			g->speed = 0;
		}
	}
	if (g->valid & M17_GNSS_VALID_POS) {
		g->lat = clamp24(s24_get(meta + 3));
		g->lon = clamp24(s24_get(meta + 6));
	}
	if (g->valid & M17_GNSS_VALID_ALT)
		g->alt = (uint16_t)(meta[9] << 8 | meta[10]);
	return g->valid != 0;
}

// a * num / den to the nearest, halves away from zero (den > 0)
static int32_t mul_div(int64_t a, int64_t num, int64_t den)
{
	int64_t p = a * num;

	return (int32_t)(p >= 0 ? (p + den / 2) / den : -((-p + den / 2) / den));
}

// The position is always valid (a fix has one); speed and course go out only with vel_ok, course 360 and up as 0
void m17_gnss_from_fix(m17_gnss_t *g, int32_t lat_e7, int32_t lon_e7, int32_t alt_m, bool alt_ok, uint16_t speed_kmh,
		       uint16_t course, bool vel_ok, uint32_t err_m, uint8_t station)
{
	memset(g, 0, sizeof *g);
	g->source = M17_GNSS_SRC_CLIENT;
	g->station = (uint8_t)(station & 0xFu);
	g->valid = M17_GNSS_VALID_POS;
	lat_e7 = lat_e7 > E7_90 ? (int32_t)E7_90 : lat_e7 < -E7_90 ? (int32_t)-E7_90 : lat_e7;
	lon_e7 = lon_e7 > 2 * E7_90 ? (int32_t)(2 * E7_90) : lon_e7 < -2 * E7_90 ? (int32_t)(-2 * E7_90) : lon_e7;
	g->lat = clamp24(mul_div(lat_e7, LAT_MAX, E7_90));
	g->lon = clamp24(mul_div(lon_e7, LAT_MAX, 2 * E7_90));
	if (alt_ok) {
		// (m + 500) x 2, 0 to 65535: -500 m to 32267.5 m
		int32_t a = alt_m < -500 ? 0 : alt_m > 32267 ? 0xFFFF : (alt_m + 500) * 2;

		g->alt = (uint16_t)a;
		g->valid |= M17_GNSS_VALID_ALT;
	}
	if (vel_ok) {
		g->speed = speed_kmh > SPEED_MAX / 2u ? (uint16_t)SPEED_MAX : (uint16_t)(speed_kmh * 2u);
		g->bearing = course > BEARING_MAX ? 0 : course;
		g->valid |= M17_GNSS_VALID_VEL;
	}
	if (err_m) {
		// The smallest n with err_m <= 2^n m, 7 for anything over 128 m
		uint8_t n = 0;

		while (n < 7 && err_m > (1ul << n))
			n++;
		g->radius = n;
		g->valid |= M17_GNSS_VALID_RADIUS;
	}
}

// Each output may be NULL. Without its validity bit a field comes out as the tree's "none": position 0, 0; altitude
// INT32_MIN; speed 0xFFFF
void m17_gnss_to_fix(const m17_gnss_t *g, int32_t *lat_e7, int32_t *lon_e7, int32_t *alt_dm, uint16_t *speed_kmh10)
{
	bool pos = (g->valid & M17_GNSS_VALID_POS) != 0;

	if (lat_e7)
		*lat_e7 = pos ? mul_div(clamp24(g->lat), E7_90, LAT_MAX) : 0;
	if (lon_e7)
		*lon_e7 = pos ? mul_div(clamp24(g->lon), 2 * E7_90, LAT_MAX) : 0;
	if (alt_dm)
		*alt_dm = (g->valid & M17_GNSS_VALID_ALT) ? (int32_t)g->alt * 5 - 5000 : INT32_MIN;
	if (speed_kmh10)
		*speed_kmh10 = (g->valid & M17_GNSS_VALID_VEL) ?
			       (uint16_t)((g->speed > SPEED_MAX ? SPEED_MAX : g->speed) * 5u) : 0xFFFFu;
}

#endif
