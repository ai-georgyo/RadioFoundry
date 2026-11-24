// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codec 2 synthesis of one 10 ms frame from its parameters (docs/spec/codec2.md 10.4-13, 17): amplitudes from the LPC
// model and the post filter, phases, the 512-point frame waveform, overlap-add and the limiter. Integers only.
//
// Where the spec's arithmetic decides an integer (bins, the harmonic count, φ_ex, the random draws), this file
// reproduces the float32 and double rounding of the reference exactly (spec 17.3); everywhere else it computes the
// spec's quantities as precisely as the fixed-point formats allow, which is closer to the exact value than float32.
//
// The LPC spectrum (11.1) is not an FFT of the coefficients: |A(e^jω)|² and arg A come from the LSP product form,
// A(e^jω) = 2^5 e^{-j5.5ω} [cos(ω/2) Π_odd (cos ω - cos ω_i) + j sin(ω/2) Π_even (cos ω - cos ω_i)], where each factor
// is a difference of two Q29 cosines. Near a formant the factors are small, and an FFT's absolute error would be a
// large part of |A|; here each keeps 15 significant bits, whatever its size. A(z/γ) (11.2) is well conditioned and
// smooth: Clenshaw's recurrence at every fourth bin and a cubic between. The power spectra live as log2 values
// (C2_LOG_Q): the post filter raises them to fractional powers, and they span over 120 dB. The phases need no
// arctangent: each harmonic keeps the direction of its filter's conjugate and multiplies it in (12.2).
#include "c2_dec_int.h"
#include "dsp/fxp.h"

_Static_assert(sizeof(c2_synth_scratch_t) <= sizeof(c2_scratch_t), "the synthesis scratch fits C2_SCRATCH_BYTES");

#define LOG1		(1 << C2_LOG_Q)
#define NB		(DSP_FFT_N)		// 256 bins below Nyquist (P(k), k = 0..255)

// --- Exact float emulation (spec 17.3) ---

// r_d = f32(2π_c/512) = 13176795 / 2^30 exactly (11.3, 12.1)
#define C2_RD_Q30	13176795u
// 2π_c = 6.283185307 as a double, times 2^48: an integer (13.1, 12.2)
#define C2_D2_Q48_HI	411774u
#define C2_D2_Q48_LO	3574613457u
// ω0 below π_c · 150/4000 = 31624307.12 / 2^28 (11.4): every float32 ω0 there is a whole number of Q28 units
#define C2_WO_150HZ	31624307

uint32_t c2_f32_round_u32(uint32_t x)
{
	int s = 8 - dsp_norm32(x);		// bits past 24

	if (s <= 0)
		return x;
	uint32_t half = 1u << (s - 1), rem = x & ((half << 1) - 1u);
	x -= rem;
	if (rem > half || (rem == half && (x & (half << 1))))
		x += half << 1;
	return x;
}

// A signed Q48 value rounded to float32's 24 significant bits. |x| < 2^53 here (φ_ex stays below 32 rad), so the
// rounding unit is below 2^30 and every mask is a 32-bit one: no variable 64-bit shift
static int64_t f32_round_q48(int64_t v)
{
	uint64_t x = v < 0 ? 0u - (uint64_t)v : (uint64_t)v;
	uint32_t hi = (uint32_t)(x >> 32);
	int n = hi ? 64 - dsp_norm32(hi) : 32 - dsp_norm32((uint32_t)x);	// significant bits

	if (n > 24) {
		uint32_t half = 1u << (n - 25), rem = (uint32_t)x & ((half << 1) - 1u);
		x -= rem;
		if (rem > half || (rem == half && ((uint32_t)x & (half << 1))))
			x += half << 1;
	}
	return v < 0 ? -(int64_t)x : (int64_t)x;
}

// φ_ex wrap (12.2): k = ⌊f32(φ/2π_c + 0.5)⌋ is the number of these φ (Q48) at or below φ: (k - 0.5 - h) 2π_c with h
// half the float32 unit just below k, rounded up; and f64(k 2π_c) in Q48
static const int64_t c2_wrap_thr[5] = {
	884279666271102, 2652839051520485, 4421398384062689, 6189957822019250, 7958517049147097,
};
static const int64_t c2_wrap_sub[6] = {
	0, 1768559437956561, 3537118875913122, 5305678313869683, 7074237751826244, 8842797189782805,
};

void c2_phi_ex_step(c2_dec_t *d, int32_t wo)
{
	int64_t v = (int64_t)(((uint64_t)(uint32_t)d->phi_ex_hi << 32) | d->phi_ex_lo);
	// f32(ω0 · 80): exact as 5 ω0 (below 2^30) times 16, then rounded; Q28 to Q48. Written so: GCC makes a 64-bit
	// product of a sum of two shifts, a library call on C-SKY
	uint64_t w80 = (uint64_t)(5u * (uint32_t)wo) << 4;
	int64_t inc = (int64_t)((uint64_t)f32_round_q48((int64_t)w80) << 20);
	unsigned k = 0;

	v = f32_round_q48(v + inc);
	while (k < 5 && v >= c2_wrap_thr[k])
		k++;
	v = f32_round_q48(v - c2_wrap_sub[k]);
	d->phi_ex_lo = (uint32_t)v;
	d->phi_ex_hi = (int32_t)(uint32_t)((uint64_t)v >> 32);
}

