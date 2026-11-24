// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codec 2's pitch estimator (docs/spec/codec2.md 6) in fixed point: the squared input through a DC notch and a 48-tap
// low-pass, decimated by 5, windowed, and the power spectrum of its 512-point DFT searched for the fundamental and its
// sub-multiples.
//
// Only every fifth low-pass output is ever read (the decimation of 6.4), so the filter computes 16 outputs a frame, not
// 80, and the history keeps the decimated values only: 48 words where the spec's q[0..319] has 320. The squares are
// whole numbers up to 2^30 (a 16-bit sample squared), and the notch output stays inside +-2^30 (it is the input less a
// weighted mean of earlier inputs, all of them in 0..2^30), so the whole chain is Q0 in 32-bit words, with the spec's
// +1.0 exactly 1. The notch output is held to [-2^30, 2^30 - 2] (only a full-scale -32768 after silence reaches the
// edge), so the symmetric low-pass can add its two inputs of each tap in 32 bits. The DFT input is scaled to its
// frame's largest value: the search compares bins of one frame only.
#include "c2_enc_int.h"
#include "../dsp/fxp.h"

#define NOTCH_B		2040109466	// 0.95 in Q31 (the notch's pole, spec 6.2)

void c2e_nlp_init(c2e_nlp_t *s)
{
	s->x_prev = 0;
	s->y_prev = 0;
	for (int i = 0; i < C2E_FIR_MEM; i++)
		s->fir[i] = 0;
	for (int i = 0; i < C2E_NLP_KEEP; i++)
		s->d[i] = 0;
	s->k_prev = C2E_NLP_LO;	// f0_prev = 50 Hz at creation; k_p of 6.6 is then 16
}

// 0.95 y rounded toward zero. Toward zero, not to nearest: rounding to nearest would hold a small output at its value
// for ever (0.95 * 10 rounds back to 10), a DC input then leaving a residue of up to 10 where the spec's notch decays
// to 0
static inline int32_t notch_pole(int32_t y)
{
	return y >= 0 ? fxp_mulshr(y, NOTCH_B, 31) : -fxp_mulshr(-y, NOTCH_B, 31);
}

