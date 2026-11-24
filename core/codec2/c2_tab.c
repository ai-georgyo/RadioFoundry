// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codec 2 quantizer levels and dequantizers, read by both the encoder and the decoder (docs/spec/codec2.md 9, 10.2,
// 14), in integers: LSP levels in Hz, ω0 in C2_WO_Q, energy in C2_E_Q, LSPs in C2_LSP_Q.
//
// Where the numbers come from (core/codec2/THIRD_PARTY, docs/CLEANROOM.md): the LSP levels are values from the Codec 2
// reference implementation (LGPL 2.1), taken by decision of the RadioFoundry maintainer (2026-11-22), not under a
// separate grant from the Codec 2 authors. The ω0 and energy levels follow from the spec's formulas: they are the
// float32 tables wo_dequant and energy_dequant of spec 14.4, converted as each table says.
// The tables were written once from tables.json (spec 14), not generated; no code was taken from anywhere.
// tests/host/c2TabTest.c checks each one against the sum and SHA-256 that spec 14.4 lists.
#include "c2_int.h"

// --- LSP levels (spec 14.1, 14.2), Hz ---

// Mode 1600: one scalar quantizer per LSP, 4, 4, 4, 4, 4, 4, 4, 3, 3, 2 bits (tables lsp1600_q1 ... q10), end to end
static const int16_t c2_lsp1600_hz[132] = {
	// LSP 1
	225, 250, 275, 300, 325, 350, 375, 400, 425, 450, 475, 500, 525, 550, 575, 600,
	// LSP 2
	325, 350, 375, 400, 425, 450, 475, 500, 525, 550, 575, 600, 625, 650, 675, 700,
	// LSP 3
	500, 550, 600, 650, 700, 750, 800, 850, 900, 950, 1000, 1050, 1100, 1150, 1200, 1250,
	// LSP 4
	700, 800, 900, 1000, 1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000, 2100, 2200,
	// LSP 5
	950, 1050, 1150, 1250, 1350, 1450, 1550, 1650, 1750, 1850, 1950, 2050, 2150, 2250, 2350, 2450,
	// LSP 6
	1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000, 2100, 2200, 2300, 2400, 2500, 2600,
	// LSP 7
	1500, 1600, 1700, 1800, 1900, 2000, 2100, 2200, 2300, 2400, 2500, 2600, 2700, 2800, 2900, 3000,
	// LSP 8
	2300, 2400, 2500, 2600, 2700, 2800, 2900, 3000,
	// LSP 9
	2500, 2600, 2700, 2800, 2900, 3000, 3100, 3200,
	// LSP 10
	2900, 3100, 3300, 3500,
};
static const uint8_t c2_lsp1600_first[C2_ORDER] = { 0, 16, 32, 48, 64, 80, 96, 112, 120, 128 };
static const uint8_t c2_lsp1600_bits[C2_ORDER] = { 4, 4, 4, 4, 4, 4, 4, 3, 3, 2 };

// Mode 3200: ten 5-bit difference quantizers of two kinds. lspd3200_q1-q3 and q7-q10 are c2_lspd3200_hz[0], q4-q6
// (LSP 4 to 6, where the formants are) c2_lspd3200_hz[1]
static const int16_t c2_lspd3200_hz[2][32] = {
	{
		25, 50, 75, 100, 125, 150, 175, 200, 225, 250, 275, 300, 325, 350, 375, 400,
		425, 450, 475, 500, 525, 550, 575, 600, 625, 650, 675, 700, 725, 750, 775, 800,
	}, {
		25, 50, 75, 100, 125, 150, 175, 200, 250, 300, 350, 400, 450, 500, 550, 600,
		650, 700, 750, 800, 850, 900, 950, 1000, 1050, 1100, 1150, 1200, 1250, 1300, 1350, 1400,
	},
};

uint8_t c2_lsp_bits(c2_mode_t mode, uint8_t i)
{
	if (i >= C2_ORDER)
		return 0;
	return mode == C2_MODE_1600 ? c2_lsp1600_bits[i] : 5;
}

