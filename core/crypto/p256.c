// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ECDSA over P-256 with SHA-256 hashes (docs/spec/p256.md; FIPS 186-5 6.4, SP 800-186, SEC 1, RFC 6979 3.2).
//
// Field elements and scalars are 8 little-endian 32-bit words in Montgomery form (a * 2^256 mod m), always fully
// reduced. Points are Jacobian with a = -3. Signing (and Q = d * G) is constant time: no branch and no memory index
// depends on d, k or anything computed from them; selects are masks, table reads scan every entry. Verifying works on
// public values only and branches freely. Every temporary lives in the caller's p256_scratch_t (the layout below,
// asserted); the stack holds only frames and spilled registers. The functions that take no scratch (key checks,
// decompression) work on public values, with 128 bytes of room on the stack.
#include "config.h"

#if CONFIG_CRYPTO_P256

#include <stddef.h>
#include <string.h>
#include "crypto/p256.h"
#include "crypto/hmac_sha256.h"
#include "crypto/crypto_wipe.h"

// G, 3G, ..., 15G, affine, Montgomery form, x then y (p256_tab.c)
extern const uint32_t p256_tab_g[8][16];

// The 32 x 32 -> 64 product by CPU (core/dsp/fxp.h's rule): C-SKY has no 64-bit product in its base instructions
// (the compiler would call __muldi3), so it multiplies 16-bit halves; FXP_SPLIT16 runs that path on the host
#if (defined(__csky__) || defined(FXP_SPLIT16)) && !defined(FXP_FORCE_PLAIN)
#define P256_HALVES	1
#else
#define P256_HALVES	0
#endif
// UMAAL (ARMv7E-M, the DSP extension: Cortex-M4); other Arm cores take UMULL / UMLAL from the plain C
#if !P256_HALVES && defined(__ARM_FEATURE_DSP) && !defined(FXP_FORCE_PLAIN)
#define P256_UMAAL	1
#else
#define P256_UMAAL	0
#endif

// The host tests count the field and scalar multiplies (p256Test's timing check): the count must not depend on d
#ifdef P256_TEST_COUNT
extern unsigned long p256_test_muls;
#define COUNT_MUL()	(p256_test_muls++)
#else
#define COUNT_MUL()	((void)0)
#endif

// A value the compiler can't see through, so a mask stays a mask and never becomes a branch
#define CT_HIDE(x)	__asm__("" : "+r"(x))

#define NOINLINE	__attribute__((noinline))
#define P256_INLINE	static inline __attribute__((always_inline))
// A product row unrolled where a word product is one instruction (UMAAL) and the loop's compare and branch would cost
// as much again; on C-SKY a product is some 26 instructions and the loop overhead small next to the flash an unrolled
// row takes
#if P256_UMAAL
#define P256_UNROLL_ROW	_Pragma("GCC unroll 8")
#else
#define P256_UNROLL_ROW
#endif
// The 8-word additions, subtractions and compares unrolled everywhere: a few instructions per word, as many again for
// the loop. Half the time of a point doubling went into them before
#define P256_UNROLL	_Pragma("GCC unroll 8")

typedef uint32_t fe_t[8];
typedef struct { fe_t x, y, z; } jac_t;		// (X/Z^2, Y/Z^3); Z = 0 is the identity
typedef struct { fe_t x, y; } aff_t;

#define NAF_LEN		257u	// digits of a width-w NAF of a value below 2^256
#define POW_TAB		15u	// a^1 .. a^15 for the inversion mod n

// The scratch, by phase. Signing: RFC 6979's HMAC, then k * G, then the inversion of k, the scalars beside them.
// Verifying: the inversion of s, then the NAF digits, Q's table and the accumulator
typedef struct {
	fe_t k, e, r, s, d;
	union {
		struct {
			hmac_sha256_t h;
			uint8_t key[32], v[32], hb[32];		// RFC 6979's K and V, bits2octets(H)
		} rfc;
		struct {
			jac_t acc, dbl;
			aff_t sel;
			fe_t kn;			// the odd scalar
			int8_t dig[64];			// its signed digits
		} kg;
		fe_t pow[POW_TAB];
	} u;
} sign_t;

typedef struct {
	fe_t u, v, r;
	jac_t acc;
	aff_t sel;
	union {
		fe_t pow[POW_TAB];
		struct {
			jac_t qt[4];			// Q, 3Q, 5Q, 7Q
			int8_t nu[NAF_LEN + 3], nv[NAF_LEN + 3];
		} m;
	} x;
} verify_t;

typedef struct {
	uint32_t w[16];		// a product before its reduction
	fe_t t[6];		// the point formulas' and the inversion's temporaries
	union {
		sign_t sg;
		verify_t vf;
	} u;
} ctx_t;

// 8 bytes of room: the context is aligned to 8 inside the scratch (sha256_t has a 64-bit count)
_Static_assert(sizeof(ctx_t) + 8u <= P256_SCRATCH_BYTES, "p256: the context doesn't fit p256_scratch_t");

static ctx_t *ctx_of(p256_scratch_t *s)
{
	return (ctx_t *)(((uintptr_t)s + 7u) & ~(uintptr_t)7u);
}

static const uint32_t P[8] = {
	0xffffffffu, 0xffffffffu, 0xffffffffu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0xffffffffu,
};
static const uint32_t N[8] = {
	0xfc632551u, 0xf3b9cac2u, 0xa7179e84u, 0xbce6faadu, 0xffffffffu, 0xffffffffu, 0x00000000u, 0xffffffffu,
};
#define N0	0xee00bc4fu	// -n^-1 mod 2^32 (-p^-1 is 1)
// n - 2, the inversion exponent
static const uint32_t NM2[8] = {
	0xfc63254fu, 0xf3b9cac2u, 0xa7179e84u, 0xbce6faadu, 0xffffffffu, 0xffffffffu, 0x00000000u, 0xffffffffu,
};
// 2^512 mod p and mod n: into Montgomery form
static const uint32_t RR_P[8] = {
	0x00000003u, 0x00000000u, 0xffffffffu, 0xfffffffbu, 0xfffffffeu, 0xffffffffu, 0xfffffffdu, 0x00000004u,
};
static const uint32_t RR_N[8] = {
	0xbe79eea2u, 0x83244c95u, 0x49bd6fa6u, 0x4699799cu, 0x2b6bec59u, 0x2845b239u, 0xf3d95620u, 0x66e12d94u,
};
// 1, 3 and b in Montgomery form mod p; 1 as a plain value (out of Montgomery form by a multiply with it)
static const uint32_t ONE_M[8] = {
	0x00000001u, 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xfffffffeu, 0x00000000u,
};
static const uint32_t THREE_M[8] = {
	0x00000003u, 0x00000000u, 0x00000000u, 0xfffffffdu, 0xffffffffu, 0xffffffffu, 0xfffffffcu, 0x00000002u,
};
static const uint32_t B_M[8] = {
	0x29c4bddfu, 0xd89cdf62u, 0x78843090u, 0xacf005cdu, 0xf7212ed6u, 0xe5a220abu, 0x04874834u, 0xdc30061du,
};
static const uint32_t ONE[8] = { 1u };
static const uint32_t ZERO[8] = { 0u };
// n as 32 big-endian bytes, for the range checks on encoded scalars
static const uint8_t N_BE[32] = {
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};

