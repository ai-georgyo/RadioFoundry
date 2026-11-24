// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codec 2's LPC analysis (docs/spec/codec2.md 8, 18) in fixed point: the windowed buffer's autocorrelation, the
// reflection coefficients and the prediction error energy (Levinson-Durbin in its Schur form, with k = 0 at the orders
// where the reference's float32 autocorrelation stops being positive definite, and on such a frame from the order
// that resolves two steady lines or more), the predictor, bandwidth expansion and the LSPs by the Chebyshev grid search.
//
// Formats. The windowed buffer is scaled to its largest value (under 2^14, as int16), so R is exact in 64 bits; R is
// then scaled so R[0] is in [2^29, 2^30] (Q30 of R[0]'s scale), the reflection coefficients are Q30 and the predictor
// Q22. The predictor of an order-10 analysis with |k_i| <= 1 has its roots inside the unit circle, so |a_i| <= C(10, i)
// <= 252 and the sum of |a_i| is at most 2^10: Q22 holds it in 32 bits, and the sums below in 64. The energy is the
// recursion's own prediction error R[0] prod(1 - k_i^2), which the spec's sum of a_i R[i] equals in exact arithmetic
// (each order's error is that sum, 8.3), without the difference of large terms, and without the predictor: that is
// formed only for the LSPs. The polynomials of 8.5 are scaled to their largest coefficient and evaluated in 32 bits for
// their sign only.
#include "c2_enc_int.h"
#include "../dsp/fxp.h"

#define ONE_A		(1 << C2E_A_Q)
#define X_STEP		10737418	// f32(0.01) in Q30: the grid step of 8.5
#define X_ONE		(1 << 30)