uint32_t c2_bin_band(int32_t wo, unsigned j)
{
	// j/2 · ω0 / r_d = 2 j wo / Rn exactly; + 0.5 and trunc is ⌊(2 j wo + (Rn - 1)/2) / Rn⌋ for an odd Rn (the sum
	// cannot sit on a multiple of Rn). The double arithmetic of the reference cannot cross a whole number here: the
	// quotient's distance from one is at least 1/(2 Rn), far above its rounding. 2 j wo < 3.6e9 for j <= 2L + 1
	return (2u * j * (uint32_t)wo + (C2_RD_Q30 - 1u) / 2u) / C2_RD_Q30;
}

uint32_t c2_bin_phase(int32_t wo, unsigned m)
{
	// q = f32(m ω0) / r_d = 4M / Rn, M = f32(m ω0) in Q28 (m ω0 <= π, so 4M < 2^32). b = ⌊f32(q) + 0.5⌋ is Q or Q + 1:
	// Q + 1 when q's fraction is 1/2 or more, or when f32 rounds q up to Q + 0.5, that is when q is within half a
	// float32 unit (of the binade below Q + 0.5) of it. A tie rounds to Q + 0.5 (its significand is even)
	uint32_t n4 = 4u * c2_f32_round_u32(m * (uint32_t)wo);
	uint32_t q = n4 / C2_RD_Q30, rem = n4 - q * C2_RD_Q30;

	if (2u * rem >= C2_RD_Q30)
		return q + 1u;
	if (q == 0)
		return 0;			// below 0.5 the unit is 2^-25: nothing rounds up to it
	// unit 2^(e - 23) for Q in [2^e, 2^(e+1)); up when (Rn - 2 rem) 2^(23 - e) <= Rn
	return q + ((C2_RD_Q30 - 2u * rem) <= (C2_RD_Q30 >> (23 - (31 - dsp_norm32(q)))));
}

// 13.1: b = trunc(f32(M · 512) / 2π_c + 0.5) with M = f32(m ω0) in Q28 and the division in double: b >= j exactly when
// M 2^30 >= (2j - 1) 2π_c 2^48. The quotient's distance from a half integer can be below 2^-50 of it (0.012 of a Q28
// unit at j = 81), under the double's rounding, so the decision is made on the integers: 2π_c 2^48 and M 2^30 are
// both whole. A 32-bit estimate first, then the exact test either way
static uint64_t synth_thr(uint32_t j)
{
	// (2j - 1) · 2π_c 2^48 for j <= 256 from 16-bit pieces: no 64-bit product
	uint32_t k = 2u * j - 1u;
	uint64_t lo = ((uint64_t)(k * (C2_D2_Q48_LO >> 16)) << 16) + (uint64_t)(k * (C2_D2_Q48_LO & 0xFFFFu));

	return ((uint64_t)(k * C2_D2_Q48_HI) << 32) + lo;
}

uint32_t c2_bin_synth(int32_t wo, unsigned m)
{
	uint32_t M = c2_f32_round_u32(m * (uint32_t)wo);
	uint64_t x = (uint64_t)M << 30;
	uint32_t b = (M + 1647099u) / 3294199u;		// ⌊M / (2^19 2π_c) + 0.5⌋ give or take one

	while (b > 0 && x < synth_thr(b))
		b--;
	while (b < 255u && x >= synth_thr(b + 1u))
		b++;
	return b > 255u ? 255u : b;
}

// --- log2, 2^x, cosine ---

// log2(1 + i/64) and 2^(i/64), C2_LOG_Q, i = 0..65 (two past the end for the second difference)
static const int32_t c2_log2_tab[66] = {
	0, 375270, 744810, 1108793, 1467383, 1820738, 2169009, 2512340,
	2850868, 3184728, 3514044, 3838941, 4159533, 4475935, 4788255, 5096595,
	5401057, 5701737, 5998727, 6292118, 6581994, 6868440, 7151536, 7431359,
	7707984, 7981483, 8251926, 8519380, 8783912, 9045584, 9304457, 9560591,
	9814042, 10064867, 10313120, 10558852, 10802114, 11042956, 11281425, 11517568,
	11751428, 11983051, 12212479, 12439752, 12664911, 12887994, 13109041, 13328087,
	13545168, 13760320, 13973576, 14184969, 14394532, 14602297, 14808293, 15012551,
	15215099, 15415967, 15615181, 15812769, 16008758, 16203172, 16396036, 16587377,
	16777216, 16965578,
};
static const int32_t c2_exp2_tab[66] = {
	16777216, 16959908, 17144589, 17331282, 17520007, 17710787, 17903645, 18098603,
	18295684, 18494911, 18696307, 18899897, 19105703, 19313750, 19524063, 19736666,
	19951585, 20168843, 20388467, 20610483, 20834917, 21061794, 21291142, 21522987,
	21757357, 21994279, 22233781, 22475891, 22720638, 22968049, 23218155, 23470984,
	23726566, 23984932, 24246111, 24510133, 24777031, 25046835, 25319578, 25595290,
	25874004, 26155754, 26440571, 26728490, 27019544, 27313768, 27611195, 27911861,
	28215802, 28523052, 28833647, 29147625, 29465022, 29785875, 30110222, 30438101,
	30769550, 31104608, 31443315, 31785710, 32131834, 32481727, 32835430, 33192984,
	33554432, 33919816,
};

// Newton's forward formula of second order between table entries i and i + 1 at t / 2^15 of the step (t <= 2^15):
// f_i + t Δ + t (t - 1)/2 Δ². Error within 1e-6 of a unit (the straight line's is 4e-5, and of one sign); t takes 15
// bits, since the step is 1/64 of an octave and every bit fewer costs a part in 10^6 there
FXP_INLINE int32_t tab_interp(const int32_t *f, uint32_t i, int32_t t)
{
	int32_t d1 = f[i + 1] - f[i], d2 = f[i + 2] - 2 * f[i + 1] + f[i];

	return f[i] + fxp_mulshr16(d1, t, 15) + ((d2 * ((t * (t - 32768)) >> 15)) >> 16);
}