uint8_t c2e_nlp(c2e_nlp_t *s, const int16_t B[C2E_M], c2e_nlp_scr_t *scr)
{
	int32_t *u = scr->u, *buf = scr->fft;
	int32_t *dn = u;	// the 16 new decimated values, each over a low-pass input no later output reads

	// 6.1-6.2: square, notch (y = x - x_prev + 0.95 y_prev), + 1.0, appended to the low-pass's kept inputs
	for (int i = 0; i < C2E_FIR_MEM; i++)
		u[i] = s->fir[i];
	int32_t xp = s->x_prev, yp = s->y_prev;
	for (int n = 0; n < C2_N; n++) {
		int32_t v = B[C2E_M - C2_N + n];
		int32_t x = v * v;
		int32_t y = fxp_clamp((x - xp) + notch_pole(yp), -(1 << 30), (1 << 30) - 2);

		xp = x;
		yp = y;
		u[C2E_FIR_MEM + n] = y + 1;
	}
	s->x_prev = xp;
	s->y_prev = yp;
	for (int i = 0; i < C2E_FIR_MEM; i++)
		s->fir[i] = u[C2_N + i];

	// 6.3: the low-pass at q[240], q[245] ... q[315], the inputs u[5i .. 5i + 47] (oldest first). The taps are
	// symmetric, so each multiplies the sum of its two inputs (under 2^31: each is in [-2^30 + 1, 2^30 - 1]). A 64-bit
	// sum of Q31 products: the taps' magnitudes add to 1.38, so the output stays within int32
	for (int i = 0; i < C2_N / 5; i++) {
		const int32_t *p = u + 5 * i;
		int64_t acc = 0;

		for (int j = 0; j < C2E_FIR_TAPS / 2; j++)
			acc = fxp_mac64(acc, p[j] + p[C2E_FIR_TAPS - 1 - j], c2e_fir[j]);
		acc = (acc + (INT64_C(1) << 30)) >> 31;
		// Output i reads u[5i..5i + 47], so u[i] (i <= 5i) is past every later output's window: it holds dn[i]
		dn[i] = acc > INT32_MAX ? INT32_MAX : acc < INT32_MIN ? INT32_MIN : (int32_t)acc;
	}

	// 6.4: window the 64 decimated values (48 kept, 16 new) into the FFT's input, each scaled so the largest is under
	// 2^24 (then times g < 1): the 64 inputs sum to under 2^30 and no stage of the transform overflows
	uint32_t mx = 0;
	for (int i = 0; i < C2E_NLP_KEEP; i++)
		mx |= c2e_mag(s->d[i]);	// the OR has the largest one's top bit, all the scale needs
	for (int i = 0; i < C2_N / 5; i++)
		mx |= c2e_mag(dn[i]);
	int sh = c2e_bits(mx) - 24;
	for (int i = 0; i < C2E_NLP_DEC; i++) {
		int32_t d = i < C2E_NLP_KEEP ? s->d[i] : dn[i - C2E_NLP_KEEP];
		int32_t g = c2e_nlp_win[i < C2E_NLP_DEC / 2 ? i : C2E_NLP_DEC - 1 - i];

		d = sh > 0 ? (d + (1 << (sh - 1))) >> sh : d * (1 << -sh);
		buf[i] = fxp_mulshr16(d, g, 15);
	}
	for (int i = C2E_NLP_DEC; i < DSP_FFT_SIZE; i++)
		buf[i] = 0;
	dsp_rfft(buf);

	// F(k) = |DFT(k)|^2 for the bins the search reads (15..129: the global range and one either side of a
	// sub-multiple's range), then scaled so the largest is under 2^27 (20 F of the thresholds below then fits 32 bits).
	// In place: bin k's 64-bit power over its own two words, then each 32-bit value into word k, below the words of
	// every bin still to be read
	enum { KLO = C2E_NLP_LO - 1, KHI = C2E_NLP_HI + 1 };
	uint64_t pmax = 0;
	for (int k = KLO; k <= KHI; k++) {
		int32_t re = buf[2 * k], im = buf[2 * k + 1];
		uint64_t p = (uint64_t)fxp_mul64(re, re) + (uint64_t)fxp_mul64(im, im);

		buf[2 * k] = (int32_t)(uint32_t)p;
		buf[2 * k + 1] = (int32_t)(uint32_t)(p >> 32);
		if (p > pmax)
			pmax = p;
	}
	int ps = c2e_bits64(pmax) - 27;
	if (ps < 0)
		ps = 0;
	uint32_t *F = (uint32_t *)buf;
	for (int k = KLO; k <= KHI; k++) {
		uint64_t p = (uint64_t)(uint32_t)buf[2 * k] | ((uint64_t)(uint32_t)buf[2 * k + 1] << 32);

		F[k] = c2e_shr64(p, ps);
	}

	// 6.5: the global peak, ties to the lowest bin, from a running maximum of 0 at bin 16
	int kg = C2E_NLP_LO;
	uint32_t Fg = 0;
	for (int k = C2E_NLP_LO; k <= C2E_NLP_HI; k++) {
		if (F[k] > Fg) {
			Fg = F[k];
			kg = k;
		}
	}

	// 6.6: sub-multiples. T = 0.15 F_g near the previous pitch, else 0.3 F_g: F_l > T is 20 F_l > 3 F_g, or
	// 10 F_l > 3 F_g, exact in 32 bits for F under 2^27
	int kc = kg, kp = s->k_prev;
	uint32_t fg3 = 3 * Fg;
	for (int m = 2; kg / m >= C2E_NLP_LO; m++) {
		int b = kg / m;
		int lo = 4 * b / 5, hi = 6 * b / 5;	// trunc(0.8 b), trunc(1.2 b): exact for these b in single or double

		if (lo < C2E_NLP_LO)
			lo = C2E_NLP_LO;
		bool near = lo < kp && kp < hi;
		int kl = lo;
		uint32_t Fl = 0;
		for (int k = lo; k <= hi; k++) {
			if (F[k] > Fl) {
				Fl = F[k];
				kl = k;
			}
		}
		if ((near ? 20 : 10) * Fl > fg3 && Fl > F[kl - 1] && Fl > F[kl + 1])
			kc = kl;
	}

	// 6.7: the history moves on by a frame
	for (int i = 0; i < C2E_NLP_KEEP - C2_N / 5; i++)
		s->d[i] = s->d[i + C2_N / 5];
	for (int i = 0; i < C2_N / 5; i++)
		s->d[C2E_NLP_KEEP - C2_N / 5 + i] = dn[i];
	s->k_prev = (uint8_t)kc;
	return (uint8_t)kc;
}
