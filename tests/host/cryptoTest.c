// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The generic symmetric crypto (core/crypto/aes.c, hmac_sha256.c; docs/spec/crypto.md) against the standards' known
// answers: FIPS 197 appendix B and C.1-C.3, NIST SP 800-38A F.5.1, F.5.3, F.5.5, FIPS 180-2 appendix B (through the
// tree's core/support/sha256.c), RFC 4231 section 4 test cases 1-7, NIST CAVP HMAC_DRBG. Plus the contracts the headers
// promise: counters, splits, key lengths, wiping
#include <stdio.h>
#include <string.h>
#include "crypto/aes.h"
#include "crypto/hmac_sha256.h"
#include "crypto/crypto_wipe.h"
#include "support/sha256.h"

static unsigned checks, failed;

static void check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		failed++;
		printf("FAIL %s\n", what);
	}
}

// Hex text to bytes; returns the byte count. Spaces are skipped so long vectors can be broken up
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

static int all_zero(const void *p, size_t n)
{
	const uint8_t *b = p;

	for (size_t i = 0; i < n; i++)
		if (b[i])
			return 0;
	return 1;
}

static int eq_hex(const uint8_t *got, size_t n, const char *want)
{
	uint8_t w[256];
	size_t wn = hex(want, w, sizeof w);

	return wn == n && memcmp(got, w, n) == 0;
}

// ---- AES ----

// FIPS 197 appendix B and C.1-C.3; the last word of each key schedule from appendix A.1-A.3
static const struct {
	const char *key, *plain, *cipher, *last_word, *name;
} ecb[] = {
	{ "2b7e151628aed2a6abf7158809cf4f3c", "3243f6a8885a308d313198a2e0370734", "3925841d02dc09fbdc118597196a0b32",
	  "b6630ca6", "FIPS 197 B" },
	{ "000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff", "69c4e0d86a7b0430d8cdb78070b4c55a",
	  NULL, "FIPS 197 C.1" },
	{ "000102030405060708090a0b0c0d0e0f1011121314151617", "00112233445566778899aabbccddeeff",
	  "dda97ca4864cdfe06eaf70a0ec0d7191", NULL, "FIPS 197 C.2" },
	{ "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "00112233445566778899aabbccddeeff",
	  "8ea2b7ca516745bfeafc49904b496089", NULL, "FIPS 197 C.3" },
	{ "8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b", NULL, NULL, "01002202", "FIPS 197 A.2 schedule" },
	{ "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", NULL, NULL, "706c631e",
	  "FIPS 197 A.3 schedule" },
};

// SP 800-38A F.5.1, F.5.3, F.5.5: CTR-AES128/192/256.Encrypt, four blocks from counter f0f1..feff
static const char ctr_init[] = "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";
static const char ctr_plain[] = "6bc1bee22e409f96e93d7e117393172a ae2d8a571e03ac9c9eb76fac45af8e51"
				"30c81c46a35ce411e5fbc1191a0a52ef f69f2445df4f9b17ad2b417be66c3710";
static const struct {
	const char *key, *cipher, *name;
} ctr_vec[] = {
	{ "2b7e151628aed2a6abf7158809cf4f3c",
	  "874d6191b620e3261bef6864990db6ce 9806f66b7970fdff8617187bb9fffdff"
	  "5ae4df3edbd5d35e5b4f09020db03eab 1e031dda2fbe03d1792170a0f3009cee", "SP 800-38A F.5.1" },
	{ "8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b",
	  "1abc932417521ca24f2b0459fe7e6e0b 090339ec0aa6faefd5ccc2c6f4ce8e94"
	  "1e36b26bd1ebc670d1bd1d665620abf7 4f78a7f6d29809585a97daec58c6b050", "SP 800-38A F.5.3" },
	{ "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4",
	  "601ec313775789a5b7a7f504bbf3d228 f443e3ca4d62b59aca84e990cacaf5c5"
	  "2b0930daa23de94ce87017ba2d84988d dfc9c58db67aada613c2dd08457941a6", "SP 800-38A F.5.5" },
};

