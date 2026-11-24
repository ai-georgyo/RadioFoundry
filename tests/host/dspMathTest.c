// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// core/dsp/dsp_math.c's integer helpers against exact references in the host's 64-bit arithmetic (and its double
// atan2): dsp_norm32 at every bit position, dsp_recip and dsp_isqrt32 on the 2^16 smallest values, every square's
// edges and a million random values of every magnitude, dsp_isqrt64 the same over 64 bits, dsp_atan2 on dense circles
// of radii from 1 to 2^31, every small point and random points, against the bound dsp.h states (2^16 phase units).
// The Makefile builds it plain and with -DFXP_SPLIT16 (as a C-SKY build runs): nothing in it may depend on that.
#include <math.h>
#include <stdio.h>
#include <stdint.h>

#include "dsp/dsp.h"

#ifndef M_PI
#define M_PI	3.14159265358979323846
#endif

static int failures;
static long checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; if (failures < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint32_t rng_state = 0x6C078965u;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

// A random value of a random magnitude: every bit length 1..32 about as often
static uint32_t pick32(void)
{
	uint32_t r = rng();
	unsigned sh = rng() % 32u;

	return r >> sh;
}

static uint64_t pick64(void)
{
	uint64_t r = ((uint64_t)rng() << 32) | rng();
	unsigned sh = rng() % 64u;

	return r >> sh;
}

static int ref_norm32(uint32_t x)
{
	int n = 0;

	if (!x)
		return 32;
	while (!(x & 0x80000000u)) {
		x <<= 1;
		n++;
	}
	return n;
}

static void test_norm32(void)
{
	CHECK(dsp_norm32(0) == 32, "norm32(0) = %d", dsp_norm32(0));
	for (int b = 0; b < 32; b++) {
		uint32_t top = 1u << b, below = top - 1u;

		CHECK(dsp_norm32(top) == 31 - b, "norm32(2^%d) = %d", b, dsp_norm32(top));
		CHECK(dsp_norm32(top | below) == 31 - b, "norm32(2^%d | lower) = %d", b, dsp_norm32(top | below));
		for (int k = 0; k < 64; k++) {
			uint32_t x = top | (rng() & below);

			CHECK(dsp_norm32(x) == 31 - b, "norm32(%08X) = %d", x, dsp_norm32(x));
		}
	}
	for (int k = 0; k < 100000; k++) {
		uint32_t x = pick32();

		CHECK(dsp_norm32(x) == ref_norm32(x), "norm32(%08X) = %d, want %d", x, dsp_norm32(x), ref_norm32(x));
	}
}

static void one_recip(uint32_t x)
{
	int e = -1, re = 62 - ref_norm32(x);
	uint32_t r = dsp_recip(x, &e);
	uint64_t want = (((uint64_t)1 << re) - 1u) / x;

	CHECK(e == re && r == want, "recip(%u) = %u / 2^%d, want %llu / 2^%d", x, r, e, (unsigned long long)want, re);
	CHECK(r >= (1u << 30) && r < 0x80000000u, "recip(%u) = %u: not 31 significant bits", x, r);
}

static void test_recip(void)
{
	int e = -1;
	uint32_t r = dsp_recip(0, &e);

	CHECK(r == 0x7FFFFFFFu && e == 0, "recip(0) = %u / 2^%d", r, e);
	for (uint32_t x = 1; x <= 65536u; x++)
		one_recip(x);
	for (int b = 0; b < 32; b++) {
		uint32_t p = 1u << b;

		one_recip(p);
		if (b > 1)
			one_recip(p - 1u);
		one_recip(p + 1u);
		one_recip(p | (p - 1u));
	}
	one_recip(0xFFFFFFFFu);
	one_recip(0xFFFFFFFEu);
	one_recip(0x80000001u);
	for (int k = 0; k < 1000000; k++) {
		uint32_t x = k & 1 ? rng() : pick32();

		one_recip(x ? x : 1u);
	}
}

static uint32_t ref_isqrt(uint64_t x)
{
	uint64_t r = (uint64_t)sqrt((double)x);

	// the double's rounding can be one off either way near 2^64: settle it exactly (128-bit squares)
	while (r && (unsigned __int128)r * r > x)
		r--;
	while ((unsigned __int128)(r + 1) * (r + 1) <= x)
		r++;
	return (uint32_t)r;
}

static void one_isqrt32(uint32_t x)
{
	uint32_t r = dsp_isqrt32(x), want = ref_isqrt(x);

	CHECK(r == want, "isqrt32(%u) = %u, want %u", x, r, want);
}

static void one_isqrt64(uint64_t x)
{
	uint32_t r = dsp_isqrt64(x), want = ref_isqrt(x);

	CHECK(r == want, "isqrt64(%llu) = %u, want %u", (unsigned long long)x, r, want);
}

static void test_isqrt(void)
{
	for (uint32_t x = 0; x < 65536u; x++)
		one_isqrt32(x);
	// every square and its neighbors: where the root steps
	for (uint32_t k = 1; k < 65536u; k++) {
		uint32_t s = k * k;

		one_isqrt32(s - 1u);
		one_isqrt32(s);
		one_isqrt32(s + 1u);
	}
	one_isqrt32(0xFFFFFFFFu);
	one_isqrt32(0xFFFE0001u);	// 65535^2
	one_isqrt32(0x80000000u);
	for (int k = 0; k < 1000000; k++)
		one_isqrt32(k & 1 ? rng() : pick32());

	static const uint64_t edge[] = {
		0, 1, 2, 3, 4, 0xFFFFFFFFull, 0x100000000ull, 0x100000001ull, 0x1FFFFFFFFull, 0x200000000ull,
		0xFFFFFFFE00000001ull,	// (2^32 - 1)^2
		0xFFFFFFFE00000000ull, 0xFFFFFFFE00000002ull, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000000ull,
		0x7FFFFFFFFFFFFFFFull, 0x4000000000000000ull, 0x3FFFFFFFFFFFFFFFull, 0x0001000000000000ull,
	};
	for (unsigned i = 0; i < sizeof edge / sizeof edge[0]; i++)
		one_isqrt64(edge[i]);
	for (int b = 32; b < 64; b++) {
		uint64_t p = (uint64_t)1 << b;

		one_isqrt64(p - 1u);
		one_isqrt64(p);
		one_isqrt64(p + 1u);
	}
	for (int k = 0; k < 1000000; k++) {
		uint64_t r = (uint64_t)pick32(), s = r * r;

		switch (k % 4) {
		case 0: one_isqrt64(s); one_isqrt64(s - 1u); one_isqrt64(s + 1u); break;
		case 1: one_isqrt64(((uint64_t)rng() << 32) | rng()); break;
		default: one_isqrt64(pick64()); break;
		}
	}
}

// The true phase of (x, y) and the circular distance to what dsp_atan2 gave
static double worst_atan;
static void one_atan2(int32_t y, int32_t x)
{
	uint32_t got = dsp_atan2(y, x);
	double t = atan2((double)y, (double)x) / (2.0 * M_PI) * 4294967296.0;
	double d;

	if (t < 0)
		t += 4294967296.0;
	d = (double)got - t;
	if (d > 2147483648.0)
		d -= 4294967296.0;
	if (d < -2147483648.0)
		d += 4294967296.0;
	if (!x && !y) {
		CHECK(got == 0, "atan2(0, 0) = %u", got);
		return;
	}
	if (fabs(d) > worst_atan)
		worst_atan = fabs(d);
	CHECK(fabs(d) <= 65536.0, "atan2(%d, %d) = %u, true %.1f: off by %.1f", y, x, got, t, d);
}

static int32_t clamp32(double v)
{
	v = nearbyint(v);
	return v > 2147483647.0 ? 2147483647 : v < -2147483648.0 ? (int32_t)0x80000000u : (int32_t)v;
}

static void test_atan2(void)
{
	static const double radius[] = { 1, 2, 3, 5, 10, 100, 1000, 32767, 65536, 1e6, 1 << 24, 1 << 28, 1 << 30,
					 2147483647.0, 3e9 };	// the last clamps to the square's edge
	static const int32_t big[] = { 0, 1, -1, 2147483647, -2147483647, (int32_t)0x80000000u, 1 << 28, -(1 << 28),
				       (1 << 29) - 1, 1 << 29, 46341, -46341 };

	for (unsigned r = 0; r < sizeof radius / sizeof radius[0]; r++)
		for (int k = 0; k < 65536; k++) {
			double th = 2.0 * M_PI * (k + 0.5 * (r & 1)) / 65536.0;

			one_atan2(clamp32(radius[r] * sin(th)), clamp32(radius[r] * cos(th)));
		}
	for (int y = -64; y <= 64; y++)
		for (int x = -64; x <= 64; x++)
			one_atan2(y, x);
	for (unsigned i = 0; i < sizeof big / sizeof big[0]; i++)
		for (unsigned j = 0; j < sizeof big / sizeof big[0]; j++)
			one_atan2(big[i], big[j]);
	for (int k = 0; k < 1000000; k++) {
		int32_t y = (int32_t)rng(), x = (int32_t)rng();

		if (k & 1) {
			y >>= rng() % 32u;
			x >>= rng() % 32u;
		}
		one_atan2(y, x);
	}
	// exact on the axes
	CHECK(dsp_atan2(0, 5) == 0 && dsp_atan2(7, 0) == 0x40000000u && dsp_atan2(0, -9) == 0x80000000u &&
	      dsp_atan2(-3, 0) == 0xC0000000u && dsp_atan2((int32_t)0x80000000u, 0) == 0xC0000000u &&
	      dsp_atan2(0, (int32_t)0x80000000u) == 0x80000000u, "atan2 on the axes");
	// the diagonals: an eighth of a turn each, within the bound
	CHECK(dsp_atan2(1, 1) - 0x20000000u + 65536u <= 131072u, "atan2(1, 1) = %u", dsp_atan2(1, 1));
	CHECK(dsp_atan2(-2147483647, -2147483647) - 0xA0000000u + 65536u <= 131072u, "atan2(-max, -max) = %u",
	      dsp_atan2(-2147483647, -2147483647));
}

int main(void)
{
	test_norm32();
	test_recip();
	test_isqrt();
	test_atan2();
	printf("atan2: worst error %.0f phase units (dsp.h allows 65536)\n", worst_atan);
	printf("dspMathTest%s: %ld checks, %d failed\n",
#if defined(FXP_SPLIT16)
	       " (FXP_SPLIT16)",
#else
	       "",
#endif
	       checks, failures);
	return failures != 0;
}