FXP_INLINE int32_t log2_i(uint32_t x)
{
	int n = __builtin_clz(x);		// x > 0
	uint32_t f = x << n << 1;		// the fraction after the leading 1, 32 bits

	return ((31 - n) << C2_LOG_Q) + tab_interp(c2_log2_tab, f >> 26, (int32_t)((((f >> 10) & 0xFFFFu) + 1u) >> 1));
}

int32_t c2_log2(uint32_t x)
{
	return log2_i(x);
}

FXP_INLINE int32_t exp2_i(int32_t x, int q)
{
	int ip = (x >> C2_LOG_Q) + q - C2_LOG_Q;	// the mantissa below is 2^24 .. 2^25
	uint32_t fr = (uint32_t)x & (LOG1 - 1u);
	int32_t m;

	if (x <= C2_LOG_ZERO || ip < -C2_LOG_Q - 1)
		return 0;
	if (ip > 6)
		return INT32_MAX;
	m = tab_interp(c2_exp2_tab, fr >> 18, (int32_t)((((fr >> 2) & 0xFFFFu) + 1u) >> 1));
	if (ip < 0)
		return m >> -ip;
	// The top of the table is 2^25 exactly, so the last sliver of ip == 6 would shift into the sign bit.
	return m > (INT32_MAX >> ip) ? INT32_MAX : m << ip;
}

int32_t c2_exp2(int32_t x, int q)
{
	return exp2_i(x, q);
}

// 2^(x / 2^C2_LOG_Q) · 2^q for x <= 0 and q <= 24, the terms of the spectra's sums: the straight line between table
// entries, whose error (at most 1.8e-5, of one sign) scales a whole sum alike and so the ratio of two (11.2) and each
// amplitude by under 1e-5 (spec 17.2)
FXP_INLINE uint32_t exp2_sum(int32_t x, int q)
{
	uint32_t fr = (uint32_t)x & (LOG1 - 1u), i = fr >> 18, t = (fr >> 5) & 0x1FFFu;
	int s = C2_LOG_Q - q - (x >> C2_LOG_Q);
	uint32_t m = (uint32_t)c2_exp2_tab[i] + (((uint32_t)(c2_exp2_tab[i + 1] - c2_exp2_tab[i]) * t) >> 13);

	return s < 32 ? m >> s : 0u;
}

// cos(2π k/512), Q30, k = 0..128 (a quarter wave; the rest by symmetry)
static const int32_t c2_cos_tab[129] = {
	1073741824, 1073660973, 1073418433, 1073014240, 1072448455, 1071721163,
	1070832474, 1069782521, 1068571464, 1067199483, 1065666786, 1063973603,
	1062120190, 1060106826, 1057933813, 1055601479, 1053110176, 1050460278,
	1047652185, 1044686319, 1041563127, 1038283080, 1034846671, 1031254418,
	1027506862, 1023604567, 1019548121, 1015338134, 1010975242, 1006460100,
	1001793390, 996975812, 992008094, 986890984, 981625251, 976211688,
	970651112, 964944360, 959092290, 953095785, 946955747, 940673101,
	934248793, 927683790, 920979082, 914135678, 907154608, 900036924,
	892783698, 885396022, 877875009, 870221790, 862437520, 854523370,
	846480531, 838310216, 830013654, 821592095, 813046808, 804379079,
	795590213, 786681534, 777654384, 768510122, 759250125, 749875788,
	740388522, 730789757, 721080937, 711263525, 701339000, 691308855,
	681174602, 670937767, 660599890, 650162530, 639627258, 628995660,
	618269338, 607449906, 596538995, 585538248, 574449320, 563273883,
	552013618, 540670223, 529245404, 517740883, 506158392, 494499676,
	482766489, 470960600, 459083786, 447137835, 435124548, 423045732,
	410903207, 398698801, 386434353, 374111709, 361732726, 349299266,
	336813204, 324276419, 311690799, 299058239, 286380643, 273659918,
	260897982, 248096755, 235258165, 222384147, 209476638, 196537583,
	183568930, 170572633, 157550647, 144504935, 131437462, 118350194,
	105245103, 92124163, 78989349, 65842639, 52686014, 39521455,
	26350943, 13176464, 0,
};

FXP_INLINE int32_t cos512(uint32_t k)
{
	k &= 511u;
	if (k <= 128u)
		return c2_cos_tab[k];
	if (k <= 256u)
		return -c2_cos_tab[256u - k];
	if (k <= 384u)
		return -c2_cos_tab[k - 256u];
	return c2_cos_tab[512u - k];
}

#define C2_TURN_K	683565276	// 2^4/(2π) · 2^28: C2_LSP_Q rad to turns (2^32) with a shift of 28
#define C2_HALFPI_Q30	1686629713	// π/2 · 2^30: turns (2^32) to rad in Q30 with a shift of 30

