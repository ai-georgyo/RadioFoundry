// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 stream encryption (core/m17/m17_crypt.c; docs/spec/m17.md 6) against known answers generated from the rules of
// m17.md 6.1 and 6.2, since M17 publishes none for the scrambler or AES (m17.md 11): the scrambler's from an
// independent model of its LFSR, the AES block outputs from the OpenSSL CLI. Plus a bit-serial LFSR written here from
// m17.md 6.1 alone that the matrix jump must agree with, the refusals, and failing closed without a key. Prints the
// host time of a seek to FN 7FFF
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "m17/m17_crypt.h"
#include "crypto/aes.h"

static unsigned checks, failed;

static void check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		failed++;
		printf("FAIL %s\n", what);
	}
}

// Hex text to bytes; returns the byte count. Spaces are skipped
static size_t hex(const char *s, uint8_t *out, size_t max)
{
	size_t n = 0;
	int hi = -1;

	for (; *s; s++) {
		int v;
		if (*s >= '0' && *s <= '9')
			v = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			v = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F')
			v = *s - 'A' + 10;
		else
			continue;
		if (hi < 0) {
			hi = v;
		} else {
			if (n == max) {
				printf("FAIL vector longer than %zu bytes\n", max);
				failed++;
				return n;
			}
			out[n++] = (uint8_t)(hi << 4 | v);
			hi = -1;
		}
	}
	return n;
}

static int eq_hex(const uint8_t *got, size_t n, const char *want)
{
	uint8_t w[64];
	size_t wn = hex(want, w, sizeof w);

	return wn == n && memcmp(got, w, n) == 0;
}

