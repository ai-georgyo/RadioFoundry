// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codec 2 inside core/codec2: what the encoder and the decoder share (codec2.h is the outside's). The frame's fields
// as indexes, packing them to bits and back (spec 4), the quantizer levels and dequantizers both sides read (spec 9,
// 10.2), and the fixed-point formats of the decoded parameters (also what the host tests compare against the reference
// harness's dumps). Not for use outside core/codec2 and its host tests.
//
// Ownership: c2_bits.c (pack, unpack, Gray code) and c2_tab.c (levels, dequantizers, the decoder's harmonic count) are
// the shared tables package's, written before either side; the encoder and the decoder only read them. Each side adds
// what only it needs in its own files.
#ifndef RFOUNDRY_C2_INT_H
#define RFOUNDRY_C2_INT_H

#include <stdbool.h>
#include <stdint.h>
#include "codec2.h"

#define C2_N		80	// samples per 10 ms analysis / synthesis frame
#define C2_ORDER	10	// LPC order, LSPs per frame
#define C2_L_MAX	79	// harmonics at most (spec 3.2); arrays hold C2_L_MAX + 1, index 0 unused

// Fixed-point formats of decoded parameters. Chosen so the decoded values meet the harness's params_close (spec 15.1:
// ω0 within 1e-5, E within 1e-4 relative, LSPs within 1e-4 rad) with room to spare
#define C2_WO_Q		28	// ω0, rad/sample (0.039..0.314)
#define C2_LSP_Q	28	// LSPs, rad; signed 32-bit reaches 8 rad (3200's random bits put the upper ones past π)
#define C2_E_Q		18	// LPC energy, linear, unsigned (0.1 .. 10^4)

// One codec frame's fields as indexes, before Gray coding (spec 4.2, 4.3)
typedef struct {
	uint8_t voiced[4];	// per 10 ms frame, 0 / 1: 3200 uses [0..1], 1600 [0..3]
	uint8_t wo[2];		// ω0 indexes, 7 bits: 3200 [0] (frame 2); 1600 [0] frame 2, [1] frame 4
	uint8_t e[2];		// energy indexes, 5 bits, as wo
	uint8_t lsp[C2_ORDER];	// 1600: LSP indexes (4,4,4,4,4,4,4,3,3,2 bits); 3200: LSP difference indexes (5 bits each)
} c2_index_t;

// The decoded parameters of one 10 ms frame (spec 10.2-10.4), before the LPC model gives the amplitudes
typedef struct {
	int32_t wo;		// C2_WO_Q
	uint32_t e;		// C2_E_Q
	int32_t lsp[C2_ORDER];	// C2_LSP_Q
	uint8_t L;		// harmonics (spec 10.2: the decoder's count, 79 at ω0_min)
	bool voiced;
} c2_params_t;

// --- c2_bits.c (tables package) ---
// Fields to the frame's 8 bytes in the spec's order, each Gray coded, MSB first; and back
void c2_pack(c2_mode_t mode, const c2_index_t *ix, uint8_t bits[C2_FRAME_BYTES]);
void c2_unpack(c2_mode_t mode, const uint8_t bits[C2_FRAME_BYTES], c2_index_t *ix);

// --- c2_tab.c (tables package) ---
// Quantizer levels in Hz (integers in both modes, spec 14.1, 14.2): LSP i (0-based) at index idx. bits: the field's
// width for LSP i in mode
uint8_t c2_lsp_bits(c2_mode_t mode, uint8_t i);
int16_t c2_lsp_level_hz(c2_mode_t mode, uint8_t i, uint8_t idx);
// ω0 for a 7-bit index (spec 9.1, table wo_dequant), C2_WO_Q; the decoder's L for it (table wo_dequant_L)
int32_t c2_wo_dequant(uint8_t idx);
uint8_t c2_wo_L(uint8_t idx);
// The decoder's L for an interpolated ω0 (spec 10.2: trunc(π_c / ω0) with the quotient in double), exactly
uint8_t c2_L_of_wo(int32_t wo);
// Energy for a 5-bit index (spec 9.2, table energy_dequant), C2_E_Q
uint32_t c2_e_dequant(uint8_t idx);
// LSPs from their indexes (spec 10.2): 3200 by summing the differences, 1600 with its ordering and spacing fixes
void c2_lsp_dequant(c2_mode_t mode, const uint8_t idx[C2_ORDER], int32_t lsp[C2_ORDER]);

// --- c2_dec.c (decoder package): for the host tests' parameter dumps ---
// The parameters of 10 ms frame k (0 or 1) of the block c2_dec_block gave last, and the random draws that frame took.
// False before the first block
bool c2_dec_params(const c2_dec_state_t *d, uint8_t k, c2_params_t *out, uint16_t *draws);

#endif
