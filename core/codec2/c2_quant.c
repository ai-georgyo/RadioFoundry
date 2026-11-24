// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codec 2's encoder quantizers (docs/spec/codec2.md 9): ω0 and the LSPs to the nearest of c2_tab.c's levels (the
// decoder's own, so the two sides cannot disagree on a level), the energy on its uniform dB scale from the log2 the
// LPC analysis gives.
#include "c2_enc_int.h"
#include "../dsp/fxp.h"

// 9.1: the nearest of the 128 reproduction points ω0_min + i s, ties to the upper (the spec's floor(128 n + 0.5)). A
// first guess from the level spacing, then the midpoints decide
uint8_t c2e_wo_index(int32_t wo)
{
	int32_t lo = c2_wo_dequant(0), step = c2_wo_dequant(1) - lo;
	int32_t i;

	if (wo > c2_wo_dequant(127))
		wo = c2_wo_dequant(127);
	i = wo <= lo ? 0 : (wo - lo) / step;

	if (i > 127)
		i = 127;
	// Twice ω0 against the sum of two levels: all under 2^28, so in 32 bits
	while (i > 0 && 2 * wo < c2_wo_dequant((uint8_t)(i - 1)) + c2_wo_dequant((uint8_t)i))
		i--;
	while (i < 127 && 2 * wo >= c2_wo_dequant((uint8_t)i) + c2_wo_dequant((uint8_t)(i + 1)))
		i++;
	return (uint8_t)i;
}

// 9.2: index = floor(32 (e_dB + 10) / 50 + 0.5) with e_dB = 10 log10 E = 3.0103 log2 E, so
// floor(1.9265920 log2 E + 6.9); clamped to 0..31, and 0 for E <= 0
#define E_SLOPE		126261		// 10 log10(2) / 1.5625 in Q16
#define E_OFFSET	926102323	// 6.9 in Q27

uint8_t c2e_e_index(const c2e_lpc_t *l)
{
	if (!l->e_pos)
		return 0;
	int64_t v = fxp_mul64(l->e_log2, E_SLOPE) + E_OFFSET;	// Q11 x Q16 = Q27
	int64_t i = v >> 27;

	return (uint8_t)(i < 0 ? 0 : i > 31 ? 31 : i);
}

// The level of LSP i nearest to f (Hz, C2E_LSPHZ_Q): minimum squared error, ties to the lower index (9.3)
static uint8_t nearest(c2_mode_t mode, uint8_t i, int32_t f, int16_t *level)
{
	uint8_t n = (uint8_t)(1u << c2_lsp_bits(mode, i)), best = 0;
	uint32_t dmin = UINT32_MAX;

	for (uint8_t j = 0; j < n; j++) {
		int16_t lv = c2_lsp_level_hz(mode, i, j);
		int32_t d = (lv << C2E_LSPHZ_Q) - f;
		uint32_t ad = d < 0 ? 0u - (uint32_t)d : (uint32_t)d;

		if (ad < dmin) {
			dmin = ad;
			best = j;
			*level = lv;
		}
	}
	return best;
}

void c2e_lsp_index(c2_mode_t mode, const int32_t lsp_hz[C2_ORDER], uint8_t idx[C2_ORDER])
{
	int16_t lv = 0;

	if (mode == C2_MODE_1600) {
		// 9.3: each LSP on its own
		for (uint8_t i = 0; i < C2_ORDER; i++)
			idx[i] = nearest(mode, i, lsp_hz[i], &lv);
		return;
	}
	// 9.4: each difference from the LSP below as the decoder will rebuild it, so the error does not accumulate
	int32_t prev = 0;	// the quantized LSP below, Hz
	for (uint8_t i = 0; i < C2_ORDER; i++) {
		idx[i] = nearest(mode, i, lsp_hz[i] - (prev << C2E_LSPHZ_Q), &lv);
		prev += lv;
	}
}
