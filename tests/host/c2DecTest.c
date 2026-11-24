// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codec 2's decoder (core/codec2/c2_dec.c, c2_synth.c) against docs/spec/codec2.md 10-13 and 17:
//   - the random generator's first 64 values (spec 12.3, table rng_first64: the first five, the sum and the SHA-256)
//   - the exact helpers against float32 and double models of the spec: f32 rounding, the three harmonic bins (11.3,
//     12.1, 13.1) for every ω0 level and every mean of two levels, φ_ex's update (12.2), cosine, log2 and 2^x
//   - the API: c2_dec_block without a frame, the blocks of each mode, a frame given early, init resetting everything
//   - concealment: the fade (energy halved every 10 ms), then silence, then a good frame resumes
//   - a golden CRC of the PCM of synthetic streams (random and structured frames, bad frames, both modes), so a change
//     to the output is seen; the limiter's set point on every sample
// With the reference harness (RFOUNDRY_C2_HARNESS=<dir> or the first argument; kept outside the repository, spec 1):
// every 10 ms frame of every key decodes to the reference's parameters within compare.py's params_close (voicing, L
// exact; ω0 1e-5, energy 1e-4 relative, LSPs 1e-4 rad), with the reference's random draw count, and the PCM of two
// keys has its golden CRC.
//
// The Makefile builds this twice: plain (64-bit products) and with -DFXP_SPLIT16 (the 16-bit halves a C-SKY build
// runs). Both must give the same CRCs. A change meant to change the output changes them: write the new ones down with
// the reason.
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec2/c2_dec_int.h"
#include "support/sha256.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; if (failures < 30) { printf("FAIL %s:%d: ", __FILE__, \
	__LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

#define Q28	268435456.0
#define D2	6.283185307		// 2π_c (spec 3.2)

