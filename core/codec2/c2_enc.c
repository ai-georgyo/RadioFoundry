// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codec 2 encoder, modes 3200 and 1600 (docs/spec/codec2.md 5-9, and "What this firmware does: the encoder"), in
// fixed point for every platform: the block driver, the speech DFT (5.3), pitch refinement, harmonic sums and voicing
// (7). The pitch estimator is c2_nlp.c, the LPC analysis c2_lpc.c, the quantizers c2_quant.c; packing c2_bits.c.
//
// Each 20 ms block is two 10 ms analysis frames and one LPC analysis (of the second): a 3200 frame every block, a 1600
// frame every second block (its first block sends ω0 and energy, the second those and the LSPs), so every block costs
// about the same.
//
// Formats. The speech DFT's input is the windowed buffer scaled so its largest value is under 2^22 (the 277 inputs then
// sum to under 2^31: no stage of the transform overflows). Its output is scaled so the largest part is under 2^15 and
// becomes a table of the bins' powers |S_w(k)|^2 in place, 32 bits each, which the refinement and the band sums read;
// the bins below 1 kHz are also kept as 16-bit parts for the voicing fit. Everything the frame decides from the
// spectrum is a ratio of sums (the refinement's argmax, the voicing fit's SNR, the band energy ratio), so the scale
// drops out; the spec's 1e-4 floor is carried in the same scale (G below). ω0 is Q28 (C2_WO_Q), periods Q16 samples,
// bins per harmonic Q20. docs/spec/codec2.md 18 lists where this differs from the reference and why.
#include <stddef.h>
#include <string.h>
#include "c2_enc_int.h"
#include "../dsp/fxp.h"

// The state laid over c2_enc_state_t
typedef struct {
	int16_t B[C2E_M];	// the analysis buffer (5.1), newest last
	c2e_nlp_t nlp;
	c2_index_t ix;		// the 1600 frame's first half, between its blocks
	uint8_t mode;		// c2_mode_t
	uint8_t half;		// 1600: 1 when the next block completes the frame
	uint8_t traced;		// tr holds a block
	c2e_trace_t tr;		// the block taken last (host tests)
} enc_t;

// Bins the voicing fit reads: below ceil((L/4 + 1/2) ω0 r) <= 74, since L ω0 < 0.95 pi (7.2) and ω0 r <= 25.6
#define VBINS		78

// The scratch laid over c2_scratch_t: each stage's own, one after the other
typedef union {
	c2e_nlp_scr_t nlp;			// the pitch estimator's FFT and low-pass input
	struct {
		int32_t fft[DSP_FFT_SIZE];	// the speech DFT (5.3), then the power table P[0..256] (7.1-7.3)
		int16_t v[VBINS][2];		// the DFT's bins 0..77 as 16-bit parts, for the voicing fit (7.3)
	} sw;
	int16_t x[2 * C2E_WIN_HALF - 1];	// the LPC analysis's windowed buffer
} scr_t;

_Static_assert(sizeof(enc_t) <= sizeof(c2_enc_state_t), "the encoder state grew past C2_ENC_STATE_BYTES");
_Static_assert(sizeof(scr_t) <= sizeof(c2_scratch_t), "the encoder scratch grew past C2_SCRATCH_BYTES");
_Static_assert(offsetof(c2e_nlp_scr_t, fft) == 0 && offsetof(c2e_nlp_scr_t, u) == sizeof(int32_t) * DSP_FFT_SIZE,
	       "the pitch estimator's low-pass input follows its FFT buffer");

#define PI_C_Q28	843314856	// floor(pi_c 2^28): L = floor(pi_c / ω0) for ω0 in Q28 (as c2_L_of_wo)
#define TWO_PI_C_Q28	1686629713	// 2 pi_c 2^28
// The 5% rule of 7.2, f32(ω0 L) >= 0.95 pi_c: the product (exact in Q28) rounds to float32, 64 units of Q28 there, so
// it reaches 801149120, the first float32 at or above 0.95 pi_c, from above the midpoint 801149088 (a tie goes to
// the even 801149056). Comparing the exact product with 0.95 pi_c moved L on 17 Q28 values
#define NYQ95_Q28	801149089
#define WO_MIN		10541436	// ω0_min = f32(2 pi_c / 160) in Q28 (exactly, spec 3.2)
#define WO_MAX		84331488	// ω0_max = f32(2 pi_c / 20)
#define WO_60HZ		12649723	// f32(60 2 pi_c / 8000), the voicing rule's 60 Hz (7.3)
#define BINS_Q24	1367130496	// r = f32(1 / f32(2 pi_c / 512)), bins per rad, Q24 (exactly)
#define EPS_Q30		107374		// the 1e-4 of 7.3 in Q30
#define SNR_6DB_Q24	66791300	// 10^0.6 in Q24: SNR > 6 dB
#define TEN_Q24		167772160	// 10: R beyond +-10 dB
#define R4DB_Q24	42142461	// 10^0.4: R < -4 dB
#define VFIT_Q		6		// fractional bits of the voicing fit's amplitudes (the division below assumes 6)