// Static, as the header asks of a context with a key in it
static aes_ctx_t aes;

static void test_aes_ecb(void)
{
	uint8_t key[32], in[16], want[16], out[16];
	char what[96];

	for (unsigned v = 0; v < sizeof ecb / sizeof ecb[0]; v++) {
		size_t kl = hex(ecb[v].key, key, sizeof key);
		snprintf(what, sizeof what, "%s: key accepted", ecb[v].name);
		check(aes_init(&aes, key, kl), what);
		check(aes.nr == kl / 4 + 6, "rounds from the key length");
		if (ecb[v].last_word) {
			size_t words = 4u * (aes.nr + 1u);
			snprintf(what, sizeof what, "%s: last key schedule word", ecb[v].name);
			check(eq_hex(&aes.rk[4u * (words - 1u)], 4, ecb[v].last_word), what);
			snprintf(what, sizeof what, "%s: rk past the schedule is zero", ecb[v].name);
			check(all_zero(&aes.rk[4u * words], sizeof aes.rk - 4u * words), what);
		}
		if (!ecb[v].plain)
			continue;
		hex(ecb[v].plain, in, sizeof in);
		hex(ecb[v].cipher, want, sizeof want);
		aes_encrypt(&aes, in, out);
		snprintf(what, sizeof what, "%s: cipher", ecb[v].name);
		check(memcmp(out, want, 16) == 0, what);
		aes_encrypt(&aes, in, in);	// in place
		snprintf(what, sizeof what, "%s: cipher in place", ecb[v].name);
		check(memcmp(in, want, 16) == 0, what);
	}
}

static void test_aes_keys(void)
{
	static const size_t bad[] = { 0, 1, 8, 15, 17, 20, 23, 25, 31, 33, 48, 64 };
	uint8_t key[64], blk[16], data[40];

	for (unsigned i = 0; i < sizeof key; i++)
		key[i] = (uint8_t)(0xa5 ^ i);
	for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		check(aes_init(&aes, key, 32), "a good key before a bad one");
		check(!aes_init(&aes, key, bad[i]), "a key of a wrong length is refused");
		check(all_zero(&aes, sizeof aes), "a refused key leaves the context empty");
	}
	check(!aes_init(&aes, NULL, 16), "no key refused");
	check(!aes_init(NULL, key, 16), "no context refused");

	// No key: the block out is zeros and counter mode zeroes the data, never passing the plaintext through
	memset(blk, 0x5a, sizeof blk);
	aes_encrypt(&aes, blk, blk);
	check(all_zero(blk, sizeof blk), "no key: aes_encrypt gives zeros");
	memset(data, 0x5a, sizeof data);
	memset(blk, 0x11, sizeof blk);
	aes_ctr(&aes, blk, data, sizeof data);
	check(all_zero(data, sizeof data), "no key: aes_ctr zeroes the data");

	// A 128-bit key after a 256-bit one: the tail of the old schedule doesn't stay
	check(aes_init(&aes, key, 32), "256-bit key");
	check(aes_init(&aes, key, 16), "then a 128-bit key");
	check(all_zero(&aes.rk[176], sizeof aes.rk - 176), "the 256-bit schedule's tail is wiped");

	check(aes_init(&aes, key, 24), "192-bit key");
	aes_wipe(&aes);
	check(all_zero(&aes, sizeof aes), "aes_wipe leaves zeros");
	aes_wipe(NULL);
}