// ---- Words ----

// 0 or 1 to 0 or all ones
P256_INLINE uint32_t ct_mask(uint32_t bit)
{
	uint32_t m = 0u - bit;
	CT_HIDE(m);
	return m;
}

// 1 when x is 0: no carry out of x + 2^32 - 1. An add makes the bit without a test, where a compiler shown the usual
// (x | -x) >> 31 recognizes x != 0 and makes it a compare and a conditional instruction (seen on Arm)
P256_INLINE uint32_t ct_zero32(uint32_t x)
{
#if P256_HALVES
	uint32_t s = x + 0xffffffffu, z = (s >= x);
#else
	uint32_t z = (uint32_t)(((uint64_t)x + 0xffffffffu) >> 32) ^ 1u;
#endif

	CT_HIDE(z);
	return z;
}

// a * b + c + *hi: the low word returned, the high one in *hi (at most 2^64 - 1, never overflows). Halves: four 16 x 16
// products, the carries by compares (C-SKY sets them without a branch); kept in 32-bit words, since 64-bit values there
// take register pairs and the code spills. Cortex-M4: UMAAL is this whole operation, one instruction
P256_INLINE uint32_t mac(uint32_t a, uint32_t b, uint32_t c, uint32_t *hi)
{
#if P256_HALVES
	uint32_t al = a & 0xffffu, ah = a >> 16, bl = b & 0xffffu, bh = b >> 16;
	uint32_t lo = al * bl, mid = al * bh, h = ah * bh, t = ah * bl;

	mid += t;
	h += (uint32_t)(mid < t) << 16;
	t = mid << 16;
	lo += t;
	h += (mid >> 16) + (lo < t);
	lo += c;
	h += lo < c;
	lo += *hi;
	h += lo < *hi;
	*hi = h;
	return lo;
#elif P256_UMAAL
	uint32_t lo = c, h = *hi;

	__asm__("umaal %0, %1, %2, %3" : "+r"(lo), "+r"(h) : "r"(a), "r"(b));
	*hi = h;
	return lo;
#else
	uint64_t x = (uint64_t)a * b + c + *hi;

	*hi = (uint32_t)(x >> 32);
	return (uint32_t)x;
#endif
}

// a + b + *c, the carry out in *c (*c may be up to 2 on the way in). 64-bit sums where the compiler makes them an add
// with carry (Arm, the host); compares on C-SKY
P256_INLINE uint32_t adc(uint32_t a, uint32_t b, uint32_t *c)
{
#if P256_HALVES
	uint32_t s = a + *c, k = s < *c;

	s += b;
	*c = k + (s < b);
	return s;
#else
	uint64_t s = (uint64_t)a + b + *c;

	*c = (uint32_t)(s >> 32);
	return (uint32_t)s;
#endif
}

// a - b - *br, the borrow (0 or 1) out in *br
P256_INLINE uint32_t sbb(uint32_t a, uint32_t b, uint32_t *br)
{
#if P256_HALVES
	uint32_t d = a - b, k = a < b;

	k += d < *br;
	d -= *br;
	*br = k;
	return d;
#else
	uint64_t d = (uint64_t)a - b - *br;

	*br = (uint32_t)(d >> 32) & 1u;
	return (uint32_t)d;
#endif
}

// A running column sum: 64 bits where the compiler adds those with a carry (Arm, the host), a word and a count of its
// carries on C-SKY
#if P256_HALVES
typedef struct { uint32_t lo, hi; } acc_t;
#define ACC_ZERO	{ 0, 0 }
P256_INLINE void acc_add(acc_t *a, uint32_t x)
{
	a->lo += x;
	a->hi += a->lo < x;
}
P256_INLINE uint32_t acc_low(const acc_t *a)
{
	return a->lo;
}
P256_INLINE void acc_shift(acc_t *a)
{
	a->lo = a->hi;
	a->hi = 0;
}
#else
typedef uint64_t acc_t;
#define ACC_ZERO	0
P256_INLINE void acc_add(acc_t *a, uint32_t x)
{
	*a += x;
}
P256_INLINE uint32_t acc_low(const acc_t *a)
{
	return (uint32_t)*a;
}
P256_INLINE void acc_shift(acc_t *a)
{
	*a >>= 32;
}
#endif

static void be_load(uint32_t w[8], const uint8_t *b)
{
	for (unsigned i = 0; i < 8; i++) {
		const uint8_t *p = b + 28 - 4 * i;
		w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
	}
}

static void be_store(uint8_t *b, const uint32_t w[8])
{
	for (unsigned i = 0; i < 8; i++) {
		uint8_t *p = b + 28 - 4 * i;
		p[0] = (uint8_t)(w[i] >> 24);
		p[1] = (uint8_t)(w[i] >> 16);
		p[2] = (uint8_t)(w[i] >> 8);
		p[3] = (uint8_t)w[i];
	}
}

// 1 when a < m (the borrow of a - m)
static uint32_t lt(const uint32_t a[8], const uint32_t m[8])
{
	uint32_t b = 0;

	P256_UNROLL
	for (unsigned i = 0; i < 8; i++)
		(void)sbb(a[i], m[i], &b);
	return b;
}

