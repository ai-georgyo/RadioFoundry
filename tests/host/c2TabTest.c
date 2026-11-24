// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codec 2's shared tables and bit packing (core/codec2/c2_tab.c, c2_bits.c) against docs/spec/codec2.md:
//   - every table's sum and SHA-256 as spec 14.4 and tables.json give them (the integer tables converted back to the
//     float32 or int32 values the checksums are over)
//   - the bit layout of spec 4.2 / 4.3 and the Gray code, every value of every field; pack / unpack round trips
//   - the dequantizers' known values, the harmonic count L (79 at index 0) and c2_L_of_wo against the double quotient
//     on every C2_WO_Q value from ω0_min to ω0_max
//   - c2_lsp_dequant against a float32 model of spec 10.2 on random indexes, both modes
//   - the silence frames' fields
// With the reference harness (the directory holding HARNESS.md, ref/ and inputs/; kept outside the repository, spec
// 1): RFOUNDRY_C2_HARNESS=<dir> or the first argument. Then also every reference bit stream unpacks to the indexes the
// reference decoder dumped, every encoder frame packs to the reference's bytes, the model and c2_lsp_dequant give the
// reference's decoded LSPs (exactly, and within 1e-4 rad), and the reference encoder's silence is the silence frame.
// Without it those checks are skipped with a notice.
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec2/c2_int.h"
#include "support/sha256.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; if (failures < 30) { printf("FAIL %s:%d: ", __FILE__, \
	__LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

#define PI_C	3.141592654	// spec 3.2: every π of the parameter math
#define Q28	268435456.0

static uint32_t rng_state = 0x2545F491u;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

// --- Checksums (spec 14: SHA-256 over the values as little-endian float32, or int32 for integer tables) ---

typedef struct {
	sha256_t s;
	double sum;
} tabsum_t;

static void ts_init(tabsum_t *t)
{
	sha256_init(&t->s);
	t->sum = 0;
}

static void ts_le32(tabsum_t *t, uint32_t u)
{
	uint8_t b[4] = { (uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24) };
	sha256_update(&t->s, b, 4);
}

static void ts_f32(tabsum_t *t, float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	ts_le32(t, u);
	t->sum += f;
}

static void ts_i32(tabsum_t *t, int32_t v)
{
	ts_le32(t, (uint32_t)v);
	t->sum += v;
}

static void ts_check(tabsum_t *t, const char *name, double sum, const char *hex)
{
	uint8_t d[SHA256_LEN];
	char got[2 * SHA256_LEN + 1];
	sha256_final(&t->s, d);
	for (unsigned i = 0; i < SHA256_LEN; i++)
		snprintf(got + 2 * i, 3, "%02x", d[i]);
	CHECK(strcmp(got, hex) == 0, "%s: sha256 %s, tables.json %s", name, got, hex);
	CHECK(fabs(t->sum - sum) <= 1e-9 * (fabs(sum) + 1.0), "%s: sum %.12g, tables.json %.12g", name, t->sum, sum);
}

// tables.json (spec 14.4 lists the sums rounded and the first 16 hex digits of each digest)
static const struct {
	double sum;
	const char *sha;
} lsp1600_ref[C2_ORDER] = {
	{ 6600, "a257628f5f595fe25ea36ffeab8996dde33b84306723865f6db8c181a2d3e821" },
	{ 8200, "659266857984e9347420fbed9c2149a9e28276a4c193d15c277d3714d403bfa9" },
	{ 14000, "ed4f105f0d7edf7376af3752652ce1d522aea81da0f6c7a7f8e205b19fcd7de7" },
	{ 23200, "c98d3857d8b929c658584210a6a02b2cb5b6d7a992a1564787bf409c866a6de4" },
	{ 27200, "6f1f823281ef4b62942c2f1a8a8b8faaba275c9c439ffdfeaa44adaa4c94260f" },
	{ 29600, "d60da722a404233ce682e46b00b2d6b82b015901131714eb2992335423b7bb49" },
	{ 36000, "ad0d52a9529e7bc0357e359b2263e1a2669577662643abcabe827e8dc1e87a40" },
	{ 21200, "d099cc6fa12b1d3292c9c477edc27f8c0ebc8f765c12e55527e01955db8bf358" },
	{ 22800, "ce4a8e27d12777fa3cd4fc9d860d194bfa6a2deff0438b91bc518768eaafc499" },
	{ 12800, "4c613d39eb9d83f60568f970b1230fdd0b8f85389eacbdb5ea6082acd8f80fc0" },
};
#define LSPD3200_A_SHA	"2bcef849c53a88c74e12c4f9ddf2c99f2cb2d829718d729056150fc9a6332c50"	// q1-q3, q7-q10
#define LSPD3200_B_SHA	"d79f93d1e888c0f47177972563403010d80366c56c84c6e60671b736e3a1f953"	// q4-q6
#define WO_SHA		"0f0882db6bf433e6cfd0262f2a78faa3a20a7b3801a6dc0ab7296fcc07848dda"
#define WO_SUM		22.482022054493427
#define WO_L_SHA	"06ddc64cc43537e6fbfd57da954c15fba6081545f1fd96a5e659655e695a9f25"
#define E_SHA		"5df232bf755e8587c15a94c2244686dd3e65681250c08516b76f0827001af3a6"
#define E_SUM		23093.78667924553