static void test_aes_ctr(void)
{
	uint8_t key[32], ctr[16], ctr0[16], plain[64], want[64], buf[64], c2[16];
	char what[128];

	hex(ctr_init, ctr0, sizeof ctr0);
	hex(ctr_plain, plain, sizeof plain);
	for (unsigned v = 0; v < sizeof ctr_vec / sizeof ctr_vec[0]; v++) {
		size_t kl = hex(ctr_vec[v].key, key, sizeof key);
		hex(ctr_vec[v].cipher, want, sizeof want);
		check(aes_init(&aes, key, kl), "CTR key");

		// One call over the four blocks, then decrypting is the same operation
		memcpy(ctr, ctr0, 16);
		memcpy(buf, plain, 64);
		aes_ctr(&aes, ctr, buf, 64);
		snprintf(what, sizeof what, "%s: one call", ctr_vec[v].name);
		check(memcmp(buf, want, 64) == 0, what);
		check(eq_hex(ctr, 16, "f0f1f2f3f4f5f6f7f8f9fafbfcfdff03"), "counter left at the next unused block");
		memcpy(ctr, ctr0, 16);
		aes_ctr(&aes, ctr, buf, 64);
		snprintf(what, sizeof what, "%s: decrypt", ctr_vec[v].name);
		check(memcmp(buf, plain, 64) == 0, what);

		// Block by block equals one call
		memcpy(ctr, ctr0, 16);
		memcpy(buf, plain, 64);
		for (unsigned b = 0; b < 4; b++)
			aes_ctr(&aes, ctr, buf + 16 * b, 16);
		snprintf(what, sizeof what, "%s: block by block", ctr_vec[v].name);
		check(memcmp(buf, want, 64) == 0, what);

		// Every length 0..64 in one call: the first n bytes, the rest untouched, the counter on by ceil(n / 16)
		for (size_t n = 0; n <= 64; n++) {
			memcpy(ctr, ctr0, 16);
			memcpy(buf, plain, 64);
			aes_ctr(&aes, ctr, buf, n);
			snprintf(what, sizeof what, "%s: %zu bytes in one call", ctr_vec[v].name, n);
			check(memcmp(buf, want, n) == 0 && memcmp(buf + n, plain + n, 64 - n) == 0, what);
			check(ctr[15] == (uint8_t)(0xff + (n + 15) / 16) &&
			      ctr[14] == (n ? 0xff : 0xfe) && memcmp(ctr, ctr0, 14) == 0, what);
		}

		// A call split at an odd byte count: the partial block uses up its counter, so the second call goes on at the
		// next block boundary (the header's contract; the caller of a byte stream keeps its blocks whole)
		static const size_t splits[] = { 1, 5, 15, 17, 21, 31, 33, 47, 63 };
		for (unsigned s = 0; s < sizeof splits / sizeof splits[0]; s++) {
			size_t n1 = splits[s], next = (n1 + 15) / 16 * 16;
			memcpy(ctr, ctr0, 16);
			memcpy(buf, plain, 64);
			aes_ctr(&aes, ctr, buf, n1);
			aes_ctr(&aes, ctr, buf + next, 64 - next);
			snprintf(what, sizeof what, "%s: split after %zu bytes", ctr_vec[v].name, n1);
			check(memcmp(buf, want, n1) == 0 && memcmp(buf + n1, plain + n1, next - n1) == 0 &&
			      memcmp(buf + next, want + next, 64 - next) == 0, what);
			check(eq_hex(ctr, 16, "f0f1f2f3f4f5f6f7f8f9fafbfcfdff03"), what);
		}

		// In place over an odd-length buffer that isn't aligned
		{
			uint8_t odd[1 + 37];
			memcpy(ctr, ctr0, 16);
			memcpy(odd + 1, plain, 37);
			aes_ctr(&aes, ctr, odd + 1, 37);
			snprintf(what, sizeof what, "%s: 37 bytes at an odd address", ctr_vec[v].name);
			check(memcmp(odd + 1, want, 37) == 0, what);
		}
	}

	// The counter is the whole 128-bit block: ff..ff wraps to 00..00, and each block is E(counter)
	check(aes_init(&aes, key, 32), "wrap key");
	memset(ctr, 0xff, 16);
	memset(buf, 0, 32);
	aes_ctr(&aes, ctr, buf, 32);
	check(all_zero(ctr, 15) && ctr[15] == 1, "counter wraps mod 2^128");
	memset(c2, 0xff, 16);
	aes_encrypt(&aes, c2, c2);
	check(memcmp(buf, c2, 16) == 0, "keystream block 1 = E(ff..ff)");
	memset(c2, 0, 16);
	aes_encrypt(&aes, c2, c2);
	check(memcmp(buf + 16, c2, 16) == 0, "keystream block 2 = E(00..00)");
	// A carry through the low 64 bits into the high half
	hex("0123456789abcdefffffffffffffffff", ctr, 16);
	aes_ctr(&aes, ctr, buf, 1);
	check(eq_hex(ctr, 16, "0123456789abcdf00000000000000000"), "carry across all bytes");

	// M17's counter layout (META || FN, m17.md 6.2; crypto.md 7): FN 0000-0002 with the payloads of m17.md
	// 7.5's test vector, 7FFF with a zero payload, through aes_encrypt and the XOR: the cipher as the M17 layer
	// will call it. M17 publishes no AES vector (m17.md 11): these were generated from that layout, the AES
	// blocks with the OpenSSL CLI (openssl enc -aes-<bits>-ecb -nopad -K <key>)
	static const struct {
		const char *key, *cipher[3], *ks7fff;
	} m17[] = {
		{ "000102030405060708090A0B0C0D0E0F",
		  { "9A90F29BD40FDAC354192BDA7351A8CF", "4036AD915208F5B19B4991F5210A324A",
		    "7D8F9742F5F5FAAE7A1DD2F3EEE588EC" }, "7825B4FBFAD2BE2D33B7DDDAE539E0CD" },
		{ "000102030405060708090A0B0C0D0E0F1011121314151617",
		  { "A03E986BFCCC78301C997E154830BB94", "4845F4D871DAB62F1A0929CEE55748E9",
		    "2836958F3FC57C8B714CB2CE9710322B" }, "5DFAA1F28DFCAE6B884C9946F60631C6" },
		{ "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F",
		  { "2390B6BAE71EE833859860EF765D8241", "1BCE28F5A14B2FB33E7BCD55995815C1",
		    "F455EC947D32F746F4D0439A486E9071" }, "E76A7259FD0D3EE80E861FA39F8D9DC9" },
	};
	for (unsigned v = 0; v < 3; v++) {
		size_t kl = hex(m17[v].key, key, sizeof key);
		check(aes_init(&aes, key, kl), "M17 key");
		for (unsigned fn = 0; fn < 3; fn++) {
			uint8_t pay[16];
			hex("0CF10980001122334455667788990000", c2, 16);
			c2[15] = (uint8_t)fn;
			aes_encrypt(&aes, c2, c2);
			for (unsigned i = 0; i < 16; i++)
				pay[i] = (uint8_t)(c2[i] ^ (16 * fn + i));	// plaintext 00..0F, 10..1F, 20..2F
			snprintf(what, sizeof what, "M17-AES-CTR key %zu bits FN %u", kl * 8, fn);
			check(eq_hex(pay, 16, m17[v].cipher[fn]), what);
		}
		hex("0CF10980001122334455667788997FFF", c2, 16);
		aes_encrypt(&aes, c2, c2);
		check(eq_hex(c2, 16, m17[v].ks7fff), "M17-AES-CTR FN 7FFF keystream");
	}
	aes_wipe(&aes);
}