static int all_zero(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

// A small deterministic generator for the random FNs and payloads (xorshift32)
static uint32_t rng = 0x2545F491u;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

// ---- The reference LFSR: m17.md 6.1's table read literally, one tap at a time ----

static const uint8_t ref_taps[3][4] = { { 7, 5, 4, 3 }, { 15, 14, 12, 3 }, { 23, 22, 21, 16 } };

typedef struct {
	uint32_t reg;
	uint8_t bits;
} ref_lfsr_t;

static unsigned ref_step(ref_lfsr_t *r)
{
	const uint8_t *t = ref_taps[r->bits / 8u - 1u];
	unsigned fb = 0;

	for (unsigned i = 0; i < 4; i++)
		fb ^= (r->reg >> t[i]) & 1u;
	r->reg = ((r->reg << 1) | fb) & ((1u << r->bits) - 1u);
	return fb;
}

static void ref_skip(ref_lfsr_t *r, uint32_t steps)
{
	while (steps--)
		ref_step(r);
}

static void ref_frame(ref_lfsr_t *r, uint8_t ks[M17_STREAM_BYTES])
{
	for (unsigned i = 0; i < M17_STREAM_BYTES; i++) {
		ks[i] = 0;
		for (unsigned b = 0; b < 8; b++)
			ks[i] = (uint8_t)(ks[i] << 1 | ref_step(r));
	}
}

// ---- Scrambler ----

// The M17-SCRAMBLER rows: seed, FN, key stream (the frame's STREAM XORed into zeros), generated from m17.md 6.1's rule
// by a model of it written apart from this code (taps, reg = (reg << 1 | fb) & mask, output fb, MSB first as m17.md
// 13 assumes), stepping 128 x FN mod the period from the seed. Rows marked "Python" were added for this test the same
// way by a second model, in Python
static const struct {
	const char *seed, *ks;
	uint16_t fn;
	const char *name;
} scr_vec[] = {
	{ "01", "1C4B81926E415B5961F6F5D10D8F398B", 0x0000, "8-bit 01 FN 0" },
	{ "01", "48A54EECF7E99A8C1D57CA13FC2F1A02", 0x0001, "8-bit 01 FN 1" },
	{ "01", "38970324DC82B6B2C3EDEBA21B1E7316", 0x0002, "8-bit 01 FN 2" },
	{ "A5", "4EECF7E99A8C1D57CA13FC2F1A023897", 0x0000, "8-bit A5 FN 0" },
	{ "A5", "0324DC82B6B2C3EDEBA21B1E7316914A", 0x0001, "8-bit A5 FN 1" },
	{ "A5", "9DD9EFD335183AAF9427F85E3404712E", 0x0002, "8-bit A5 FN 2" },
	{ "1234", "E96CBB6671810E6DBA7CD065A9129A03", 0x0000, "16-bit 1234 FN 0" },
	{ "1234", "4AB4C61AD781D91D74847E4BCC4653C4", 0x0001, "16-bit 1234 FN 1" },
	{ "1234", "28E30ED063CF4EC7D8F93D1396C449F1", 0x0002, "16-bit 1234 FN 2" },
	{ "123456", "64A6916C3940180ECC4F4E424BCAEA16", 0x0000, "24-bit 123456 FN 0" },
	{ "123456", "019D62CA6349468C5D978B5B231D1067", 0x0001, "24-bit 123456 FN 1" },
	{ "123456", "DB42DAA1A155B4CC72685C6B37A08A56", 0x0002, "24-bit 123456 FN 2" },
	{ "000001", "00008700401521CAFB000961048EA558", 0x0000, "24-bit 000001 FN 0" },
	{ "000001", "FBD6F7085B4114204D7CC75C15FA9E94", 0x0001, "24-bit 000001 FN 1" },
	{ "000001", "AA93BA1C27284761FA64D8D55260838F", 0x0002, "24-bit 000001 FN 2" },
	{ "123456", "E01EF3AF210DDD61CFA2C2BF0D12BBAA", 0x0064, "24-bit 123456 late join at FN 100" },
	{ "A5", "F5D10D8F398B48A54EECF7E99A8C1D57", 0x7FFF, "8-bit A5 FN 7FFF (Python)" },
	{ "1234", "CFACE3A8B2951234E96CBB6671810E6D", 0x7FFF, "16-bit 1234 FN 7FFF (Python)" },
	{ "0001", "0958DCE19B970001111A1A5B48BC59BD", 0x7FFF, "16-bit 0001 FN 7FFF (Python)" },
	{ "123456", "46ADE787B2754F25ED7F0F3DFAB34CBC", 0x7FFF, "24-bit 123456 FN 7FFF (Python)" },
	{ "000001", "20CA17877DA52AA2EE841BCB91A6BA27", 0x7FFF, "24-bit 000001 FN 7FFF (Python)" },
};

// Static like the engine's work union: seek must not need it cleared, so it starts full of junk
static m17_scr_work_t work;

static void junk_work(void)
{
	memset(&work, 0xA5, sizeof work);
}

static void test_scr_vectors(void)
{
	uint8_t seed[3], buf[M17_STREAM_BYTES];
	char what[128];
	m17_scr_t s;

	for (unsigned v = 0; v < sizeof scr_vec / sizeof scr_vec[0]; v++) {
		uint8_t len = (uint8_t)hex(scr_vec[v].seed, seed, sizeof seed);

		// Seek straight to the frame, as a late joiner does
		junk_work();
		m17_scr_seek(&s, seed, len, scr_vec[v].fn, &work);
		memset(buf, 0, sizeof buf);
		m17_scr_frame(&s, buf);
		snprintf(what, sizeof what, "%s: seek", scr_vec[v].name);
		check(eq_hex(buf, sizeof buf, scr_vec[v].ks), what);
		// The end bit doesn't count: the last frame of a stream lands on the same state
		junk_work();
		m17_scr_seek(&s, seed, len, (uint16_t)(scr_vec[v].fn | M17_FN_END), &work);
		memset(buf, 0, sizeof buf);
		m17_scr_frame(&s, buf);
		snprintf(what, sizeof what, "%s: seek with the end bit", scr_vec[v].name);
		check(eq_hex(buf, sizeof buf, scr_vec[v].ks), what);
		// From the seed frame by frame, as a receiver from the start does
		check(m17_scr_init(&s, seed, len), "seed accepted");
		for (uint32_t f = 0; f <= scr_vec[v].fn; f++) {
			memset(buf, 0, sizeof buf);
			m17_scr_frame(&s, buf);
		}
		snprintf(what, sizeof what, "%s: frame by frame", scr_vec[v].name);
		check(eq_hex(buf, sizeof buf, scr_vec[v].ks), what);
	}
}

// Sizes and subtypes; leading zero bytes count (m17.md 6.1)
static void test_scr_init(void)
{
	static const uint8_t zero[3] = { 0, 0, 0 }, one[3] = { 0, 0, 1 };
	uint8_t buf[M17_STREAM_BYTES];
	m17_scr_t s;

	check(m17_scr_init(&s, one + 2, 1) && s.bits == 8 && s.reg == 1 && m17_scr_sub(&s) == M17_SCR_8, "01: 8 bits");
	check(m17_scr_init(&s, one + 1, 2) && s.bits == 16 && s.reg == 1 && m17_scr_sub(&s) == M17_SCR_16,
	      "00 01: 16 bits");
	check(m17_scr_init(&s, one, 3) && s.bits == 24 && s.reg == 1 && m17_scr_sub(&s) == M17_SCR_24,
	      "00 00 01: 24 bits");
	{
		static const uint8_t big[3] = { 0xAB, 0xCD, 0xEF };
		check(m17_scr_init(&s, big, 3) && s.reg == 0xABCDEFu, "big endian: seed bit 23 in D23");
		check(m17_scr_init(&s, big, 2) && s.reg == 0xABCDu, "big endian: seed bit 15 in D15");
	}

	// Refusals: no key afterwards, and a frame without a key is zeroed, never left as it came
	for (uint8_t len = 1; len <= 3; len++) {
		s.bits = 24;
		s.reg = 0x123456;
		check(!m17_scr_init(&s, zero, len), "all-zero seed refused");
		check(s.bits == 0 && s.reg == 0, "refused seed leaves no key");
	}
	check(!m17_scr_init(&s, one, 0), "seed of 0 bytes refused");
	check(!m17_scr_init(&s, (const uint8_t *)"\1\2\3\4", 4), "seed of 4 bytes refused");
	{
		static const uint8_t many[255] = { 1, 2, 3, 4 };
		check(!m17_scr_init(&s, many, 255), "seed of 255 bytes refused");
	}
	check(!m17_scr_init(&s, NULL, 1), "no seed refused");
	memset(buf, 0x5A, sizeof buf);
	m17_scr_frame(&s, buf);
	check(all_zero(buf, sizeof buf), "no key: the frame is zeroed");

	// seek with a bad seed: no key either, whatever the FN
	s.bits = 8;
	junk_work();
	m17_scr_seek(&s, zero, 2, 100, &work);
	check(s.bits == 0, "seek with an all-zero seed: no key");
	memset(buf, 0x5A, sizeof buf);
	m17_scr_frame(&s, buf);
	check(all_zero(buf, sizeof buf), "seek with an all-zero seed: the frame is zeroed");
	s.bits = 8;
	m17_scr_seek(&s, one, 4, 0, &work);
	check(s.bits == 0, "seek with a 4-byte seed: no key");

	// Scrambling twice gives the payload back
	{
		static const uint8_t seed[2] = { 0x12, 0x34 };
		uint8_t p[M17_STREAM_BYTES], c[M17_STREAM_BYTES];
		m17_scr_t a, b;
		for (unsigned i = 0; i < sizeof p; i++)
			p[i] = (uint8_t)rnd();
		memcpy(c, p, sizeof c);
		m17_scr_init(&a, seed, 2);
		m17_scr_init(&b, seed, 2);
		m17_scr_frame(&a, c);
		check(memcmp(c, p, sizeof p) != 0, "scrambled differs");
		m17_scr_frame(&b, c);
		check(memcmp(c, p, sizeof p) == 0, "descrambled is the payload");
	}
}

// The periods the spec gives (m17.md 6.1), through frame-wise output against the reference
static void test_scr_period(void)
{
	static const struct { uint8_t len; uint32_t period; } per[] = { { 1, 255u }, { 2, 65535u } };
	static const uint8_t seed[3] = { 0x5A, 0x3C, 0x0F };

	for (unsigned i = 0; i < 2; i++) {
		ref_lfsr_t r = { 0, (uint8_t)(8u * per[i].len) };
		uint32_t start;
		for (uint8_t j = 0; j < per[i].len; j++)
			r.reg = r.reg << 8 | seed[j];
		start = r.reg;
		ref_skip(&r, per[i].period);
		check(r.reg == start, "reference LFSR: the period of m17.md 6.1");
	}
}

// Seek against the reference stepped 128 x FN from the seed, for random seeds and FNs up to 7FFF in each size, and the
// frames after a seek against the continuing reference
static void test_scr_seek(void)
{
	static const uint16_t edges[] = { 0, 1, 2, 3, 127, 128, 255, 256, 0x1000, 0x7FFE, 0x7FFF };
	uint8_t seed[3], got[M17_STREAM_BYTES], want[M17_STREAM_BYTES];
	char what[96];
	m17_scr_t s;

	for (uint8_t len = 1; len <= 3; len++) {
		const unsigned n_rand = 24;
		for (unsigned t = 0; t < n_rand + sizeof edges / sizeof edges[0]; t++) {
			uint16_t fn = t < n_rand ? (uint16_t)(rnd() & M17_FN_MASK) : edges[t - n_rand];
			ref_lfsr_t r = { 0, (uint8_t)(8u * len) };
			do {
				for (uint8_t j = 0; j < len; j++)
					seed[j] = (uint8_t)rnd();
				r.reg = 0;
				for (uint8_t j = 0; j < len; j++)
					r.reg = r.reg << 8 | seed[j];
			} while (r.reg == 0);
			ref_skip(&r, 128u * fn);
			junk_work();
			m17_scr_seek(&s, seed, len, fn, &work);
			snprintf(what, sizeof what, "%u-bit seek to FN %04X: register", 8u * len, fn);
			check(s.reg == r.reg && s.bits == 8u * len, what);
			// The three frames after it
			for (unsigned f = 0; f < 3; f++) {
				memset(got, 0, sizeof got);
				m17_scr_frame(&s, got);
				ref_frame(&r, want);
				snprintf(what, sizeof what, "%u-bit seek to FN %04X: frame +%u", 8u * len, fn, f);
				check(memcmp(got, want, sizeof got) == 0, what);
			}
		}
	}

	// Frame by frame against a fresh seek at every FN for the first 600 frames: past the 8-bit period twice over
	for (uint8_t len = 1; len <= 3; len++) {
		static const uint8_t sd[3] = { 0xC3, 0x01, 0x7E };
		m17_scr_t run;
		unsigned bad = 0;
		m17_scr_init(&run, sd, len);
		for (uint16_t fn = 0; fn < 600; fn++) {
			m17_scr_seek(&s, sd, len, fn, &work);
			if (s.reg != run.reg)
				bad++;
			memset(got, 0, sizeof got);
			m17_scr_frame(&run, got);
		}
		snprintf(what, sizeof what, "%u-bit: seek equals stepping for FN 0-599", 8u * len);
		check(bad == 0, what);
	}
}

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// The host time of a seek to FN 7FFF (the worst case: 15 bits set, 14 squarings), for the report
static void time_seek(void)
{
	static const uint8_t seed[3] = { 0x12, 0x34, 0x56 };
	const unsigned n = 2000;
	volatile uint32_t sink = 0;
	m17_scr_t s;

	for (uint8_t len = 1; len <= 3; len++) {
		double t0 = now_s();
		for (unsigned i = 0; i < n; i++) {
			m17_scr_seek(&s, seed, len, 0x7FFF, &work);
			sink ^= s.reg;
		}
		printf("m17CryptTest: %2u-bit seek to FN 7FFF: %.2f us on the host\n", 8u * len,
		       (now_s() - t0) * 1e6 / n);
	}
	(void)sink;
}

// ---- AES-CTR ----

// The M17-AES-CTR rows: META = 0CF10980 || 00112233445566778899, plaintexts the payloads of m17.md 7.5's test vector,
// FN 7FFF zeros; each key stream is AES of the counter block META || FN (m17.md 6.2) from the OpenSSL CLI (openssl enc
// -aes-<bits>-ecb -nopad -K <key>), as in cryptoTest. The FFFF rows are FN 7FFF with the end bit, as the last frame
// of a stream sends it (m17.md 13): the counter block 0CF1098000112233445566778899FFFF
static const char aes_meta[] = "0CF1098000112233445566778899";
static const uint8_t aes_rnd[10] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99 };
static const struct {
	const char *key;
	uint8_t sub;
	struct {
		uint16_t fn;
		const char *ks, *plain, *cipher;
	} row[5];
} aes_vec[] = {
	{ "000102030405060708090A0B0C0D0E0F", M17_AES_128, {
		{ 0x0000, "9A91F098D00ADCC45C1021D17F5CA6C0", "000102030405060708090A0B0C0D0E0F",
		  "9A90F29BD40FDAC354192BDA7351A8CF" },
		{ 0x0001, "5027BF82461DE3A683508BEE3D172C55", "101112131415161718191A1B1C1D1E1F",
		  "4036AD915208F5B19B4991F5210A324A" },
		{ 0x0002, "5DAEB561D1D0DC895234F8D8C2C8A6C3", "202122232425262728292A2B2C2D2E2F",
		  "7D8F9742F5F5FAAE7A1DD2F3EEE588EC" },
		{ 0x7FFF, "7825B4FBFAD2BE2D33B7DDDAE539E0CD", "00000000000000000000000000000000",
		  "7825B4FBFAD2BE2D33B7DDDAE539E0CD" },
		{ 0xFFFF, "9398F47E9DBF3CB0FC30065104FF593B", "00000000000000000000000000000000",
		  "9398F47E9DBF3CB0FC30065104FF593B" },
	} },
	{ "000102030405060708090A0B0C0D0E0F1011121314151617", M17_AES_192, {
		{ 0x0000, "A03F9A68F8C97E371490741E443DB59B", "000102030405060708090A0B0C0D0E0F",
		  "A03E986BFCCC78301C997E154830BB94" },
		{ 0x0001, "5854E6CB65CFA038021033D5F94A56F6", "101112131415161718191A1B1C1D1E1F",
		  "4845F4D871DAB62F1A0929CEE55748E9" },
		{ 0x0002, "0817B7AC1BE05AAC596598E5BB3D1C04", "202122232425262728292A2B2C2D2E2F",
		  "2836958F3FC57C8B714CB2CE9710322B" },
		{ 0x7FFF, "5DFAA1F28DFCAE6B884C9946F60631C6", "00000000000000000000000000000000",
		  "5DFAA1F28DFCAE6B884C9946F60631C6" },
		{ 0xFFFF, "8A7721766339B7C3B03704550BE25697", "00000000000000000000000000000000",
		  "8A7721766339B7C3B03704550BE25697" },
	} },
	{ "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", M17_AES_256, {
		{ 0x0000, "2391B4B9E31BEE348D916AE47A508C4E", "000102030405060708090A0B0C0D0E0F",
		  "2390B6BAE71EE833859860EF765D8241" },
		{ 0x0001, "0BDF3AE6B55E39A42662D74E85450BDE", "101112131415161718191A1B1C1D1E1F",
		  "1BCE28F5A14B2FB33E7BCD55995815C1" },
		{ 0x0002, "D474CEB75917D161DCF969B16443BE5E", "202122232425262728292A2B2C2D2E2F",
		  "F455EC947D32F746F4D0439A486E9071" },
		{ 0x7FFF, "E76A7259FD0D3EE80E861FA39F8D9DC9", "00000000000000000000000000000000",
		  "E76A7259FD0D3EE80E861FA39F8D9DC9" },
		{ 0xFFFF, "9F6773065C43421AA86918768A8A7D79", "00000000000000000000000000000000",
		  "9F6773065C43421AA86918768A8A7D79" },
	} },
};

