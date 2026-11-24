// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codec 2 encoder (core/codec2/c2_enc.c, c2_nlp.c, c2_lpc.c, c2_quant.c, c2_enc_tab.c) against
// docs/spec/codec2.md 5-9:
//   - the encoder's tables against the spec's formulas (windows, window spectrum, expansion factors) and its listed
//     taps
//   - golden CRCs of the bits for two inputs made here, both modes: a 200 Hz pulse train (the harness's pulses_200,
//     sample for sample) and a synthetic utterance (glides, noise, silence, level changes, clipping)
//   - init starts over (after any input, in either mode); 1600's first block of a frame gives no bits and leaves the
//     buffer alone; the scratch carries nothing between calls; digital silence gives the spec's silence frames
//   - hostile input (full scale, alternating extremes, random) gives frames that unpack and pack back
//   - near 4 kHz (spec 18.3, the recursion's stop): alternating full scale, a 3400 Hz tone and 3300-3950 Hz
//     multi-sine noise send an energy index above 0 on every frame, the full-scale one the reference's 23, and the
//     multi-sine decodes (this firmware's decoder) within 7 dB of its level, where the encoder before the fix sent 0
//     on a quarter of its frames and decoded 9.2 dB low
//   - two steady lines (spec 18.3, the line stop): two_tone (the harness's, sample for sample) and two DTMF pairs
//     decode (this firmware's decoder) no more than 3 dB under the encoder before the fix and within 5 dB under the
//     input, both modes, where the margin alone put two_tone 5.8 dB under the encoder before the fix
// With the reference harness (the directory holding HARNESS.md, ref/ and inputs/; kept outside the repository, spec
// 1): RFOUNDRY_C2_HARNESS=<dir> or the first argument. Then every PCM key is encoded and each stage compared with the
// reference encoder's dump (enc.jsonl), at the rates the harness's fixed gate asks of the encoder on speech-like keys:
// voicing exact >= 97 %, ω0 index within one >= 95 %, energy index within one >= 97 %, LSP indexes within one >= 95 %;
// the pitch estimator's bin within one >= 95 % (the gate has no pitch figure: ω0's). Golden CRCs of the bits on
// vowel_a_110 and utterance, both modes. Without the harness these checks are skipped with a notice.
// With the codec2-extra vectors (deep spectral nulls, near-Nyquist content, fricatives: the directory holding
// README.md, inputs/ and ref/, kept outside the repository like the harness): RFOUNDRY_C2_EXTRA=<dir> or the second
// argument. Then a set of its keys is encoded and its energy and LSP indexes compared with the reference encoder's
// bits, at the agreement each key reaches (18.5), and no frame may send energy index 0 where the reference sends 5 or
// more.
//
// The Makefile builds this twice, plain and with -DFXP_SPLIT16 (the 16-bit halves a C-SKY build multiplies with): the
// golden CRCs are the same constants in both, so the two give the same bits.
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec2/c2_enc_int.h"
#include "codec2/codec2.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; if (failures < 40) { printf("FAIL %s:%d: ", __FILE__, \
	__LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

#define PI_C	3.141592654	// spec 3.2

// Golden CRC-32 of the encoder's bits (every frame's 8 bytes in order). A change meant to change the encoder's output
// changes these: write the new ones down with the reason, after the harness's gate passes (tests/host/c2EncCand.c)
#define GOLD_PULSES_3200	0x0A77208Bu
#define GOLD_PULSES_1600	0x97C7B641u
#define GOLD_SPEECH_3200	0x66A4B0C9u
#define GOLD_SPEECH_1600	0x56778B11u
// On the harness's inputs
#define GOLD_VOWEL_3200		0x0D7D30B7u
#define GOLD_VOWEL_1600		0x84EA077Eu
// utterance.3200 moved with the Schur recursion (spec 18.3; 7305912F before): its reflection coefficients are exact to
// a few 2^-30 R[0] where Levinson's carried the Q22 predictor's rounding, and on two frames (72, 78) that moves LSP 8
// to 10 by one index each way (to and from the reference's); the mean LSP error against the reference's dump is 7.32e-4
// rad (7.37e-4 before). No other golden moved
#define GOLD_UTT_3200		0x7F08D860u
#define GOLD_UTT_1600		0xB2310AC4u