static float f32_bits(uint32_t u)
{
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static void test_tables(void)
{
	// LSP levels: whole Hz, so the float32 values are the integers themselves
	for (uint8_t i = 0; i < C2_ORDER; i++) {
		tabsum_t t;
		char name[32];
		uint8_t bits = c2_lsp_bits(C2_MODE_1600, i);
		ts_init(&t);
		for (unsigned x = 0; x < (1u << bits); x++)
			ts_f32(&t, (float)c2_lsp_level_hz(C2_MODE_1600, i, (uint8_t)x));
		snprintf(name, sizeof name, "lsp1600_q%u", i + 1u);
		ts_check(&t, name, lsp1600_ref[i].sum, lsp1600_ref[i].sha);

		CHECK(c2_lsp_bits(C2_MODE_3200, i) == 5, "lspd3200_q%u: %u bits", i + 1u, c2_lsp_bits(C2_MODE_3200, i));
		ts_init(&t);
		for (unsigned x = 0; x < 32; x++)
			ts_f32(&t, (float)c2_lsp_level_hz(C2_MODE_3200, i, (uint8_t)x));
		snprintf(name, sizeof name, "lspd3200_q%u", i + 1u);
		bool b = i >= 3 && i <= 5;
		ts_check(&t, name, b ? 20700 : 13200, b ? LSPD3200_B_SHA : LSPD3200_A_SHA);
	}
	static const uint8_t bits1600[C2_ORDER] = { 4, 4, 4, 4, 4, 4, 4, 3, 3, 2 };
	unsigned total = 0;
	for (uint8_t i = 0; i < C2_ORDER; i++) {
		CHECK(c2_lsp_bits(C2_MODE_1600, i) == bits1600[i], "lsp1600_q%u: %u bits", i + 1u,
		      c2_lsp_bits(C2_MODE_1600, i));
		total += c2_lsp_bits(C2_MODE_1600, i);
	}
	CHECK(total == 36, "1600 LSP bits %u", total);
	CHECK(c2_lsp_bits(C2_MODE_1600, C2_ORDER) == 0 && c2_lsp_level_hz(C2_MODE_3200, C2_ORDER, 0) == 0,
	      "LSP past the order");
	// Spec 14.1 / 14.2 by their prose: first, last, an index wider than the field keeps its low bits
	CHECK(c2_lsp_level_hz(C2_MODE_1600, 0, 0) == 225 && c2_lsp_level_hz(C2_MODE_1600, 9, 3) == 3500 &&
	      c2_lsp_level_hz(C2_MODE_1600, 9, 4) == 2900 && c2_lsp_level_hz(C2_MODE_1600, 7, 15) == 3000,
	      "1600 levels");
	CHECK(c2_lsp_level_hz(C2_MODE_3200, 3, 8) == 250 && c2_lsp_level_hz(C2_MODE_3200, 5, 31) == 1400 &&
	      c2_lsp_level_hz(C2_MODE_3200, 6, 31) == 800 && c2_lsp_level_hz(C2_MODE_3200, 0, 32) == 25,
	      "3200 levels");

	// ω0: C2_WO_Q values that are float32 values exactly, the spec's formula in float32, ω0_min's bits
	tabsum_t t;
	ts_init(&t);
	const float wmin = f32_bits(0x3D20D97Cu), step = f32_bits(0x3B0CBE4Cu);	// spec 3.2, 9.1
	int formula = 0;
	for (unsigned i = 0; i < 128; i++) {
		int32_t q = c2_wo_dequant((uint8_t)i);
		float f = (float)q;
		CHECK((int32_t)f == q, "wo_dequant[%u] = %d is not a float32 value", i, q);
		f = (float)(f / Q28);
		ts_f32(&t, f);
		float spec = wmin + (float)i * step;
		formula += spec == f;
	}
	ts_check(&t, "wo_dequant", WO_SUM, WO_SHA);
	CHECK(formula == 128, "wo_dequant = f32(ω0_min + f32(i · s)) on %d of 128", formula);
	CHECK((double)c2_wo_dequant(0) == (double)wmin * Q28 && c2_wo_dequant(0) == 0x00A0D97C,
	      "ω0_min %d (0x3D20D97C is %.1f)", c2_wo_dequant(0), (double)wmin * Q28);
	CHECK(c2_wo_dequant(128) == c2_wo_dequant(0), "ω0 index wider than 7 bits");

	// L of every ω0 index: table wo_dequant_L
	ts_init(&t);
	for (unsigned i = 0; i < 128; i++)
		ts_i32(&t, c2_wo_L((uint8_t)i));
	ts_check(&t, "wo_dequant_L", 3012, WO_L_SHA);
	CHECK(c2_wo_L(0) == 79 && c2_wo_L(127) == 10, "L at index 0 %u (79), 127 %u (10)", c2_wo_L(0), c2_wo_L(127));

	// Energy: the spec's formula in float32 gives tables.json's values; the table is each rounded to C2_E_Q
	ts_init(&t);
	double worst = 0;
	for (unsigned i = 0; i < 32; i++) {
		float f = (float)pow(10.0, (-10.0 + 1.5625 * i) / 10.0);
		ts_f32(&t, f);
		uint32_t q = c2_e_dequant((uint8_t)i);
		CHECK(q == (uint32_t)llround((double)f * 262144.0), "energy_dequant[%u] %u, f32 %.9g", i, q, (double)f);
		double rel = fabs(q / 262144.0 - (double)f) / (double)f;
		worst = rel > worst ? rel : worst;
	}
	ts_check(&t, "energy_dequant", E_SUM, E_SHA);
	CHECK(worst < 2e-5, "energy relative error %.3g", worst);
	CHECK(c2_e_dequant(0) == 26214u && c2_e_dequant(32) == 26214u, "energy index 0 (0.1)");
	CHECK(fabs(c2_e_dequant(31) / 262144.0 - 6978.3) < 0.05, "energy index 31 %.2f (6978.3)",
	      c2_e_dequant(31) / 262144.0);
	printf("tables: LSP levels (20), wo_dequant, wo_dequant_L, energy_dequant: sums and SHA-256 as tables.json; "
	       "energy within %.2g relative\n", worst);
}

// --- c2_L_of_wo: trunc(π_c / ω0) with the quotient in double, on every C2_WO_Q value from ω0_min to ω0_max ---

static void test_L_of_wo(void)
{
	const int32_t lo = c2_wo_dequant(0);
	const int32_t hi = (int32_t)((double)f32_bits(0x3EA0D97Cu) * Q28);	// ω0_max, spec 3.2
	long bad = 0;
	for (int32_t w = lo; w <= hi; w++) {
		int ref = (int)trunc(PI_C / ((double)w / Q28));
		if (c2_L_of_wo(w) != ref && bad++ < 5)
			printf("FAIL c2_L_of_wo(%d) = %u, double %d\n", w, c2_L_of_wo(w), ref);
	}
	CHECK(bad == 0, "c2_L_of_wo differs from the double quotient on %ld values", bad);
	CHECK(c2_L_of_wo(lo) == 79 && c2_L_of_wo(hi) == 9, "L(ω0_min) %u, L(ω0_max) %u", c2_L_of_wo(lo),
	      c2_L_of_wo(hi));
	// Outside the decoder's range: never past C2_L_MAX, 0 above π_c
	CHECK(c2_L_of_wo(lo - 1) == 79 && c2_L_of_wo(1) == C2_L_MAX && c2_L_of_wo(0) == C2_L_MAX &&
	      c2_L_of_wo(-5) == C2_L_MAX && c2_L_of_wo(INT32_MAX) == 0, "L out of range");
	printf("c2_L_of_wo: equal to trunc(π_c / ω0) in double on all %d C2_WO_Q values %d..%d (ω0_min..ω0_max)\n",
	       hi - lo + 1, lo, hi);
}

// --- Bit layout (spec 4.2, 4.3), written out here from the tables there ---

typedef struct {
	const char *name;
	uint8_t start, width;
	size_t off;		// in c2_index_t
} field_t;

#define FLD(n, s, w, m)	{ n, s, w, offsetof(c2_index_t, m) }
static const field_t layout_3200[] = {
	FLD("v1", 0, 1, voiced[0]), FLD("v2", 1, 1, voiced[1]), FLD("wo", 2, 7, wo[0]), FLD("e", 9, 5, e[0]),
	FLD("lsp1", 14, 5, lsp[0]), FLD("lsp2", 19, 5, lsp[1]), FLD("lsp3", 24, 5, lsp[2]), FLD("lsp4", 29, 5, lsp[3]),
	FLD("lsp5", 34, 5, lsp[4]), FLD("lsp6", 39, 5, lsp[5]), FLD("lsp7", 44, 5, lsp[6]), FLD("lsp8", 49, 5, lsp[7]),
	FLD("lsp9", 54, 5, lsp[8]), FLD("lsp10", 59, 5, lsp[9]),
};
static const field_t layout_1600[] = {
	FLD("v1", 0, 1, voiced[0]), FLD("v2", 1, 1, voiced[1]), FLD("wo2", 2, 7, wo[0]), FLD("e2", 9, 5, e[0]),
	FLD("v3", 14, 1, voiced[2]), FLD("v4", 15, 1, voiced[3]), FLD("wo4", 16, 7, wo[1]), FLD("e4", 23, 5, e[1]),
	FLD("lsp1", 28, 4, lsp[0]), FLD("lsp2", 32, 4, lsp[1]), FLD("lsp3", 36, 4, lsp[2]), FLD("lsp4", 40, 4, lsp[3]),
	FLD("lsp5", 44, 4, lsp[4]), FLD("lsp6", 48, 4, lsp[5]), FLD("lsp7", 52, 4, lsp[6]), FLD("lsp8", 56, 3, lsp[7]),
	FLD("lsp9", 59, 3, lsp[8]), FLD("lsp10", 62, 2, lsp[9]),
};
#undef FLD

static const field_t *layout(c2_mode_t m, unsigned *n)
{
	if (m == C2_MODE_1600) {
		*n = sizeof layout_1600 / sizeof layout_1600[0];
		return layout_1600;
	}
	*n = sizeof layout_3200 / sizeof layout_3200[0];
	return layout_3200;
}

static unsigned get_bits(const uint8_t *b, unsigned start, unsigned width)
{
	unsigned v = 0;
	for (unsigned k = start; k < start + width; k++)
		v = (v << 1) | ((b[k / 8] >> (7 - k % 8)) & 1u);
	return v;
}

// Spec 4.1: i = g XOR (g >> 1) XOR (g >> 2) ... over the field's width
static unsigned gray_dec_spec(unsigned g, unsigned width)
{
	unsigned i = 0;
	for (unsigned s = 0; s < width; s++)
		i ^= g >> s;
	return i;
}

static void random_index(c2_mode_t m, c2_index_t *ix)
{
	unsigned n;
	const field_t *f = layout(m, &n);
	memset(ix, 0, sizeof *ix);
	for (unsigned k = 0; k < n; k++)
		((uint8_t *)ix)[f[k].off] = (uint8_t)(rng() & ((1u << f[k].width) - 1u));
}

static void test_bits(void)
{
	CHECK(sizeof(c2_index_t) == 18, "c2_index_t is %zu bytes", sizeof(c2_index_t));
	static const char *mname[2] = { "3200", "1600" };
	unsigned widths_seen = 0;
	for (int mi = 0; mi < 2; mi++) {
		c2_mode_t m = (c2_mode_t)mi;
		unsigned n, sum = 0;
		const field_t *f = layout(m, &n);
		for (unsigned k = 0; k < n; k++) {
			CHECK(f[k].start == sum, "%s %s starts at %u", mname[mi], f[k].name, f[k].start);
			sum += f[k].width;
			if (f[k].name[0] == 'l')
				CHECK(c2_lsp_bits(m, (uint8_t)(f[k].off - offsetof(c2_index_t, lsp))) == f[k].width,
				      "%s %s width", mname[mi], f[k].name);
		}
		CHECK(sum == 64, "%s fields %u bits", mname[mi], sum);

		// Every value of every field alone: its Gray code at its place, nothing else set, and back
		for (unsigned k = 0; k < n; k++) {
			widths_seen |= 1u << f[k].width;
			for (unsigned v = 0; v < (1u << f[k].width); v++) {
				c2_index_t ix, back;
				uint8_t b[C2_FRAME_BYTES];
				memset(&ix, 0, sizeof ix);
				((uint8_t *)&ix)[f[k].off] = (uint8_t)v;
				c2_pack(m, &ix, b);
				unsigned g = get_bits(b, f[k].start, f[k].width);
				CHECK(g == (v ^ (v >> 1)), "%s %s = %u sent as %u", mname[mi], f[k].name, v, g);
				CHECK(gray_dec_spec(g, f[k].width) == v, "%s %s Gray %u", mname[mi], f[k].name, v);
				uint8_t rest[C2_FRAME_BYTES];
				memcpy(rest, b, sizeof rest);
				for (unsigned x = f[k].start; x < f[k].start + f[k].width; x++)
					rest[x / 8] &= (uint8_t)~(0x80u >> (x % 8));
				static const uint8_t zero[C2_FRAME_BYTES];
				CHECK(memcmp(rest, zero, sizeof zero) == 0, "%s %s = %u sets other bits", mname[mi],
				      f[k].name, v);
				memset(&back, 0x55, sizeof back);
				c2_unpack(m, b, &back);
				CHECK(memcmp(&back, &ix, sizeof ix) == 0, "%s %s = %u unpacks wrong", mname[mi],
				      f[k].name, v);
			}
		}

		// Random indexes round trip; random frames too (every 64-bit string is a frame)
		for (unsigned r = 0; r < 200000; r++) {
			c2_index_t ix, back;
			uint8_t b[C2_FRAME_BYTES], b2[C2_FRAME_BYTES];
			random_index(m, &ix);
			c2_pack(m, &ix, b);
			c2_unpack(m, b, &back);
			CHECK(memcmp(&back, &ix, sizeof ix) == 0, "%s random index round trip %u", mname[mi], r);
			for (unsigned k = 0; k < n; k++) {
				unsigned v = ((uint8_t *)&ix)[f[k].off];
				CHECK(get_bits(b, f[k].start, f[k].width) == (v ^ (v >> 1)), "%s %s random", mname[mi],
				      f[k].name);
			}
			for (unsigned k = 0; k < C2_FRAME_BYTES; k++)
				b[k] = (uint8_t)rng();
			c2_unpack(m, b, &ix);
			c2_pack(m, &ix, b2);
			CHECK(memcmp(b, b2, sizeof b) == 0, "%s random frame round trip %u", mname[mi], r);
		}

		// Out of range: an index keeps its low bits, a voicing flag is any non-zero value
		c2_index_t big, small;
		uint8_t bb[C2_FRAME_BYTES], bs[C2_FRAME_BYTES];
		memset(&big, 0xFF, sizeof big);
		memset(&small, 0, sizeof small);
		for (unsigned k = 0; k < n; k++)
			((uint8_t *)&small)[f[k].off] = (uint8_t)((1u << f[k].width) - 1u);
		c2_pack(m, &big, bb);
		c2_pack(m, &small, bs);
		CHECK(memcmp(bb, bs, sizeof bb) == 0, "%s out-of-range indexes", mname[mi]);
		memset(&big, 0, sizeof big);
		big.voiced[0] = 2;
		c2_pack(m, &big, bb);
		CHECK(bb[0] == 0x80, "%s voicing 2 sent as %02x", mname[mi], bb[0]);
	}
	CHECK(widths_seen == ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 7)),
	      "field widths %x", widths_seen);
	printf("bits: every value of every field (widths 1, 2, 3, 4, 5, 7) at its place, Gray coded, and back; "
	       "200000 random indexes and frames per mode round trip\n");
}