// ---- SHA-256 (the tree's, as HMAC links it) ----

static void test_sha256(void)
{
	static const struct {
		const char *msg, *md;
	} v[] = {
		{ "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
		{ "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },	// FIPS 180-2 B.1
		{ "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",			// B.2
		  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
	};
	sha256_t s;
	uint8_t md[SHA256_LEN], a[997];

	for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
		sha256_init(&s);
		sha256_update(&s, v[i].msg, strlen(v[i].msg));
		sha256_final(&s, md);
		check(eq_hex(md, sizeof md, v[i].md), "SHA-256 FIPS 180-2 B.1 / B.2 / empty");
	}
	// B.3: a million "a", fed in pieces of an odd size
	memset(a, 'a', sizeof a);
	sha256_init(&s);
	for (size_t left = 1000000; left;) {
		size_t n = left < sizeof a ? left : sizeof a;
		sha256_update(&s, a, n);
		left -= n;
	}
	sha256_final(&s, md);
	check(eq_hex(md, sizeof md, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
	      "SHA-256 FIPS 180-2 B.3");
}

// ---- HMAC-SHA-256 ----

// RFC 4231 section 4 test cases 1-7 (case 5's output truncated to 128 bits, as the RFC gives it), then cases outside
// the RFC from Python's hmac module (an independent implementation): the empty key and message, a key of exactly one
// block (not hashed), one byte over (hashed), a message over four blocks
static const struct {
	const char *key;
	unsigned key_rep;	// key byte repeated, when not 0 (key holds one byte)
	const char *data;	// text
	unsigned data_rep;	// data byte repeated, when not 0 (data is hex of one byte)
	const char *mac;
	const char *name;
} rfc4231[] = {
	{ "0b", 20, "Hi There", 0, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "RFC 4231 1" },
	{ "4a656665", 0, "what do ya want for nothing?", 0,
	  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "RFC 4231 2" },
	{ "aa", 20, "dd", 50, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", "RFC 4231 3" },
	{ "0102030405060708090a0b0c0d0e0f10111213141516171819", 0, "cd", 50,
	  "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", "RFC 4231 4" },
	{ "0c", 20, "Test With Truncation", 0, "a3b6167473100ee06e0c796c2955552b", "RFC 4231 5 (128 bits)" },
	{ "aa", 131, "Test Using Larger Than Block-Size Key - Hash Key First", 0,
	  "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "RFC 4231 6" },
	{ "aa", 131, "This is a test using a larger than block-size key and a larger than block-size data. The key needs "
	  "to be hashed before being used by the HMAC algorithm.", 0,
	  "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", "RFC 4231 7" },
};

static size_t build_key(unsigned v, uint8_t *key, size_t max)
{
	if (!rfc4231[v].key_rep)
		return hex(rfc4231[v].key, key, max);
	hex(rfc4231[v].key, key, 1);
	memset(key, key[0], rfc4231[v].key_rep);
	return rfc4231[v].key_rep;
}

static size_t build_data(unsigned v, uint8_t *data, size_t max)
{
	if (!rfc4231[v].data_rep) {
		size_t n = strlen(rfc4231[v].data);
		memcpy(data, rfc4231[v].data, n);
		return n;
	}
	hex(rfc4231[v].data, data, 1);
	memset(data, data[0], rfc4231[v].data_rep);
	return rfc4231[v].data_rep;
}

static hmac_sha256_t hm, hm_keyed;

static void test_hmac(void)
{
	uint8_t key[256], data[512], mac[HMAC_SHA256_LEN], mac2[HMAC_SHA256_LEN], want[HMAC_SHA256_LEN];
	char what[96];

	for (unsigned v = 0; v < sizeof rfc4231 / sizeof rfc4231[0]; v++) {
		size_t kl = build_key(v, key, sizeof key), dl = build_data(v, data, sizeof data);
		size_t wl = hex(rfc4231[v].mac, want, sizeof want);

		hmac_sha256(key, kl, data, dl, mac);
		snprintf(what, sizeof what, "%s one-shot", rfc4231[v].name);
		check(memcmp(mac, want, wl) == 0, what);

		// Streaming a byte at a time, and in pieces of 1, 2, 3, ... bytes
		hmac_sha256_init(&hm, key, kl);
		for (size_t i = 0; i < dl; i++)
			hmac_sha256_update(&hm, &data[i], 1);
		hmac_sha256_final(&hm, mac2);
		snprintf(what, sizeof what, "%s byte by byte", rfc4231[v].name);
		check(memcmp(mac2, mac, sizeof mac) == 0, what);
		check(all_zero(&hm, sizeof hm), "final wipes the spent context");
		hmac_sha256_init(&hm, key, kl);
		for (size_t i = 0, step = 1; i < dl; i += step, step++)
			hmac_sha256_update(&hm, &data[i], dl - i < step ? dl - i : step);
		hmac_sha256_final(&hm, mac2);
		snprintf(what, sizeof what, "%s in growing pieces", rfc4231[v].name);
		check(memcmp(mac2, mac, sizeof mac) == 0, what);
	}

	static const struct {
		size_t klen, dlen;
		const char *data, *mac;
	} extra[] = {
		{ 0, 0, "", "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad" },
		{ 64, 14, "block-size key", "1dad230598e011a4e4eabc6c8da8f55ef9a66a8881d1e16e23ea116ae28231ec" },
		{ 65, 14, "block-size key", "d0b754c1861695b26bca3bd3a8581ef73f46ba28ada969cf73dbdb39d991eb2d" },
	};
	for (unsigned i = 0; i < sizeof key; i++)
		key[i] = (uint8_t)i;
	for (unsigned v = 0; v < sizeof extra / sizeof extra[0]; v++) {
		hmac_sha256(extra[v].klen ? key : NULL, extra[v].klen, extra[v].dlen ? extra[v].data : NULL,
			    extra[v].dlen, mac);
		check(eq_hex(mac, sizeof mac, extra[v].mac), "HMAC key length edge (Python hmac)");
	}
	for (unsigned i = 0; i < 300; i++)
		data[i] = (uint8_t)(i * 7 + 3);
	hmac_sha256(key + 0x40, 32, data, 300, mac);
	check(eq_hex(mac, sizeof mac, "060d5059137515ccde796f7cec48c63f3dc969fa7218a018fb266129fe89526d"),
	      "HMAC 300-byte message (Python hmac)");

	// Keyed once, copied per message (RFC 6979's loop, the DRBG): each copy equals the one-shot MAC
	hmac_sha256_init(&hm_keyed, key, 32);
	for (size_t n = 0; n <= 130; n += 13) {
		hm = hm_keyed;
		hmac_sha256_update(&hm, data, n);
		hmac_sha256_final(&hm, mac);
		hmac_sha256(key, 32, data, n, mac2);
		check(memcmp(mac, mac2, sizeof mac) == 0, "copy of a keyed context = one-shot");
	}
	hmac_sha256_wipe(&hm_keyed);
	check(all_zero(&hm_keyed, sizeof hm_keyed), "hmac_sha256_wipe leaves zeros");

	// A keyed context holds the two states, not the key: a key longer than a block is hashed in the inner context, and
	// its last bytes (131 mod 64 = 3, left in SHA-256's block buffer by the padding) must not stay there for a copy to
	// carry along
	memset(key, 0xaa, 131);
	hmac_sha256_init(&hm_keyed, key, 131);
	check(all_zero(hm_keyed.inner.block, sizeof hm_keyed.inner.block) &&
	      all_zero(hm_keyed.outer.block, sizeof hm_keyed.outer.block), "a long key leaves no key bytes in the context");
	hm = hm_keyed;
	hmac_sha256_update(&hm, rfc4231[5].data, strlen(rfc4231[5].data));
	hmac_sha256_final(&hm, mac);
	check(eq_hex(mac, sizeof mac, rfc4231[5].mac), "RFC 4231 6 through a copied keyed context");
	hmac_sha256_wipe(&hm_keyed);
	hmac_sha256_wipe(NULL);
}

// ---- HMAC_DRBG ----

static hmac_drbg_t drbg, drbg2;

static void test_drbg(void)
{
	uint8_t seed[64], out[160], out2[160];

	// NIST CAVP HMAC_DRBG.rsp (drbgvectors_no_reseed), [SHA-256] PredictionResistance False, EntropyInputLen 256,
	// NonceLen 128, no personalization, no additional input, ReturnedBitsLen 1024, COUNT 0: instantiate with
	// entropy || nonce, generate twice, the second output is ReturnedBits. Also reproduced by the reference below
	size_t sl = hex("ca851911349384bffe89de1cbdc46e6831e44d34a4fb935ee285dd14b71a7488"
			"659ba96c601dc69fc902940805ec0ca8", seed, sizeof seed);
	hmac_drbg_seed(&drbg, seed, sl);
	check(drbg.reseeds == 0, "seed: count 0");
	hmac_drbg_generate(&drbg, out, 128);
	hmac_drbg_generate(&drbg, out, 128);
	check(drbg.reseeds == 2, "count of generate calls");
	check(eq_hex(out, 128, "e528e9abf2dece54d47c7e75e5fe302149f817ea9fb4bee6f4199697d04d5b89"
			       "d54fbb978a15b5c443c9ec21036d2460b6f73ebad0dc2aba6e624abf07745bc1"
			       "07694bb7547bb0995f70de25d6b29e2d3011bb19d27676c07162c8b5ccde0668"
			       "961df86803482cb37ed6d5c0bb8d50cf1f50d476aa0458bdaba806f48be9dcb8"),
	      "HMAC_DRBG CAVP no-reseed SHA-256 COUNT 0");

	// An independent implementation: hmac_drbg_ref.py, written for this test from SP 800-90A Rev. 1 10.1.2 on Python's
	// hmac module (kept with the package's scratch work, not in the tree). Seed bytes 0..54, generate 10, 32, 75, reseed
	// with 80..a0, generate 33, 0, 64; the state after seed and reseed; an empty seed
	for (unsigned i = 0; i < 55; i++)
		seed[i] = (uint8_t)i;
	hmac_drbg_seed(&drbg, seed, 55);
	check(eq_hex(drbg.k, 32, "44375b762293ad8c70cf359fea7552e4f47aa77112d47321e4c3ea7d6bc7fd03") &&
	      eq_hex(drbg.v, 32, "93e50cca01b04f2d9c1e79ec04d9183a492b107922c525f2c0648258905c080f"), "DRBG seed state");
	drbg2 = drbg;
	hmac_drbg_generate(&drbg, out, 10);
	check(eq_hex(out, 10, "00ad633c7dc71b056cbb"), "DRBG generate 10");
	hmac_drbg_generate(&drbg, out, 32);
	check(eq_hex(out, 32, "4d99e66ffc1b71bbaebd3c45245c633fd2188f46a3362b1593deff16cee022d9"), "DRBG generate 32");
	hmac_drbg_generate(&drbg, out, 75);
	check(eq_hex(out, 75, "41a2ad656d6e90864736b893972267993262776328e4a42ce97521451889acb9f7f686ad8a8aa60e3891cc"
			      "68423214a9573f5712f6a8843d5aeb9aafc4cfd65b83672c1dcd2580307bb304"), "DRBG generate 75");
	check(drbg.reseeds == 3, "three generate calls counted");
	for (unsigned i = 0; i < 33; i++)
		seed[i] = (uint8_t)(0x80 + i);
	hmac_drbg_reseed(&drbg, seed, 33);
	check(drbg.reseeds == 0, "reseed: count 0");
	check(eq_hex(drbg.k, 32, "62a4043b2258fb7e422827564ab12f082d1b0a98c97df6e88238a408f46ce633") &&
	      eq_hex(drbg.v, 32, "5502060081e6b20944792d13234583460bbc5745afc053b0adfce22dc231680d"), "DRBG reseed state");
	hmac_drbg_generate(&drbg, out, 33);
	check(eq_hex(out, 33, "a9c0ecf3d84529e80f3d3f2d3bf611bc5927ec62170c0454740288909a6648a683"), "DRBG generate 33");
	memset(out, 0x77, 4);
	hmac_drbg_generate(&drbg, out, 0);
	check(out[0] == 0x77, "generate 0 writes nothing");
	hmac_drbg_generate(&drbg, out, 64);
	check(eq_hex(out, 64, "1f148a8eadc5593d7e7d4cae4d2869f5bada20f67406c2d6c71002ef501d37b8"
			      "5b943d9d684b6d83ff42d13995868d3ff900cd694e93d6580d3ff1532eea0ec8"), "DRBG generate 64");
	check(eq_hex(drbg.k, 32, "50e0345aef2c6aecbab05f5373356f6369eb651c1b3c8ed260fc8634ba668562") &&
	      eq_hex(drbg.v, 32, "b6cd555e36ff6379d1914c1adda5a40edca64de344b148be859c60df6eda102e"), "DRBG end state");
	check(drbg.reseeds == 3, "generate 0 counts too");
	hmac_drbg_seed(&drbg, NULL, 0);
	hmac_drbg_generate(&drbg, out, 16);
	check(eq_hex(out, 16, "b44299907e4e42aa4fded5d6153e8bac"), "DRBG empty seed");

	// Determinism, and one request of n bytes is the first n of a longer one from the same state
	for (size_t n = 1; n <= sizeof out; n += 7) {
		hmac_drbg_t a = drbg2, b = drbg2;
		hmac_drbg_generate(&a, out, n);
		hmac_drbg_generate(&b, out2, sizeof out2);
		check(memcmp(out, out2, n) == 0, "a short request is the start of a long one");
		hmac_drbg_wipe(&a);
		hmac_drbg_wipe(&b);
	}

	// Reseeding changes the stream; reseeding with the same input on the same state does the same
	drbg = drbg2;
	hmac_drbg_generate(&drbg, out, 32);
	drbg = drbg2;
	hmac_drbg_reseed(&drbg, seed, 1);
	hmac_drbg_generate(&drbg, out2, 32);
	check(memcmp(out, out2, 32) != 0, "reseed changes the stream");
	drbg = drbg2;
	hmac_drbg_reseed(&drbg, seed, 1);
	hmac_drbg_generate(&drbg, out, 32);
	check(memcmp(out, out2, 32) == 0, "same reseed, same stream");
	drbg = drbg2;
	hmac_drbg_reseed(&drbg, NULL, 0);	// an empty reseed still moves K and V (Update with no input)
	hmac_drbg_generate(&drbg, out, 32);
	check(memcmp(out, out2, 32) != 0, "empty reseed also changes the stream");

	hmac_drbg_wipe(&drbg);
	check(all_zero(&drbg, sizeof drbg), "hmac_drbg_wipe leaves zeros");
	hmac_drbg_wipe(&drbg2);
	check(all_zero(&drbg2, sizeof drbg2), "hmac_drbg_wipe leaves zeros");
	hmac_drbg_wipe(NULL);
}

static void test_wipe_helpers(void)
{
	uint8_t a[33], b[33];

	memset(a, 0xee, sizeof a);
	crypto_wipe(a, sizeof a);
	check(all_zero(a, sizeof a), "crypto_wipe");
	memset(a, 1, sizeof a);
	memcpy(b, a, sizeof b);
	check(crypto_equal(a, b, sizeof a), "crypto_equal same");
	b[32] ^= 0x80;
	check(!crypto_equal(a, b, sizeof a), "crypto_equal last byte differs");
}

int main(void)
{
	test_aes_ecb();
	test_aes_keys();
	test_aes_ctr();
	test_sha256();
	test_hmac();
	test_drbg();
	test_wipe_helpers();
	if (failed) {
		printf("cryptoTest: %u of %u checks failed\n", failed, checks);
		return 1;
	}
	printf("cryptoTest: %u checks passed\n", checks);
	return 0;
}