int32_t c2_cos(int32_t x)
{
	// The nearest of the 512 grid angles and the rest δ (|δ| <= π/512): cos x0 cos δ - sin x0 sin δ with
	// cos δ = 1 - δ²/2 and sin δ = δ - δ³/6, both within 1e-10
	uint32_t t = (uint32_t)fxp_mulshr(x, C2_TURN_K, 28);
	uint32_t k = (t + (1u << 22)) >> 23;
	int32_t r = (int32_t)(t - (k << 23));		// |r| <= 2^22, turns
	int32_t dl = fxp_mulshr(r, C2_HALFPI_Q30, 30);	// δ = r · 2π / 2^32 rad, Q30
	int32_t d2 = fxp_mulshr(dl, dl, 30);
	int32_t cd = (1 << 30) - (d2 >> 1), sd = dl - fxp_mulshr(dl, d2, 30) / 6;

	return fxp_mulshr(cos512(k), cd, 30) - fxp_mulshr(cos512(k + 384u), sd, 30);
}

// --- The frame ---

#define LOG_BASS	16288221	// log2(1.4 · 1.4) (11.2, the double 1.9599999999999997)
#define LOG_LOWPITCH	(-83312036)	// log2(0.032) (11.4)
#define LOG_1E4		(-222930821)	// log2(1e-4) (11.2)
#define LOG_1E12	(-668792462)	// log2(1e-12) (12.4)
#define C2_DB_K		808071242	// 10 log10(2) / 16 · 2^32: C2_LOG_Q log2 to C2_DB_Q dB through the high word
#define C2_T_K		1426757253	// log2(10)/20 · 16 · 2^29: C2_DB_Q dB to C2_LOG_Q log2 of the amplitude
#define C2_RAND_TURN	131076u		// 2π_c/32767 in turns: 2^32 / 32767 · π_c/π = 131076.00014

uint32_t c2_rand(c2_dec_t *d)
{
	d->rng = d->rng * 1103515245u + 12345u;
	return (d->rng >> 16) & 0x7FFFu;
}

// log2(2^a + 2^b), both C2_LOG_Q
static int32_t log2_add(int32_t a, int32_t b)
{
	if (a < b) {
		int32_t t = a;
		a = b;
		b = t;
	}
	if (b == C2_LOG_ZERO || a - b >= 30 * LOG1)
		return a;
	return a + c2_log2((1u << 30) + (uint32_t)c2_exp2(b - a, 30)) - 30 * LOG1;
}

// --- The spectra (11.1, 11.2) ---

// The coefficients of A(z/γ) (10.4, γ = 1/2) from the LSPs, C2_A_Q: P(z/γ) = (1 + γ z^-1) Π_odd (1 - 2γ cos ω_i z^-1 +
// γ² z^-2), Q(z/γ) the same with (1 - γ z^-1) and the even LSPs, A = (P + Q)/2. Their absolute sums stay below
// 1.5^11 = 86.5 (each factor's are at most 2.25, 1.5): no product leaves 32 bits
#define C2_A_Q		23
static void lpc_w(const int32_t *cosl, int32_t *ak)
{
	int32_t pq[2][12];

	for (int h = 0; h < 2; h++) {
		int32_t *c = pq[h];
		int deg = 1;

		c[0] = 1 << C2_A_Q;
		c[1] = h ? -(1 << (C2_A_Q - 1)) : (1 << (C2_A_Q - 1));
		for (int i = h; i < C2_ORDER; i += 2) {
			int32_t co = cosl[i];		// Q29: 2γ cos ω_i = cos ω_i
			c[deg + 1] = 0;
			c[deg + 2] = 0;
			for (int n = deg + 2; n >= 1; n--)
				c[n] += -fxp_mulshr(co, c[n - 1], 29) + (n >= 2 ? c[n - 2] >> 2 : 0);
			deg += 2;
		}
	}
	for (int n = 0; n <= C2_ORDER; n++)
		ak[n] = (pq[0][n] + pq[1][n]) >> 1;
}

// log2 |H_W(k)|² (11.2), C2_LW_Q, at every fourth bin k = 0, 4, .. 256, into g[1..65], with g[0] and g[66] their mirror
// images (|H_W| is even in ω and about π); the bins between come from a 4-point cubic (Catmull-Rom) through them.
// |A(z/γ)| is smooth (its zeros lie at half the radius of A's), so the cubic is within 1.5e-5 of log2 |H_W|² · 0.2,
// the part that reaches the amplitudes, on the reference set (spec 17.2). Each point by Clenshaw's recurrence in
// Q30 cosines: b_n = a_n + 2 cos ω b_{n+1} - b_{n+2}, then H = a_0 + b_1 cos ω - b_2 - j b_1 sin ω
#define C2_LW_Q		16
#define C2_LW_MIN	(-60 * (1 << C2_LW_Q))	// a zero of A(z/γ) on the circle: the post filter takes the bin to nothing
static void spectrum_w(const int32_t *ak, int32_t *g)
{
	// |b_n| <= 11 Σ |a_n| (Chebyshev polynomials of the second kind are at most n + 1): scaled to stay below 2^30
	uint32_t sum = 0;
	for (int n = 0; n <= C2_ORDER; n++)
		sum += (uint32_t)(ak[n] < 0 ? -ak[n] : ak[n]);
	int sh = 9 - dsp_norm32((sum >> 7) * 11u);	// the bits of 11 Σ |a_n| past 2^30
	sh = sh < 0 ? 0 : sh;

	for (unsigned j = 0; j <= 64; j++) {
		int32_t c = cos512(4 * j), sn = cos512(4 * j + 384), b1 = 0, b2 = 0;

		for (int n = C2_ORDER; n >= 1; n--) {
			int32_t b0 = (ak[n] >> sh) + fxp_mulshr(b1, c, 29) - b2;
			b2 = b1;
			b1 = b0;
		}
		int32_t re = (ak[0] >> sh) + fxp_mulshr(b1, c, 30) - b2, im = fxp_mulshr(b1, sn, 30);
		uint32_t ar = re < 0 ? 0u - (uint32_t)re : (uint32_t)re, ai = im < 0 ? 0u - (uint32_t)im : (uint32_t)im;
		uint32_t mx = ar | ai;
		int32_t v = C2_LW_MIN;

		if (mx) {
			int s2 = 17 - dsp_norm32(mx);		// both below 2^15 after it
			if (s2 > 0) {
				ar >>= s2;
				ai >>= s2;
			} else {
				ar <<= -s2;
				ai <<= -s2;
			}
			v = (c2_log2(ar * ar + ai * ai) >> (C2_LOG_Q - C2_LW_Q)) +
			    (2 * (s2 + sh) - 2 * C2_A_Q) * (1 << C2_LW_Q);
			v = v < C2_LW_MIN ? C2_LW_MIN : v;
		}
		g[j + 1] = v;
	}
	g[0] = g[2];
	g[66] = g[64];
}