// --- LSP dequantization: a float32 model of spec 10.2 ---

static float hz_rad_f32(float hz)
{
	return (float)((PI_C / 4000.0) * (double)hz);
}

// The model's decoded LSPs; *fixes: the ordering fixes it made
static void lsp_model(c2_mode_t m, const uint8_t idx[C2_ORDER], float w[C2_ORDER], int *fixes)
{
	*fixes = 0;
	if (m == C2_MODE_3200) {
		float hz = 0;
		for (uint8_t i = 0; i < C2_ORDER; i++) {
			hz = hz + (float)c2_lsp_level_hz(m, i, idx[i]);
			w[i] = hz_rad_f32(hz);
		}
		return;
	}
	for (uint8_t i = 0; i < C2_ORDER; i++)
		w[i] = hz_rad_f32((float)c2_lsp_level_hz(m, i, idx[i]));
	for (int i = 1; i < C2_ORDER; i++) {
		if (w[i] < w[i - 1]) {
			float low = w[i - 1];
			w[i - 1] = w[i] - 0.1f;
			w[i] = low + 0.1f;
			if (++*fixes > 1000)
				break;
			i = 1;
		}
	}
	for (int i = 1; i < C2_ORDER; i++) {
		double min = (i < 4 ? 50.0 : 100.0) * PI_C / 4000.0;
		float d = w[i] - w[i - 1];
		if ((double)d < min)
			w[i] = (float)((double)w[i - 1] + min);
	}
}