static uint32_t is_zero(const uint32_t a[8])
{
	uint32_t x = 0;

	P256_UNROLL
	for (unsigned i = 0; i < 8; i++)
		x |= a[i];
	return ct_zero32(x);
}

// r = hi * 2^256 + a - m when that is not negative, else a; the caller knows the value is below 2m. Two passes: the
// borrow first, then the subtraction of m masked, so r may be a and nothing else is written
P256_INLINE void cond_sub_i(uint32_t r[8], const uint32_t a[8], uint32_t hi, const uint32_t m[8])
{
	uint32_t mask = ct_mask(hi | (lt(a, m) ^ 1u)), b = 0;

	P256_UNROLL
	for (unsigned i = 0; i < 8; i++)
		r[i] = sbb(a[i], m[i] & mask, &b);
}

// The same out of line, for everything but the reduction mod p (one copy of the unrolled code)
static NOINLINE void cond_sub(uint32_t r[8], const uint32_t a[8], uint32_t hi, const uint32_t m[8])
{
	cond_sub_i(r, a, hi, m);
}

// r = a + b mod m, inputs below m; r may be either. Out of line: a point doubling has 16 additions and subtractions
static NOINLINE void mod_add(uint32_t r[8], const uint32_t a[8], const uint32_t b[8], const uint32_t m[8])
{
	uint32_t c = 0;

	P256_UNROLL
	for (unsigned i = 0; i < 8; i++)
		r[i] = adc(a[i], b[i], &c);
	cond_sub(r, r, c, m);
}

// r = a - b mod m, inputs below m; r may be either
static NOINLINE void mod_sub(uint32_t r[8], const uint32_t a[8], const uint32_t b[8], const uint32_t m[8])
{
	uint32_t br = 0, mask, c = 0;

	P256_UNROLL
	for (unsigned i = 0; i < 8; i++)
		r[i] = sbb(a[i], b[i], &br);
	mask = ct_mask(br);
	P256_UNROLL
	for (unsigned i = 0; i < 8; i++)
		r[i] = adc(r[i], m[i] & mask, &c);
}

// r = mask ? a : r
static void cmov(uint32_t *r, const uint32_t *a, unsigned words, uint32_t mask)
{
	for (unsigned i = 0; i < words; i++)
		r[i] = (a[i] & mask) | (r[i] & ~mask);
}

// ---- Montgomery multiplication ----

// w = a * b, 16 words (operand scanning: one row of a_i * b + w + carry per word of a; the first row starts from 0)
static void mul_wide(uint32_t w[16], const uint32_t a[8], const uint32_t b[8])
{
	uint32_t c = 0;

	P256_UNROLL_ROW
	for (unsigned j = 0; j < 8; j++)
		w[j] = mac(a[0], b[j], 0, &c);
	w[8] = c;
	for (unsigned i = 1; i < 8; i++) {
		uint32_t ai = a[i];
		c = 0;
		P256_UNROLL_ROW
		for (unsigned j = 0; j < 8; j++)
			w[i + j] = mac(ai, b[j], w[i + j], &c);
		w[i + 8] = c;
	}
}

// w = a^2 in 36 word products instead of 64: each a_i a_j (i < j) once, the sum doubled by a shift, then the squares
// a_i^2 added. Row i adds a_i * a_(i+1..7) at word 2i + 1 and leaves its carry in word i + 8, which no earlier row
// has written
static void sqr_wide(uint32_t w[16], const uint32_t a[8])
{
	uint32_t c = 0;

	w[0] = 0;
	P256_UNROLL_ROW
	for (unsigned j = 1; j < 8; j++)
		w[j] = mac(a[0], a[j], 0, &c);
	w[8] = c;
	P256_UNROLL_ROW
	for (unsigned i = 1; i < 7; i++) {
		uint32_t ai = a[i];
		c = 0;
		P256_UNROLL_ROW
		for (unsigned j = i + 1; j < 8; j++)
			w[i + j] = mac(ai, a[j], w[i + j], &c);
		w[i + 8] = c;
	}
	w[15] = 0;
	P256_UNROLL_ROW
	for (unsigned j = 15; j > 0; j--)
		w[j] = w[j] << 1 | w[j - 1] >> 31;	// below 2^511 before: no bit lost
	c = 0;
	P256_UNROLL_ROW
	for (unsigned i = 0; i < 8; i++) {
		uint32_t hi = 0, lo = mac(a[i], a[i], 0, &hi);
		w[2 * i] = adc(w[2 * i], lo, &c);
		w[2 * i + 1] = adc(w[2 * i + 1], hi, &c);
	}
}

// r = w * 2^-256 mod p (p256.md 4.3). With -p^-1 mod 2^32 = 1 round i's factor m_i is word i as the rounds before
// left it, and m * p = m * 2^256 - m * 2^224 + m * 2^192 + m * 2^96 - m: the -m clears that word exactly, the rest adds
// m at words i + 3 and i + 6 and m * (2^32 - 1) at words i + 7 and i + 8 (low word -m, high word m - 1; 0 and 0 for
// m = 0). No multiply. Summed a column at a time instead of a round at a time: column j takes w_j, the carry and the
// m_i that land on it, and for j < 8 its low word is m_j (kept in w_j for the columns after) and leaves; for j >= 8 it
// is the result's word j - 8. The sum stays below 2p (inputs below p): one conditional subtraction
static void redc_p(uint32_t r[8], uint32_t w[16])
{
	acc_t a = ACC_ZERO;

#pragma GCC unroll 16
	for (unsigned j = 0; j < 16; j++) {
		acc_add(&a, w[j]);
		if (j >= 3 && j < 11)
			acc_add(&a, w[j - 3]);
		if (j >= 6 && j < 14)
			acc_add(&a, w[j - 6]);
		if (j >= 7 && j < 15)
			acc_add(&a, 0u - w[j - 7]);
		if (j >= 8)
			acc_add(&a, w[j - 8] - (ct_zero32(w[j - 8]) ^ 1u));
		w[j] = acc_low(&a);
		acc_shift(&a);
	}
	cond_sub_i(r, w + 8, acc_low(&a), P);
}