// Static, as aes.h asks of a context holding a key
static aes_ctx_t aes;

static void test_aes_vectors(void)
{
	uint8_t key[32], meta[M17_META_BYTES], buf[M17_STREAM_BYTES], p[M17_STREAM_BYTES];
	char what[96];

	m17_aes_nonce(0x0CF10980u, aes_rnd, meta);
	check(eq_hex(meta, sizeof meta, aes_meta), "nonce: timestamp, then the 80 random bits");
	for (unsigned v = 0; v < sizeof aes_vec / sizeof aes_vec[0]; v++) {
		size_t kl = hex(aes_vec[v].key, key, sizeof key);
		check(aes_init(&aes, key, kl), "key accepted");
		snprintf(what, sizeof what, "AES-%zu: subtype", 8u * kl);
		check(m17_aes_sub((uint8_t)kl) == aes_vec[v].sub, what);
		for (unsigned i = 0; i < 5; i++) {
			uint16_t fn = aes_vec[v].row[i].fn;
			memset(buf, 0, sizeof buf);
			m17_aes_frame(&aes, meta, fn, buf);
			snprintf(what, sizeof what, "AES-%zu FN %04X: key stream", 8u * kl, fn);
			check(eq_hex(buf, sizeof buf, aes_vec[v].row[i].ks), what);
			hex(aes_vec[v].row[i].plain, p, sizeof p);
			memcpy(buf, p, sizeof buf);
			m17_aes_frame(&aes, meta, fn, buf);
			snprintf(what, sizeof what, "AES-%zu FN %04X: cipher", 8u * kl, fn);
			check(eq_hex(buf, sizeof buf, aes_vec[v].row[i].cipher), what);
			m17_aes_frame(&aes, meta, fn, buf);
			snprintf(what, sizeof what, "AES-%zu FN %04X: decrypts", 8u * kl, fn);
			check(memcmp(buf, p, sizeof p) == 0, what);
		}
		// The end bit is part of the counter: the last frame's key stream is not FN 7FFF's
		{
			uint8_t a[M17_STREAM_BYTES] = { 0 }, b[M17_STREAM_BYTES] = { 0 };
			m17_aes_frame(&aes, meta, 0x7FFF, a);
			m17_aes_frame(&aes, meta, 0xFFFF, b);
			check(memcmp(a, b, sizeof a) != 0, "the end bit changes the counter block");
		}
	}
	aes_wipe(&aes);
}