// a > k b, k in Q24 below 2^31: both scaled under 2^31 first, so the products fit 64 bits
static bool ratio_gt(uint64_t a, uint64_t b, int32_t k)
{
	int s = c2e_bits64(a > b ? a : b) - 31;

	if (s > 0) {
		a >>= s;
		b >>= s;
	}
	return (a << 24) > (uint64_t)fxp_mul64((int32_t)b, k);
}

// |S_w(k)|^2 for k = 0..511 from the table of bins 0..256 (those above mirror those below, 5.3)
static inline uint32_t power(const uint32_t *P, unsigned k)
{
	return P[k > DSP_FFT_N ? DSP_FFT_SIZE - k : k];
}

// round(n 2^(2 s) / q) for n < 2^31, q < 2^24 and a quotient under 2^31: long division by digits of s bits, s <= 8,
// each a 32-bit hardware divide (both cores have one; the remainder stays under q, so a remainder shifted by 8 fits 32
// bits)
static uint32_t div_ext(uint32_t n, uint32_t q, int s)
{
	uint32_t a = n / q, r = n - a * q;

	r <<= s;
	a = (a << s) + r / q;
	r = (r % q) << s;
	return (a << s) + (r + q / 2) / q;
}

// ω0 = 2 pi_c / q for a period q in Q16, as Q28: 2 pi_c 2^28 2^16 / q
static int32_t wo_of_period(int32_t q)
{
	return (int32_t)div_ext(TWO_PI_C_Q28, (uint32_t)q, 8);
}

// Bins per harmonic of the candidate ω = 2 pi_c / q: 512 / q, Q20, so 2^45 / q
static int32_t bins_of_period(int32_t q)
{
	return (int32_t)div_ext(1u << 31, (uint32_t)q, 7);
}

// One pass of 7.1 around the period q0 (Q16): n candidates from q0 - delta in steps, the one with the largest harmonic
// sum, ties to the earlier; q0 itself when none beats 0
static int32_t refine(const uint32_t *P, int32_t q0, int32_t delta, int32_t step, int n)
{
	uint32_t Lr = PI_C_Q28 / (uint32_t)wo_of_period(q0);	// trunc(pi_c / ω0), fixed for the pass
	uint64_t emax = 0;
	int32_t best = q0, q = q0 - delta;

	for (int j = 0; j < n; j++, q += step) {
		uint32_t c = (uint32_t)bins_of_period(q), cm = 0;
		uint64_t sum = 0;

		// The bins by repeated addition, as the spec forms them: L_r c stays under 342 bins, 2^29 in Q20
		for (uint32_t m = 1; m <= Lr; m++) {
			cm += c;
			sum += power(P, (cm + (1u << (C2E_BIN_Q - 1))) >> C2E_BIN_Q);
		}
		if (sum > emax) {
			emax = sum;
			best = q;
		}
	}
	return best;
}

// 7.2: L = floor(f32(pi_c / ω0)), less one when the last harmonic is within 5% of Nyquist. The reference rounds the
// quotient to float32 before the floor, so a quotient less than half a float32 step under the next whole number counts
// as that number (then the 5% rule takes it off again). The same L as the spec's on every Q28 ω0 (an exhaustive run)
uint8_t c2e_harmonics(int32_t wo)
{
	uint32_t w = (uint32_t)wo, L = PI_C_Q28 / w, rem = PI_C_Q28 - L * w;
	int e2 = c2e_bits(L + 1) - 1;	// floor(log2(L + 1)): half a float32 step there is 2^(e2 - 24)

	if (((uint64_t)(w - rem) << (24 - e2)) <= w)
		L++;
	if (w * L >= NYQ95_Q28)
		L--;
	return (uint8_t)L;
}