// The cubic between grid points p1 (bin 4i) and p2 (4i + 4) at bin 4i + r
FXP_INLINE int32_t lw_at(const int32_t *g, uint32_t k)
{
	const int32_t *p = g + (k >> 2);	// p[0..3] = the points at 4i - 4 .. 4i + 8

	switch (k & 3u) {
	case 0:
		return p[1];
	case 1:
		return (-9 * p[0] + 111 * p[1] + 29 * p[2] - 3 * p[3]) >> 7;
	case 2:
		return (-p[0] + 9 * p[1] + 9 * p[2] - p[3]) >> 4;
	default:
		return (-3 * p[0] + 29 * p[1] + 111 * p[2] - 9 * p[3]) >> 7;
	}
}

// Π (cos ω - cos ω_i) over the 5 LSPs at cl[0], cl[2], .. cl[8] (c and the cl Q29), as m / 2^15 · 2^e with m in
// [2^15, 2^16) (0 for an exact zero). Each difference's magnitude to 15 significant bits, rounded (f in [2^14, 2^15]),
// times the running product's 16 (p = m f below 2^31), renormalized by at most one bit and cut back to 16: the
// product form keeps its relative precision where an FFT's absolute error would swamp a formant's small |A| (spec
// 17.2). 16 x 16-bit products only, one instruction on both cores. The cut loses 1/(2^17 ln 2) of a factor on
// average (Benford: the mantissas are log-uniform), 5.5e-5 of the product, which lsp_log2pow puts back. The signs are
// not kept: the phase bins recompute them. Unrolled by hand: -Os keeps a rolled loop's product on the stack. One copy
// for both sets, not inlined: 300 B less on Arm and no slower (the call costs what inlining spills)
#define LSP_FACTOR(i)								\
	do {									\
		int32_t df = c - cl[i];						\
		uint32_t a = df < 0 ? 0u - (uint32_t)df : (uint32_t)df;		\
		int n = __builtin_clz(a | 1u);					\
		uint32_t y = a << n;						\
		uint32_t p = m * ((y >> 17) + ((y >> 16) & 1u));		\
		int k = __builtin_clz(p | 1u) - 1;				\
		m = (p << k) >> 15;						\
		sn += n + k;							\
	} while (0)

static __attribute__((noinline)) uint32_t lsp_prod(int32_t c, const int32_t *cl, int *ep)
{
	uint32_t m = 1u << 15;
	int sn = 0;

	LSP_FACTOR(0);
	LSP_FACTOR(2);
	LSP_FACTOR(4);
	LSP_FACTOR(6);
	LSP_FACTOR(8);
	*ep = 15 - sn;
	return m;
}

#undef LSP_FACTOR

// The sign of Π (cos ω - cos ω_i) over cl[0], cl[2], .. (bit 31)
FXP_INLINE uint32_t lsp_sign(int32_t c, const int32_t *cl)
{
	return (uint32_t)((c - cl[0]) ^ (c - cl[2]) ^ (c - cl[4]) ^ (c - cl[6]) ^ (c - cl[8]));
}

// 1e-6 / 2^10 (11.1, with |A|² / 2^10 below) = 1125899907 · 2^-60
#define C2_EPS_M	1125899907u
#define C2_EPS_E	(-60)

// x · 2^e with x < 2^32 to a mantissa in [2^15, 2^16] and its exponent, rounded
FXP_INLINE uint32_t norm16(uint32_t x, int *e)
{
	int n = __builtin_clz(x | 1u);

	*e += 16 - n;
	return (((x << n) >> 15) + 1u) >> 1;
}

// log2 (|A|² + 1e-6) from the two products (11.1): |A|² / 2^10 = cos²(ω/2) Π_odd² + sin²(ω/2) Π_even², with
// cos²(ω/2) = (1 + cos ω)/2. Each term: its product to 15 bits, squared (q² >> 15 in [2^13, 2^15]), times (1 ± c)/2
// to 16 bits: below 2^31, value t · 2^e. An exact zero product (two LSPs of one cosine) leaves its term out
FXP_INLINE int32_t lsp_log2pow(int32_t c, uint32_t mo, int eo, uint32_t me, int ee)
{
	int w0 = -30, w1 = -30;
	// the products to 15 bits, with the 5.5e-5 their cut lost (lsp_prod) back: q (1 + 6.1e-5), value q · 2^(e - 14)
	uint32_t q0 = (mo + (mo >> 14) + 1u) >> 1, q1 = (me + (me >> 14) + 1u) >> 1;
	uint32_t t0 = (q0 * q0 >> 15) * norm16((uint32_t)((1 << 29) + c), &w0);
	uint32_t t1 = (q1 * q1 >> 15) * norm16((uint32_t)((1 << 29) - c), &w1);
	int e0 = q0 ? 2 * eo - 13 + w0 : INT16_MIN, e1 = q1 ? 2 * ee - 13 + w1 : INT16_MIN;
	// The sum and 1e-6 / 2^10 = C2_EPS_M · 2^C2_EPS_E at one exponent above all three: each term below 2^30 after
	// its shift, the sum below 2^32
	int ex = (e0 > e1 ? e0 : e1) > C2_EPS_E ? (e0 > e1 ? e0 : e1) + 1 : C2_EPS_E + 1;
	uint32_t sum = (ex - e0 < 32 ? t0 >> (ex - e0) : 0u) + (ex - e1 < 32 ? t1 >> (ex - e1) : 0u) +
		       (ex - C2_EPS_E < 32 ? C2_EPS_M >> (ex - C2_EPS_E) : 0u);

	return log2_i(sum) + (ex + 10) * LOG1;
}