static void test_aes_refusals(void)
{
	static const uint8_t zkey[33] = { 0 };
	uint8_t meta[M17_META_BYTES] = { 0 }, buf[M17_STREAM_BYTES];
	char what[64];

	for (unsigned len = 0; len <= 255; len++) {
		uint8_t want = len == 16 ? M17_AES_128 : len == 24 ? M17_AES_192 : len == 32 ? M17_AES_256 : 0xFF;
		if (m17_aes_sub((uint8_t)len) != want) {
			snprintf(what, sizeof what, "subtype of a %u-byte key", len);
			check(0, what);
		}
	}
	check(m17_aes_sub(0) == 0xFF && m17_aes_sub(15) == 0xFF && m17_aes_sub(33) == 0xFF, "bad key lengths: no subtype");
	{
		static const unsigned bad[] = { 0, 1, 8, 15, 17, 20, 23, 25, 31, 33 };
		for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
			snprintf(what, sizeof what, "%u-byte AES key refused", bad[i]);
			check(!aes_init(&aes, zkey, bad[i]), what);
			memset(buf, 0x5A, sizeof buf);
			m17_aes_frame(&aes, meta, 1, buf);
			snprintf(what, sizeof what, "%u-byte AES key: the frame is zeroed", bad[i]);
			check(all_zero(buf, sizeof buf), what);
		}
	}
	// A wiped key fails closed too
	check(aes_init(&aes, zkey, 16), "16-byte key accepted");
	aes_wipe(&aes);
	memset(buf, 0x5A, sizeof buf);
	m17_aes_frame(&aes, meta, 1, buf);
	check(all_zero(buf, sizeof buf), "wiped key: the frame is zeroed");

	// Timestamp 0 (no clock) and the byte order of the timestamp
	{
		static const uint8_t r[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
		m17_aes_nonce(0, r, meta);
		check(eq_hex(meta, sizeof meta, "00000000 0102030405060708090A"), "nonce without a clock");
		m17_aes_nonce(0xA1B2C3D4u, r, meta);
		check(eq_hex(meta, sizeof meta, "A1B2C3D4 0102030405060708090A"), "nonce timestamp big endian");
	}
}

int main(void)
{
	test_scr_vectors();
	test_scr_init();
	test_scr_period();
	test_scr_seek();
	test_aes_vectors();
	test_aes_refusals();
	time_seek();
	if (failed) {
		printf("m17CryptTest: %u of %u checks failed\n", failed, checks);
		return 1;
	}
	printf("m17CryptTest: %u checks passed\n", checks);
	return 0;
}