// r = w * 2^-256 mod n: the generic rounds, m = w_i * (-n^-1) and w += m * n * 2^(32 i), the carry deferred the same way
static void redc_n(uint32_t r[8], uint32_t w[16])
{
	uint32_t k = 0;

	for (unsigned i = 0; i < 8; i++) {
		uint32_t m = w[i] * N0, c = 0;

		P256_UNROLL_ROW
		for (unsigned j = 0; j < 8; j++)
			w[i + j] = mac(m, N[j], w[i + j], &c);
		w[i + 8] = adc(w[i + 8], c, &k);
	}
	cond_sub(r, w + 8, k, N);
}

// r = a * b * 2^-256 mod p; w is the product's room in the scratch, so r may be a or b
static NOINLINE void fe_mul(uint32_t *w, uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{
	COUNT_MUL();
	mul_wide(w, a, b);
	redc_p(r, w);
}

static NOINLINE void fe_sqr(uint32_t *w, uint32_t r[8], const uint32_t a[8])
{
	COUNT_MUL();
	sqr_wide(w, a);
	redc_p(r, w);
}

static NOINLINE void sc_mul(uint32_t *w, uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{
	COUNT_MUL();
	mul_wide(w, a, b);
	redc_n(r, w);
}

static NOINLINE void sc_sqr(uint32_t *w, uint32_t r[8], const uint32_t a[8])
{
	COUNT_MUL();
	sqr_wide(w, a);
	redc_n(r, w);
}

#define fe_add(r, a, b)		mod_add(r, a, b, P)
#define fe_sub(r, a, b)		mod_sub(r, a, b, P)
#define fe_neg(r, a)		mod_sub(r, ZERO, a, P)

// r = a^(2^n)
static void fe_sqr_n(uint32_t *w, uint32_t r[8], const uint32_t a[8], unsigned n)
{
	fe_sqr(w, r, a);
	while (--n)
		fe_sqr(w, r, r);
}

// r = a^(p-2) = a^-1 (Montgomery in, Montgomery out; 0 gives 0), the chain of p256.md 4.4 on runs of ones: 255
// squarings and 13 multiplications for every a. t[0..4] hold a^(2^k - 1) for k = 2, 4, 8, 16, 32; r is not a or one
// of them
static void fe_inv(ctx_t *c, uint32_t r[8], const uint32_t a[8])
{
	uint32_t *w = c->w;
	fe_t *t = c->t;

	fe_sqr(w, t[0], a);
	fe_mul(w, t[0], t[0], a);
	fe_sqr_n(w, t[1], t[0], 2);
	fe_mul(w, t[1], t[1], t[0]);
	fe_sqr_n(w, t[2], t[1], 4);
	fe_mul(w, t[2], t[2], t[1]);
	fe_sqr_n(w, t[3], t[2], 8);
	fe_mul(w, t[3], t[3], t[2]);
	fe_sqr_n(w, t[4], t[3], 16);
	fe_mul(w, t[4], t[4], t[3]);
	// p - 2: 32 ones, 31 zeros and a one, 96 zeros, 94 ones, then 01
	fe_sqr_n(w, r, t[4], 32);
	fe_mul(w, r, r, a);
	fe_sqr_n(w, r, r, 96);
	fe_sqr_n(w, r, r, 32);
	fe_mul(w, r, r, t[4]);
	fe_sqr_n(w, r, r, 32);
	fe_mul(w, r, r, t[4]);
	fe_sqr_n(w, r, r, 16);
	fe_mul(w, r, r, t[3]);
	fe_sqr_n(w, r, r, 8);
	fe_mul(w, r, r, t[2]);
	fe_sqr_n(w, r, r, 4);
	fe_mul(w, r, r, t[1]);
	fe_sqr_n(w, r, r, 2);
	fe_mul(w, r, r, t[0]);
	fe_sqr_n(w, r, r, 2);
	fe_mul(w, r, r, a);
}

// r = a^(n-2) mod n = a^-1 (Montgomery form mod n), a 4-bit fixed window over the public exponent: the exponent's
// nibbles pick the table entries and the branches, never a. pow is the 15-entry table's room; r may be a
static void sc_inv(uint32_t *w, uint32_t r[8], const uint32_t a[8], fe_t *pow)
{
	memcpy(pow[0], a, sizeof pow[0]);
	sc_sqr(w, pow[1], a);
	for (unsigned i = 2; i < POW_TAB; i++)
		sc_mul(w, pow[i], pow[i - 1], a);
	memcpy(r, pow[(NM2[7] >> 28) - 1], sizeof pow[0]);
	for (int i = 62; i >= 0; i--) {
		unsigned nib = (NM2[i >> 3] >> (4 * (i & 7))) & 15u;
		for (unsigned j = 0; j < 4; j++)
			sc_sqr(w, r, r);
		if (nib)
			sc_mul(w, r, r, pow[nib - 1]);
	}
}

// ---- Points ----

// r = 2a (dbl-2001-b for a = -3: 3M + 5S). No exceptional input: the identity (Z = 0) doubles to Z = 0, and P-256
// has no point of order 2. r may be a. Uses t[0..3]
static void pt_dbl(ctx_t *c, jac_t *r, const jac_t *a)
{
	uint32_t *w = c->w;
	fe_t *t = c->t;

	fe_sqr(w, t[0], a->z);			// delta = Z^2
	fe_sqr(w, t[1], a->y);			// gamma = Y^2
	fe_mul(w, t[2], a->x, t[1]);		// beta = X gamma
	fe_add(t[3], a->y, a->z);
	fe_sqr(w, t[3], t[3]);
	fe_sub(t[3], t[3], t[1]);
	fe_sub(r->z, t[3], t[0]);		// Z3 = (Y + Z)^2 - gamma - delta
	fe_sub(t[3], a->x, t[0]);
	fe_add(t[0], a->x, t[0]);
	fe_mul(w, t[3], t[3], t[0]);
	fe_add(t[0], t[3], t[3]);
	fe_add(t[3], t[0], t[3]);		// alpha = 3 (X - delta)(X + delta)
	fe_add(t[2], t[2], t[2]);
	fe_add(t[2], t[2], t[2]);		// 4 beta
	fe_add(t[0], t[2], t[2]);		// 8 beta
	fe_sqr(w, r->x, t[3]);
	fe_sub(r->x, r->x, t[0]);		// X3 = alpha^2 - 8 beta
	fe_sub(t[2], t[2], r->x);
	fe_mul(w, t[2], t[2], t[3]);
	fe_sqr(w, t[1], t[1]);
	fe_add(t[1], t[1], t[1]);
	fe_add(t[1], t[1], t[1]);
	fe_add(t[1], t[1], t[1]);		// 8 gamma^2
	fe_sub(r->y, t[2], t[1]);		// Y3 = alpha (4 beta - X3) - 8 gamma^2
}

// r = a + b with b affine (8M + 3S). Returns all ones when a = b (H = 0 and R = 0: the formula gives 0 : 0 : 0 and
// the caller must double instead), else 0; a = -b gives Z3 = 0, the identity, by itself; a must not be the identity.
// Constant time, the returned mask too. r may be a. Uses t[0..4]
static uint32_t pt_madd(ctx_t *c, jac_t *r, const jac_t *a, const aff_t *b)
{
	uint32_t *w = c->w, same;
	fe_t *t = c->t;

	fe_sqr(w, t[0], a->z);
	fe_mul(w, t[1], t[0], a->z);
	fe_mul(w, t[0], t[0], b->x);		// U2 = x2 Z1^2
	fe_mul(w, t[1], t[1], b->y);		// S2 = y2 Z1^3
	fe_sub(t[0], t[0], a->x);		// H = U2 - X1
	fe_sub(t[1], t[1], a->y);		// R = S2 - Y1
	same = ct_mask(is_zero(t[0]) & is_zero(t[1]));
	fe_mul(w, r->z, a->z, t[0]);		// Z3 = Z1 H
	fe_sqr(w, t[2], t[0]);
	fe_mul(w, t[3], t[2], t[0]);		// H^3
	fe_mul(w, t[2], t[2], a->x);		// U = X1 H^2
	fe_sqr(w, t[4], t[1]);
	fe_sub(t[4], t[4], t[3]);
	fe_sub(t[4], t[4], t[2]);
	fe_sub(r->x, t[4], t[2]);		// X3 = R^2 - H^3 - 2U
	fe_sub(t[2], t[2], r->x);
	fe_mul(w, t[2], t[2], t[1]);
	fe_mul(w, t[3], t[3], a->y);
	fe_sub(r->y, t[2], t[3]);		// Y3 = R (U - X3) - Y1 H^3
	return same;
}

// r = a + b, both Jacobian, variable time (verifying only): the identity and a = -b by branches; a = b returns true
// with r untouched, and the caller doubles (one call level less on the stack). neg adds -b. r may be a, not b. Uses
// t[0..5]
static bool pt_add_vt(ctx_t *c, jac_t *r, const jac_t *a, const jac_t *b, bool neg)
{
	uint32_t *w = c->w;
	fe_t *t = c->t;

	if (is_zero(b->z)) {
		if (r != a)
			*r = *a;
		return false;
	}
	if (is_zero(a->z)) {
		*r = *b;
		if (neg)
			fe_neg(r->y, r->y);
		return false;
	}
	fe_sqr(w, t[0], a->z);			// Z1^2
	fe_sqr(w, t[1], b->z);			// Z2^2
	fe_mul(w, t[2], a->x, t[1]);		// U1
	fe_mul(w, t[3], b->x, t[0]);		// U2
	fe_mul(w, t[4], t[1], b->z);
	fe_mul(w, t[4], t[4], a->y);		// S1
	fe_mul(w, t[5], t[0], a->z);
	fe_mul(w, t[5], t[5], b->y);		// S2
	if (neg)
		fe_neg(t[5], t[5]);
	fe_sub(t[3], t[3], t[2]);		// H
	fe_sub(t[5], t[5], t[4]);		// R
	if (is_zero(t[3])) {
		if (is_zero(t[5]))
			return true;
		memset(r->z, 0, sizeof r->z);
		return false;
	}
	fe_mul(w, t[0], a->z, b->z);
	fe_mul(w, r->z, t[0], t[3]);		// Z3 = Z1 Z2 H
	fe_sqr(w, t[0], t[3]);
	fe_mul(w, t[1], t[0], t[3]);		// H^3
	fe_mul(w, t[2], t[2], t[0]);		// U1 H^2
	fe_sqr(w, t[3], t[5]);
	fe_sub(t[3], t[3], t[1]);
	fe_sub(t[3], t[3], t[2]);
	fe_sub(r->x, t[3], t[2]);		// X3 = R^2 - H^3 - 2 U1 H^2
	fe_sub(t[2], t[2], r->x);
	fe_mul(w, t[2], t[2], t[5]);
	fe_mul(w, t[1], t[4], t[1]);
	fe_sub(r->y, t[2], t[1]);		// Y3 = R (U1 H^2 - X3) - S1 H^3
	return false;
}

// x, y = the affine coordinates of a, out of Montgomery form (a not the identity). Uses t[0..5]
static void pt_affine(ctx_t *c, uint32_t x[8], uint32_t y[8], const jac_t *a)
{
	uint32_t *w = c->w;
	fe_t *t = c->t;

	fe_inv(c, t[5], a->z);
	fe_sqr(w, t[0], t[5]);
	fe_mul(w, x, a->x, t[0]);
	fe_mul(w, t[0], t[0], t[5]);
	fe_mul(w, y, a->y, t[0]);
	fe_mul(w, x, x, ONE);
	fe_mul(w, y, y, ONE);
}

// sel = |d| G, negated when d < 0, for an odd digit d in [-15, 15]: every entry is read and one kept by mask
static void g_select(aff_t *sel, int32_t d)
{
	uint32_t v = (uint32_t)d, neg = v >> 31, mag = (v ^ (0u - neg)) + neg, idx = mag >> 1, b = 0, m;

	memset(sel, 0, sizeof *sel);
	for (uint32_t j = 0; j < 8; j++) {
		m = ct_mask(ct_zero32(j ^ idx));
		for (unsigned i = 0; i < 8; i++) {
			sel->x[i] |= p256_tab_g[j][i] & m;
			sel->y[i] |= p256_tab_g[j][8 + i] & m;
		}
	}
	// y = p - y when negative (y is never 0: no point of order 2)
	m = ct_mask(neg);
	for (unsigned i = 0; i < 8; i++) {
		uint32_t n = sbb(P[i], sel->y[i], &b);
		sel->y[i] = (n & m) | (sel->y[i] & ~m);
	}
}

// x, y = k G, constant time (p256.md 6.1), 1 <= k <= n - 1, x and y out of Montgomery form. k is replaced by n - k
// when even and the result's y negated, so k is odd and so are its 64 signed digits. The recoding of 6.1 step 2 in
// closed form: digit i is k's bits 4i .. 4i+4 with bit 4i set, less 16; the top digit is k's bits 252-255 with bit 252
// set, in [1, 15]. Then 63 times: four doublings and a mixed addition of the digit's table point.
//
// Exceptional additions (p256.md 5): before digit i is added the accumulator is 16 m G, m the value of the digits
// above i (odd, so m >= 1). For i >= 1, 16 m < n / 16 + 16: it is neither +-d nor n +- d for |d| <= 15, so those
// additions never meet a = +-b or the identity, for any k. The last one can (k = n - 2 is odd, ends in digit -1 and
// adds -G to -G): it is complete, computing the doubling too and selecting by mask. The same instructions run for
// every k
static void mul_g(ctx_t *c, const uint32_t k[8], uint32_t x[8], uint32_t y[8])
{
	sign_t *g = &c->u.sg;
	jac_t *acc = &g->u.kg.acc;
	aff_t *sel = &g->u.kg.sel;
	uint32_t *kn = g->u.kg.kn, even = ct_mask((k[0] & 1u) ^ 1u), b = 0, inf, same;

	for (unsigned i = 0; i < 8; i++)
		kn[i] = (sbb(N[i], k[i], &b) & even) | (k[i] & ~even);
	for (unsigned i = 0; i < 63; i++) {
		unsigned bit = 4 * i, sh = bit & 31u;
		uint32_t v = kn[bit >> 5] >> sh;
		if (sh > 27)
			v |= kn[(bit >> 5) + 1] << (32u - sh);
		g->u.kg.dig[i] = (int8_t)((int32_t)((v & 0x1eu) | 1u) - 16);
	}
	g->u.kg.dig[63] = (int8_t)((kn[7] >> 28) | 1u);

	g_select(sel, g->u.kg.dig[63]);
	memcpy(acc->x, sel->x, sizeof acc->x);
	memcpy(acc->y, sel->y, sizeof acc->y);
	memcpy(acc->z, ONE_M, sizeof acc->z);
	for (int i = 62; i >= 0; i--) {
		for (unsigned j = 0; j < 4; j++)
			pt_dbl(c, acc, acc);
		g_select(sel, g->u.kg.dig[i]);
		if (i)
			(void)pt_madd(c, acc, acc, sel);
	}
	// The last digit, complete: the doubling when acc = sel, sel itself when acc is the identity
	inf = ct_mask(is_zero(acc->z));
	pt_dbl(c, &g->u.kg.dbl, acc);
	same = pt_madd(c, acc, acc, sel);
	cmov(acc->x, g->u.kg.dbl.x, 8, same);
	cmov(acc->y, g->u.kg.dbl.y, 8, same);
	cmov(acc->z, g->u.kg.dbl.z, 8, same);
	cmov(acc->x, sel->x, 8, inf);
	cmov(acc->y, sel->y, 8, inf);
	cmov(acc->z, ONE_M, 8, inf);
	// -R for an even k
	b = 0;
	for (unsigned i = 0; i < 8; i++) {
		uint32_t n = sbb(P[i], acc->y[i], &b);
		acc->y[i] = (n & even) | (acc->y[i] & ~even);
	}
	pt_affine(c, x, y, acc);
}

// ---- Keys and encodings ----

// 1 when the 32-byte big-endian value is in [1, n - 1], without a branch on it (d is secret)
static uint32_t scalar_ok(const uint8_t v[32])
{
	uint32_t b = 0, any = 0;

	for (int i = 31; i >= 0; i--) {
		b = ((uint32_t)v[i] - N_BE[i] - b) >> 31;
		any |= v[i];
	}
	return b & (ct_zero32(any) ^ 1u);
}

bool p256_priv_valid(const uint8_t d[P256_BYTES])
{
	return scalar_ok(d) != 0;
}

// The point q (64 bytes) on the curve with coordinates below p (p256.md 9). t is two field elements of room, w a
// product's. Public values: branches are fine
static bool on_curve(uint32_t *w, fe_t *t, const uint8_t q[P256_PUB_BYTES])
{
	be_load(t[0], q);
	be_load(t[1], q + 32);
	if (!lt(t[0], P) || !lt(t[1], P))
		return false;
	fe_mul(w, t[0], t[0], RR_P);
	fe_sqr(w, t[1], t[0]);
	fe_sub(t[1], t[1], THREE_M);
	fe_mul(w, t[1], t[1], t[0]);
	fe_add(t[1], t[1], B_M);		// x^3 - 3x + b
	be_load(t[0], q + 32);
	fe_mul(w, t[0], t[0], RR_P);
	fe_sqr(w, t[0], t[0]);			// y^2
	return memcmp(t[0], t[1], sizeof t[1]) == 0;
}

// No scratch: a product and two elements on the stack (public values)
typedef struct {
	uint32_t w[16];
	fe_t t[2];
} small_t;

bool p256_pub_valid(const uint8_t q[P256_PUB_BYTES])
{
	small_t m;

	return on_curve(m.w, m.t, q);
}

// y = c^((p+1)/4) for c = x^3 - 3x + b: (p + 1) / 4 = 2^254 - 2^222 + 2^190 + 2^94 is 32 ones, 31 zeros, a one, 95
// zeros, a one and 94 zeros, taken bit by bit (253 squarings, 33 multiplies) so that two elements of room do. y goes
// through q's second half while y^2 is compared with c
bool p256_pub_decompress(const uint8_t cq[P256_PUB_C_BYTES], uint8_t q[P256_PUB_BYTES])
{
	small_t m;
	uint32_t *w = m.w;
	fe_t *t = m.t;

	if (cq[0] != 2 && cq[0] != 3)
		return false;
	be_load(t[0], cq + 1);
	if (!lt(t[0], P))
		return false;
	fe_mul(w, t[0], t[0], RR_P);
	fe_sqr(w, t[1], t[0]);
	fe_sub(t[1], t[1], THREE_M);
	fe_mul(w, t[1], t[1], t[0]);
	fe_add(t[1], t[1], B_M);		// c
	memcpy(t[0], t[1], sizeof t[0]);
	for (unsigned i = 1; i < 32; i++) {
		fe_sqr(w, t[0], t[0]);
		fe_mul(w, t[0], t[0], t[1]);	// c^(2^(i+1) - 1)
	}
	fe_sqr_n(w, t[0], t[0], 32);
	fe_mul(w, t[0], t[0], t[1]);
	fe_sqr_n(w, t[0], t[0], 96);
	fe_mul(w, t[0], t[0], t[1]);
	fe_sqr_n(w, t[0], t[0], 94);		// y (Montgomery)
	fe_mul(w, t[0], t[0], ONE);
	be_store(q + 32, t[0]);
	fe_sqr(w, t[0], t[0]);
	fe_mul(w, t[0], t[0], RR_P);		// y^2, plain
	fe_mul(w, t[1], t[1], ONE);		// c, plain
	if (memcmp(t[0], t[1], sizeof t[1]) != 0) {
		memset(q, 0, P256_PUB_BYTES);
		return false;			// c is not a square: no point with this x
	}
	be_load(t[0], q + 32);
	if ((t[0][0] & 1u) != (cq[0] & 1u))
		fe_neg(t[0], t[0]);		// y is never 0: no point of order 2
	memcpy(q, cq + 1, P256_BYTES);
	be_store(q + 32, t[0]);
	return true;
}

void p256_pub_compress(const uint8_t q[P256_PUB_BYTES], uint8_t c[P256_PUB_C_BYTES])
{
	c[0] = (uint8_t)(2u | (q[63] & 1u));
	memcpy(c + 1, q, P256_BYTES);
}

bool p256_pub_from_priv(const uint8_t d[P256_BYTES], uint8_t q[P256_PUB_BYTES], p256_scratch_t *s)
{
	ctx_t *c = ctx_of(s);
	bool ok = p256_priv_valid(d);

	if (ok) {
		be_load(c->u.sg.k, d);
		mul_g(c, c->u.sg.k, c->u.sg.r, c->u.sg.s);
		be_store(q, c->u.sg.r);
		be_store(q + 32, c->u.sg.s);
	}
	p256_wipe(s);
	return ok;
}

// ---- Signing ----

// RFC 6979 3.2 with HMAC-SHA-256, qlen = hlen = 256: k into g->k. d is valid. bits2octets(H) is H mod n (H < 2n)
static void rfc6979(ctx_t *c, const uint8_t d[P256_BYTES], const uint8_t h[P256_BYTES])
{
	sign_t *g = &c->u.sg;
	hmac_sha256_t *m = &g->u.rfc.h;
	uint8_t *key = g->u.rfc.key, *v = g->u.rfc.v, sep;

	be_load(g->e, h);
	cond_sub(g->e, g->e, 0, N);
	be_store(g->u.rfc.hb, g->e);
	memset(v, 0x01, 32);
	memset(key, 0x00, 32);
	// steps d-g: K = HMAC_K(V || sep || x || h1) and V = HMAC_K(V), sep 00 then 01
	for (sep = 0; sep < 2; sep++) {
		hmac_sha256_init(m, key, 32);
		hmac_sha256_update(m, v, 32);
		hmac_sha256_update(m, &sep, 1);
		hmac_sha256_update(m, d, 32);
		hmac_sha256_update(m, g->u.rfc.hb, 32);
		hmac_sha256_final(m, key);
		hmac_sha256_init(m, key, 32);
		hmac_sha256_update(m, v, 32);
		hmac_sha256_final(m, v);
	}
	// step h: a candidate per V, taken whole when in [1, n - 1] (about 2^-32 of them aren't)
	for (;;) {
		hmac_sha256_init(m, key, 32);
		hmac_sha256_update(m, v, 32);
		hmac_sha256_final(m, v);
		if (scalar_ok(v))
			break;
		sep = 0;
		hmac_sha256_init(m, key, 32);
		hmac_sha256_update(m, v, 32);
		hmac_sha256_update(m, &sep, 1);
		hmac_sha256_final(m, key);
		hmac_sha256_init(m, key, 32);
		hmac_sha256_update(m, v, 32);
		hmac_sha256_final(m, v);
	}
	be_load(g->k, v);
}

// FIPS 186-5 6.4.1: r = x(k G) mod n, s = k^-1 (e + r d) mod n, with k given (and checked) or, k NULL, RFC 6979's
// already in the context. False for an invalid d or k, or r or s of 0; rs is zeros then. The scratch is wiped either
// way. p256_sign runs RFC 6979 from its own frame first, so the HMAC's stack and this function's never add up
static bool sign(const uint8_t d[P256_BYTES], const uint8_t h[P256_BYTES], const uint8_t *k, uint8_t rs[P256_SIG_BYTES],
		 p256_scratch_t *s)
{
	ctx_t *c = ctx_of(s);
	sign_t *g = &c->u.sg;
	uint32_t *w = c->w;
	bool ok = p256_priv_valid(d) && (!k || scalar_ok(k));

	if (ok) {
		if (k)
			be_load(g->k, k);
		mul_g(c, g->k, g->r, g->s);
		cond_sub(g->r, g->r, 0, N);		// x < p < 2n
		be_load(g->e, h);
		cond_sub(g->e, g->e, 0, N);
		be_load(g->d, d);
		sc_mul(w, g->d, g->d, RR_N);		// d R
		sc_mul(w, g->s, g->r, g->d);		// r d
		mod_add(g->s, g->s, g->e, N);
		sc_mul(w, g->k, g->k, RR_N);		// k R
		sc_inv(w, g->k, g->k, g->u.pow);	// k^-1 R
		sc_mul(w, g->s, g->s, g->k);		// k^-1 (e + r d)
		ok = !is_zero(g->r) && !is_zero(g->s);
	}
	if (ok) {
		be_store(rs, g->r);
		be_store(rs + 32, g->s);
	} else {
		memset(rs, 0, P256_SIG_BYTES);
	}
	p256_wipe(s);
	return ok;
}

bool p256_sign(const uint8_t d[P256_BYTES], const uint8_t h[P256_BYTES], uint8_t rs[P256_SIG_BYTES],
	       p256_scratch_t *s)
{
	if (p256_priv_valid(d))
		rfc6979(ctx_of(s), d, h);
	return sign(d, h, NULL, rs, s);
}

bool p256_sign_k(const uint8_t d[P256_BYTES], const uint8_t h[P256_BYTES], const uint8_t k[P256_BYTES],
		 uint8_t rs[P256_SIG_BYTES], p256_scratch_t *s)
{
	return sign(d, h, k, rs, s);
}

bool p256_rfc6979_k(const uint8_t d[P256_BYTES], const uint8_t h[P256_BYTES], uint8_t k[P256_BYTES],
		    p256_scratch_t *s)
{
	ctx_t *c = ctx_of(s);
	bool ok = p256_priv_valid(d);

	if (ok) {
		rfc6979(c, d, h);
		be_store(k, c->u.sg.k);
	}
	p256_wipe(s);
	return ok;
}

// ---- Verifying (variable time: everything is public) ----

// Width-w NAF of k (destroyed): digits 0 or odd in [-(2^(w-1) - 1), 2^(w-1) - 1], least significant first
static void naf(int8_t *out, uint32_t k[8], unsigned wb)
{
	unsigned i = 0;

	memset(out, 0, NAF_LEN);
	while (!is_zero(k) && i < NAF_LEN) {
		if (k[0] & 1u) {
			int32_t d = (int32_t)(k[0] & ((1u << wb) - 1u));
			if (d >= (int32_t)(1u << (wb - 1)))
				d -= (int32_t)(1u << wb);
			out[i] = (int8_t)d;
			// k -= d clears k's low w bits: d > 0 is those bits (no borrow), d < 0 adds 2^w less them, which may
			// carry (k stays below 2^256: k < n and |d| < 16)
			if (d > 0) {
				k[0] -= (uint32_t)d;
			} else {
				uint32_t cy = 0;
				k[0] = adc(k[0], (uint32_t)-d, &cy);
				for (unsigned j = 1; j < 8; j++)
					k[j] = adc(k[j], 0, &cy);
			}
		}
		for (unsigned j = 0; j < 7; j++)
			k[j] = k[j] >> 1 | k[j + 1] << 31;
		k[7] >>= 1;
		i++;
	}
}

// acc += sel (affine), with the identity and acc = sel handled
static void add_aff_vt(ctx_t *c, jac_t *acc, const aff_t *sel)
{
	bool same = false;

	if (!is_zero(acc->z)) {
		if (!pt_madd(c, acc, acc, sel))
			return;
		same = true;
	}
	memcpy(acc->x, sel->x, sizeof acc->x);
	memcpy(acc->y, sel->y, sizeof acc->y);
	memcpy(acc->z, ONE_M, sizeof acc->z);
	if (same)
		pt_dbl(c, acc, acc);
}

bool p256_verify(const uint8_t q[P256_PUB_BYTES], const uint8_t h[P256_BYTES], const uint8_t rs[P256_SIG_BYTES],
		 p256_scratch_t *s)
{
	ctx_t *c = ctx_of(s);
	verify_t *v = &c->u.vf;
	uint32_t *w = c->w;
	jac_t *acc = &v->acc, *qt = v->x.m.qt;

	if (!scalar_ok(rs) || !scalar_ok(rs + 32) || !on_curve(w, c->t, q))
		return false;
	be_load(v->u, h);
	cond_sub(v->u, v->u, 0, N);		// e
	be_load(v->r, rs);
	be_load(v->v, rs + 32);
	sc_mul(w, v->v, v->v, RR_N);
	sc_inv(w, v->v, v->v, v->x.pow);	// s^-1 R
	sc_mul(w, v->u, v->u, v->v);		// u = e s^-1
	sc_mul(w, v->v, v->r, v->v);		// v = r s^-1
	naf(v->x.m.nu, v->u, 5);
	naf(v->x.m.nv, v->v, 4);

	// Q, 3Q, 5Q, 7Q, with 2Q in acc
	be_load(qt[0].x, q);
	be_load(qt[0].y, q + 32);
	fe_mul(w, qt[0].x, qt[0].x, RR_P);
	fe_mul(w, qt[0].y, qt[0].y, RR_P);
	memcpy(qt[0].z, ONE_M, sizeof qt[0].z);
	pt_dbl(c, acc, &qt[0]);
	for (unsigned i = 1; i < 4; i++)
		if (pt_add_vt(c, &qt[i], &qt[i - 1], acc, false))
			pt_dbl(c, &qt[i], &qt[i - 1]);	// never for Q of order n; the add asks it

	// u G + v Q, Straus-Shamir from the top digit
	memset(acc, 0, sizeof *acc);
	for (int i = NAF_LEN - 1; i >= 0; i--) {
		int du = v->x.m.nu[i], dv = v->x.m.nv[i];
		if (!is_zero(acc->z))
			pt_dbl(c, acc, acc);
		if (du) {
			unsigned j = (unsigned)(du < 0 ? -du : du) >> 1;
			memcpy(v->sel.x, p256_tab_g[j], sizeof v->sel.x);
			memcpy(v->sel.y, p256_tab_g[j] + 8, sizeof v->sel.y);
			if (du < 0)
				fe_neg(v->sel.y, v->sel.y);
			add_aff_vt(c, acc, &v->sel);
		}
		if (dv && pt_add_vt(c, acc, acc, &qt[(unsigned)(dv < 0 ? -dv : dv) >> 1], dv < 0))
			pt_dbl(c, acc, acc);
	}
	if (is_zero(acc->z))
		return false;

	// x(R) = X / Z^2 and r agree mod n: X = r Z^2, or X = (r + n) Z^2 when r + n < p (p256.md 8 step 5)
	fe_sqr(w, c->t[0], acc->z);
	fe_mul(w, c->t[1], v->r, RR_P);
	fe_mul(w, c->t[1], c->t[1], c->t[0]);
	if (memcmp(c->t[1], acc->x, sizeof c->t[1]) == 0)
		return true;
	{
		uint32_t cy = 0;
		for (unsigned i = 0; i < 8; i++)
			v->r[i] = adc(v->r[i], N[i], &cy);
		if (cy || !lt(v->r, P))
			return false;
	}
	fe_mul(w, c->t[1], v->r, RR_P);
	fe_mul(w, c->t[1], c->t[1], c->t[0]);
	return memcmp(c->t[1], acc->x, sizeof c->t[1]) == 0;
}

void p256_wipe(p256_scratch_t *s)
{
	if (s)
		crypto_wipe(s, sizeof *s);
}

#endif