// 12.1 at bin k: the direction of H_m = conj H_A(k) for every harmonic m.. whose phase bin is k, so that 12.2 needs no
// arctangent: φ_m = arg(H_m X_m) makes e^{jφ_m} = X_m H_m / |H_m|, a product and a scale. H_A(k) = A(e^{jω}) =
// e^{-j5.5ω} 2^5 (cos(ω/2) Π_odd + j sin(ω/2) Π_even), so H_m points along e^{j5.5ω} (x - jy), x and y the two terms
// (sin and cos of ω/2 = 2π k/1024 in Q15). Stored as two int16 in d->phi[m] (real part low), the larger in
// [2^13, 2^14). Both terms 0 (two LSPs of one cosine at the bin): the reference's atan2(0, 0 + 1e-12) is 0, so x = 1.
// The next harmonic's bin out. Not inlined: it runs on at most L of the 257 bins, and kept apart the product loop
// holds its values in registers
static __attribute__((noinline)) uint32_t phase_bin(c2_dec_t *d, const c2_params_t *p, unsigned *mp, uint32_t k,
						    int32_t c, const int32_t *cl, uint32_t mo, int eo, uint32_t me, int ee)
{
	int32_t ex = eo - ee;
	int32_t xr = (int32_t)(((uint32_t)dsp_sin_idx(k + 256u) * mo) >> 1);	// not dsp_cos_idx: its C-SKY table
	int32_t yi = (int32_t)(((uint32_t)dsp_sin_idx(k) * me) >> 1);		// is 2 KB the decoder need not bring
	unsigned m = *mp;
	uint32_t pb = k;

	if (!mo || !me)
		ex = 0;		// a zero's exponent means nothing
	if (ex > 0)
		yi = ex < 32 ? yi >> ex : 0;
	else
		xr = -ex < 32 ? xr >> -ex : 0;
	if (!xr && !yi)
		xr = 1;
	int sh = 18 - dsp_norm32((uint32_t)(xr | yi));	// both below 2^14
	if (sh > 0) {
		xr >>= sh;
		yi >>= sh;
	} else {
		xr <<= -sh;
		yi <<= -sh;
	}
	if (lsp_sign(c, cl) >> 31)
		xr = -xr;
	if (lsp_sign(c, cl + 1) >> 31)
		yi = -yi;
	// (cos 5.5ω + j sin 5.5ω)(x - jy), 5.5ω = 2π · 11k/1024, then the larger part into [2^13, 2^14)
	int32_t cr = dsp_sin_idx(11u * k + 256u), sr = dsp_sin_idx(11u * k);
	int32_t hr = (cr * xr + sr * yi) >> 15, hi = (sr * xr - cr * yi) >> 15;
	uint32_t ar = (uint32_t)(hr < 0 ? -hr : hr), ai = (uint32_t)(hi < 0 ? -hi : hi);
	sh = 18 - dsp_norm32((ar | ai) | 1u);
	if (sh > 0) {
		hr >>= sh;
		hi >>= sh;
	} else {
		hr *= 1 << -sh;
		hi *= 1 << -sh;
	}
	uint32_t dir = (uint32_t)(uint16_t)hr | (uint32_t)hi << 16;
	while (m <= p->L && pb == k) {
		d->phi[m] = dir;
		m++;
		pb = m <= p->L ? c2_bin_phase(p->wo, m) : 512u;
	}
	*mp = m;
	return pb;
}

// Step 11.1, 11.2 and 12.1: log2 P(k) into buf[256 + k] and the post-filtered log2 P(k) R(k)^0.4 into buf[k],
// k = 0..255, the largest of each kind out; the direction of conj H_A at each harmonic's phase bin into d->phi[m]
static void spectrum_a(c2_dec_t *d, c2_synth_scratch_t *s, const c2_params_t *p, int32_t *maxlp, int32_t *maxlf)
{
	int32_t *buf = s->buf;
	const int32_t *cl = s->cosl;
	unsigned m = 1;
	uint32_t pb = p->L ? c2_bin_phase(p->wo, 1) : 512u;

	for (uint32_t k = 0; k <= NB; k++) {
		int32_t c = cos512(k) >> 1;		// Q29
		int eo, ee;
		uint32_t mo = lsp_prod(c, cl, &eo), me = lsp_prod(c, cl + 1, &ee);	// [2^15, 2^16) or 0

		if (k < NB)
			buf[NB + k] = -lsp_log2pow(c, mo, eo, me, ee);
		if (pb == k)
			pb = phase_bin(d, p, &m, k, c, cl, mo, eo, me, ee);
	}

	// P R^(2β) = P^1.2 |H_W|^0.4: lp + 0.2 (lp + log2 |H_W|²)
	const int32_t *g = s->lwg;
	int32_t mlp = INT32_MIN, mlf = INT32_MIN;
	for (uint32_t k = 0; k < NB; k++) {
		int32_t lp = buf[NB + k];
		int32_t lf = lp + fxp_mulhr(lp + lw_at(g, k) * (1 << (C2_LOG_Q - C2_LW_Q)), 858993459);

		buf[k] = lf;
		mlp = lp > mlp ? lp : mlp;
		mlf = lf > mlf ? lf : mlf;
	}
	*maxlp = mlp;
	*maxlf = mlf;
}