int16_t c2_lsp_level_hz(c2_mode_t mode, uint8_t i, uint8_t idx)
{
	if (i >= C2_ORDER)
		return 0;
	// An index wider than the field keeps its low bits, as packing it would
	idx &= (uint8_t)((1u << c2_lsp_bits(mode, i)) - 1u);
	if (mode == C2_MODE_1600)
		return c2_lsp1600_hz[c2_lsp1600_first[i] + idx];
	return c2_lspd3200_hz[(i >= 3 && i <= 5) ? 1 : 0][idx];
}

// --- ω0 (spec 9.1, 10.2) ---

// wo_dequant: f32(ω0_min + i · s), C2_WO_Q. Exact: every float32 at or above 2^-5 rad is a whole number of 2^-28 and
// ω0_min = 0.0393 is above 2^-5 = 0.03125, so these are the float32 levels themselves
static const int32_t c2_wo_q28[128] = {
	10541436, 11117921, 11694406, 12270890, 12847375, 13423860, 14000344, 14576829,
	15153314, 15729799, 16306284, 16882768, 17459252, 18035738, 18612222, 19188708,
	19765192, 20341676, 20918162, 21494646, 22071132, 22647616, 23224100, 23800584,
	24377070, 24953556, 25530040, 26106524, 26683008, 27259494, 27835978, 28412464,
	28988948, 29565432, 30141918, 30718402, 31294888, 31871372, 32447856, 33024342,
	33600824, 34177312, 34753796, 35330280, 35906764, 36483248, 37059736, 37636220,
	38212704, 38789188, 39365672, 39942160, 40518644, 41095128, 41671612, 42248096,
	42824584, 43401064, 43977552, 44554036, 45130520, 45707004, 46283492, 46859976,
	47436460, 48012944, 48589428, 49165916, 49742400, 50318884, 50895368, 51471852,
	52048340, 52624824, 53201308, 53777792, 54354276, 54930760, 55507248, 56083732,
	56660216, 57236700, 57813184, 58389672, 58966156, 59542640, 60119124, 60695608,
	61272092, 61848580, 62425064, 63001548, 63578032, 64154516, 64731004, 65307488,
	65883972, 66460456, 67036940, 67613424, 68189912, 68766400, 69342880, 69919360,
	70495856, 71072336, 71648816, 72225304, 72801792, 73378272, 73954760, 74531248,
	75107728, 75684208, 76260696, 76837184, 77413664, 77990144, 78566640, 79143120,
	79719600, 80296096, 80872576, 81449056, 82025552, 82602032, 83178512, 83754992,
};

int32_t c2_wo_dequant(uint8_t idx)
{
	return c2_wo_q28[idx & 127u];
}

// trunc(π_c / ω0) with ω0 = wo / 2^28 is floor(π_c · 2^28 / wo), and for a whole wo that is
// floor(floor(π_c · 2^28) / wo): one 32-bit divide. π_c · 2^28 = 843314856.643 (π_c = 3.141592654, spec 3.2). The
// reference forms the quotient in double; its rounding cannot carry a quotient up to the next whole number here,
// because π_c · 2^28 is 0.357 short of one and so π_c · 2^28 / wo is at least 0.357 / wo below any whole number, far
// more than a double's rounding error. c2TabTest proves the equality on every C2_WO_Q value from ω0_min to ω0_max
#define C2_PI_C_Q28	843314856u

uint8_t c2_L_of_wo(int32_t wo)
{
	// Below ω0_min (or not positive) L would pass the arrays' C2_L_MAX: the decoder never forms such an ω0
	if (wo <= 0)
		return C2_L_MAX;
	uint32_t L = C2_PI_C_Q28 / (uint32_t)wo;
	return L > C2_L_MAX ? C2_L_MAX : (uint8_t)L;
}

uint8_t c2_wo_L(uint8_t idx)
{
	// The table wo_dequant_L of spec 14.4 (79 at index 0, 10 at index 127), computed: c2TabTest checks all 128
	return c2_L_of_wo(c2_wo_dequant(idx));
}

// --- Energy (spec 9.2) ---

