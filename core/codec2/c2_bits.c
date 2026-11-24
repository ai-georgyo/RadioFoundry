// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codec 2 frame fields to bits and back (docs/spec/codec2.md 4): every field Gray coded, MSB first, packed without
// padding from the MSB of byte 0. The field order of each mode is a table of offsets into c2_index_t (all its fields
// are bytes), so pack and unpack share one loop and cannot disagree on the layout.
#include <stddef.h>
#include "c2_int.h"

typedef struct {
	uint8_t off;		// offsetof(c2_index_t, field)
	uint8_t width;		// bits
} c2_field_t;

#define F(member, w)	{ (uint8_t)offsetof(c2_index_t, member), (w) }

// Spec 4.2: 2 + 7 + 5 + 10 x 5 = 64 bits
static const c2_field_t c2_fields_3200[] = {
	F(voiced[0], 1), F(voiced[1], 1), F(wo[0], 7), F(e[0], 5),
	F(lsp[0], 5), F(lsp[1], 5), F(lsp[2], 5), F(lsp[3], 5), F(lsp[4], 5),
	F(lsp[5], 5), F(lsp[6], 5), F(lsp[7], 5), F(lsp[8], 5), F(lsp[9], 5),
};

// Spec 4.3: frames 1-2 (voicing, ω0, energy), frames 3-4 the same, then frame 4's LSPs: 14 + 14 + 36 = 64 bits
static const c2_field_t c2_fields_1600[] = {
	F(voiced[0], 1), F(voiced[1], 1), F(wo[0], 7), F(e[0], 5),
	F(voiced[2], 1), F(voiced[3], 1), F(wo[1], 7), F(e[1], 5),
	F(lsp[0], 4), F(lsp[1], 4), F(lsp[2], 4), F(lsp[3], 4), F(lsp[4], 4),
	F(lsp[5], 4), F(lsp[6], 4), F(lsp[7], 3), F(lsp[8], 3), F(lsp[9], 2),
};

#undef F

#define N_FIELDS(t)	(sizeof(t) / sizeof((t)[0]))

static const c2_field_t *c2_fields(c2_mode_t mode, uint8_t *n)
{
	if (mode == C2_MODE_1600) {
		*n = (uint8_t)N_FIELDS(c2_fields_1600);
		return c2_fields_1600;
	}
	*n = (uint8_t)N_FIELDS(c2_fields_3200);
	return c2_fields_3200;
}

// Inverse of g = i ^ (i >> 1) for up to 8 bits: the prefix XOR of g from its MSB down
static uint8_t gray_decode(uint8_t g)
{
	g ^= (uint8_t)(g >> 1);
	g ^= (uint8_t)(g >> 2);
	g ^= (uint8_t)(g >> 4);
	return g;
}

void c2_pack(c2_mode_t mode, const c2_index_t *ix, uint8_t bits[C2_FRAME_BYTES])
{
	const uint8_t *src = (const uint8_t *)ix;
	uint8_t n;
	const c2_field_t *f = c2_fields(mode, &n);
	unsigned pos = 0;

	for (unsigned k = 0; k < C2_FRAME_BYTES; k++)
		bits[k] = 0;
	for (uint8_t k = 0; k < n; k++) {
		uint8_t w = f[k].width;
		uint8_t v = src[f[k].off];
		// An index wider than its field keeps its low bits; a voicing flag is any non-zero value
		v = (w == 1) ? (uint8_t)(v != 0) : (uint8_t)(v & ((1u << w) - 1u));
		v ^= (uint8_t)(v >> 1);
		for (int b = w - 1; b >= 0; b--, pos++)
			if ((v >> b) & 1u)
				bits[pos >> 3] |= (uint8_t)(0x80u >> (pos & 7u));
	}
}

void c2_unpack(c2_mode_t mode, const uint8_t bits[C2_FRAME_BYTES], c2_index_t *ix)
{
	uint8_t *dst = (uint8_t *)ix;
	uint8_t n;
	const c2_field_t *f = c2_fields(mode, &n);
	unsigned pos = 0;

	// The fields the mode does not carry (3200: voicing 3-4, the second ω0 and energy) read as 0
	for (unsigned k = 0; k < sizeof(*ix); k++)
		dst[k] = 0;
	for (uint8_t k = 0; k < n; k++) {
		uint8_t v = 0;
		for (uint8_t b = 0; b < f[k].width; b++, pos++)
			v = (uint8_t)((v << 1) | ((bits[pos >> 3] >> (7u - (pos & 7u))) & 1u));
		dst[f[k].off] = gray_decode(v);
	}
}

// What the spec's encoder sends for digital silence once its start-up frames are past (codec2.md 16): unvoiced; ω0
// index 3, since on zero input the pitch estimator still sees the 1.0 its DC notch adds and settles on 59.375 Hz;
// energy index 0 (E = 0, the lowest level); the LSPs of zero energy, (π_c/10) · i, quantized.
//   3200: ω0 3, energy 0, LSP difference indexes 0, 14, 15, 11, 11, 11, 15, 15, 15, 15
//   1600: ω0 3 and 3, energy 0 and 0, LSP indexes 0, 3, 6, 5, 6, 9, 9, 5, 7, 3
// A decoder fed these continues as it would on a silent microphone. c2TabTest checks the fields
const uint8_t c2_silence_3200[C2_FRAME_BYTES] = { 0x01, 0x00, 0x09, 0x43, 0x9C, 0xE4, 0x21, 0x08 };
const uint8_t c2_silence_1600[C2_FRAME_BYTES] = { 0x01, 0x00, 0x04, 0x00, 0x25, 0x75, 0xDD, 0xF2 };