// log2 of 1e-4 (or 0) plus the sum of 2^v[k] over n bins, max their largest (C2_LOG_Q)
static int32_t log2_sum(const int32_t *v, unsigned n, int32_t max, int32_t extra)
{
	uint32_t sum = 0;

	for (unsigned k = 0; k < n; k++)
		sum += exp2_sum(v[k] - max, 22);
	return log2_add(c2_log2(sum) - 22 * LOG1 + max, extra);
}

// log2 Σ 2^(amp_m · (1 + sq)) over m = 1..L (sq: the sum of squares), C2_LOG_ZERO when every band is empty
static int32_t log2_amps(const int32_t *amp, unsigned L, int sq)
{
	int32_t mx = INT32_MIN;
	uint32_t sum = 0;

	for (unsigned m = 1; m <= L; m++)
		if (amp[m] != C2_LOG_ZERO && amp[m] * (1 + sq) > mx)
			mx = amp[m] * (1 + sq);
	if (mx == INT32_MIN)
		return C2_LOG_ZERO;
	for (unsigned m = 1; m <= L; m++)
		if (amp[m] != C2_LOG_ZERO)
			sum += (uint32_t)c2_exp2(amp[m] * (1 + sq) - mx, 24);	// L <= 79 terms of at most 2^24
	return c2_log2(sum) - 24 * LOG1 + mx;
}

// 13.3: the limiter on the frame's 80 values (C2_OUT_Q, in place) and the conversion to 16 bits
static void synth_out(int32_t *out, int16_t pcm[C2_N])
{
	int32_t xmax = 0;

	for (int i = 0; i < C2_N; i++)
		xmax = out[i] > xmax ? out[i] : xmax;
	if (xmax > (30000 << C2_OUT_Q)) {
		// g = (30000 / x_max)², Q30. 1/x_max = r / 2^e (r in [2^30, 2^31), e = 49..61 for x_max in (480000, 2^31)), and
		// u = 30000 / x_max = (480000 · 2^12 · r / 2^32) / 2^(e - 50) in Q30: 32-bit shifts only
		int e;
		uint32_t r = dsp_recip((uint32_t)xmax, &e);
		int32_t v = fxp_mulhr((30000 << C2_OUT_Q) << 12, (int32_t)r);
		int32_t u = e >= 50 ? v >> (e - 50) : v << (50 - e);
		int32_t g = fxp_mulshr(u, u, 30);

		for (int i = 0; i < C2_N; i++)
			out[i] = fxp_mulshr(out[i], g, 30);
	}
	for (int i = 0; i < C2_N; i++) {
		int32_t v = fxp_clamp(out[i], -(32767 << C2_OUT_Q), 32767 << C2_OUT_Q);

		pcm[i] = (int16_t)(v >= 0 ? v >> C2_OUT_Q : -((-v) >> C2_OUT_Q));	// toward zero
	}
}