// The 1e-4 of 7.3 in the spectrum's scale: powers here are the spec's times 2^(2 G)
static uint64_t eps_of(int G)
{
	int s = 2 * G - 30;

	if (s > 40)
		s = 40;
	if (s <= -40)
		return 0;
	return s >= 0 ? (uint64_t)EPS_Q30 << s : (uint64_t)EPS_Q30 >> -s;
}

// One 10 ms analysis frame on the buffer as it stands (5.3, 6, 7)
static void analyze(enc_t *e, scr_t *s, c2e_frame_t *fr)
{
	int32_t *X = s->sw.fft;
	const int16_t *B = e->B;

	fr->k_c = c2e_nlp(&e->nlp, B, &s->nlp);

	// 5.3: the windowed buffer with its center at index 0: B[160 + d] w[160 + d] at d mod 512, d = -138..138
	uint32_t mx = 0;
	X[0] = B[160] * (int32_t)c2e_win[0];
	mx = c2e_mag(X[0]);
	for (int d = 1; d < C2E_WIN_HALF; d++) {
		int32_t a = B[160 + d] * (int32_t)c2e_win[d], b = B[160 - d] * (int32_t)c2e_win[d];

		X[d] = a;
		X[DSP_FFT_SIZE - d] = b;
		mx |= c2e_mag(a) | c2e_mag(b);	// the OR has the largest one's top bit, all the scale needs
	}
	for (int k = C2E_WIN_HALF; k <= DSP_FFT_SIZE - C2E_WIN_HALF; k++)
		X[k] = 0;
	int s_in = c2e_bits(mx) - 22;
	if (s_in > 0) {
		int32_t h = 1 << (s_in - 1);

		X[0] = (X[0] + h) >> s_in;
		for (int d = 1; d < C2E_WIN_HALF; d++) {
			X[d] = (X[d] + h) >> s_in;
			X[DSP_FFT_SIZE - d] = (X[DSP_FFT_SIZE - d] + h) >> s_in;
		}
	} else if (s_in < 0) {
		int32_t g = 1 << -s_in;

		X[0] *= g;
		for (int d = 1; d < C2E_WIN_HALF; d++) {
			X[d] *= g;
			X[DSP_FFT_SIZE - d] *= g;
		}
	}
	dsp_rfft(X);

	// The parts scaled by 2^-sv, sv bringing the largest of the whole DFT under 2^15 (rounded, held to +-32767): the
	// voicing fit's bins kept as 16-bit parts, and the power table P[k] = |X_k|^2 < 2^31 in place. Bin k's parts are
	// words 2k and 2k + 1 (bin 0's and bin 256's real values words 0 and 1), and P[k] goes to word k, which no later
	// bin reads
	mx = 0;
	for (int k = 0; k < DSP_FFT_SIZE; k++)
		mx |= c2e_mag(X[k]);
	int sv = c2e_bits(mx) - 15;
	if (sv < 0) {
		// A quiet frame: the parts scaled up first (exactly), so the loops below only shift down
		for (int k = 0; k < DSP_FFT_SIZE; k++)
			X[k] *= 1 << -sv;
		sv = 0;
	}
	// Rounded, a part reaches at most 2^15: its square still fits, and the 16-bit copy is held to 32767
	int32_t h = sv > 0 ? 1 << (sv - 1) : 0;
	uint32_t *P = (uint32_t *)X;
	int32_t x0 = (X[0] + h) >> sv, xn = (X[1] + h) >> sv;
	for (int k = 1; k < VBINS; k++) {
		int32_t re = (X[2 * k] + h) >> sv, im = (X[2 * k + 1] + h) >> sv;

		s->sw.v[k][0] = (int16_t)fxp_clamp(re, -32767, 32767);
		s->sw.v[k][1] = (int16_t)fxp_clamp(im, -32767, 32767);
		P[k] = (uint32_t)(re * re) + (uint32_t)(im * im);
	}
	for (int k = VBINS; k < DSP_FFT_N; k++) {
		int32_t re = (X[2 * k] + h) >> sv, im = (X[2 * k + 1] + h) >> sv;

		P[k] = (uint32_t)(re * re) + (uint32_t)(im * im);
	}
	P[0] = (uint32_t)(x0 * x0);
	P[DSP_FFT_N] = (uint32_t)(xn * xn);
	// Powers here are the spec's |S_w|^2 times 2^(2 G): w is Q23, the input was scaled by 2^-s_in, the parts by 2^-sv
	int G = 23 - s_in - sv;

	// 7.1: from the estimator's period P = 2560 / k_c, a coarse and a fine pass, then the clamp
	int32_t per = (int32_t)((((uint32_t)2560 << C2E_PER_Q) + fr->k_c / 2) / fr->k_c);
	per = refine(P, per, 5 << C2E_PER_Q, 1 << C2E_PER_Q, 11);
	per = refine(P, per, 1 << C2E_PER_Q, 1 << (C2E_PER_Q - 2), 9);
	int32_t wo = wo_of_period(per);
	wo = wo < WO_MIN ? WO_MIN : wo > WO_MAX ? WO_MAX : wo;
	fr->wo = wo;

	// 7.2: harmonic band sums |S_w|^2 over [a_m, b_m), a_m = trunc((m - 0.5) ω0 r + 0.5); only their sums over the
	// bands below 1, 2 and 4 kHz are needed (7.3), not the amplitudes themselves. The bands tile the bins without a
	// gap: each starts where the one below ended
	uint8_t L = c2e_harmonics(wo);
	fr->L = L;
	uint32_t fb = (uint32_t)((fxp_mul64(wo, BINS_Q24) + ((int64_t)1 << 31)) >> 32);	// ω0 r, Q20
	unsigned m1k = L / 4u, m2k = L / 2u;
	uint64_t sig = 0, elo = 0, ehi = 0;
	unsigned k = (fb + (1u << C2E_BIN_Q)) >> (C2E_BIN_Q + 1);	// a_1
	for (unsigned m = 1; m <= L; m++) {
		unsigned b = ((2 * m + 1) * fb + (1u << C2E_BIN_Q)) >> (C2E_BIN_Q + 1);
		uint64_t am2 = 0;

		if (b > DSP_FFT_N)
			b = DSP_FFT_N;	// never: L ω0 r < 0.95 pi r = 243.2 bins and ω0 r / 2 <= 12.8
		for (; k < b; k++)
			am2 += P[k];
		if (m <= m1k)
			sig += am2;
		if (m <= m2k)
			elo += am2;
		if (m >= m2k)
			ehi += am2;
	}

	// 7.3: in each band below 1 kHz, the least-squares fit of one window-shaped sinusoid centered on the harmonic's
	// nearest bin, and the energy it leaves; in the 16-bit parts, whose powers are the table's to the parts' rounding
	uint64_t err = 0;
	for (unsigned m = 1; m <= m1k; m++) {
		int a = (int)(((2 * m - 1) * fb + (2u << C2E_BIN_Q) - 1) >> (C2E_BIN_Q + 1));	// ceil((m - 0.5) ω0 r)
		int b = (int)(((2 * m + 1) * fb + (2u << C2E_BIN_Q) - 1) >> (C2E_BIN_Q + 1));
		int c0 = (int)((m * fb + (1u << (C2E_BIN_Q - 1)) - 1) >> C2E_BIN_Q);	// ceil(m ω0 r - 0.5): W's center
		int64_t swr = 0, swi = 0;
		uint32_t sww = 0;

		if (b > VBINS)
			b = VBINS;
		// |S| < 2^15 and the |W| of a band add to under 2.32 in Q15, so each sum is under 2^31.3; sum W^2 < 2^30
		for (int j = a; j < b; j++) {
			int d = j - c0 < 0 ? c0 - j : j - c0;
			int32_t w = d < C2E_WSPEC_N ? c2e_wspec[d] : 0;

			swr += s->sw.v[j][0] * w;
			swi += s->sw.v[j][1] * w;
			sww += (uint32_t)(w * w);
		}
		// B_m = sum S W / sum W^2 (W in Q15, so times 2^15), with VFIT_Q (6) fractional bits: sum S W 2^21 / sum W^2.
		// The band holds the center bin, so sum W^2 >= W(0)^2 (0.36 in Q30) and |B| < 2^18 S units; as
		// (sum S W / 2) 2^14 / (sum W^2 / 2^8), a divisor in [2^20, 2^22]
		if (sww < (1u << 28))
			continue;	// never (above); keeps the divisor in div_ext's range
		uint32_t dw = sww >> 8;
		int32_t br = (int32_t)div_ext((uint32_t)(swr < 0 ? -swr : swr) >> 1, dw, 7);
		int32_t bi = (int32_t)div_ext((uint32_t)(swi < 0 ? -swi : swi) >> 1, dw, 7);
		if (swr < 0)
			br = -br;
		if (swi < 0)
			bi = -bi;
		for (int j = a; j < b; j++) {
			int d = j - c0 < 0 ? c0 - j : j - c0;
			int32_t w = d < C2E_WSPEC_N ? c2e_wspec[d] : 0;
			int32_t er = s->sw.v[j][0] * (1 << VFIT_Q) - fxp_mulshr16(br, w, 15);
			int32_t ei = s->sw.v[j][1] * (1 << VFIT_Q) - fxp_mulshr16(bi, w, 15);

			err += (uint64_t)fxp_mul64(er, er) + (uint64_t)fxp_mul64(ei, ei);
		}
	}
	uint64_t eps = eps_of(G);
	sig += eps;
	elo += eps;
	ehi += eps;
	uint64_t D = eps + (err >> (2 * VFIT_Q));

	bool v = ratio_gt(sig, D, SNR_6DB_Q24);
	// The band energy ratio R = 10 log10(E_lo / E_hi)
	if (!v && ratio_gt(elo, ehi, TEN_Q24))
		v = true;
	if (v && (ratio_gt(ehi, elo, TEN_Q24) || (ratio_gt(ehi, elo, R4DB_Q24) && wo <= WO_60HZ)))
		v = false;
	fr->voiced = v;
}