static double lsp_err_model;	// largest |c2_lsp_dequant - model|, rad

static void lsp_compare(c2_mode_t m, const uint8_t idx[C2_ORDER], const char *what)
{
	float w[C2_ORDER];
	int32_t q[C2_ORDER];
	int fixes;
	lsp_model(m, idx, w, &fixes);
	c2_lsp_dequant(m, idx, q);
	for (int i = 0; i < C2_ORDER; i++) {
		double e = fabs(q[i] / Q28 - (double)w[i]);
		lsp_err_model = e > lsp_err_model ? e : lsp_err_model;
		CHECK(e < 1e-6, "%s %s LSP %d: %.9f, model %.9f", what, m == C2_MODE_1600 ? "1600" : "3200", i + 1,
		      q[i] / Q28, (double)w[i]);
	}
}

static void test_lsp(void)
{
	uint8_t idx[C2_ORDER];
	// Corners: all lowest, all highest, alternating, descending (the most ordering fixes)
	for (int mi = 0; mi < 2; mi++) {
		c2_mode_t m = (c2_mode_t)mi;
		for (unsigned pat = 0; pat < 4; pat++) {
			for (uint8_t i = 0; i < C2_ORDER; i++) {
				uint8_t top = (uint8_t)((1u << c2_lsp_bits(m, i)) - 1u);
				idx[i] = pat == 0 ? 0 : pat == 1 ? top : pat == 2 ? ((i & 1) ? top : 0) :
					 (i < 5 ? top : 0);
			}
			lsp_compare(m, idx, "corner");
		}
		for (unsigned r = 0; r < 1000000; r++) {
			for (uint8_t i = 0; i < C2_ORDER; i++)
				idx[i] = (uint8_t)(rng() & ((1u << c2_lsp_bits(m, i)) - 1u));
			lsp_compare(m, idx, "random");
		}
	}
	// 1600: index sets with the most ordering fixes of all 2^36 (15, found by an exhaustive search of the model):
	// the firmware's cap on the scan (C2_LSP_FIX_MAX) must not cut them short. Random sets reach 15 once in 1.5e8
	static const uint8_t most_fixes[][C2_ORDER] = {
		{ 6, 0, 0, 15, 15, 12, 9, 0, 0, 0 },
		{ 8, 0, 0, 15, 15, 15, 9, 0, 0, 0 },
	};
	for (unsigned s = 0; s < sizeof most_fixes / sizeof most_fixes[0]; s++) {
		float w[C2_ORDER];
		int fixes;
		lsp_model(C2_MODE_1600, most_fixes[s], w, &fixes);
		CHECK(fixes == 15, "most-fixes set %u: %d ordering fixes (15)", s, fixes);
		lsp_compare(C2_MODE_1600, most_fixes[s], "most fixes");
	}
	// 3200: the sum of the largest levels, 9800 Hz, past π but inside C2_LSP_Q's range
	int32_t q[C2_ORDER];
	memset(idx, 31, sizeof idx);
	c2_lsp_dequant(C2_MODE_3200, idx, q);
	CHECK(fabs(q[9] / Q28 - 9800 * PI_C / 4000) < 1e-8, "3200 top LSP %.9f", q[9] / Q28);
	// 1600 spacing: LSP 1 and 2 at 400 and 425 Hz, 25 Hz apart, become 50 Hz apart
	memset(idx, 0, sizeof idx);
	idx[0] = 7;
	idx[1] = 4;
	c2_lsp_dequant(C2_MODE_1600, idx, q);
	CHECK(fabs((q[1] - q[0]) / Q28 - 50 * PI_C / 4000) < 1e-8, "1600 50 Hz spacing %.9f", (q[1] - q[0]) / Q28);
	printf("c2_lsp_dequant: within %.3g rad of the float32 model of spec 10.2 on 1000004 index sets per mode\n",
	       lsp_err_model);
}