// Golden CRC-32s of the PCM (this file's decoder, 2026-11-22)
#define GOLD_SYNTH_3200	0xE009AAC5u
#define GOLD_SYNTH_1600	0x34609154u
#define GOLD_KEY_A	"vowel_a_vibrato.3200"
#define GOLD_KEY_A_CRC	0x70E48AE7u
#define GOLD_KEY_B	"utterance.1600"
#define GOLD_KEY_B_CRC	0x7772A8DEu

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static uint32_t crc32(uint32_t crc, const void *p, size_t n)
{
	const uint8_t *b = p;

	crc = ~crc;
	while (n--) {
		crc ^= *b++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static c2_dec_state_t st;
static c2_scratch_t scr;
#define D	((c2_dec_t *)&st)

// --- Random generator (12.3) ---

static void test_rng(void)
{
	c2_dec_init(&st, C2_MODE_3200);
	static const uint32_t first[5] = { 16838, 5758, 10113, 17515, 31051 };
	uint8_t le[64 * 4], h[32];
	uint32_t sum = 0;
	for (int i = 0; i < 64; i++) {
		uint32_t r = c2_rand(D);
		if (i < 5)
			CHECK(r == first[i], "rng %d: %u, not %u", i, r, first[i]);
		sum += r;
		for (int b = 0; b < 4; b++)
			le[4 * i + b] = (uint8_t)(r >> (8 * b));
	}
	sha256_t sh;
	sha256_init(&sh);
	sha256_update(&sh, le, sizeof le);
	sha256_final(&sh, h);
	CHECK(sum == 1110943u, "rng_first64 sum %u (1110943)", sum);
	CHECK(h[0] == 0x32 && h[1] == 0xb6 && h[2] == 0xa9 && h[3] == 0x01 && h[4] == 0x7b && h[5] == 0xdc &&
	      h[6] == 0xf7 && h[7] == 0xa7, "rng_first64 SHA-256 %02x%02x%02x%02x... (32b6a9017bdcf7a7)", h[0], h[1], h[2],
	      h[3]);
}

// --- Exact helpers ---

static float rd_f(void)
{
	return (float)(D2 / 512.0);
}

// The ω0 a decoder can meet: the 128 levels and the float32 mean of any two (10.3)
static unsigned wo_set(int32_t *out)
{
	unsigned n = 0;
	for (int a = 0; a < 128; a++) {
		out[n++] = c2_wo_dequant((uint8_t)a);
		for (int b = a + 1; b < 128; b++) {
			float fa = (float)(c2_wo_dequant((uint8_t)a) / Q28), fb = (float)(c2_wo_dequant((uint8_t)b) / Q28);
			float mean = (fa + fb) / 2.0f;
			out[n++] = (int32_t)((double)mean * Q28);
			CHECK((double)out[n - 1] == (double)mean * Q28, "mean of %d and %d is not whole in Q28", a, b);
			// the decoder's own mean (c2_dec.c): the sum rounded to 24 bits, halved
			int32_t mine = (int32_t)(c2_f32_round_u32((uint32_t)(c2_wo_dequant((uint8_t)a) +
									      c2_wo_dequant((uint8_t)b))) >> 1);
			CHECK(mine == out[n - 1], "mean of %d and %d: %d, float32 %d", a, b, mine, out[n - 1]);
		}
	}
	return n;
}

static void test_helpers(void)
{
	// f32 rounding of whole numbers below 2^30: every bit width, the midpoints, random values
	unsigned bad = 0;
	for (int k = 0; k < 2000000; k++) {
		uint32_t x = k < 1000 ? (uint32_t)k : rng() >> (2 + rng() % 24);
		if (k >= 1000 && k < 1100)
			x = (1u << (k - 1000) % 30) + (uint32_t)((k % 3) - 1);
		if (x >= (1u << 30))
			continue;
		if (c2_f32_round_u32(x) != (uint32_t)(float)x)
			bad++;
		uint32_t mid = ((rng() >> 8) | (1u << 23)) << 6 | (1u << 5);	// halfway between two float32 values
		if (c2_f32_round_u32(mid) != (uint32_t)(float)mid)
			bad++;
	}
	CHECK(bad == 0, "c2_f32_round_u32: %u differ from float32", bad);

	// The bins of every harmonic of every reachable ω0
	static int32_t wos[128 * 129 / 2 + 1];
	unsigned n = wo_set(wos), nb = 0, bad_band = 0, bad_ph = 0, bad_syn = 0;
	float rd = rd_f();
	for (unsigned i = 0; i < n; i++) {
		int32_t wo = wos[i];
		float wof = (float)(wo / Q28);
		unsigned L = c2_L_of_wo(wo);
		for (unsigned m = 1; m <= L; m++) {
			int a = (int)(((double)m - 0.5) * wof / rd + 0.5), b = (int)(((double)m + 0.5) * wof / rd + 0.5);
			if (c2_bin_band(wo, 2 * m - 1) != (uint32_t)a || c2_bin_band(wo, 2 * m + 1) != (uint32_t)b)
				bad_band++;
			float mf = (float)m * wof;
			int ph = (int)((float)(mf / rd) + 0.5f);
			if (c2_bin_phase(wo, m) != (uint32_t)ph)
				bad_ph++;
			int sy = (int)((double)(mf * 512.0f) / D2 + 0.5);
			if (c2_bin_synth(wo, m) != (uint32_t)(sy > 255 ? 255 : sy))
				bad_syn++;
			nb++;
		}
	}
	CHECK(bad_band == 0 && bad_ph == 0 && bad_syn == 0, "bins: %u band, %u phase, %u synthesis of %u differ",
	      bad_band, bad_ph, bad_syn, nb);
	printf("bins: %u harmonics of %u ω0 values, 11.3, 12.1 and 13.1 as the float model's\n", nb, n);

	// φ_ex (12.2) against float32 arithmetic, 1,000,000 frames of random reachable ω0
	float ph = 0.0f;
	c2_dec_init(&st, C2_MODE_3200);
	bad = 0;
	for (int k = 0; k < 1000000; k++) {
		int32_t wo = wos[rng() % n];
		float wof = (float)(wo / Q28);
		ph = ph + wof * 80.0f;
		ph = (float)((double)ph - D2 * floor((float)((double)ph / D2 + 0.5)));
		c2_phi_ex_step(D, wo);
		int64_t v = (int64_t)(((uint64_t)(uint32_t)D->phi_ex_hi << 32) | D->phi_ex_lo);
		if ((double)v / 281474976710656.0 != (double)ph)
			bad++;
	}
	CHECK(bad == 0, "φ_ex: %u of 1000000 steps differ from float32", bad);

	// cos, log2, 2^x
	double ec = 0, el = 0, ee = 0;
	for (int k = 0; k < 400000; k++) {
		int32_t x = (int32_t)(-2 * Q28) + (int32_t)(rng() % (uint32_t)(10 * Q28));
		ec = fmax(ec, fabs(c2_cos(x) / 1073741824.0 - cos(x / Q28)));
		uint32_t y = rng() >> (rng() % 32);
		if (y)
			el = fmax(el, fabs(c2_log2(y) / 16777216.0 - log2(y)));
		int32_t z = (int32_t)(rng() % (40u << 24)) - (int32_t)(30u << 24);
		int32_t e2 = c2_exp2(z, 30);
		double want = exp2(z / 16777216.0 + 30);
		if (want >= 65536.0 && want < 2147483647.0)
			ee = fmax(ee, (fabs(e2 - want) - 1) / want);	// beyond the result's last bit
	}
	CHECK(ec < 1e-8 && el < 1.5e-6 && ee < 1.5e-6, "cos error %.3g (1e-8), log2 %.3g (1.5e-6), exp2 %.3g (1.5e-6)",
	      ec, el, ee);
	CHECK(c2_exp2(C2_LOG_ZERO, 30) == 0 && c2_exp2(0, 30) == (1 << 30) && c2_exp2(7 << 24, 24) == INT32_MAX,
	      "exp2 ends");
	// The last fraction below 7: the table's top entry is 2^25, shifted by 6 it is 2^31 and must saturate.
	CHECK(c2_exp2(7 * (1 << 24) - 1, 24) == INT32_MAX && c2_exp2(7 * (1 << 24) - 2, 24) == INT32_MAX &&
	      c2_exp2(6 * (1 << 24), 24) == (1 << 30), "exp2 saturates at the top of ip 6");
	printf("helpers: cos within %.2g, log2 within %.2g, 2^x within %.2g relative\n", ec, el, ee);
}

// --- Streams ---

static void frame_random(c2_mode_t m, uint8_t *bits, int kind)
{
	if (kind == 0) {
		for (int i = 0; i < C2_FRAME_BYTES; i++)
			bits[i] = (uint8_t)rng();
		return;
	}
	// structured: mostly voiced, moderate energy, increasing LSPs
	c2_index_t ix;
	memset(&ix, 0, sizeof ix);
	for (int i = 0; i < 4; i++)
		ix.voiced[i] = (uint8_t)(rng() % 5 != 0);
	for (int i = 0; i < 2; i++) {
		ix.wo[i] = (uint8_t)(10 + rng() % 100);
		ix.e[i] = (uint8_t)(8 + rng() % 24);
	}
	for (int i = 0; i < C2_ORDER; i++)
		ix.lsp[i] = (uint8_t)(m == C2_MODE_1600 ? rng() % (1u << c2_lsp_bits(m, (uint8_t)i)) : 2 + rng() % 12);
	c2_pack(m, &ix, bits);
}

// Decode n frames of a stream into pcm (n · 160 or 320 samples); bad[i] marks lost frames. Returns the samples
static unsigned decode(c2_mode_t m, const uint8_t *bits, const uint8_t *bad, unsigned n, int16_t *pcm)
{
	unsigned out = 0;
	int nb = m == C2_MODE_1600 ? 2 : 1;

	for (unsigned i = 0; i < n; i++) {
		c2_dec_frame(&st, bits + i * C2_FRAME_BYTES, bad != NULL && bad[i]);
		for (int b = 0; b < nb; b++) {
			bool ok = c2_dec_block(&st, &scr, pcm + out);
			CHECK(ok, "block %u.%d", i, b);
			out += C2_BLOCK_SAMPLES;
		}
	}
	return out;
}

#define NSYN	400u
static uint8_t syn_bits[NSYN * C2_FRAME_BYTES], syn_bad[NSYN];
static int16_t syn_pcm[NSYN * 2 * C2_BLOCK_SAMPLES];

static void make_stream(c2_mode_t m)
{
	for (unsigned i = 0; i < NSYN; i++) {
		uint8_t *b = syn_bits + i * C2_FRAME_BYTES;
		if (i % 50 < 4)
			memcpy(b, m == C2_MODE_1600 ? c2_silence_1600 : c2_silence_3200, C2_FRAME_BYTES);
		else
			frame_random(m, b, i % 3 != 0);
		// a frame lost now and then, and a burst past the fade
		syn_bad[i] = (uint8_t)(i % 23 == 7 || (i >= 200 && i < 212));
	}
}

static void test_golden(void)
{
	for (int mi = 0; mi < 2; mi++) {
		c2_mode_t m = mi ? C2_MODE_1600 : C2_MODE_3200;
		rng_state = 0x2545F491u + (uint32_t)mi;
		make_stream(m);
		c2_dec_init(&st, m);
		unsigned ns = decode(m, syn_bits, syn_bad, NSYN, syn_pcm);
		uint32_t crc = crc32(0, syn_pcm, ns * sizeof(int16_t));
		int16_t mx = 0, mn = 0;
		for (unsigned i = 0; i < ns; i++) {
			mx = syn_pcm[i] > mx ? syn_pcm[i] : mx;
			mn = syn_pcm[i] < mn ? syn_pcm[i] : mn;
		}
		uint32_t gold = mi ? GOLD_SYNTH_1600 : GOLD_SYNTH_3200;
		CHECK(crc == gold, "synthetic %s: PCM CRC %08x, golden %08x", mi ? "1600" : "3200", crc, gold);
		// The limiter (13.3): no positive sample past its set point
		CHECK(mx <= 30000, "synthetic %s: a sample at %d, over the limiter's 30000", mi ? "1600" : "3200", mx);
		printf("synthetic %s: %u samples, PCM CRC %08x, peaks %d / %d\n", mi ? "1600" : "3200", ns, crc, mn, mx);
	}
}

// --- API ---

static void test_api(void)
{
	int16_t pcm[C2_BLOCK_SAMPLES];
	c2_params_t p;
	uint8_t bits[C2_FRAME_BYTES];

	for (int mi = 0; mi < 2; mi++) {
		c2_mode_t m = mi ? C2_MODE_1600 : C2_MODE_3200;
		c2_dec_init(&st, m);
		CHECK(!c2_dec_params(&st, 0, &p, NULL), "params before a block");
		memset(pcm, 0x55, sizeof pcm);
		CHECK(!c2_dec_block(&st, &scr, pcm), "%d: a block with no frame", mi);
		bool zero = true;
		for (int i = 0; i < C2_BLOCK_SAMPLES; i++)
			zero &= pcm[i] == 0;
		CHECK(zero, "%d: no frame: silence", mi);
		CHECK(!c2_dec_params(&st, 0, &p, NULL), "params after a block with no frame");

		frame_random(m, bits, 1);
		c2_dec_frame(&st, bits, false);
		CHECK(c2_dec_block(&st, &scr, pcm), "%d: first block", mi);
		CHECK(c2_dec_params(&st, 0, &p, NULL) && c2_dec_params(&st, 1, &p, NULL) && !c2_dec_params(&st, 2, &p, NULL),
		      "%d: params of a block", mi);
		if (m == C2_MODE_1600)
			CHECK(c2_dec_block(&st, &scr, pcm), "1600: second block");
		CHECK(!c2_dec_block(&st, &scr, pcm), "%d: the frame's blocks are out", mi);
		CHECK(!c2_dec_block(&st, &scr, pcm), "%d: still out", mi);

		// A frame given before the last one's blocks are out replaces them: its blocks come next, all of them
		if (m == C2_MODE_1600) {
			c2_dec_frame(&st, bits, false);
			CHECK(c2_dec_block(&st, &scr, pcm), "1600: block 1 of 2");
			frame_random(m, bits, 1);
			c2_dec_frame(&st, bits, false);
			CHECK(c2_dec_block(&st, &scr, pcm) && c2_dec_block(&st, &scr, pcm), "1600: the next frame's two");
			CHECK(!c2_dec_block(&st, &scr, pcm), "1600: then out");
		}
	}

	// The state of spec 10.1 after init: ω0_min unvoiced, E 1, the LSPs (i - 1) π_c/11, φ_ex 0, b 0, seed 1
	memset(&st, 0xA5, sizeof st);
	c2_dec_init(&st, C2_MODE_3200);
	bool clean = D->prev.wo == c2_wo_dequant(0) && !D->prev.voiced && D->prev.e == (1u << C2_E_Q) &&
		     D->phi_ex_lo == 0 && D->phi_ex_hi == 0 && D->bg == 0 && D->rng == 1 && D->blocks == 0;
	for (int i = 0; i < C2_ORDER; i++)
		clean &= fabs(D->prev.lsp[i] / Q28 - (float)(3.141592654 * i / 11)) < 1e-8;
	for (int i = 0; i < C2_N - 1; i++)
		clean &= D->ola[i] == 0;
	CHECK(clean, "init: the state of spec 10.1");

	// Init resets: the same stream after init over a used state and over garbage gives the same PCM
	rng_state = 77;
	make_stream(C2_MODE_3200);
	c2_dec_init(&st, C2_MODE_3200);
	unsigned ns = decode(C2_MODE_3200, syn_bits, syn_bad, 60, syn_pcm);
	uint32_t a = crc32(0, syn_pcm, ns * 2);
	decode(C2_MODE_3200, syn_bits + 60 * C2_FRAME_BYTES, NULL, 30, syn_pcm);
	c2_dec_init(&st, C2_MODE_3200);
	uint32_t b = crc32(0, syn_pcm, decode(C2_MODE_3200, syn_bits, syn_bad, 60, syn_pcm) * 2);
	memset(&st, 0x5A, sizeof st);
	c2_dec_init(&st, C2_MODE_3200);
	uint32_t c = crc32(0, syn_pcm, decode(C2_MODE_3200, syn_bits, syn_bad, 60, syn_pcm) * 2);
	CHECK(a == b && a == c, "init resets: %08x %08x %08x", a, b, c);
}

// --- Concealment ---

static double rms(const int16_t *x, unsigned n)
{
	double s = 0;
	for (unsigned i = 0; i < n; i++)
		s += (double)x[i] * x[i];
	return sqrt(s / n);
}

static void test_conceal(void)
{
	for (int mi = 0; mi < 2; mi++) {
		c2_mode_t m = mi ? C2_MODE_1600 : C2_MODE_3200;
		int nb = mi ? 2 : 1;
		c2_index_t ix;
		uint8_t bits[C2_FRAME_BYTES];
		int16_t pcm[2 * C2_BLOCK_SAMPLES];
		static const uint8_t l3200[C2_ORDER] = { 8, 10, 6, 6, 6, 6, 8, 8, 8, 8 };
		static const uint8_t l1600[C2_ORDER] = { 4, 6, 6, 6, 6, 6, 6, 4, 4, 2 };

		memset(&ix, 0, sizeof ix);
		memset(ix.voiced, 1, sizeof ix.voiced);
		ix.wo[0] = ix.wo[1] = 40;
		ix.e[0] = ix.e[1] = 18;		// under the limiter's set point: the fade is seen at the output
		memcpy(ix.lsp, mi ? l1600 : l3200, C2_ORDER);
		c2_pack(m, &ix, bits);

		// Lost before any good frame: silence
		c2_dec_init(&st, m);
		c2_dec_frame(&st, bits, true);
		double r0 = 0;
		for (int b = 0; b < nb; b++) {
			c2_dec_block(&st, &scr, pcm);
			r0 += rms(pcm, C2_BLOCK_SAMPLES);
		}
		c2_dec_frame(&st, NULL, false);		// no bits: lost as well
		for (int b = 0; b < nb; b++) {
			c2_dec_block(&st, &scr, pcm);
			r0 += rms(pcm, C2_BLOCK_SAMPLES);
		}
		CHECK(r0 == 0, "%d: lost frames before a good one: silence (%g)", mi, r0);

		for (int i = 0; i < 12; i++)
			decode(m, bits, NULL, 1, pcm);
		int32_t full[C2_L_MAX + 1];
		unsigned L = D->prev.L;
		memcpy(full, D->amp, sizeof full);
		double level = rms(pcm, (unsigned)nb * C2_BLOCK_SAMPLES);
		CHECK(level > 300, "%d: the test frame is loud (%g)", mi, level);

		// Lost: each 10 ms frame's amplitudes 3 dB under the one before, held otherwise; then the overlap-add's
		// tail and silence
		int f = 0;
		bool fade = true, muted = true;
		double tail = 0;
		while (f < 24) {
			c2_dec_frame(&st, bits, true);
			for (int b = 0; b < nb; b++) {
				c2_dec_block(&st, &scr, pcm);
				f += 2;
				if (f <= C2_FADE_FRAMES) {
					// within 2^-16: the halved energy is floored in C2_E_Q
					for (unsigned k = 1; k <= L; k++)
						fade &= abs(D->amp[k] - (full[k] - f * (1 << (C2_LOG_Q - 1)))) < 256;
				} else if (f == C2_FADE_FRAMES + 2) {
					tail = rms(pcm, C2_N);
					muted &= rms(pcm + C2_N, C2_N) == 0;
				} else {
					muted &= rms(pcm, C2_BLOCK_SAMPLES) == 0;
				}
			}
		}
		CHECK(fade, "%d: the fade: 3 dB per 10 ms", mi);
		CHECK(muted && tail > 0, "%d: after %d frames the tail (%.1f), then silence", mi, C2_FADE_FRAMES, tail);

		// A good frame resumes, from the faded level up, then the level is the one before
		decode(m, bits, NULL, 1, pcm);
		CHECK(rms(pcm + C2_N * (2 * nb - 1), C2_N) > 0, "%d: resumes", mi);
		decode(m, bits, NULL, 1, pcm);
		bool same = true;
		for (unsigned k = 1; k <= L; k++)
			same &= abs(D->amp[k] - full[k]) < 64;
		for (int i = 0; i < 6; i++)
			decode(m, bits, NULL, 1, pcm);
		double again = rms(pcm, (unsigned)nb * C2_BLOCK_SAMPLES);
		CHECK(same && fabs(again / level - 1) < 0.1, "%d: back to the level (%.0f of %.0f)", mi, again, level);

		// One lost frame between good ones: held at -3 dB
		c2_dec_frame(&st, bits, true);
		c2_dec_block(&st, &scr, pcm);
		c2_params_t p;
		CHECK(c2_dec_params(&st, 0, &p, NULL) && p.voiced && p.wo == c2_wo_dequant(40) &&
		      p.e == c2_e_dequant(18) >> 1, "%d: a lost frame holds the parameters, energy halved", mi);
		if (nb == 2)
			c2_dec_block(&st, &scr, pcm);
	}
}

// Frames lost before the first good one leave the decoder as init left it: the stream after them decodes to the same
// PCM as from init (the reference starts there, spec 10.1), whatever the count (one past the fade included)
static int16_t lead_a[60 * 2 * C2_BLOCK_SAMPLES], lead_b[(60 + 12) * 2 * C2_BLOCK_SAMPLES];

static void test_lead_loss(void)
{
	static const unsigned leads[] = { 1, 3, C2_FADE_FRAMES / 2 + 1, 12 };

	for (int mi = 0; mi < 2; mi++) {
		c2_mode_t m = mi ? C2_MODE_1600 : C2_MODE_3200;
		unsigned spf = (mi ? 2u : 1u) * C2_BLOCK_SAMPLES;
		uint8_t lost[12];

		rng_state = 4242u + (uint32_t)mi;
		make_stream(m);
		c2_dec_init(&st, m);
		unsigned na = decode(m, syn_bits, NULL, 60, lead_a);
		memset(lost, 1, sizeof lost);
		for (unsigned li = 0; li < sizeof leads / sizeof leads[0]; li++) {
			unsigned n = leads[li];
			c2_dec_init(&st, m);
			decode(m, syn_bits, lost, n, lead_b);		// any bits: marked lost
			unsigned nb = decode(m, syn_bits, NULL, 60, lead_b + n * spf);
			bool quiet = true;
			for (unsigned i = 0; i < n * spf; i++)
				quiet &= lead_b[i] == 0;
			CHECK(quiet && nb == na && memcmp(lead_a, lead_b + n * spf, na * sizeof(int16_t)) == 0,
			      "%s: %u lost frames first: silence, then the stream as from init", mi ? "1600" : "3200", n);
		}
	}
}

// Every byte value in every byte, both modes: nothing out of range (and, built with SANITIZE=1, no undefined
// behavior); the parameters inside the spec's ranges
static void test_extremes(void)
{
	for (int mi = 0; mi < 2; mi++) {
		c2_mode_t m = mi ? C2_MODE_1600 : C2_MODE_3200;
		int nb = mi ? 2 : 1;
		int16_t pcm[C2_BLOCK_SAMPLES];
		unsigned bad = 0, peak = 0;

		c2_dec_init(&st, m);
		for (unsigned v = 0; v < 256 * 9; v++) {
			uint8_t bits[C2_FRAME_BYTES];
			for (int i = 0; i < C2_FRAME_BYTES; i++)
				bits[i] = v < 256 ? (uint8_t)v : (unsigned)i == v / 256 - 1 ? (uint8_t)v : 0;
			c2_dec_frame(&st, bits, false);
			for (int b = 0; b < nb; b++) {
				c2_dec_block(&st, &scr, pcm);
				for (uint8_t k = 0; k < 2; k++) {
					c2_params_t p;
					c2_dec_params(&st, k, &p, NULL);
					bad += p.L < 10 || p.L > C2_L_MAX || p.wo < c2_wo_dequant(0) || p.wo > c2_wo_dequant(127);
				}
				for (int i = 0; i < C2_BLOCK_SAMPLES; i++)
					peak = pcm[i] > (int)peak ? (unsigned)pcm[i] : peak;
			}
		}
		CHECK(bad == 0 && peak <= 30000, "%d: %u frames out of range, peak %u", mi, bad, peak);
	}
}

// --- The reference harness (optional) ---

// The numbers of "key":[...] (or a single number) in a JSON line; count read, or -1 without the key
static int json_nums(const char *line, const char *key, double *out, int max)
{
	char pat[48];
	snprintf(pat, sizeof pat, "\"%s\":", key);
	const char *p = strstr(line, pat);
	if (p == NULL)
		return -1;
	p += strlen(pat);
	if (*p != '[') {
		out[0] = strtod(p, NULL);
		return 1;
	}
	p++;
	int n = 0;
	while (n < max) {
		char *end;
		double v = strtod(p, &end);
		if (end == p)
			break;
		out[n++] = v;
		p = end;
		while (*p == ' ' || *p == ',')
			p++;
	}
	return n;
}

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
	*len = b != NULL && n > 0 ? fread(b, 1, (size_t)n, f) : 0;
	fclose(f);
	return b;
}

typedef struct {
	unsigned keys, frames, close, draws_same, gold;
	unsigned worst_draws_key_ok, worst_draws_key_n;
	char worst_draws_key[64];
} harness_t;

static void harness_key(const char *dir, const char *key, harness_t *h)
{
	char path[1024], name[256];
	c2_mode_t m = strstr(key, ".1600") != NULL ? C2_MODE_1600 : C2_MODE_3200;
	int nb = m == C2_MODE_1600 ? 2 : 1;
	snprintf(name, sizeof name, "%s", key);
	*strrchr(name, '.') = 0;

	size_t nbits;
	snprintf(path, sizeof path, "%s/ref/%s.bit", dir, key);
	uint8_t *bits = read_file(path, &nbits);
	if (bits == NULL) {
		snprintf(path, sizeof path, "%s/inputs/%s.bit", dir, name);
		bits = read_file(path, &nbits);
	}
	CHECK(bits != NULL && nbits % C2_FRAME_BYTES == 0, "%s: no bit stream", key);
	if (bits == NULL)
		return;
	snprintf(path, sizeof path, "%s/ref/%s.dec.jsonl", dir, key);
	FILE *f = fopen(path, "r");
	CHECK(f != NULL, "%s: no %s", key, path);
	if (f == NULL) {
		free(bits);
		return;
	}
	h->keys++;
	unsigned nframes = (unsigned)(nbits / C2_FRAME_BYTES), ok = 0, same = 0, n = 0;
	uint32_t crc = 0;
	char *line = NULL;
	size_t cap = 0;
	c2_dec_init(&st, m);
	for (unsigned i = 0; i < nframes; i++) {
		c2_dec_frame(&st, bits + i * C2_FRAME_BYTES, false);
		for (int b = 0; b < nb; b++) {
			int16_t pcm[C2_BLOCK_SAMPLES];
			c2_dec_block(&st, &scr, pcm);
			crc = crc32(crc, pcm, sizeof pcm);
			for (uint8_t k = 0; k < 2; k++) {
				c2_params_t p;
				uint16_t draws;
				c2_dec_params(&st, k, &p, &draws);
				// the next synth record
				double v = -1, wo = 0, L = 0, e = 0, lsp[C2_ORDER], rx = 0, rp = 0;
				int nl = 0;
				while (getline(&line, &cap, f) > 0) {
					if (strncmp(line, "{\"type\":\"synth\"", 15) != 0)
						continue;
					json_nums(line, "voiced", &v, 1);
					json_nums(line, "Wo", &wo, 1);
					json_nums(line, "L", &L, 1);
					json_nums(line, "e", &e, 1);
					nl = json_nums(line, "lsp", lsp, C2_ORDER);
					json_nums(line, "rand_calls_excitation", &rx, 1);
					json_nums(line, "rand_calls_postfilter", &rp, 1);
					break;
				}
				bool close = nl == C2_ORDER && (int)v == (p.voiced ? 1 : 0) && (int)L == p.L &&
					     fabs(p.wo / Q28 - wo) <= 1e-5 && fabs(p.e / 262144.0 - e) <= 1e-4 * fabs(e);
				for (int j = 0; j < C2_ORDER && close; j++)
					close = fabs(p.lsp[j] / Q28 - lsp[j]) <= 1e-4;
				if (!close && ok == n)
					printf("  %s 10 ms frame %u: voiced %d/%g L %u/%g ω0 %.9g/%.9g e %.9g/%.9g\n", key, n,
					       p.voiced, v, p.L, L, p.wo / Q28, wo, p.e / 262144.0, e);
				ok += close;
				same += draws == (unsigned)(rx + rp);
				n++;
			}
		}
	}
	free(line);
	fclose(f);
	free(bits);
	CHECK(ok == n && n == nframes * (unsigned)nb * 2, "%s: %u of %u frames' parameters within params_close", key,
	      ok, n);
	// Exact (spec 15.1), stricter than the fixed gate's 99%: one draw off desynchronizes every later random phase
	CHECK(same == n, "%s: random draws as the reference's on %u of %u frames", key, same, n);
	h->frames += n;
	h->close += ok;
	h->draws_same += same;
	if (h->worst_draws_key_n == 0 || same * h->worst_draws_key_n < h->worst_draws_key_ok * n) {
		h->worst_draws_key_ok = same;
		h->worst_draws_key_n = n;
		snprintf(h->worst_draws_key, sizeof h->worst_draws_key, "%s", key);
	}
	if (strcmp(key, GOLD_KEY_A) == 0 || strcmp(key, GOLD_KEY_B) == 0) {
		uint32_t gold = strcmp(key, GOLD_KEY_A) == 0 ? GOLD_KEY_A_CRC : GOLD_KEY_B_CRC;
		CHECK(crc == gold, "%s: PCM CRC %08x, golden %08x", key, crc, gold);
		printf("harness: %s PCM CRC %08x\n", key, crc);
		h->gold++;
	}
}

static int key_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void test_harness(const char *dir)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/ref", dir);
	DIR *d = opendir(path);
	CHECK(d != NULL, "harness: cannot open %s", path);
	if (d == NULL)
		return;
	char *keys[128];
	unsigned n = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && n < 128) {
		size_t l = strlen(de->d_name);
		if (l > 10 && strcmp(de->d_name + l - 10, ".dec.jsonl") == 0) {
			keys[n] = strdup(de->d_name);
			keys[n][l - 10] = 0;
			n++;
		}
	}
	closedir(d);
	qsort(keys, n, sizeof keys[0], key_cmp);
	harness_t h;
	memset(&h, 0, sizeof h);
	for (unsigned i = 0; i < n; i++) {
		harness_key(dir, keys[i], &h);
		free(keys[i]);
	}
	CHECK(h.keys == 60 && h.gold == 2, "harness: %u keys (60), %u golden keys (2)", h.keys, h.gold);
	printf("harness: %u keys, %u 10 ms frames: parameters within params_close on %u; random draws as the "
	       "reference's on %u (fewest: %s, %u of %u)\n", h.keys, h.frames, h.close, h.draws_same, h.worst_draws_key,
	       h.worst_draws_key_ok, h.worst_draws_key_n);
}

int main(int argc, char **argv)
{
	const char *harness = argc > 1 ? argv[1] : getenv("RFOUNDRY_C2_HARNESS");
	_Static_assert(sizeof(c2_dec_t) <= C2_DEC_STATE_BYTES, "state");
	_Static_assert(sizeof(c2_synth_scratch_t) <= C2_SCRATCH_BYTES, "scratch");
	printf("decoder state %u of %u bytes, scratch %u of %u\n", (unsigned)sizeof(c2_dec_t), C2_DEC_STATE_BYTES,
	       (unsigned)sizeof(c2_synth_scratch_t), C2_SCRATCH_BYTES);
	test_rng();
	test_helpers();
	test_api();
	test_conceal();
	test_lead_loss();
	test_extremes();
	test_golden();
	if (harness != NULL && harness[0] != 0)
		test_harness(harness);
	else
		printf("harness: skipped (RFOUNDRY_C2_HARNESS=<dir with HARNESS.md, ref/, inputs/> runs it)\n");
	printf("c2DecTest (%s): %d checks, %d failed\n",
#if defined(FXP_SPLIT16)
	       "products from 16-bit halves",
#else
	       "64-bit products",
#endif
	       checks, failures);
	return failures != 0;
}
