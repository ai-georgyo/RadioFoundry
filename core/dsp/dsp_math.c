// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Integer helpers of the shared DSP (dsp.h): normalize, reciprocal, square root, atan2. 32-bit operations, shifts and
// compares only: no float, no divide, no 64-bit multiply. A C-SKY core without HI/LO makes `(uint64_t)a * b` a library
// call (core/dsp/fxp.h), and a 64-bit divide is one on every core; the 64-bit square root uses 64-bit adds, compares
// and constant shifts, which both compilers do inline. tests/host/dspMathTest.c checks each against exact references.
#include "dsp.h"

int dsp_norm32(uint32_t x)
{
	return x ? __builtin_clz(x) : 32;
}

uint32_t dsp_recip(uint32_t x, int *e)
{
	uint32_t m, rem, lo, q = 0;
	int n;

	if (!x) {
		*e = 0;
		return 0x7FFFFFFFu;
	}
	n = dsp_norm32(x);
	*e = 62 - n;
	// (2^e - 1) / x = (2^62 - 2^n) / m with m = x 2^n in [2^31, 2^32): the same quotient, from a normalized divisor.
	// The numerator's high word is 2^30 - 1 < m, so the quotient fits 32 bits (it is under 2^31): restoring division of
	// the low word's 32 bits into that remainder, one bit a step. rem < m < 2^32, so 2 rem + 1 overflows 32 bits by at
	// most the carry, and the subtraction of m then wraps back to the true remainder
	m = x << n;
	rem = (1u << 30) - 1u;
	lo = 0u - (1u << n);
	for (int i = 0; i < 32; i++) {
		uint32_t c = rem >> 31;

		rem = (rem << 1) | (lo >> 31);
		lo <<= 1;
		q <<= 1;
		if (c || rem >= m) {
			rem -= m;
			q |= 1u;
		}
	}
	return q;
}

// Digit by digit, two bits of x per bit of the root (b walks the even powers of 2 down from x's top one)
uint32_t dsp_isqrt32(uint32_t x)
{
	uint32_t r = 0, b;

	if (!x)
		return 0;
	b = 1u << ((31 - dsp_norm32(x)) & ~1);
	for (; b; b >>= 2) {
		if (x >= r + b) {
			x -= r + b;
			r = (r >> 1) + b;
		} else {
			r >>= 1;
		}
	}
	return r;
}

uint32_t dsp_isqrt64(uint64_t x)
{
	uint32_t h = (uint32_t)(x >> 32);
	uint64_t r = 0, b;

	if (!h)
		return dsp_isqrt32((uint32_t)x);
	// the top even power of 2 under x, set in the high word: a 32-bit shift, no variable 64-bit one
	b = (uint64_t)(1u << ((31 - dsp_norm32(h)) & ~1)) << 32;
	for (; b; b >>= 2) {
		if (x >= r + b) {
			x -= r + b;
			r = (r >> 1) + b;
		} else {
			r >>= 1;
		}
	}
	return (uint32_t)r;
}

// atan(2^-i) in phase units (2^32 = one turn), rounded. After the 18 rotations the angle left is at most the last
// entry, 5215 units, a twelfth of the 2^16 dsp.h allows; the shifts' truncation adds a few units more
#define ATAN_STEPS	18
static const uint32_t atan_tab[ATAN_STEPS] = {
	536870912, 316933406, 167458907, 85004756, 42667331, 21354465, 10679838, 5340245, 2670163, 1335087,
	667544, 333772, 166886, 83443, 41722, 20861, 10430, 5215,
};

uint32_t dsp_atan2(int32_t y, int32_t x)
{
	// magnitudes as unsigned: |INT32_MIN| is 2^31
	uint32_t ux = x < 0 ? 0u - (uint32_t)x : (uint32_t)x, uy = y < 0 ? 0u - (uint32_t)y : (uint32_t)y;
	uint32_t mx = ux > uy ? ux : uy, a = 0;
	int32_t cx, cy;
	int s;

	// the axes exactly (and (0, 0) as 0)
	if (!uy)
		return x < 0 ? 0x80000000u : 0;
	if (!ux)
		return y < 0 ? 0xC0000000u : 0x40000000u;
	// Scale the larger magnitude into [2^28, 2^29): a vector of the first quadrant is then under 2^29.5 long, and
	// the rotations' gain (1.647) keeps it under 2^31. Small inputs gain bits, large ones lose at most three
	s = dsp_norm32(mx) - 3;
	if (s >= 0) {
		cx = (int32_t)(ux << s);
		cy = (int32_t)(uy << s);
	} else {
		cx = (int32_t)(ux >> -s);
		cy = (int32_t)(uy >> -s);
	}
	// CORDIC, vectoring: rotate (cx, cy) onto the +x axis, adding up the angles turned. The first quadrant's angle
	// [0, 2^30] is inside what the rotations reach (about 1.1 quarter turns)
	for (int i = 0; i < ATAN_STEPS; i++) {
		int32_t dx = cy >> i, dy = cx >> i;

		if (cy > 0) {
			cx += dx;
			cy -= dy;
			a += atan_tab[i];
		} else {
			cx -= dx;
			cy += dy;
			a -= atan_tab[i];
		}
	}
	// the quadrant back (mod 2^32)
	if (x < 0)
		a = 0x80000000u - a;
	return y < 0 ? 0u - a : a;
}