// --- Silence frames ---

static void test_silence(void)
{
	c2_index_t ix;
	// What the spec's encoder sends for digital silence (c2_bits.c): unvoiced, ω0 index 3, energy index 0, the LSPs
	// (π_c/10) · i quantized
	static const uint8_t lsp3200[C2_ORDER] = { 0, 14, 15, 11, 11, 11, 15, 15, 15, 15 };
	static const uint8_t lsp1600[C2_ORDER] = { 0, 3, 6, 5, 6, 9, 9, 5, 7, 3 };
	c2_unpack(C2_MODE_3200, c2_silence_3200, &ix);
	CHECK(ix.voiced[0] == 0 && ix.voiced[1] == 0 && ix.wo[0] == 3 && ix.e[0] == 0 &&
	      memcmp(ix.lsp, lsp3200, sizeof lsp3200) == 0, "3200 silence fields");
	c2_unpack(C2_MODE_1600, c2_silence_1600, &ix);
	CHECK(ix.voiced[0] == 0 && ix.voiced[1] == 0 && ix.voiced[2] == 0 && ix.voiced[3] == 0 && ix.wo[0] == 3 &&
	      ix.wo[1] == 3 && ix.e[0] == 0 && ix.e[1] == 0 && memcmp(ix.lsp, lsp1600, sizeof lsp1600) == 0,
	      "1600 silence fields");
	CHECK(c2_e_dequant(0) == c2_e_dequant(ix.e[0]), "silence at the lowest energy");

	// The spec's encoder quantizing those LSPs (9.3, 9.4) gives these indexes
	for (int mi = 0; mi < 2; mi++) {
		c2_mode_t m = (c2_mode_t)mi;
		const uint8_t *want = m == C2_MODE_1600 ? lsp1600 : lsp3200;
		float prev = 0;
		for (uint8_t i = 0; i < C2_ORDER; i++) {
			float w = (float)((PI_C / 10.0) * i);
			float f = (float)((4000.0 / PI_C) * (double)w);
			float d = m == C2_MODE_1600 ? f : f - prev;
			unsigned best = 0;
			float be = 0;
			for (unsigned x = 0; x < (1u << c2_lsp_bits(m, i)); x++) {
				float e = (float)c2_lsp_level_hz(m, i, (uint8_t)x) - d;
				e = e * e;
				if (x == 0 || e < be) {
					be = e;
					best = x;
				}
			}
			prev += (float)c2_lsp_level_hz(m, i, (uint8_t)best);
			CHECK(best == want[i], "%s silence LSP %u: encoder index %u, frame %u", mi ? "1600" : "3200",
			      i + 1u, best, want[i]);
		}
	}
	printf("silence: 3200 %02x%02x%02x%02x%02x%02x%02x%02x, 1600 %02x%02x%02x%02x%02x%02x%02x%02x: unvoiced, "
	       "ω0 index 3, energy index 0, the encoder's LSP indexes for zero energy\n",
	       c2_silence_3200[0], c2_silence_3200[1], c2_silence_3200[2], c2_silence_3200[3], c2_silence_3200[4],
	       c2_silence_3200[5], c2_silence_3200[6], c2_silence_3200[7], c2_silence_1600[0], c2_silence_1600[1],
	       c2_silence_1600[2], c2_silence_1600[3], c2_silence_1600[4], c2_silence_1600[5], c2_silence_1600[6],
	       c2_silence_1600[7]);
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

static const char *json_str(const char *line, const char *key, char *out, size_t max)
{
	char pat[48];
	snprintf(pat, sizeof pat, "\"%s\":", key);
	const char *p = strstr(line, pat);
	if (p == NULL)
		return NULL;
	p += strlen(pat);
	while (*p == ' ')
		p++;
	if (*p++ != '"')
		return NULL;
	size_t n = 0;
	while (*p && *p != '"' && n + 1 < max)
		out[n++] = *p++;
	out[n] = 0;
	return out;
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
	unsigned keys, frames, enc_frames, lsp_exact, order_fixes_equal, silence_frames;
	double lsp_err;		// largest |c2_lsp_dequant - reference|
	int max_fixes;
} harness_t;

static void harness_key(const char *dir, const char *key, harness_t *h)
{
	char path[1024], input[256];
	c2_mode_t m = strstr(key, ".1600") != NULL ? C2_MODE_1600 : C2_MODE_3200;
	const char *ms = m == C2_MODE_1600 ? "1600" : "3200";
	snprintf(input, sizeof input, "%s", key);
	*strrchr(input, '.') = 0;

	size_t nbits;
	snprintf(path, sizeof path, "%s/ref/%s.bit", dir, key);
	uint8_t *bits = read_file(path, &nbits);
	if (bits == NULL) {
		snprintf(path, sizeof path, "%s/inputs/%s.bit", dir, input);
		bits = read_file(path, &nbits);
	}
	CHECK(bits != NULL && nbits % C2_FRAME_BYTES == 0, "%s: no bit stream", key);
	if (bits == NULL)
		return;
	unsigned nframes = (unsigned)(nbits / C2_FRAME_BYTES);
	h->keys++;

	// Decoder dump: each frame's indexes as received and its LSPs after the 1600 fixes
	snprintf(path, sizeof path, "%s/ref/%s.dec.jsonl", dir, key);
	FILE *f = fopen(path, "r");
	CHECK(f != NULL, "%s: no %s", key, path);
	char *line = NULL;
	size_t cap = 0;
	unsigned seen = 0;
	while (f != NULL && getline(&line, &cap, f) > 0) {
		if (strncmp(line, "{\"type\":\"unpack\"", 16) != 0)
			continue;
		double fr, v[4], wo[2], e[2], li[C2_ORDER], lf[C2_ORDER], fixes;
		json_nums(line, "frame", &fr, 1);
		unsigned k = (unsigned)fr;
		int nv = json_nums(line, "voiced", v, 4), nw = json_nums(line, "Wo_index", wo, 2);
		int ne = json_nums(line, "E_index", e, 2), nl = json_nums(line, "lsp_index", li, C2_ORDER);
		int nf = json_nums(line, "lsp_final", lf, C2_ORDER), nx = json_nums(line, "lsp_order_fixes", &fixes, 1);
		unsigned sub = m == C2_MODE_1600 ? 4 : 2, half = sub / 2;
		CHECK(k < nframes && nv == (int)sub && nw == (int)half && ne == (int)half && nl == C2_ORDER &&
		      nf == C2_ORDER, "%s frame %u: record", key, k);
		if (k >= nframes)
			continue;
		seen++;
		c2_index_t ix;
		c2_unpack(m, bits + k * C2_FRAME_BYTES, &ix);
		bool same = true;
		for (unsigned i = 0; i < sub; i++)
			same &= ix.voiced[i] == (uint8_t)v[i];
		for (unsigned i = 0; i < half; i++)
			same &= ix.wo[i] == (uint8_t)wo[i] && ix.e[i] == (uint8_t)e[i];
		for (unsigned i = 0; i < C2_ORDER; i++)
			same &= ix.lsp[i] == (uint8_t)li[i];
		CHECK(same, "%s frame %u: unpacked indexes differ from the reference decoder's", key, k);
		uint8_t again[C2_FRAME_BYTES];
		c2_pack(m, &ix, again);
		CHECK(memcmp(again, bits + k * C2_FRAME_BYTES, sizeof again) == 0, "%s frame %u: repacks differently",
		      key, k);

		float w[C2_ORDER];
		int32_t q[C2_ORDER];
		int mf;
		lsp_model(m, ix.lsp, w, &mf);
		c2_lsp_dequant(m, ix.lsp, q);
		bool exact = true;
		for (unsigned i = 0; i < C2_ORDER; i++) {
			exact &= w[i] == (float)lf[i];
			double err = fabs(q[i] / Q28 - lf[i]);
			h->lsp_err = err > h->lsp_err ? err : h->lsp_err;
			CHECK(err < 1e-4, "%s frame %u LSP %u: %.9f, reference %.9f", key, k, i + 1, q[i] / Q28, lf[i]);
		}
		CHECK(exact, "%s frame %u: the model's LSPs differ from the reference's", key, k);
		h->lsp_exact += exact;
		if (m == C2_MODE_1600) {
			CHECK(nx == 1 && (int)fixes == mf, "%s frame %u: %d ordering fixes, reference %d", key, k, mf,
			      (int)fixes);
			h->order_fixes_equal += nx == 1 && (int)fixes == mf;
			h->max_fixes = mf > h->max_fixes ? mf : h->max_fixes;
		}
		h->frames++;
	}
	CHECK(seen == nframes, "%s: %u unpack records for %u frames", key, seen, nframes);
	free(line);
	line = NULL;
	cap = 0;
	if (f != NULL)
		fclose(f);

	// Encoder dump (PCM inputs): each frame's indexes, with the analysis frames' voicing, pack to its bytes
	snprintf(path, sizeof path, "%s/ref/%s.enc.jsonl", dir, key);
	f = fopen(path, "r");
	uint8_t voiced[4] = { 0 };
	while (f != NULL && getline(&line, &cap, f) > 0) {
		double d[C2_ORDER];
		char type[16];
		if (json_str(line, "type", type, sizeof type) == NULL)
			continue;
		if (strcmp(type, "analysis") == 0) {
			double sub, v;
			json_nums(line, "sub", &sub, 1);
			json_nums(line, "voiced", &v, 1);
			if ((unsigned)sub < 4)
				voiced[(unsigned)sub] = (uint8_t)v;
			continue;
		}
		if (strcmp(type, "frame") != 0)
			continue;
		double fr;
		char hex[24];
		json_nums(line, "frame", &fr, 1);
		c2_index_t ix;
		memset(&ix, 0, sizeof ix);
		memcpy(ix.voiced, voiced, sizeof voiced);
		int nw = json_nums(line, "Wo_index", d, 2);
		for (int i = 0; i < nw && i < 2; i++)
			ix.wo[i] = (uint8_t)d[i];
		int ne = json_nums(line, "E_index", d, 2);
		for (int i = 0; i < ne && i < 2; i++)
			ix.e[i] = (uint8_t)d[i];
		int nl = json_nums(line, "lsp_index", d, C2_ORDER);
		for (int i = 0; i < nl && i < C2_ORDER; i++)
			ix.lsp[i] = (uint8_t)d[i];
		uint8_t b[C2_FRAME_BYTES];
		char got[24];
		c2_pack(m, &ix, b);
		for (unsigned i = 0; i < C2_FRAME_BYTES; i++)
			snprintf(got + 2 * i, 3, "%02x", b[i]);
		CHECK(json_str(line, "bytes", hex, sizeof hex) != NULL && strcmp(hex, got) == 0,
		      "%s frame %u: packed %s, reference %s", key, (unsigned)fr, got, hex);
		CHECK((unsigned)fr < nframes && memcmp(b, bits + (unsigned)fr * C2_FRAME_BYTES, sizeof b) == 0,
		      "%s frame %u: packed bytes differ from the bit stream", key, (unsigned)fr);
		h->enc_frames++;
		memset(voiced, 0, sizeof voiced);
	}
	free(line);
	if (f != NULL)
		fclose(f);

	// The reference encoder on digital silence settles on the silence frame (after its start-up frames)
	if (strcmp(input, "silence") == 0) {
		const uint8_t *sil = m == C2_MODE_1600 ? c2_silence_1600 : c2_silence_3200;
		unsigned same = 0;
		for (unsigned k = 0; k < nframes; k++)
			same += memcmp(bits + k * C2_FRAME_BYTES, sil, C2_FRAME_BYTES) == 0;
		CHECK(same + 2 >= nframes && memcmp(bits + (nframes - 1) * C2_FRAME_BYTES, sil, C2_FRAME_BYTES) == 0,
		      "%s: %u of %u frames are the silence frame", key, same, nframes);
		printf("harness: %s silence: %u of %u reference frames are c2_silence_%s\n", ms, same, nframes, ms);
		h->silence_frames += same;
	}
	free(bits);
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
	CHECK(h.keys == 60, "harness: %u keys (60)", h.keys);
	printf("harness: %u keys, %u frames: indexes as the reference decoder's; decoded LSPs within %.3g rad "
	       "(model exact on %u); 1600 ordering fixes as the reference's on %u frames (up to %d)\n",
	       h.keys, h.frames, h.lsp_err, h.lsp_exact, h.order_fixes_equal, h.max_fixes);
	printf("harness: %u encoder frames pack to the reference's bytes\n", h.enc_frames);
}

int main(int argc, char **argv)
{
	const char *harness = argc > 1 ? argv[1] : getenv("RFOUNDRY_C2_HARNESS");
	test_tables();
	test_L_of_wo();
	test_bits();
	test_lsp();
	test_silence();
	if (harness != NULL && harness[0] != 0)
		test_harness(harness);
	else
		printf("harness: skipped (RFOUNDRY_C2_HARNESS=<dir with HARNESS.md, ref/, inputs/> runs it)\n");
	printf("c2TabTest: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