void c2_synth_frame(c2_dec_t *d, c2_scratch_t *scr, const c2_params_t *p, int16_t pcm[C2_N], uint16_t *draws)
{
	c2_synth_scratch_t *s = (c2_synth_scratch_t *)scr;
	int32_t *buf = s->buf;
	const unsigned L = p->L;
	int32_t maxlp, maxlf;
	unsigned nd = 0;

	for (int i = 0; i < C2_ORDER; i++)
		s->cosl[i] = c2_cos(p->lsp[i]) >> 1;	// Q29: a difference of two stays inside 32 bits
	lpc_w(s->cosl, s->ak);
	spectrum_w(s->ak, s->lwg);
	spectrum_a(d, s, p, &maxlp, &maxlf);

	// 11.2: g = E_before / E_after · E, the sums over 0..255 each with 1e-4
	int32_t lg = log2_sum(buf + NB, NB, maxlp, LOG_1E4) - log2_sum(buf, NB, maxlf, LOG_1E4)
		     + c2_log2(p->e) - C2_E_Q * LOG1;

	// 11.3: A_m² = g Σ P_f(k) (bass boost below bin 64) over a_m <= k < b_m, each band summed from its own largest
	uint32_t lo = c2_bin_band(p->wo, 1);
	for (unsigned m = 1; m <= L; m++) {
		uint32_t hi = c2_bin_band(p->wo, 2 * m + 1);
		int32_t bmax = INT32_MIN;
		uint32_t sum = 0;

		if (hi > NB)
			hi = NB;
		for (uint32_t k = lo; k < hi; k++) {
			int32_t v = buf[k] + (k < 64 ? LOG_BASS : 0);
			bmax = v > bmax ? v : bmax;
		}
		for (uint32_t k = lo; k < hi; k++)
			sum += exp2_sum(buf[k] + (k < 64 ? LOG_BASS : 0) - bmax, 24);	// <= 27 bins of 2^24
		d->amp[m] = lo < hi ? (lg + bmax + c2_log2(sum) - 24 * LOG1) >> 1 : C2_LOG_ZERO;
		lo = hi;
	}
	// 11.4
	if (p->wo <= C2_WO_150HZ && d->amp[1] != C2_LOG_ZERO)
		d->amp[1] += LOG_LOWPITCH;

	// 12.4: e = 10 log10((1e-12 + Σ A_m²) / L); the background estimate on unvoiced frames below 40 dB; the threshold
	int32_t la2 = log2_amps(d->amp, L, 1);
	int32_t e = fxp_mulhr(log2_add(la2, LOG_1E12) - c2_log2(L), C2_DB_K);

	if (!p->voiced && e < (40 << C2_DB_Q))
		d->bg += fxp_mulhr(e - d->bg, 429496730);	// b + 0.1 (e - b) = 0.9 b + 0.1 e
	int32_t lt = fxp_mulshr(d->bg + (6 << C2_DB_Q), C2_T_K, 29);

	// 13.1: Y(b_m) = A_m e^{jφ_m} at a scale 2^sc that keeps the transform's values below 2^30 (they are at most
	// 4 Σ A_m; Σ A_m <= sqrt(L Σ A_m²) bounds it without a pass). Σ A_m is held to 2^24, 5 times the loudest frame of
	// the reference set: the output, at most 2 Σ A_m, then fits C2_OUT_Q with the overlap-add's sum. A frame past it
	// is quieter than the spec's, before a limiter that takes it to near silence anyway (spec 17.2)
	int32_t lsum = la2 == C2_LOG_ZERO ? C2_LOG_ZERO : (la2 + c2_log2(L)) >> 1, over = 0;
	if (lsum > 24 * LOG1) {
		lsum = log2_amps(d->amp, L, 0);
		over = lsum > 24 * LOG1 ? lsum - 24 * LOG1 : 0;
		lsum -= over;
	}
	int sc = (28 * LOG1 - lsum) >> C2_LOG_Q;
	sc = sc > 24 ? 24 : sc;

	// 12.2 and 12.4: φ_ex; X_m = e^{jθ_m}, θ_m = m φ_ex (voiced) or a random phase (unvoiced), and e^{jφ_m} =
	// X_m H_m / |H_m|; a voiced harmonic under the threshold takes a random phase instead. The draws in increasing m,
	// as the reference makes them (one kind per frame)
	c2_phi_ex_step(d, p->wo);
	uint32_t ex = (uint32_t)fxp_mulshr((int32_t)(((uint64_t)(uint32_t)d->phi_ex_hi << 12) | (d->phi_ex_lo >> 20)),
					   C2_TURN_K, 28);
	for (int n = 0; n < DSP_FFT_SIZE; n++)
		buf[n] = 0;
	for (unsigned m = 1; m <= L; m++) {
		uint32_t b = c2_bin_synth(p->wo, m), th;
		int32_t la = d->amp[m] - over;
		bool filt = true;

		if (!p->voiced) {
			th = c2_rand(d) * C2_RAND_TURN;
			nd++;
		} else if (d->amp[m] < lt) {
			th = c2_rand(d) * C2_RAND_TURN;
			nd++;
			filt = false;
		} else {
			th = m * ex;
		}
		int32_t vr = dsp_cos_i(th), vi = dsp_cos_i(th - (1u << 30));
		if (filt) {
			int32_t hr = (int16_t)d->phi[m], hi = (int16_t)(d->phi[m] >> 16), t = vr;
			vr = (t * hr - vi * hi) >> 15;		// below 2^15: |H_m| < 2^14.5
			vi = (vi * hr + t * hi) >> 15;
			la += 15 * LOG1 - (c2_log2((uint32_t)(hr * hr + hi * hi)) >> 1);
		}
		int32_t a = c2_exp2(la, sc);

		buf[2 * b] = fxp_mulshr16(a, vr, 15);
		buf[2 * b + 1] = fxp_mulshr16(a, vi, 15);
	}
	*draws = (uint16_t)nd;
	dsp_irfft(buf);

	// 13.2: out[i] = o[i] + y[433 + i] t[i], out[79] = y[0] t[79], o[i] = y[1 + i] t[80 + i]; t[i] = i/80 (Q15),
	// C2_OUT_Q. The output lands on buf[432 .. 511], each slot read before it is written
	// sc >= 4 (Σ A_m is held to 2^24), so sh >= 0; sh = 0 (a bound past 2^23, not met on random bits) rounds nothing
	const int sh = sc - C2_OUT_Q;
	const int32_t rnd = (1 << sh) >> 1;
	for (int i = 0; i < C2_N - 1; i++) {
		int32_t t = (i * 419431) >> 10;		// i · 32768/80
		int32_t o = d->ola[i] + fxp_mulshr16((buf[433 + i] + rnd) >> sh, t, 15);

		d->ola[i] = fxp_mulshr16((buf[1 + i] + rnd) >> sh, 32768 - t, 15);
		buf[432 + i] = o;
	}
	buf[511] = fxp_mulshr16((buf[0] + rnd) >> sh, (79 * 419431) >> 10, 15);
	synth_out(buf + 432, pcm);
}

void c2_synth_mute(c2_dec_t *d, c2_scratch_t *scr, int16_t pcm[C2_N])
{
	int32_t *out = ((c2_synth_scratch_t *)scr)->buf;

	for (int i = 0; i < C2_N - 1; i++) {
		out[i] = d->ola[i];
		d->ola[i] = 0;
	}
	out[C2_N - 1] = 0;
	synth_out(out, pcm);
}