static uint32_t rng_state = 0x2545F491u;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n)
{
	crc = ~crc;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static c2_enc_state_t st;
static c2_scratch_t scr;

// Encode n samples (whole blocks) from a fresh state: the frames' bytes to out (when given), their count, their CRC
static unsigned encode(c2_mode_t m, const int16_t *pcm, size_t n, uint8_t *out, uint32_t *crc)
{
	unsigned frames = 0;
	uint32_t c = 0;

	c2_enc_init(&st, m);
	for (size_t i = 0; i + C2_BLOCK_SAMPLES <= n; i += C2_BLOCK_SAMPLES) {
		uint8_t bits[C2_FRAME_BYTES];

		if (c2_enc_block(&st, &scr, pcm + i, bits)) {
			c = crc32(c, bits, C2_FRAME_BYTES);
			if (out != NULL)
				memcpy(out + C2_FRAME_BYTES * frames, bits, C2_FRAME_BYTES);
			frames++;
		}
	}
	if (crc != NULL)
		*crc = c;
	return frames;
}

// --- Inputs made here ---

#define FS	8000

// The harness's pulses_200: -6 dBFS impulses every 40 samples, 2 s (round(0.501187 * 32767) = 16422)
static void make_pulses(int16_t *x, size_t n)
{
	for (size_t i = 0; i < n; i++)
		x[i] = i % 40 == 0 ? 16422 : 0;
}

// A synthetic utterance in integers (so the same on every host): a pulse train through two resonators with the pitch
// gliding 70-300 Hz and jumping, noise bursts, a quiet and a loud (clipping) stretch, silence. 4 s
static void make_speech(int16_t *x, size_t n)
{
	int32_t y1a = 0, y2a = 0, y1b = 0, y2b = 0;
	uint32_t ph = 0;

	rng_state = 0x1234567u;
	for (size_t t = 0; t < n; t++) {
		uint32_t seg = (uint32_t)t / 2000u, k = seg % 7u;	// a quarter second each
		uint32_t period = 27u + ((uint32_t)t / 61u) % 87u;
		int32_t gain = k == 4 ? 12 : k == 5 ? 0 : k == 6 ? 1 : 2 + (int32_t)(seg % 3u);
		int32_t v;

		ph++;
		if (k == 3)
			v = (int32_t)(rng() >> 21) - 1024;
		else
			v = ph % period == 0 ? 6000 : 0;
		int32_t ya = v + ((2 * 13722 * y1a) >> 14) - ((15550 * y2a) >> 14);
		y2a = y1a;
		y1a = ya / 2;
		int32_t yb = ya / 4 + ((2 * 11019 * y1b) >> 14) - ((15550 * y2b) >> 14);
		y2b = y1b;
		y1b = yb / 2;
		yb *= gain;
		if (k == 6)
			yb /= 64;	// quiet: tens of LSB
		x[t] = (int16_t)(yb > 32767 ? 32767 : yb < -32768 ? -32768 : yb);
	}
}

// --- Tables (spec 5.2, 5.4, 6.4, 8.4, 14.3) ---

static void test_tables(void)
{
	// 5.2: the Hann window over n = 21..298, then scaled so the sum of squares is 1/512
	double w[320], s2 = 0, sum = 0;
	for (int n = 0; n < 320; n++) {
		w[n] = n >= 21 && n <= 298 ? 0.5 - 0.5 * cos(2 * PI_C * (n - 21) / 278) : 0;
		s2 += w[n] * w[n];
	}
	int worst = 0;
	for (int n = 0; n < 320; n++) {
		w[n] /= sqrt(512 * s2);
		sum += w[n];
	}
	for (int d = 0; d < C2E_WIN_HALF; d++) {
		int e = abs((int)lrint(w[160 + d] * 8388608.0) - c2e_win[d]);
		int e2 = abs((int)lrint(w[160 - d] * 8388608.0) - c2e_win[d]);

		worst = e > worst ? e : e2 > worst ? e2 : worst;
	}
	// The table is the float32 values (cosine in single precision, spec 5.2) rounded: within a unit of the double
	// formula
	CHECK(worst <= 1, "analysis window: %d units from the formula", worst);
	CHECK(fabs(sum - 0.601647003) < 1e-6, "analysis window sum %.9f (tables.json 0.601647003)", sum);
	CHECK(w[21] == 0 && w[160 + C2E_WIN_HALF] == 0, "window ends");

	// 5.4: W(256 + d), the real DFT of the window centered at index 0
	worst = 0;
	for (int d = 0; d < C2E_WSPEC_N; d++) {
		double re = 0;
		for (int k = -138; k <= 138; k++)
			re += w[160 + k] * cos(2 * PI_C * k * d / 512);
		int e = abs((int)lrint(re * 32768) - c2e_wspec[d]);
		worst = e > worst ? e : worst;
	}
	CHECK(worst <= 1, "window spectrum: %d units from the DFT", worst);

	// 6.4: g[i] = 0.5 - 0.5 cos(2 pi_c i / 63)
	worst = 0;
	for (int i = 0; i < C2E_NLP_DEC / 2; i++) {
		int e = abs((int)lrint((0.5 - 0.5 * cos(2 * PI_C * i / 63)) * 32768) - c2e_nlp_win[i]);
		worst = e > worst ? e : worst;
	}
	CHECK(worst == 0, "pitch estimator window: %d units from the formula", worst);

	// 8.4: c_i = f32(g^i), g = f32(0.994), exactly as Q30
	float g = 0.994f;
	int same = 0;
	for (int i = 0; i <= C2_ORDER; i++)
		same += (double)(float)pow((double)g, i) * 1073741824.0 == (double)c2e_bw[i];
	CHECK(same == C2_ORDER + 1, "bandwidth expansion: %d of 11 factors exact", same);

	// 14.3: the low-pass taps as the spec lists them (8 digits), and their sum (tables.json 1.00000001)
	static const double h[24] = {
		-1.0818124e-03, -1.1008344e-03, -9.2768838e-04, -4.2289438e-04, 5.5034190e-04, 2.0029849e-03,
		3.7058509e-03, 5.1449415e-03, 5.5924666e-03, 4.3036754e-03, 8.0284511e-04, -4.8204610e-03,
		-1.1705810e-02, -1.8199275e-02, -2.2065282e-02, -2.0920610e-02, -1.2808831e-02, 3.2204775e-03,
		2.6683811e-02, 5.5520624e-02, 8.6305944e-02, 1.1480192e-01, 1.3674206e-01, 1.4867556e-01,
	};
	double hs = 0, he = 0;
	for (int j = 0; j < 24; j++) {
		double e = fabs(c2e_fir[j] / 2147483648.0 - h[j]);
		he = e > he ? e : he;
		hs += 2 * c2e_fir[j] / 2147483648.0;
	}
	CHECK(he < 1e-8, "low-pass taps: %.3g from spec 14.3's eight digits", he);
	CHECK(fabs(hs - 1.00000001) < 1e-8, "low-pass sum %.10f", hs);
	printf("tables: analysis window, window spectrum, pitch estimator window and low-pass, expansion factors as "
	       "the spec gives them\n");
}

// --- Behavior ---

// Spec 7.2's harmonic count as the reference forms it: the double quotient rounded to float32 and floored, less one
// when f32(ω0 L) >= 0.95 pi_c. ω0 in Q28 is exact in double, and so is ω0 L
static unsigned spec_L(int32_t wo)
{
	double w = wo / 268435456.0;
	unsigned L = (unsigned)floorf((float)(PI_C / w));

	if ((double)(float)(w * L) >= 0.95 * PI_C)
		L--;
	return L;
}

// Every analysis frame's L is spec_L of its ω0, its ω0 within [ω0_min, ω0_max] and its pitch bin within 16..128, on
// the synthetic utterance in both modes (the harness's keys check the same frame by frame)
static void test_harmonics(void)
{
	static int16_t x[4 * FS];
	unsigned bad = 0, n = 0;

	make_speech(x, 4 * FS);
	for (int m = 0; m < 2; m++) {
		c2_enc_init(&st, m ? C2_MODE_1600 : C2_MODE_3200);
		for (size_t i = 0; i + C2_BLOCK_SAMPLES <= 4 * FS; i += C2_BLOCK_SAMPLES) {
			uint8_t bits[C2_FRAME_BYTES];
			c2e_trace_t t;

			c2_enc_block(&st, &scr, x + i, bits);
			c2_enc_trace(&st, &t);
			for (int k = 0; k < 2; k++, n++)
				bad += t.f[k].L != spec_L(t.f[k].wo) || t.f[k].wo < 10541436 || t.f[k].wo > 84331488 ||
				       t.f[k].k_c < C2E_NLP_LO || t.f[k].k_c > C2E_NLP_HI;	// ω0_min, ω0_max in Q28
		}
	}
	CHECK(bad == 0, "harmonic count, ω0 or pitch bin out of the spec on %u of %u frames", bad, n);

	// And c2e_harmonics on every Q28 ω0 from ω0_min to ω0_max (the 5% rule compares the float32 product: exact
	// arithmetic gave another L on 17 of them)
	unsigned lbad = 0, first = 0;
	for (int32_t wo = 10541436; wo <= 84331488; wo++) {
		if (c2e_harmonics(wo) != spec_L(wo) && lbad++ == 0)
			first = (unsigned)wo;
	}
	CHECK(lbad == 0, "c2e_harmonics differs from spec 7.2 on %u Q28 values (first %u)", lbad, first);
	printf("harmonics: L as spec 7.2 forms it on every Q28 ω0 and on %u frames, ω0 and the pitch bin in range\n", n);
}

static void test_golden(void)
{
	static int16_t x[4 * FS];
	uint32_t c;
	unsigned f;

	make_pulses(x, 2 * FS);
	f = encode(C2_MODE_3200, x, 2 * FS, NULL, &c);
	CHECK(f == 100 && c == GOLD_PULSES_3200, "pulses 3200: %u frames, CRC %08X (golden %08X)", f, c,
	      GOLD_PULSES_3200);
	f = encode(C2_MODE_1600, x, 2 * FS, NULL, &c);
	CHECK(f == 50 && c == GOLD_PULSES_1600, "pulses 1600: %u frames, CRC %08X (golden %08X)", f, c,
	      GOLD_PULSES_1600);
	make_speech(x, 4 * FS);
	f = encode(C2_MODE_3200, x, 4 * FS, NULL, &c);
	CHECK(f == 200 && c == GOLD_SPEECH_3200, "speech 3200: %u frames, CRC %08X (golden %08X)", f, c,
	      GOLD_SPEECH_3200);
	f = encode(C2_MODE_1600, x, 4 * FS, NULL, &c);
	CHECK(f == 100 && c == GOLD_SPEECH_1600, "speech 1600: %u frames, CRC %08X (golden %08X)", f, c,
	      GOLD_SPEECH_1600);
	printf("golden: pulses and synthetic speech, both modes (%s)\n",
#if defined(FXP_SPLIT16)
	       "products from 16-bit halves"
#else
	       "64-bit products"
#endif
	);
}

static void test_init_and_halves(void)
{
	static int16_t x[4 * FS];
	static uint8_t ref[200 * C2_FRAME_BYTES], got[200 * C2_FRAME_BYTES];
	uint8_t bits[C2_FRAME_BYTES];
	c2e_trace_t t;

	make_speech(x, 4 * FS);
	c2_enc_init(&st, C2_MODE_3200);
	CHECK(!c2_enc_trace(&st, &t), "no trace before the first block");

	for (int m = 0; m < 2; m++) {
		c2_mode_t mode = m ? C2_MODE_1600 : C2_MODE_3200;
		unsigned n = encode(mode, x + 8000, 2 * FS, ref, NULL);

		// After any input, in the other mode and with an odd number of blocks, init starts over exactly
		c2_enc_init(&st, m ? C2_MODE_3200 : C2_MODE_1600);
		for (int b = 0; b < 37; b++)
			c2_enc_block(&st, &scr, x + 160 * b, bits);
		c2_enc_init(&st, mode);
		unsigned k = 0;
		bool ok = true;
		for (size_t i = 0; i + C2_BLOCK_SAMPLES <= 2 * FS; i += C2_BLOCK_SAMPLES) {
			memset(bits, 0xA5, sizeof bits);
			// The scratch is undefined between calls: fill it with garbage first
			for (unsigned w = 0; w < C2_SCRATCH_BYTES / 4; w++)
				scr.w[w] = (int32_t)rng();
			bool done = c2_enc_block(&st, &scr, x + 8000 + i, bits);
			unsigned blk = (unsigned)(i / C2_BLOCK_SAMPLES);

			if (mode == C2_MODE_1600) {
				ok &= done == (blk % 2 == 1);
				if (!done) {
					uint8_t a5[C2_FRAME_BYTES];
					memset(a5, 0xA5, sizeof a5);
					ok &= memcmp(bits, a5, sizeof a5) == 0;
				}
			} else {
				ok &= done;
			}
			if (done)
				memcpy(got + C2_FRAME_BYTES * k++, bits, C2_FRAME_BYTES);
		}
		CHECK(ok, "%s: frames on the right blocks, bits untouched otherwise", m ? "1600" : "3200");
		CHECK(k == n && memcmp(got, ref, C2_FRAME_BYTES * n) == 0,
		      "%s: init after other input (and garbage scratch) gives a fresh encoder's %u frames (%u)",
		      m ? "1600" : "3200", n, k);
		CHECK(c2_enc_trace(&st, &t) && t.lsps == true, "trace after a frame's last block");
	}
	// A mode value outside the enum works as 3200
	c2_enc_init(&st, (c2_mode_t)7);
	CHECK(c2_enc_block(&st, &scr, x, bits), "an unknown mode encodes 3200");
	printf("init and halves: init starts over after either mode; 1600 completes a frame every second block, the "
	       "first leaving the bits alone; scratch carries nothing\n");
}

// Spec 16.3: the reference encoder sends the silence frame for digital silence from its third frame (3200) or its
// second (1600)
static void test_silence(void)
{
	static int16_t z[FS];
	static uint8_t out[100 * C2_FRAME_BYTES];
	unsigned n = encode(C2_MODE_3200, z, FS, out, NULL), same = 0;

	for (unsigned f = 2; f < n; f++)
		same += memcmp(out + C2_FRAME_BYTES * f, c2_silence_3200, C2_FRAME_BYTES) == 0;
	CHECK(same == n - 2, "3200 silence: %u of %u frames from the third are c2_silence_3200", same, n - 2);
	n = encode(C2_MODE_1600, z, FS, out, NULL);
	unsigned same16 = 0;
	for (unsigned f = 1; f < n; f++)
		same16 += memcmp(out + C2_FRAME_BYTES * f, c2_silence_1600, C2_FRAME_BYTES) == 0;
	CHECK(same16 == n - 1, "1600 silence: %u of %u frames from the second are c2_silence_1600", same16, n - 1);
	printf("silence: the spec's silence frames once started (%u + %u frames)\n", same, same16);
}

// Full scale, alternating extremes, -32768 held, random: every frame unpacks and packs back to itself (any 64 bits
// do: the check is that the encoder ran to the end of each, which the sanitizer build watches)
static void test_hostile(void)
{
	static int16_t x[FS];
	static uint8_t out[50 * C2_FRAME_BYTES];
	unsigned bad = 0, frames = 0;

	for (int kind = 0; kind < 5; kind++) {
		for (int i = 0; i < FS; i++) {
			switch (kind) {
			case 0: x[i] = (i / 27) & 1 ? 32767 : -32768; break;
			case 1: x[i] = i & 1 ? 32767 : -32768; break;
			case 2: x[i] = -32768; break;
			case 3: x[i] = (int16_t)rng(); break;
			default: x[i] = (int16_t)(i < FS / 2 ? 0 : (i & 1) ? 1 : -1); break;
			}
		}
		for (int m = 0; m < 2; m++) {
			c2_mode_t mode = m ? C2_MODE_1600 : C2_MODE_3200;
			unsigned n = encode(mode, x, FS, out, NULL);

			for (unsigned f = 0; f < n; f++) {
				c2_index_t ix;
				uint8_t back[C2_FRAME_BYTES];

				c2_unpack(mode, out + C2_FRAME_BYTES * f, &ix);
				c2_pack(mode, &ix, back);
				bad += memcmp(back, out + C2_FRAME_BYTES * f, C2_FRAME_BYTES) != 0;
			}
			frames += n;
		}
	}
	CHECK(bad == 0, "hostile input: %u of %u frames do not round trip", bad, frames);
	printf("hostile input: full scale, alternating extremes, -32768, random, +-1: %u frames\n", frames);
}

// --- Near 4 kHz (spec 18.3) ---

// This firmware's decoder (c2_dec.c): n frames to PCM from a fresh state, the rms over samples [skip, end)
static double decode_rms(c2_mode_t m, const uint8_t *bits, unsigned n, size_t skip)
{
	static c2_dec_state_t ds;
	double sum = 0;
	size_t t = 0;

	c2_dec_init(&ds, m);
	for (unsigned f = 0; f < n; f++) {
		int16_t pcm[C2_BLOCK_SAMPLES];

		c2_dec_frame(&ds, bits + C2_FRAME_BYTES * f, false);
		while (c2_dec_block(&ds, &scr, pcm))
			for (int i = 0; i < C2_BLOCK_SAMPLES; i++, t++)
				if (t >= skip)
					sum += (double)pcm[i] * pcm[i];
	}
	return t > skip ? sqrt(sum / (double)(t - skip)) : 0;
}

static double rms(const int16_t *x, size_t from, size_t to)
{
	double sum = 0;

	for (size_t i = from; i < to; i++)
		sum += (double)x[i] * x[i];
	return to > from ? sqrt(sum / (double)(to - from)) : 0;
}

// codec2-extra's tone_3400_pk_m12, sample for sample: trunc(32767 10^(-12/20) sin(2 pi 3400 n / 8000))
static void make_tone3400(int16_t *x, size_t n)
{
	for (size_t i = 0; i < n; i++)
		x[i] = (int16_t)(32767.0 * pow(10.0, -12.0 / 20) * sin(2 * 3.14159265358979323846 * 3400.0 * (double)i / FS));
}

// codec2-extra's alt_fullscale: -32768 on even n, 32767 on odd (full-scale 4 kHz)
static void make_alt(int16_t *x, size_t n)
{
	for (size_t i = 0; i < n; i++)
		x[i] = i & 1 ? 32767 : -32768;
}

// 64 sines spread evenly over 3300-3950 Hz with random phases, rms -12 dBFS: band noise whose spectrum outside the band
// is only the int16 rounding (and the analysis window's sidelobes), nulls as deep as codec2-extra's brickwall noise
static void make_multisine(int16_t *x, size_t n)
{
	static double y[2 * FS];
	double ph[64], sum = 0;

	rng_state = 0x5EEDu;
	for (int k = 0; k < 64; k++)
		ph[k] = 2 * 3.14159265358979323846 * (rng() >> 8) / 16777216.0;
	for (size_t i = 0; i < n; i++) {
		y[i] = 0;
		for (int k = 0; k < 64; k++)
			y[i] += sin(2 * 3.14159265358979323846 * (3300.0 + 650.0 * (k + 0.5) / 64) * (double)i / FS + ph[k]);
		sum += y[i] * y[i];
	}
	double g = 32767.0 * pow(10.0, -12.0 / 20) / sqrt(sum / (double)n);
	// The peak stays under full scale (3.8 times the rms with this seed: 30,980); held there all the same
	for (size_t i = 0; i < n; i++)
		x[i] = (int16_t)fmax(-32768.0, fmin(32767.0, round(g * y[i])));
}

static void test_nyquist(void)
{
	static int16_t x[2 * FS];
	static uint8_t out[100 * C2_FRAME_BYTES];
	static const struct {
		const char *name;
		void (*make)(int16_t *, size_t);
		int e_ref, e_tol;	// the reference's energy index on every field from the second (codec2-extra's
					// copy of the input), and how far from it this one may be; -1: not known
	} in[] = {
		{ "alternating full scale", make_alt, 23, 0 },
		// The reference's 7 is its round-off: its float32 recursion's error at order 3 is 0.75 of the exact one
		{ "3400 Hz tone", make_tone3400, 7, 1 },
		{ "3300-3950 Hz multi-sine", make_multisine, -1, 0 },
	};

	for (unsigned k = 0; k < sizeof in / sizeof in[0]; k++) {
		in[k].make(x, 2 * FS);
		for (int m = 0; m < 2; m++) {
			c2_mode_t mode = m ? C2_MODE_1600 : C2_MODE_3200;
			unsigned n = encode(mode, x, 2 * FS, out, NULL), zero = 0, off = 0, fields = 0;
			int e_min = 31;

			// From the second energy field on: the first sees the buffer's start (1.0, then the input)
			for (unsigned f = 0; f < n; f++) {
				c2_index_t ix;

				c2_unpack(mode, out + C2_FRAME_BYTES * f, &ix);
				for (int h = 0; h < (m ? 2 : 1); h++) {
					if (f == 0 && h == 0)
						continue;
					fields++;
					zero += ix.e[h] == 0;
					off += in[k].e_ref >= 0 && abs(ix.e[h] - in[k].e_ref) > in[k].e_tol;
					if (ix.e[h] < e_min)
						e_min = ix.e[h];
				}
			}
			CHECK(zero == 0, "%s %s: energy index 0 on %u of %u fields", in[k].name, m ? "1600" : "3200", zero,
			      fields);
			CHECK(off == 0, "%s %s: energy index more than %d from the reference's %d on %u of %u fields",
			      in[k].name, m ? "1600" : "3200", in[k].e_tol, in[k].e_ref, off, fields);
			// The multi-sine is inside the decoder's harmonics (under 0.95 of 4 kHz in 3200's full band): its
			// level must come through. The reference's own decode of brickwall 3300-3950 Hz noise is 6.4 dB low
			// (codec2-extra), this encoder's was 9.2 dB low on this input before the fix and is 4.9 dB low now
			if (make_multisine == in[k].make && mode == C2_MODE_3200) {
				double dl = 20 * log10(decode_rms(mode, out, n, 640) / rms(x, 640, (size_t)n * 160));
				CHECK(dl > -7.0 && dl < 3.0, "%s 3200: decoded level %+.1f dB against the input (-7 to +3)",
				      in[k].name, dl);
				printf("near 4 kHz: %s 3200 decodes %+.1f dB against the input\n", in[k].name, dl);
			}
			printf("near 4 kHz: %s %s: energy index %d or more on all %u fields\n", in[k].name,
			       m ? "1600" : "3200", e_min, fields);
		}
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
	while (*p == ' ')
		p++;
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

static int16_t *read_pcm(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	int16_t *p = malloc(len > 0 ? (size_t)len : 2);
	*n = p != NULL && len > 0 ? fread(p, 2, (size_t)len / 2, f) : 0;
	fclose(f);
	return p;
}

// Agreement counts of one stage: equal, within one, of
typedef struct {
	unsigned eq, near, n;
} agree_t;

static void agree(agree_t *a, int ref, int got)
{
	a->eq += ref == got;
	a->near += abs(ref - got) <= 1;
	a->n++;
}

static double rate(const agree_t *a, bool near)
{
	return a->n ? (double)(near ? a->near : a->eq) / a->n : 1.0;
}

// compare.py's key classes: tones, DC and sweeps are "tonal" (LPC round-off decided), silence its own
static bool speech_like(const char *name)
{
	static const char *const tonal[] = { "tone_", "two_tone", "dc_offset", "full_scale_tone", "chirp_", "silence" };

	for (unsigned i = 0; i < sizeof tonal / sizeof tonal[0]; i++)
		if (strncmp(name, tonal[i], strlen(tonal[i])) == 0)
			return false;
	return true;
}

typedef struct {
	unsigned keys, speech_keys, frames;
	agree_t all_v, all_wo, all_e, all_lsp, all_k;
} harness_t;

static void harness_key(const char *dir, const char *key, harness_t *h)
{
	char path[1024], name[256];
	c2_mode_t m = strstr(key, ".1600") != NULL ? C2_MODE_1600 : C2_MODE_3200;

	snprintf(name, sizeof name, "%s", key);
	*strrchr(name, '.') = 0;
	snprintf(path, sizeof path, "%s/inputs/%s.raw", dir, name);
	size_t n;
	int16_t *pcm = read_pcm(path, &n);
	CHECK(pcm != NULL, "harness: no input %s", path);
	if (pcm == NULL)
		return;

	// This encoder, frame by frame, with each block's trace (pitch bin and voicing per 10 ms)
	size_t nb = n / C2_BLOCK_SAMPLES;
	uint8_t *bits = malloc(nb * C2_FRAME_BYTES + 8);
	uint8_t *kc = malloc(2 * nb + 2), *vc = malloc(2 * nb + 2);
	unsigned frames = 0, lbad = 0;
	uint32_t crc = 0;
	c2_enc_init(&st, m);
	for (size_t b = 0; b < nb; b++) {
		c2e_trace_t t;

		if (c2_enc_block(&st, &scr, pcm + b * C2_BLOCK_SAMPLES, bits + C2_FRAME_BYTES * frames)) {
			crc = crc32(crc, bits + C2_FRAME_BYTES * frames, C2_FRAME_BYTES);
			frames++;
		}
		c2_enc_trace(&st, &t);
		for (int k = 0; k < 2; k++) {
			kc[2 * b + k] = t.f[k].k_c;
			vc[2 * b + k] = t.f[k].voiced;
			lbad += t.f[k].L != spec_L(t.f[k].wo);
		}
	}

	// The reference's dump: analysis records in order (k_c, voiced), frame records (indexes)
	snprintf(path, sizeof path, "%s/ref/%s.enc.jsonl", dir, key);
	FILE *f = fopen(path, "r");
	CHECK(f != NULL, "harness: no %s", path);
	agree_t v = { 0 }, wo = { 0 }, e = { 0 }, lsp = { 0 }, kp = { 0 };
	unsigned ai = 0, fi = 0;
	static char line[1 << 16];
	while (f != NULL && fgets(line, sizeof line, f) != NULL) {
		double d[C2_ORDER];
		if (strstr(line, "\"type\":\"analysis\"") != NULL) {
			if (ai < 2 * nb) {
				json_nums(line, "k_c", d, 1);
				agree(&kp, (int)d[0], kc[ai]);
				json_nums(line, "voiced", d, 1);
				agree(&v, (int)d[0], vc[ai]);
			}
			ai++;
		} else if (strstr(line, "\"type\":\"frame\"") != NULL) {
			if (fi < frames) {
				c2_index_t ix;
				c2_unpack(m, bits + C2_FRAME_BYTES * fi, &ix);
				int nw = json_nums(line, "Wo_index", d, 2);
				for (int k = 0; k < nw; k++)
					agree(&wo, (int)d[k], ix.wo[k]);
				int ne = json_nums(line, "E_index", d, 2);
				for (int k = 0; k < ne; k++)
					agree(&e, (int)d[k], ix.e[k]);
				int nl = json_nums(line, "lsp_index", d, C2_ORDER);
				for (int k = 0; k < nl; k++)
					agree(&lsp, (int)d[k], ix.lsp[k]);
			}
			fi++;
		}
	}
	if (f != NULL)
		fclose(f);
	CHECK(lbad == 0, "%s: L is not spec 7.2's for its ω0 on %u frames", key, lbad);
	CHECK(fi == frames && ai == 2 * nb, "%s: %u frames, %zu analyses (reference %u, %u)", key, frames, 2 * nb, fi,
	      ai);

	h->keys++;
	h->frames += frames;
	if (speech_like(name)) {
		h->speech_keys++;
		CHECK(rate(&v, false) >= 0.97, "%s: voicing %.1f %% (>= 97)", key, 100 * rate(&v, false));
		CHECK(rate(&wo, true) >= 0.95, "%s: ω0 index within one %.1f %% (>= 95)", key, 100 * rate(&wo, true));
		CHECK(rate(&e, true) >= 0.97, "%s: energy index within one %.1f %% (>= 97)", key, 100 * rate(&e, true));
		CHECK(rate(&lsp, true) >= 0.95, "%s: LSP indexes within one %.1f %% (>= 95)", key, 100 * rate(&lsp, true));
		CHECK(rate(&kp, true) >= 0.95, "%s: pitch bin within one %.1f %% (>= 95)", key, 100 * rate(&kp, true));
	}
	printf("  %-22s %4u frames  k_c %5.1f %%  v %5.1f %%  ω0 %5.1f/%5.1f %%  E %5.1f/%5.1f %%  LSP %5.1f/%5.1f %%%s\n",
	       key, frames, 100 * rate(&kp, false), 100 * rate(&v, false), 100 * rate(&wo, false), 100 * rate(&wo, true),
	       100 * rate(&e, false), 100 * rate(&e, true), 100 * rate(&lsp, false), 100 * rate(&lsp, true),
	       speech_like(name) ? "" : "  (not gated)");
	// The chirps reach orders whose k is rejected and then later orders that take theirs: 8.2 sets k to 0 and goes on
	// (spec 18.3). Energy within one 49.0 and 38.7 % here, 43.0 and 33.3 % with a recursion that stopped at the first
	// rejected order
	static const struct {
		const char *name;
		uint8_t e1;
	} chirp[] = { { "chirp_up", 47 }, { "chirp_down_loud", 37 } };
	for (unsigned i = 0; i < sizeof chirp / sizeof chirp[0]; i++)
		if (strcmp(name, chirp[i].name) == 0)
			CHECK(100 * rate(&e, true) >= chirp[i].e1, "%s: energy index within one %.1f %% (>= %u)", key,
			      100 * rate(&e, true), chirp[i].e1);
	agree_t *sums[5][2] = { { &h->all_v, &v }, { &h->all_wo, &wo }, { &h->all_e, &e }, { &h->all_lsp, &lsp },
				{ &h->all_k, &kp } };
	for (int i = 0; i < 5; i++) {
		sums[i][0]->eq += sums[i][1]->eq;
		sums[i][0]->near += sums[i][1]->near;
		sums[i][0]->n += sums[i][1]->n;
	}

	static const struct {
		const char *key;
		uint32_t crc;
	} gold[] = {
		{ "vowel_a_110.3200", GOLD_VOWEL_3200 }, { "vowel_a_110.1600", GOLD_VOWEL_1600 },
		{ "utterance.3200", GOLD_UTT_3200 }, { "utterance.1600", GOLD_UTT_1600 },
	};
	for (unsigned i = 0; i < sizeof gold / sizeof gold[0]; i++)
		if (strcmp(key, gold[i].key) == 0)
			CHECK(crc == gold[i].crc, "%s: CRC %08X (golden %08X)", key, crc, gold[i].crc);
	// The harness's pulses_200 is make_pulses's, sample for sample (so the CI golden is on a harness input)
	if (strcmp(name, "pulses_200") == 0) {
		static int16_t mine[2 * FS];
		make_pulses(mine, 2 * FS);
		CHECK(n == 2 * FS && memcmp(mine, pcm, sizeof mine) == 0, "pulses_200 is the input made here");
	}
	free(bits);
	free(kc);
	free(vc);
	free(pcm);
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
		if (l > 10 && strcmp(de->d_name + l - 10, ".enc.jsonl") == 0) {
			keys[n] = strdup(de->d_name);
			keys[n][l - 10] = 0;
			n++;
		}
	}
	closedir(d);
	qsort(keys, n, sizeof keys[0], key_cmp);
	harness_t h;
	memset(&h, 0, sizeof h);
	printf("harness: per key, exact (and within one) against the reference encoder's dump:\n");
	for (unsigned i = 0; i < n; i++) {
		harness_key(dir, keys[i], &h);
		free(keys[i]);
	}
	CHECK(h.keys == 56 && h.speech_keys == 36, "harness: %u PCM keys (56), %u speech-like (36)", h.keys,
	      h.speech_keys);
	printf("harness: %u keys, %u frames; all keys: k_c %.1f %%, voicing %.1f %%, ω0 %.1f %% (%.1f within one), "
	       "energy %.1f %% (%.1f), LSPs %.1f %% (%.1f)\n", h.keys, h.frames, 100 * rate(&h.all_k, false),
	       100 * rate(&h.all_v, false), 100 * rate(&h.all_wo, false), 100 * rate(&h.all_wo, true),
	       100 * rate(&h.all_e, false), 100 * rate(&h.all_e, true), 100 * rate(&h.all_lsp, false),
	       100 * rate(&h.all_lsp, true));
}

// --- codec2-extra (optional): deep spectral nulls, near-Nyquist content, fricatives ---

// Per key, the lowest agreement with the reference's indexes this encoder must keep: energy within one (one analysis
// feeds both modes' fields), LSP indexes within one per mode, in percent; z: no energy index 0 where the reference
// sends 5 or more. The figures sit a few points under what the encoder reaches (spec 18.5, where the key's limit is
// explained): band noise with brickwall edges is decided by the reference's float32 round-off frame by frame, and
// steps_fullscale's -32768 segments get index 4 from the reference and 23 on +32767 for the same reason
static const struct {
	const char *name;
	uint8_t e1, lsp1_3200, lsp1_1600;
	bool z;
} extra_keys[] = {
	{ "alt_fullscale", 99, 99, 99, true },
	{ "tone_3400_pk_m12", 99, 99, 99, true },
	{ "steps_fullscale", 50, 52, 72, true },
	{ "band_brickwall_3300_3950_m12", 28, 46, 89, true },
	{ "band_brickwall_3600_3990_m12", 31, 52, 85, true },
	{ "band_brickwall_3000_3950_m12", 31, 38, 93, true },
	{ "band_brickwall_3300_3950_m30", 88, 27, 87, false },
	{ "band_brickwall_3600_3990_m30", 95, 37, 80, false },
	{ "ctrl_bp_3200_q2_m12", 99, 99, 99, true },
	{ "ctrl_hp2_2500_m12", 99, 99, 99, true },
	{ "fresh30_s1", 97, 96, 99, true },
	{ "fresh30_s2", 97, 95, 99, true },
};

static uint8_t *read_file(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *p = malloc(len > 0 ? (size_t)len : 1);
	*n = p != NULL && len > 0 ? fread(p, 1, (size_t)len, f) : 0;
	fclose(f);
	return p;
}

static void test_extra(const char *dir)
{
	char path[1024];
	unsigned keys = 0;

	printf("codec2-extra: per key, exact (and within one) against the reference encoder's bits:\n");
	for (unsigned k = 0; k < sizeof extra_keys / sizeof extra_keys[0]; k++) {
		size_t n;

		snprintf(path, sizeof path, "%s/inputs/%s.raw", dir, extra_keys[k].name);
		int16_t *pcm = read_pcm(path, &n);
		CHECK(pcm != NULL, "codec2-extra: no input %s", path);
		if (pcm == NULL)
			continue;
		// The near-4 kHz inputs made here are these, sample for sample (so the CI checks run on the same signals)
		if (strcmp(extra_keys[k].name, "alt_fullscale") == 0 || strcmp(extra_keys[k].name, "tone_3400_pk_m12") == 0) {
			static int16_t mine[10 * FS];
			size_t len = n < 10 * FS ? n : 10 * FS;

			(extra_keys[k].name[0] == 'a' ? make_alt : make_tone3400)(mine, len);
			CHECK(memcmp(mine, pcm, len * 2) == 0, "%s is the input made here", extra_keys[k].name);
		}
		for (int m = 0; m < 2; m++) {
			c2_mode_t mode = m ? C2_MODE_1600 : C2_MODE_3200;
			const char *ms = m ? "1600" : "3200";
			size_t nr;

			snprintf(path, sizeof path, "%s/ref/%s.%s.bit", dir, extra_keys[k].name, ms);
			uint8_t *ref = read_file(path, &nr);
			CHECK(ref != NULL, "codec2-extra: no %s", path);
			if (ref == NULL)
				continue;
			uint8_t *out = malloc(n / C2_BLOCK_SAMPLES * C2_FRAME_BYTES + C2_FRAME_BYTES);
			unsigned frames = encode(mode, pcm, n, out, NULL), z = 0;
			agree_t e = { 0 }, lsp = { 0 };

			CHECK(nr == (size_t)frames * C2_FRAME_BYTES, "%s.%s: %u frames, the reference %zu", extra_keys[k].name,
			      ms, frames, nr / C2_FRAME_BYTES);
			for (unsigned f = 0; f < frames && (size_t)(f + 1) * C2_FRAME_BYTES <= nr; f++) {
				c2_index_t a, b;

				c2_unpack(mode, out + C2_FRAME_BYTES * f, &a);
				c2_unpack(mode, ref + C2_FRAME_BYTES * f, &b);
				for (int h = 0; h < (m ? 2 : 1); h++) {
					agree(&e, b.e[h], a.e[h]);
					z += a.e[h] == 0 && b.e[h] >= 5;
				}
				for (int i = 0; i < C2_ORDER; i++)
					agree(&lsp, b.lsp[i], a.lsp[i]);
			}
			unsigned lsp_min = m ? extra_keys[k].lsp1_1600 : extra_keys[k].lsp1_3200;
			CHECK(100 * rate(&e, true) >= extra_keys[k].e1, "%s.%s: energy index within one %.1f %% (>= %u)",
			      extra_keys[k].name, ms, 100 * rate(&e, true), extra_keys[k].e1);
			CHECK(100 * rate(&lsp, true) >= lsp_min, "%s.%s: LSP indexes within one %.1f %% (>= %u)",
			      extra_keys[k].name, ms, 100 * rate(&lsp, true), lsp_min);
			CHECK(!extra_keys[k].z || z == 0, "%s.%s: energy index 0 where the reference sends 5 or more, %u times",
			      extra_keys[k].name, ms, z);
			snprintf(path, sizeof path, "%s.%s", extra_keys[k].name, ms);
			printf("  %-34s %4u frames  E %5.1f/%5.1f %%  LSP %5.1f/%5.1f %%  E 0 for 5+: %u\n", path, frames,
			       100 * rate(&e, false), 100 * rate(&e, true), 100 * rate(&lsp, false),
			       100 * rate(&lsp, true), z);
			keys++;
			free(out);
			free(ref);
		}
		free(pcm);
	}
	printf("codec2-extra: %u keys\n", keys);
}

// --- Two steady lines (spec 18.3): two_tone and DTMF ---

// Two sines at -18 dBFS each, 2 s, rounded as the harness's generator does: its two_tone is 500 + 1500 Hz
static void make_pair(int16_t *x, size_t n, double f1, double f2)
{
	double g = pow(10.0, -18.0 / 20);

	for (size_t i = 0; i < n; i++) {
		double t = 2 * 3.14159265358979323846 * (double)i / FS;

		x[i] = (int16_t)fmax(-32768.0, fmin(32767.0, round(32767.0 * (g * sin(f1 * t) + g * sin(f2 * t)))));
	}
}

// Decoded (c2_dec.c) against the input, from the fifth block on. The encoder before 18.3's margin and line stop (the
// pre-fix one) decoded these at the levels below; the margin alone up to 5.8 dB under them (two_tone 3200: -14.5 dB);
// with the line stop they are within 2.4 dB of the input. The check: no more than 3 dB under the pre-fix level (the
// 1600 figures are so low that this alone fails the margin's -22.9 on two_tone by only 1.7 dB and passes its DTMF
// pairs), within 5 dB under the input (what the line stop is for) and under +3 dB over it
static void test_lines(const char *harness)
{
	static int16_t x[2 * FS];
	static uint8_t out[100 * C2_FRAME_BYTES];
	static const struct {
		const char *name;
		double f1, f2;
		double prefix[2];	// dB, 3200 and 1600
	} in[] = {
		{ "two_tone", 500, 1500, { -8.7, -18.2 } },
		{ "DTMF 1 (697 + 1209 Hz)", 697, 1209, { -8.4, -19.1 } },
		{ "DTMF # (941 + 1633 Hz)", 941, 1633, { -14.6, -31.8 } },
	};

	for (unsigned k = 0; k < sizeof in / sizeof in[0]; k++) {
		make_pair(x, 2 * FS, in[k].f1, in[k].f2);
		if (k == 0 && harness != NULL && harness[0] != 0) {
			char path[1024];
			size_t n;

			snprintf(path, sizeof path, "%s/inputs/two_tone.raw", harness);
			int16_t *pcm = read_pcm(path, &n);
			CHECK(pcm != NULL && n == 2 * FS && memcmp(pcm, x, sizeof x) == 0, "two_tone is the harness's input");
			free(pcm);
		}
		for (int m = 0; m < 2; m++) {
			c2_mode_t mode = m ? C2_MODE_1600 : C2_MODE_3200;
			unsigned n = encode(mode, x, 2 * FS, out, NULL);
			// The samples the n frames cover: 1600 sends a frame every second block
			size_t end = (size_t)n * C2_BLOCK_SAMPLES * (m ? 2 : 1);
			double dl = 20 * log10(decode_rms(mode, out, n, 640) / rms(x, 640, end));

			CHECK(dl >= in[k].prefix[m] - 3.0 && dl > -5.0 && dl < 3.0,
			      "%s %s: decoded level %+.1f dB against the input (pre-fix %+.1f, -3 dB at most; -5 to +3)",
			      in[k].name, m ? "1600" : "3200", dl, in[k].prefix[m]);
			printf("two lines: %s %s decodes %+.1f dB against the input (pre-fix %+.1f)\n", in[k].name,
			       m ? "1600" : "3200", dl, in[k].prefix[m]);
		}
	}
}

int main(int argc, char **argv)
{
	const char *harness = argc > 1 ? argv[1] : getenv("RFOUNDRY_C2_HARNESS");
	const char *extra = argc > 2 ? argv[2] : getenv("RFOUNDRY_C2_EXTRA");

	test_tables();
	test_golden();
	test_harmonics();
	test_init_and_halves();
	test_silence();
	test_hostile();
	test_nyquist();
	test_lines(harness);
	if (harness != NULL && harness[0] != 0)
		test_harness(harness);
	else
		printf("harness: skipped (RFOUNDRY_C2_HARNESS=<dir with HARNESS.md, ref/, inputs/> runs it)\n");
	if (extra != NULL && extra[0] != 0)
		test_extra(extra);
	else
		printf("codec2-extra: skipped (RFOUNDRY_C2_EXTRA=<dir with README.md, inputs/, ref/> runs it)\n");
	printf("c2EncTest: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
