// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fixed-point signal helpers the voice codecs share (the MBE core in core/mbe, Codec 2 in core/codec2): sine and
// cosine of a 32-bit phase, 2^x and log2 in Q11, a noise source, and the 512-point real FFT both codecs use, moved here
// from the MBE core unchanged (same tables, same results bit for bit: tests/host/mbeExactTest.c), so a second codec
// links them without the MBE core; and the integer helpers Codec 2 needs beside them (dsp_math.c: normalize,
// reciprocal, square root, atan2). No codec knowledge, no float, no libm. Built with CONFIG_DSP (derived: MBE or
// CODEC2; the firmware's Makefile globs core/dsp under it, so the files carry no switch of their own). The per-sample
// kernels (dot products, scaled products) are core/dsp/fxp.h's.
#ifndef RFOUNDRY_DSP_H
#define RFOUNDRY_DSP_H

#include <stdint.h>

extern const int16_t dsp_sin_tab[257];		// sin(pi/2 * i / 256), Q15, one quarter wave
extern const uint16_t dsp_exp2_tab[65];		// 2^(14 + i/64)
extern const uint16_t dsp_log2_tab[65];		// 32768 * log2(1 + i/64)

// Always inlined: -Os otherwise makes both calls, and an oscillator's per-sample loop then spills its registers
// around them (a third of that loop's instructions on the C-SKY build)
#define DSP_INLINE static inline __attribute__((always_inline))

// sin(2 pi i / 1024), Q15, from the quarter wave
DSP_INLINE int32_t dsp_sin_idx(uint32_t i)
{
	uint32_t q = i & 255u;
	int32_t v = dsp_sin_tab[(i & 256u) ? 256u - q : q];

	return (i & 512u) ? -v : v;
}
// cos(2 pi i / 1024), Q15, i < 1024: always dsp_sin_idx(i + 256). On C-SKY (and the host's build as C-SKY runs,
// FXP_SPLIT16) from the quarter wave unfolded into a 2 KB table: folding it in an oscillator's per-sample loop cost
// two branches and a third of that loop's instructions there. Elsewhere folded: an Arm core does it in a few
// conditional instructions, and the table's 2 KB are what an Arm radio's fullest build has no room for
#if defined(__csky__) || defined(FXP_SPLIT16)
#define DSP_COS_TAB	1
extern const int16_t dsp_cos_tab[1024];
DSP_INLINE int32_t dsp_cos_idx(uint32_t i) { return dsp_cos_tab[i]; }
#else
DSP_INLINE int32_t dsp_cos_idx(uint32_t i) { return dsp_sin_idx(i + 256u); }
#endif
// cos of a phase, 2^32 = one turn, Q15, nearest of 1024 steps (an oscillator's per-sample lookup)
DSP_INLINE int32_t dsp_cos(uint32_t ph) { return dsp_cos_idx((ph + 0x200000u) >> 22); }
// the same with linear interpolation, for dequantizers and windows (a few hundred calls per frame)
int32_t dsp_cos_i(uint32_t ph);

// 2^(x / 2048) for x in Q11, as an integer (saturates at 2^30, rounds down to 0 below 1/2)
int32_t dsp_exp2_q11(int32_t x);
// log2(x) in Q11 for x > 0 (x = 0 gives -64 * 2048)
int32_t dsp_log2_q11(uint32_t x);
int32_t dsp_log2_q11_64(uint64_t x);

// 32-bit xorshift noise (not for keys or nonces: core/crypto/entropy.h)
static inline uint32_t dsp_rand(uint32_t *s)
{
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

// Inverse real FFT of size 2 * DSP_FFT_N: the half spectrum X[0..N] (re, im interleaved; X[0] and X[N] real, X[N].re in
// buf[1]) to x[0..2N-1], in place on 2N int32 values, no scaling:
// x[n] = Re(X0) + Re(XN)(-1)^n + 2 sum_k Re(X_k e^{i pi k n / N}). The caller keeps the headroom: each of the
// DSP_FFT_LOG + 1 stages can double a value
#define DSP_FFT_LOG	8
#define DSP_FFT_N	(1 << DSP_FFT_LOG)
#define DSP_FFT_SIZE	(2 * DSP_FFT_N)		// 512 real points
void dsp_irfft(int32_t *buf);
// forward real FFT with the inverse's layout: x[0..2N-1] to X[0..N]; X_k = sum x[n] e^{-i pi k n / N}
void dsp_rfft(int32_t *buf);

// --- dsp_math.c: integer helpers (exact or with the error stated; no 64-bit divide, no float) ---
// Left shifts that bring x's top set bit to bit 31 (count leading zeros); 32 for x = 0
int dsp_norm32(uint32_t x);
// 1 / x for x > 0 as r / 2^e: e = 62 - dsp_norm32(x) and r = floor((2^e - 1) / x), exact, so r is in [2^30, 2^31)
// (31 significant bits). x = 0: r = 0x7FFFFFFF, e = 0
uint32_t dsp_recip(uint32_t x, int *e);
// floor(sqrt(x)), exact
uint32_t dsp_isqrt32(uint32_t x);
uint32_t dsp_isqrt64(uint64_t x);
// atan2(y, x) as a phase in dsp_cos's unit (2^32 = one turn, 0 along +x, counterclockwise), |error| <= 2^16 (about
// 1e-4 rad); (0, 0) gives 0
uint32_t dsp_atan2(int32_t y, int32_t x);

#endif