static inline int32_t sat32(int64_t v)
{
	return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

// The LSPs of zero energy or of a failed search (8.1, 8.5): (pi_c / 10) i for i = 0..9, in Hz 400 i
static void lsp_fallback(int32_t hz[C2_ORDER])
{
	for (int i = 0; i < C2_ORDER; i++)
		hz[i] = (400 * i) << C2E_LSPHZ_Q;
}

// Sign of C(x) of 8.5 at x (Q30): c_5 + c_4 T_1 + c_3 T_2 + ... + c_0 T_5, the Chebyshev series sum_k a_k T_k with
// a_k = c_{5-k}, by Clenshaw's recurrence b_k = a_k + 2 x b_{k+1} - b_{k+2}, C = a_0 + x b_1 - b_2: five products
// instead of the nine of the T_k themselves. The a_k are scaled to their largest (under 2^25: the sign does not care),
// so |b_k| <= sum_j (j - k + 1) |a_j| < 15 2^25 for |x| <= 1.01 and 2 x b stays in 32 bits. x keeps all its 30 bits: a
// point rounded to Q14 (3e-5) moved the LSP indexes of a few percent of the harness's vowel frames
static int cheb_sign(const int32_t a[6], int32_t x)
{
	int32_t b1 = a[5], b2 = 0;

	for (int k = 4; k >= 1; k--) {
		int32_t b0 = a[k] + fxp_mulshr(b1, x, 29) - b2;	// 2 x b_{k+1}

		b2 = b1;
		b1 = b0;
	}
	int32_t c = a[0] + fxp_mulshr(b1, x, 30) - b2;
	return c > 0 ? 1 : c < 0 ? -1 : 0;
}

// The series coefficients a_0..a_5 = (P'_5, 2P'_4, 2P'_3, 2P'_2, 2P'_1, 2P'_0) of a reduced polynomial v[0..5],
// scaled so the largest is under 2^25
static void cheb_scale(const int64_t v[6], int32_t a[6])
{
	uint64_t mx = 0;

	for (int k = 0; k < 6; k++) {
		int64_t c = k ? 2 * v[5 - k] : v[5];
		uint64_t m = c < 0 ? (uint64_t)-c : (uint64_t)c;

		if (m > mx)
			mx = m;
	}
	int sh = c2e_bits64(mx) - 24;	// mx >> sh < 2^24, so under 2^25 after the rounding

	for (int k = 0; k < 6; k++) {
		int64_t c = k ? 2 * v[5 - k] : v[5];

		a[k] = (int32_t)(sh > 0 ? (c + ((int64_t)1 << (sh - 1))) >> sh : c * (1 << -sh));
	}
}

// acos(x) in Hz Q16 (4000 Hz at pi): the angle of (x, sqrt(1 - x^2)) from dsp_atan2 (1e-4 rad)
static int32_t acos_hz(int32_t x)
{
	if (x > X_ONE)
		x = X_ONE;
	if (x < -X_ONE)
		x = -X_ONE;
	uint64_t s = ((uint64_t)1 << 60) - (uint64_t)fxp_mul64(x, x);
	uint32_t y = dsp_isqrt64(s);
	uint32_t ph = dsp_atan2((int32_t)y, x);	// 0..2^31 for y >= 0: 2^31 is pi

	return (int32_t)((uint64_t)fxp_mul64((int32_t)(ph >> 1), 4000) >> (30 - C2E_LSPHZ_Q));
}

// 8.5: the ten roots, P' for the odd ones and Q' for the even, each search from the root before. False when fewer than
// ten were found
__attribute__((noinline)) static bool lsp_search(const int32_t a[C2_ORDER + 1], int32_t hz[C2_ORDER])
{
	int64_t pv[2][6];
	int32_t p[2][6];

	pv[0][0] = pv[1][0] = ONE_A;
	for (int i = 1; i <= 5; i++) {
		pv[0][i] = (int64_t)a[i] + a[C2_ORDER + 1 - i] - pv[0][i - 1];
		pv[1][i] = (int64_t)a[i] - a[C2_ORDER + 1 - i] + pv[1][i - 1];
	}
	cheb_scale(pv[0], p[0]);
	cheb_scale(pv[1], p[1]);

	int32_t xl = X_ONE, xr = X_ONE;
	for (int r = 0; r < C2_ORDER; r++) {
		const int32_t *c = p[r & 1];
		int sl = cheb_sign(c, xl);
		bool found = false;

		// The test comes before each step, so the last point tried may lie just below -1
		while (xr >= -X_ONE) {
			xr = xl - X_STEP;
			int sr = cheb_sign(c, xr);

			if (sr * sl < 0 || sr == 0) {
				int32_t xm = xl;

				for (int b = 0; b < 6; b++) {
					xm = xl + ((xr - xl) >> 1);
					int sm = cheb_sign(c, xm);

					if (sm * sl > 0)
						xl = xm;
					else
						xr = xm;
				}
				hz[r] = acos_hz(xm);
				xl = xm;
				found = true;
				break;
			}
			xl = xr;
			sl = sr;
		}
		if (!found)
			return false;
	}
	return true;
}

enum { N0 = 160 - (C2E_WIN_HALF - 1), NX = 2 * C2E_WIN_HALF - 1 };	// the window's non-zero n = 22..298

// 8.1: x[n] = B[n] w[n], scaled to its largest magnitude, under 2^14 (so 8 products of a dot product fit 32 bits, as
// fxp_dot16_8 sums them). The scale's exponent (x = B w 2^(23 - sh), w being Q23), or false for a window of zeros
static bool windowed(const int16_t B[C2E_M], int16_t *x, int *sh_out)
{
	int32_t mx = 0;

	for (int i = 0; i < NX; i++) {
		int d = N0 + i - 160;
		int32_t v = B[N0 + i] * (int32_t)c2e_win[d < 0 ? -d : d];

		if ((int32_t)c2e_mag(v) > mx)
			mx = (int32_t)c2e_mag(v);
	}
	if (mx == 0)
		return false;
	int sh = c2e_bits((uint32_t)mx) - 14;	// mx >> sh < 2^14
	for (int i = 0; i < NX; i++) {
		int d = N0 + i - 160;
		int32_t v = B[N0 + i] * (int32_t)c2e_win[d < 0 ? -d : d];

		x[i] = (int16_t)fxp_clamp(sh > 0 ? (v + (1 << (sh - 1))) >> sh : v * (1 << -sh), -16383, 16383);
	}
	*sh_out = sh;
	return true;
}

// R[j], exact (each product under 2^28, eight of them summed in 32 bits, the rest in 64), scaled so R[0] is in
// [2^29, 2^30]: R[0] first, for the scale. R[0] > 0 (the largest x is at least 2^13), and every |R[j]| <= R[0], so
// when R[0] is under 2^30 the left shift stays in 32 bits. Returns the scale's exponent t (r = R 2^-t)
static int autocorr(const int16_t *x, int32_t r[C2_ORDER + 1])
{
	int t = 0;

	for (int j = 0; j <= C2_ORDER; j++) {
		int64_t acc = 0;
		int n = NX - j, i = 0;

		for (; i + 8 <= n; i += 8)
			acc += fxp_dot16_8(x + i, x + i + j);
		for (; i < n; i++)
			acc += x[i] * x[i + j];
		if (j == 0)
			t = c2e_bits64((uint64_t)acc) - 30;
		r[j] = t > 0 ? (int32_t)((acc + ((int64_t)1 << (t - 1))) >> t) : (int32_t)acc * (1 << -t);
	}
	return t;
}

// 8.2 by the Schur recursion, the lattice form of Levinson-Durbin: it carries, in place of the predictor, the
// correlations of the forward and backward prediction errors with the signal, f_n[j] = sum_i a_i R[j - i] for j > n
// and g_n[j] = sum_i a_{n-i} R[j - i] for j >= n. k_{n+1} = -f_n[n+1] / g_n[n], where g_n[n] is the order's error
// e_n; then f_{n+1}[j] = f_n[j] + k g_n[j-1] and g_{n+1}[j] = g_n[j-1] + k f_n[j]. While R is positive definite every
// one is bounded by R[0] (Cauchy-Schwarz on the error filters, |k| < 1 at every order); R less the margin below moves
// each by at most the margin times |a|^2 <= C(20, 10), 2 % of R[0]. So all of it is 32-bit in r's scale (R[0] <=
// 2^30) with one rounding a step (a few 2^-30 R[0] at order 10). Levinson's sums of a_j R[i - j] carry the Q22
// predictor's rounding instead, near 2^-22 R[0] at order 10: as large as the margin below, and which orders take k = 0
// would then depend on it. f[m] holds f_n[n + m] (m >= 1), g[m] g_n[n + m].
//
// On r with r0 in place of r[0]: k (Q30) in k[1..10]. An order whose |k| would be 1 or more (or that has no error left
// to divide) takes k = 0 and the recursion goes on, as 8.2's Levinson does: with k = 0 the order's step is a shift, f
// moves down one and g and e stay, so the next k is the spec's next one, from the same predictor. The orders in skip
// take k = 0 the same way. lines: the first order from 3 on whose |k| is K_LINE or more takes k = 0 with every order
// after it, unless order 1 or 2 already had such a k (see reflection). Returns the orders that took k = 0, bit n - 1
// for order n; the error after order 10 in *e (r's scale)
#define K_LINE		1069446857	// 0.996 in Q30: the order takes the error down 125 times or more

static unsigned schur(const int32_t r[C2_ORDER + 1], int32_t r0, unsigned skip, bool lines, int32_t k[C2_ORDER + 1],
		      int32_t *e)
{
	int32_t f[C2_ORDER + 1], g[C2_ORDER + 1];
	unsigned zero = 0;

	g[0] = r0;
	k[0] = 0;
	for (int j = 1; j <= C2_ORDER; j++)
		f[j] = g[j] = r[j];
	for (int n = 0; n < C2_ORDER; n++) {
		int32_t en = g[0], num = f[1], kn = 0;
		bool take = !(skip >> n & 1) && en > 0 && c2e_mag(num) < (uint32_t)en;

		if (take) {
			int ee;
			uint32_t rc = dsp_recip((uint32_t)en, &ee);	// 2^ee / en, 31 bits

			kn = (int32_t)-(fxp_mul64(num, (int32_t)rc) >> (ee - 30));	// |kn| < 2^30 as |num| < en
			if (lines && c2e_mag(kn) >= K_LINE) {
				lines = false;	// the frame's first line decides
				if (n >= 2) {
					skip = ~0u;	// this order and every one after it
					take = false;
				}
			}
		}
		if (!take) {
			zero |= 1u << n;
			kn = 0;
		}
		k[n + 1] = kn;
		// m ascending: f[m] takes f[m + 1] before that is overwritten
		for (int m = 0; m < C2_ORDER - n; m++) {
			int32_t gm = g[m], fm = f[m + 1];

			g[m] = gm + (int32_t)((fxp_mul64(kn, fm) + (1 << 29)) >> 30);
			if (m > 0)
				f[m] = fm + (int32_t)((fxp_mul64(kn, gm) + (1 << 29)) >> 30);
		}
	}
	*e = g[0];
	return zero;
}

// Which orders take k = 0. The reference sums R in float32 (8.1), which leaves each R[j] off by about 2^-22 R[0]
// (1e-7 to 3e-7 measured on band noise and tones), and that error is not the autocorrelation of any signal. Where the
// spectrum's floor is under it (band noise with brickwall edges, nulls 60 dB deep; a tone or full-scale square near
// 4 kHz) its R stops being positive definite at some order: Levinson meets |k| > 1 there and sets it to 0 (8.2), most
// often the later orders too, and E is the error of an order before, 1e-6 to 1e-4 of R[0]. This R is exact, the
// windowed buffer's own autocorrelation, positive definite at every order: run to order 10 its error is 1e-8 to 1e-10
// of R[0], under the quantizer's lowest level, so it sent energy index 0 where the reference sends 5 to 23, and 2 to 3
// dB (3200) or 1 to 7 dB (1600) less level on band noise above 2 kHz. So the orders that take k = 0 come from R with
// 2^-23 R[0] taken off R[0], positive definite at an order exactly while that order's smallest eigenvalue is above
// it; k and E come from R itself, with the same orders at 0, as the reference's sum would give them without its
// round-off. Speech stays clear of the margin: no speech-like frame of the reference harness encodes otherwise with
// it.
//
// Two steady lines or more (18.3). On a frame the margin marks (an order takes k = 0) the reference's own stop is its
// round-off, so the stop can serve the decoder. An order with |k| near 1 resolves a steady line: a single tone (and DC,
// 4 kHz, a chirp's frame) at order 1 or 2; two tones (two_tone, DTMF pairs) at order 4, |k| 0.9972 to 0.9985, after
// three orders that resolve neither. A predictor holding two such lines decodes, after 8.4's 15 Hz expansion and the
// LSP quantizer, 6 to 25 dB under the input; the orders before it, with their error, within 4 dB of the input's level.
// So on a marked frame whose first such order is 3 or later, that order and every one after it take k = 0. A single
// line keeps the margin's orders: the envelope before it is flat and spreads the tone over every harmonic, and the
// reference's energy index on tones near 4 kHz, DC and alternating full scale is that of the margin's orders. On
// codec2-extra's band noise no marked frame has |k| over 0.995 from order 3 on, nor do the 30 s speech-like inputs'
// (0.955); speech-like frames of the harness are not marked. k (Q30) in k[1..10]; returns E in r's scale. noinline,
// as lsp_search: the 64-bit products spill on C-SKY, and inlined both frames would sit in c2e_lpc's at once
#define R_MARGIN_SH	23	// the margin: 2^-23 R[0]

__attribute__((noinline)) static int32_t reflection(const int32_t r[C2_ORDER + 1], int32_t k[C2_ORDER + 1])
{
	int32_t e;
	unsigned zero = schur(r, r[0] - (r[0] >> R_MARGIN_SH), 0, false, k, &e);

	schur(r, r[0], zero, zero != 0, k, &e);
	return e;
}

// The predictor (Q22) from the reflection coefficients (Q30, in a[1..10] on entry): a_i^(i) = k_i,
// a_j^(i) = a_j^(i-1) + k_i a_{i-j}^(i-1) (8.2), in place, k_i read before a_i takes its place. Only the LSPs read it.
// noinline: its 64-bit products spill on C-SKY, and inlined they added 112 B to c2e_lpc's frame
__attribute__((noinline)) static void predictor(int32_t a[C2_ORDER + 1])
{
	a[0] = ONE_A;
	for (int i = 1; i <= C2_ORDER; i++) {
		int32_t k = a[i];

		// a_j + k a_{i-j} for j = 1..i-1, a pair (j, i - j) at a time
		for (int j = 1; 2 * j <= i; j++) {
			int32_t lo = a[j], hi = a[i - j];

			a[j] = sat32((int64_t)lo + ((fxp_mul64(k, hi) + (1 << 29)) >> 30));
			if (2 * j != i)
				a[i - j] = sat32((int64_t)hi + ((fxp_mul64(k, lo) + (1 << 29)) >> 30));
		}
		a[i] = (k + (1 << 7)) >> 8;	// Q30 to Q22
	}
}

void c2e_lpc(const int16_t B[C2E_M], int16_t *x, bool lsps, c2e_lpc_t *out)
{
	int sh = 0;

	out->e_pos = false;
	out->lsp_ok = false;
	if (!windowed(B, x, &sh)) {
		// A window of zeros: E = 0 and the LSPs of 8.1
		lsp_fallback(out->lsp_hz);
		return;
	}
	int32_t r[C2_ORDER + 1], a[C2_ORDER + 1];
	int t = autocorr(x, r);
	int32_t e = reflection(r, a);

	// 8.3: E = e in R's scale. x = x_spec 2^(23 - sh) (w is Q23), so R = R_spec 2^(46 - 2 sh) and r = R 2^-t
	if (e > 0) {
		out->e_pos = true;
		out->e_log2 = dsp_log2_q11((uint32_t)e) + (t + 2 * sh - 46) * (1 << C2E_LOG_Q);	// negative: no shift
	}
	if (!lsps)
		return;

	// The predictor from k, then 8.4: bandwidth expansion, a_i c_i
	predictor(a);
	for (int i = 1; i <= C2_ORDER; i++)
		a[i] = (int32_t)((fxp_mul64(a[i], c2e_bw[i]) + (1 << 29)) >> 30);

	// 8.5
	out->lsp_ok = lsp_search(a, out->lsp_hz);
	if (!out->lsp_ok)
		lsp_fallback(out->lsp_hz);
}
