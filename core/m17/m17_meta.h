// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 META (m17.md 4): the 14 bytes of the LSF that TYPE's META type describes. Text in up to four 13-byte blocks
// collected by their control bytes, a GNSS position in its bit fields, an extended callsign. Encoding and decoding
// only, in the protocol's own units; conversion to the tree's position types is a pair of helpers. Pure functions.
// Built with CONFIG_M17.
#ifndef RFOUNDRY_M17_META_H
#define RFOUNDRY_M17_META_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "m17.h"

// --- Text (m17.md 4.1) ---
#define M17_TEXT_BLOCK		13u
#define M17_TEXT_BLOCKS		4u
#define M17_TEXT_MAX		(M17_TEXT_BLOCK * M17_TEXT_BLOCKS)	// 52 bytes of UTF-8

// What a receiver has collected of one transmission's text
typedef struct {
	uint8_t want;			// the OR of the control bytes' high nibbles: blocks the message has
	uint8_t have;			// ... and of their low nibbles: blocks received
	uint8_t text[M17_TEXT_MAX];	// block i at 13 i; spaces padding the last
} m17_text_t;

void m17_text_clear(m17_text_t *t);
// A META of type text: its block taken in. True when something new came (a block not seen before)
bool m17_text_add(m17_text_t *t, const uint8_t meta[M17_META_BYTES]);
// Complete: every block the message has is in (the nibbles equal, not zero)
static inline bool m17_text_done(const m17_text_t *t)
{
	return t->want != 0 && t->want == t->have;
}
// The blocks received in order, trailing spaces dropped, as UTF-8 (out holds M17_TEXT_MAX + 1); a missing block is
// left out. Returns the length
size_t m17_text_get(const m17_text_t *t, char *out);
// How many blocks n bytes of text take (0 for none, at most 4: longer text is cut at 52 bytes, never inside a UTF-8
// character), and block i's META (control byte, 13 bytes, the last padded with spaces)
uint8_t m17_text_blocks(const uint8_t *utf8, size_t n);
void m17_text_meta(const uint8_t *utf8, size_t n, uint8_t i, uint8_t meta[M17_META_BYTES]);

// --- GNSS (m17.md 4.2) ---
#define M17_GNSS_VALID_POS	0x8u
#define M17_GNSS_VALID_ALT	0x4u
#define M17_GNSS_VALID_VEL	0x2u
#define M17_GNSS_VALID_RADIUS	0x1u
enum { M17_GNSS_SRC_CLIENT = 0, M17_GNSS_SRC_OTHER = 15 };	// what this firmware sends: 0 (m17.md 4.2)
enum { M17_STATION_FIXED = 0, M17_STATION_MOBILE = 1, M17_STATION_HANDHELD = 2, M17_STATION_OTHER = 15 };

// The fields as sent
typedef struct {
	uint8_t source, station, valid;	// 4 bits each
	uint8_t radius;			// 3 bits: radius <= 2^n m
	uint16_t bearing;		// degrees, 0..359
	int32_t lat, lon;		// 24-bit signed: degrees x 8388607 / 90 and / 180
	uint16_t alt;			// (m + 500) x 2
	uint16_t speed;			// km/h x 2, 12 bits
} m17_gnss_t;

void m17_gnss_encode(const m17_gnss_t *g, uint8_t meta[M17_META_BYTES]);
// False when nothing valid is in it
bool m17_gnss_decode(const uint8_t meta[M17_META_BYTES], m17_gnss_t *g);
// From and to the tree's units (core/position: degrees x 1e7, meters, km/h, degrees); err_m 0 = unknown radius
void m17_gnss_from_fix(m17_gnss_t *g, int32_t lat_e7, int32_t lon_e7, int32_t alt_m, bool alt_ok, uint16_t speed_kmh,
		       uint16_t course, bool vel_ok, uint32_t err_m, uint8_t station);
void m17_gnss_to_fix(const m17_gnss_t *g, int32_t *lat_e7, int32_t *lon_e7, int32_t *alt_dm, uint16_t *speed_kmh10);

// --- Extended callsign (m17.md 4.3) ---
static inline uint64_t m17_extcall_1(const uint8_t meta[M17_META_BYTES])
{
	return m17_addr_get(meta);
}
static inline uint64_t m17_extcall_2(const uint8_t meta[M17_META_BYTES])
{
	return m17_addr_get(meta + M17_ADDR_BYTES);
}

#endif