// energy_dequant: 10^((-10 + 1.5625 i) / 10), the float32 value rounded to C2_E_Q (0.1 at index 0 is 26214 / 2^18,
// 1.5e-5 relative, the largest error of the table)
static const uint32_t c2_e_q18[32] = {
	26214u, 37566u, 53832u, 77142u, 110545u, 158413u, 227007u, 325304u,
	466165u, 668021u, 957282u, 1371797u, 1965803u, 2817020u, 4036825u, 5784821u,
	8289721u, 11879275u, 17023150u, 24394388u, 34957464u, 50094484u, 71786024u, 102870280u,
	147414400u, 211246688u, 302719168u, 433800384u, 621641408u, 890819904u, 1276556160u, 1829320960u,
};

uint32_t c2_e_dequant(uint8_t idx)
{
	return c2_e_q18[idx & 31u];
}

// --- LSPs (spec 10.2) ---

// Hz to rad in C2_LSP_Q: hz · π_c/4000 · 2^28, with the factor 210828.714161 as 210828 + 46803 / 2^16 so both products
// fit 32 bits for any sum of levels (at most 9800 Hz: 2.07e9 < 2^31). Within 0.6 of a unit of the exact product
static int32_t c2_hz_to_rad(uint32_t hz)
{
	return (int32_t)(hz * 210828u + ((hz * 46803u + 0x8000u) >> 16));
}

#define C2_LSP_0P1		26843546	// f32(0.1) rad, the ordering fix's step: exactly 26843546 / 2^28
// The minimum spacings, 50 and 100 Hz as rad (50 · π_c/4000 = 10541435.708 / 2^28, 100 Hz 21082871.416): a gap is
// short when below the exact value, so when below its ceiling in whole units; a short gap is set to the rounded value
#define C2_LSP_50HZ_MIN		10541436
#define C2_LSP_50HZ		10541436
#define C2_LSP_100HZ_MIN	21082872
#define C2_LSP_100HZ		21082871
// The ordering scan stops after this many fixes whatever is left. It never gets there: over all 2^36 index
// combinations it makes 15 fixes at most (470 of them; docs/spec/codec2.md 16). The cap bounds the loop by construction
// and the values with it: each fix moves the lowest value down and the highest up by at most 0.1 rad, so after 20 the
// LSPs stay within -1.9..4.8 rad and every difference below fits 32 bits
#define C2_LSP_FIX_MAX		20u

void c2_lsp_dequant(c2_mode_t mode, const uint8_t idx[C2_ORDER], int32_t lsp[C2_ORDER])
{
	if (mode != C2_MODE_1600) {
		// 3200: each level is the step from the LSP below; the sum in Hz is exact, then one conversion each
		uint32_t hz = 0;
		for (uint8_t i = 0; i < C2_ORDER; i++) {
			hz += (uint16_t)c2_lsp_level_hz(mode, i, idx[i]);
			lsp[i] = c2_hz_to_rad(hz);
		}
		return;
	}

	for (uint8_t i = 0; i < C2_ORDER; i++)
		lsp[i] = c2_hz_to_rad((uint32_t)c2_lsp_level_hz(mode, i, idx[i]));

	// 1. Ordering: a pair out of order swaps and moves 0.1 rad apart each way, then the scan starts again at the
	// third pair (LSPs 2 and 3); the first pair is looked at only on the first pass
	unsigned fixes = 0;
	for (uint8_t i = 1; i < C2_ORDER; i++) {
		if (lsp[i] < lsp[i - 1]) {
			int32_t above = lsp[i - 1];
			lsp[i - 1] = lsp[i] - C2_LSP_0P1;
			lsp[i] = above + C2_LSP_0P1;
			if (++fixes >= C2_LSP_FIX_MAX)
				break;
			i = 1;	// the loop's increment makes it 2: the pair (lsp[1], lsp[2])
		}
	}

	// 2. Minimum spacing, in increasing i: 50 Hz above LSPs 1-3, 100 Hz above LSPs 4-9
	for (uint8_t i = 1; i < C2_ORDER; i++) {
		int32_t min = i < 4 ? C2_LSP_50HZ_MIN : C2_LSP_100HZ_MIN;
		if (lsp[i] - lsp[i - 1] < min)
			lsp[i] = lsp[i - 1] + (i < 4 ? C2_LSP_50HZ : C2_LSP_100HZ);
	}
}
