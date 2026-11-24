// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codec 2 encoder's own declarations (docs/spec/codec2.md 5-9 and "What this firmware does: the encoder"): the
// stages its files share, their tables and fixed-point formats, and what the host tests read back. Only the encoder's
// files (c2_enc.c, c2_nlp.c, c2_lpc.c, c2_quant.c, c2_enc_tab.c) and their host tests include it; the rest of the
// firmware sees codec2.h, and the decoder c2_int.h.
#ifndef RFOUNDRY_C2_ENC_INT_H
#define RFOUNDRY_C2_ENC_INT_H

#include <stdbool.h>
#include <stdint.h>
#include "c2_int.h"
#include "../dsp/dsp.h"

#define C2E_M		320	// analysis buffer (40 ms, spec 5.1)
#define C2E_WIN_HALF	139	// analysis window w[160 + d] for d = 0..138, symmetric about 160 (spec 5.2)
#define C2E_FIR_TAPS	48	// pitch estimator low-pass (spec 6.3)
#define C2E_FIR_MEM	(C2E_FIR_TAPS - 1)	// its inputs kept from the frame before
#define C2E_NLP_DEC	64	// decimated pitch estimator history, every fifth of q[0..319] (spec 6.4)
#define C2E_NLP_KEEP	(C2E_NLP_DEC - C2_N / 5)	// of it, kept between frames (q[0..239])
#define C2E_NLP_LO	16	// pitch estimator bins searched (spec 6.5): 50 Hz
#define C2E_NLP_HI	128	// 400 Hz
#define C2E_WSPEC_N	17	// window spectrum W(256 + d) for |d| = 0..16 (spec 5.4)

// Formats. ω0 is C2_WO_Q (Q28) as the dequantizer's; periods Q16 samples; bins per harmonic Q20; the LPC polynomial
// Q22; cos ω of the LSP search Q30; LSPs in Hz Q16; log2 of the LPC energy Q11 (dsp_log2_q11)
#define C2E_PER_Q	16
#define C2E_BIN_Q	20
#define C2E_A_Q		22
#define C2E_LSPHZ_Q	16
#define C2E_LOG_Q	11

// v 2^-s for a 64-bit v whose result fits 32 bits, 0 <= s < 64, in 32-bit operations: a variable 64-bit shift is a
// library call on C-SKY
static inline uint32_t c2e_shr64(uint64_t v, int s)
{
	uint32_t h = (uint32_t)(v >> 32), l = (uint32_t)v;

	if (s >= 32)
		return h >> (s - 32);
	return s == 0 ? l : (h << (32 - s)) | (l >> s);
}

// Bits of a magnitude: 0 for 0, else floor(log2 v) + 1
static inline int c2e_bits(uint32_t v)
{
	return 32 - dsp_norm32(v);
}

static inline int c2e_bits64(uint64_t v)
{
	uint32_t h = (uint32_t)(v >> 32);

	return h ? 64 - dsp_norm32(h) : c2e_bits((uint32_t)v);
}

static inline uint32_t c2e_mag(int32_t v)
{
	return v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
}

// --- c2_enc_tab.c ---
extern const uint16_t c2e_win[C2E_WIN_HALF];	// w[160 + d] = w[160 - d], Q23 (table analysis_window)
extern const int16_t c2e_nlp_win[C2E_NLP_DEC / 2];	// g[i] = g[63 - i], Q15 (table nlp_dft_window)
extern const int32_t c2e_fir[C2E_FIR_TAPS / 2];	// h[j] = h[47 - j], Q31 (table nlp_lpf_fir, spec 14.3)
extern const int16_t c2e_wspec[C2E_WSPEC_N];	// W(256 + d) = W(256 - d), Q15 (table analysis_window_dft)
extern const int32_t c2e_bw[C2_ORDER + 1];	// bandwidth expansion factors c_i, Q30 (spec 8.4)

// --- c2_nlp.c: the pitch estimator (spec 6) ---
typedef struct {
	int32_t x_prev, y_prev;		// DC notch memories: its last input (a square) and output, Q0
	int32_t fir[C2E_FIR_MEM];	// the low-pass's last inputs (notch output + 1), Q0
	int32_t d[C2E_NLP_KEEP];	// the decimated history q[0], q[5] ... q[235], Q0
	uint8_t k_prev;			// the previous estimate's bin (f0_prev = 3.125 k_prev Hz; 16 at creation)
} c2e_nlp_t;

// Per call; the FFT buffer is dsp_rfft's 2 * DSP_FFT_N words
typedef struct {
	int32_t fft[DSP_FFT_SIZE];
	int32_t u[C2E_FIR_MEM + C2_N];	// the low-pass's input over this frame
} c2e_nlp_scr_t;

void c2e_nlp_init(c2e_nlp_t *s);
// The 80 newest samples are B[240..319]. Returns k_c, the bin of f0 = 3.125 k_c Hz (16..128)
uint8_t c2e_nlp(c2e_nlp_t *s, const int16_t B[C2E_M], c2e_nlp_scr_t *scr);

// --- c2_lpc.c: LPC analysis, energy and LSPs (spec 8) ---
typedef struct {
	int32_t e_log2;			// log2 of the LPC energy E, Q11; meaningless when !e_pos
	bool e_pos;			// E > 0 (a window of zeros has E = 0, spec 8.1)
	bool lsp_ok;			// all ten roots found (else lsp_hz holds the fallback of spec 8.5)
	int32_t lsp_hz[C2_ORDER];	// LSPs in Hz, Q16, ascending
} c2e_lpc_t;

// x: 277 values of scratch (the windowed buffer as int16). lsps: also find the LSPs (else only the energy)
void c2e_lpc(const int16_t B[C2E_M], int16_t *x, bool lsps, c2e_lpc_t *out);

// --- c2_quant.c: the quantizers (spec 9), over c2_tab.c's levels ---
uint8_t c2e_wo_index(int32_t wo);
uint8_t c2e_e_index(const c2e_lpc_t *l);
void c2e_lsp_index(c2_mode_t mode, const int32_t lsp_hz[C2_ORDER], uint8_t idx[C2_ORDER]);

// --- c2_enc.c: what the host tests read back ---
// The harmonic count of 7.2 for ω0 (C2_WO_Q, ω0_min..ω0_max): the spec's float32 L on every such value
uint8_t c2e_harmonics(int32_t wo);

// One 10 ms analysis frame of the block c2_enc_block took last
typedef struct {
	uint8_t k_c;		// pitch estimator bin (f0 = 3.125 k_c Hz)
	uint8_t L;		// harmonics (spec 7.2)
	uint8_t voiced;		// after the ratio rules (spec 7.3)
	int32_t wo;		// after refinement and the clamp, C2_WO_Q
} c2e_frame_t;

typedef struct {
	c2e_frame_t f[2];	// the block's two analysis frames, earlier first
	c2e_lpc_t lpc;		// the LPC analysis of the second (energy; LSPs when the block sends them)
	bool lsps;		// lpc.lsp_hz is this block's
} c2e_trace_t;

// False before the first block after init
bool c2_enc_trace(const c2_enc_state_t *e, c2e_trace_t *out);

#endif