void c2_enc_init(c2_enc_state_t *st, c2_mode_t mode)
{
	enc_t *e = (enc_t *)(void *)st;

	memset(e, 0, sizeof(*e));
	// 5.1: every B[n] is 1.0 at creation, not 0
	for (int n = 0; n < C2E_M; n++)
		e->B[n] = 1;
	c2e_nlp_init(&e->nlp);
	e->mode = mode == C2_MODE_1600 ? C2_MODE_1600 : C2_MODE_3200;
}

bool c2_enc_block(c2_enc_state_t *st, c2_scratch_t *scr, const int16_t pcm[C2_BLOCK_SAMPLES],
		  uint8_t bits[C2_FRAME_BYTES])
{
	enc_t *e = (enc_t *)(void *)st;
	scr_t *s = (scr_t *)(void *)scr;
	c2e_trace_t *t = &e->tr;

	for (int f = 0; f < 2; f++) {
		memmove(e->B, e->B + C2_N, sizeof(e->B[0]) * (C2E_M - C2_N));
		memcpy(e->B + C2E_M - C2_N, pcm + f * C2_N, sizeof(e->B[0]) * C2_N);
		analyze(e, s, &t->f[f]);
	}
	// 8: on the second frame's buffer; the LSPs only where the frame sends them (3200, the second half of 1600)
	bool second = e->mode == C2_MODE_1600 && e->half;
	t->lsps = e->mode == C2_MODE_3200 || second;
	c2e_lpc(e->B, s->x, t->lsps, &t->lpc);
	e->traced = 1;

	// 9.5: the fields, then the bits in the order of section 4
	c2_index_t *ix = &e->ix;
	unsigned h = second ? 1 : 0;
	if (!second)
		memset(ix, 0, sizeof(*ix));
	ix->voiced[2 * h] = t->f[0].voiced;
	ix->voiced[2 * h + 1] = t->f[1].voiced;
	ix->wo[h] = c2e_wo_index(t->f[1].wo);
	ix->e[h] = c2e_e_index(&t->lpc);
	if (e->mode == C2_MODE_1600 && !second) {
		e->half = 1;
		return false;
	}
	c2e_lsp_index((c2_mode_t)e->mode, t->lpc.lsp_hz, ix->lsp);
	c2_pack((c2_mode_t)e->mode, ix, bits);
	e->half = 0;
	return true;
}

bool c2_enc_trace(const c2_enc_state_t *st, c2e_trace_t *out)
{
	const enc_t *e = (const enc_t *)(const void *)st;

	if (!e->traced)
		return false;
	*out = e->tr;
	return true;
}
