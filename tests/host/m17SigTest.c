// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 stream signatures (core/m17/m17_sig.c; docs/spec/m17.md 7) against M17-SIG, the M17 specification's own test
// vector (m17.md 7.5: hash input, H, the signature frames), and M17-SIG-AES128 (the same stream sent AES-128
// encrypted: the hash takes the ciphertext; generated from m17.md 6.2 and 7.2, the AES blocks as in m17CryptTest,
// the signature by RFC 6979), what a receiver concludes for a late join, an FN gap, a lost tail frame and a stream
// that ends without its tail, and a public key's fingerprint
#include <stdio.h>
#include <string.h>
#include "m17/m17_sig.h"
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
	uint8_t w[128];
	size_t wn = hex(want, w, sizeof w);

	return wn == n && memcmp(got, w, n) == 0;
}

// ---- M17-SIG: the M17 specification's test vector (m17.md 7.5) ----
#define N0CALL		0x00004B13D106ull
#define W1AW		0x0000001680B7ull
#define TS_20261118	217123200u	// 2026-11-18T00:00:00Z in seconds since 2020-01-01 (0CF10980)

static const char sig_tag[] = "4D31372D5349472D7632";
static const char sig_pa[] = "00004B13D1060000001680B70CF10980";
static const char sig_input[] = "4D31372D5349472D7632"
				"0000000102030405060708090A0B0C0D0E0F"
				"0001101112131415161718191A1B1C1D1E1F"
				"0002202122232425262728292A2B2C2D2E2F"
				"7FFB00004B13D1060000001680B70CF10980";
static const char sig_h[] = "2DD1BC7B5C017F100F9379F42EAA205CE90B8C7EA263F2F80E37BA6E6D6B588D";
static const char sig_rs[] = "E689788A5C2FBDD0FA2137429517B7C9DD5E3C2BFFB9667D4EDB9A8472F3CF31"
			     "C45421DED7F7025608BFD928B06BEFD4A44526609B3DC54FC2B36944F6121D54";
static const char *const sig_frames[4] = {
	"E689788A5C2FBDD0FA2137429517B7C9", "DD5E3C2BFFB9667D4EDB9A8472F3CF31",
	"C45421DED7F7025608BFD928B06BEFD4", "A44526609B3DC54FC2B36944F6121D54",
};
static const char sig_qx[] = "B91931F4DC5EFC54C23C0B8C87704C45585BD791046FEF05BFBFD5A0CE9B53B3";

// ---- M17-SIG-AES128 (key and META of m17CryptTest's M17-AES-CTR rows; generated, not published by M17) ----
static const char aes_key[] = "000102030405060708090A0B0C0D0E0F";
static const char aes_meta[] = "0CF1098000112233445566778899";
static const char *const aes_cipher[3] = {
	"9A90F29BD40FDAC354192BDA7351A8CF", "4036AD915208F5B19B4991F5210A324A", "7D8F9742F5F5FAAE7A1DD2F3EEE588EC",
};
static const char aes_input[] = "4D31372D5349472D7632"
				"00009A90F29BD40FDAC354192BDA7351A8CF"
				"00014036AD915208F5B19B4991F5210A324A"
				"00027D8F9742F5F5FAAE7A1DD2F3EEE588EC"
				"7FFB00004B13D1060000001680B70CF10980";
static const char aes_h[] = "E2866335B08795A5E3EC0F6B27B3989D518F82935553E856F3D4AC82A70A5F42";
static const char aes_rs[] = "18B30AB7DD375CE17C03FC6EE6998CF9EF9DDC447C6B6855D7715141E78C9C3B"
			     "D21986C2E36B83163781D10712769E14B65AE35C922AD0FC317C964ABB4F8FC6";

// The spec's payloads: frame i holds bytes 16i .. 16i + 15
static void plain(unsigned i, uint8_t p[M17_STREAM_BYTES])
{
	for (unsigned j = 0; j < M17_STREAM_BYTES; j++)
		p[j] = (uint8_t)(16u * i + j);
}

// Static like the engine's: the hash contexts live in its transmission union, not on a stack
static m17_sig_tx_t tx;
static m17_sig_rx_t rx;
static aes_ctx_t aes;

static void test_tag_auth(void)
{
	uint8_t a[M17_AUTH_BYTES], b[sizeof M17_SIG_TAG - 1u];
	uint64_t signer, dst;
	uint32_t ts;

	memcpy(b, M17_SIG_TAG, sizeof b);
	check(sizeof b == M17_SIG_TAG_LEN && eq_hex(b, sizeof b, sig_tag), "tag M17-SIG-v2, 10 bytes");

	m17_auth_make(N0CALL, W1AW, TS_20261118, a);
	check(eq_hex(a, sizeof a, sig_pa), "authentication frame: signer, destination, timestamp");
	m17_auth_read(a, &signer, &dst, &ts);
	check(signer == N0CALL && dst == W1AW && ts == TS_20261118, "authentication frame read back");

	m17_auth_make(0xFFFFFFFFFFFFull, 0x123456789ABCull, 0xFFFFFFFFu, a);
	check(eq_hex(a, sizeof a, "FFFFFFFFFFFF 123456789ABC FFFFFFFF"), "broadcast signer, all-ones timestamp");
	m17_auth_read(a, &signer, &dst, &ts);
	check(signer == 0xFFFFFFFFFFFFull && dst == 0x123456789ABCull && ts == 0xFFFFFFFFu, "read back");

	check(m17_sig_fn(0) == 0x7FFC && m17_sig_fn(1) == 0x7FFD && m17_sig_fn(2) == 0x7FFE && m17_sig_fn(3) == 0xFFFF,
	      "signature frame numbers");
}

// The 82-byte hash input as the vector lists it, against the frames it is made of; and SHA-256 of it is H
static void test_hash_input(void)
{
	uint8_t in[82], want[82], p[M17_STREAM_BYTES], d[SHA256_LEN];
	sha256_t h;
	size_t n = 0;

	memcpy(in, M17_SIG_TAG, M17_SIG_TAG_LEN);
	n = M17_SIG_TAG_LEN;
	for (unsigned i = 0; i < 3; i++) {
		in[n++] = 0;
		in[n++] = (uint8_t)i;
		plain(i, p);
		memcpy(&in[n], p, sizeof p);
		n += sizeof p;
	}
	in[n++] = 0x7F;
	in[n++] = 0xFB;
	m17_auth_make(N0CALL, W1AW, TS_20261118, &in[n]);
	n += M17_AUTH_BYTES;
	check(n == 82 && hex(sig_input, want, sizeof want) == 82 && memcmp(in, want, n) == 0, "the 82-byte hash input");
	sha256_init(&h);
	sha256_update(&h, want, sizeof want);
	sha256_final(&h, d);
	check(eq_hex(d, sizeof d, sig_h), "SHA-256 of the hash input is H");

	hex(aes_input, want, sizeof want);
	sha256_init(&h);
	sha256_update(&h, want, sizeof want);
	sha256_final(&h, d);
	check(eq_hex(d, sizeof d, aes_h), "AES-128: SHA-256 of the hash input is H");
}

// The transmitter: three frames and the authentication frame give H; the signature splits over four frames
static void test_tx(void)
{
	uint8_t p[M17_STREAM_BYTES], a[M17_AUTH_BYTES], hash[M17_HASH_BYTES], rs[M17_SIG_RS_BYTES];

	m17_sig_tx_start(&tx);
	for (unsigned i = 0; i < 3; i++) {
		plain(i, p);
		m17_sig_tx_frame(&tx, (uint16_t)i, p);
	}
	check(tx.frames == 3, "three data frames hashed");
	m17_auth_make(N0CALL, W1AW, TS_20261118, a);
	m17_sig_tx_auth(&tx, a, hash);
	check(eq_hex(hash, sizeof hash, sig_h), "M17-SIG: H");

	hex(sig_rs, rs, sizeof rs);
	for (uint8_t i = 0; i < 4; i++)
		check(eq_hex(&rs[16u * i], M17_STREAM_BYTES, sig_frames[i]), "signature frame payload");

	// One bit anywhere changes H: the FN is hashed, so is the payload
	m17_sig_tx_start(&tx);
	for (unsigned i = 0; i < 3; i++) {
		plain(i, p);
		m17_sig_tx_frame(&tx, (uint16_t)(i == 2 ? 3 : i), p);
	}
	m17_sig_tx_auth(&tx, a, hash);
	check(!eq_hex(hash, sizeof hash, sig_h), "another FN, another H");
}

// The same stream AES-128 encrypted: the hash is over the ciphertext, the authentication frame stays plain
static void test_tx_aes(void)
{
	uint8_t key[16], meta[M17_META_BYTES], p[M17_STREAM_BYTES], a[M17_AUTH_BYTES], hash[M17_HASH_BYTES];

	hex(aes_key, key, sizeof key);
	hex(aes_meta, meta, sizeof meta);
	check(aes_init(&aes, key, sizeof key), "AES-128 key");
	m17_sig_tx_start(&tx);
	for (unsigned i = 0; i < 3; i++) {
		plain(i, p);
		m17_aes_frame(&aes, meta, (uint16_t)i, p);
		check(eq_hex(p, sizeof p, aes_cipher[i]), "M17-SIG-AES128: ciphertext");
		m17_sig_tx_frame(&tx, (uint16_t)i, p);
	}
	m17_auth_make(N0CALL, W1AW, TS_20261118, a);
	m17_sig_tx_auth(&tx, a, hash);
	check(eq_hex(hash, sizeof hash, aes_h), "M17-SIG-AES128: H over the ciphertext");
	aes_wipe(&aes);
}

// The tail of a stream into rx: the authentication frame, then the signature frames in skip's absence (bit i of skip:
// leave out tail part i, 0 the authentication frame)
static void rx_tail(const char *rs_hex, unsigned skip, unsigned *tails)
{
	uint8_t a[M17_AUTH_BYTES], rs[M17_SIG_RS_BYTES];

	m17_auth_make(N0CALL, W1AW, TS_20261118, a);
	hex(rs_hex, rs, sizeof rs);
	if (!(skip & 1u))
		*tails += m17_sig_rx_frame(&rx, M17_FN_AUTH, a) == M17_SIG_FRAME_TAIL;
	for (uint8_t i = 0; i < 4; i++) {
		if (skip & (2u << i))
			continue;
		check(!m17_sig_rx_ready(&rx), "not ready before the last signature frame");
		*tails += m17_sig_rx_frame(&rx, m17_sig_fn(i), &rs[16u * i]) == M17_SIG_FRAME_TAIL;
	}
}

// A whole signed stream received from frame 0: ready, with H, r || s and the authentication fields
static void test_rx_whole(void)
{
	uint8_t p[M17_STREAM_BYTES];
	uint64_t signer, dst;
	uint32_t ts;
	unsigned data = 0, tails = 0;

	m17_sig_rx_start(&rx, 0);
	check(rx.state == M17_SIG_PENDING && !m17_sig_rx_ready(&rx), "start at FN 0: pending");
	for (unsigned i = 0; i < 3; i++) {
		plain(i, p);
		data += m17_sig_rx_frame(&rx, (uint16_t)i, p) == M17_SIG_FRAME_DATA;
	}
	check(data == 3, "data frames are played");
	rx_tail(sig_rs, 0, &tails);
	check(tails == 5, "the five tail frames are never played");
	check(rx.state == M17_SIG_PENDING && m17_sig_rx_ready(&rx), "the jump from FN 2 to 7FFB is no gap: ready");
	check(eq_hex(rx.hash, sizeof rx.hash, sig_h), "receiver's H");
	check(eq_hex(rx.rs, sizeof rx.rs, sig_rs), "receiver's r || s");
	check(eq_hex(rx.auth, sizeof rx.auth, sig_pa), "receiver's authentication frame");
	m17_auth_read(rx.auth, &signer, &dst, &ts);
	check(signer == N0CALL && dst == W1AW && ts == TS_20261118, "receiver's signer, destination, timestamp");

	// A second authentication frame (a corrupt FN, say) changes nothing already hashed
	{
		uint8_t junk[M17_STREAM_BYTES];
		memset(junk, 0xEE, sizeof junk);
		check(m17_sig_rx_frame(&rx, M17_FN_AUTH, junk) == M17_SIG_FRAME_TAIL, "repeat authentication frame: tail");
		check(eq_hex(rx.auth, sizeof rx.auth, sig_pa) && eq_hex(rx.hash, sizeof rx.hash, sig_h),
		      "repeat authentication frame: ignored");
	}

	// A payload corrupted on the way still gives a hash (the verify fails, the receiver can't tell it from tampering)
	m17_sig_rx_start(&rx, 0);
	for (unsigned i = 0; i < 3; i++) {
		plain(i, p);
		if (i == 1)
			p[5] ^= 0x10;
		m17_sig_rx_frame(&rx, (uint16_t)i, p);
	}
	rx_tail(sig_rs, 0, &tails);
	check(m17_sig_rx_ready(&rx) && !eq_hex(rx.hash, sizeof rx.hash, sig_h), "a flipped bit: ready, another H");
}

// The encrypted stream as received: the hash takes the frames before decrypting, so it needs no key
static void test_rx_aes(void)
{
	uint8_t c[M17_STREAM_BYTES];
	unsigned tails = 0;

	m17_sig_rx_start(&rx, 0);
	for (unsigned i = 0; i < 3; i++) {
		hex(aes_cipher[i], c, sizeof c);
		check(m17_sig_rx_frame(&rx, (uint16_t)i, c) == M17_SIG_FRAME_DATA, "ciphertext frame: data");
	}
	rx_tail(aes_rs, 0, &tails);
	check(tails == 5 && m17_sig_rx_ready(&rx), "M17-SIG-AES128: ready");
	check(eq_hex(rx.hash, sizeof rx.hash, aes_h), "M17-SIG-AES128: receiver's H over the ciphertext");
	check(eq_hex(rx.rs, sizeof rx.rs, aes_rs), "M17-SIG-AES128: receiver's r || s");
}

// The conclusions a receiver draws without any curve work
static void test_rx_conclusions(void)
{
	uint8_t p[M17_STREAM_BYTES];
	unsigned tails = 0, data = 0;

	// Late join: frame 1 first. The frames still play, the tail is still kept for showing the signer, no hash
	m17_sig_rx_start(&rx, 1);
	check(rx.state == M17_SIG_LATE, "late join: LATE at once");
	for (unsigned i = 1; i < 3; i++) {
		plain(i, p);
		data += m17_sig_rx_frame(&rx, (uint16_t)i, p) == M17_SIG_FRAME_DATA;
	}
	rx_tail(sig_rs, 0, &tails);
	check(data == 2 && tails == 5, "late join: data played, tail not");
	check(rx.state == M17_SIG_LATE && !m17_sig_rx_ready(&rx), "late join: LATE at the end, never ready");
	check(eq_hex(rx.auth, sizeof rx.auth, sig_pa), "late join: the signer is still known");
	m17_sig_rx_start(&rx, 0x7FFB);
	check(rx.state == M17_SIG_LATE, "joined in the tail: LATE");
	m17_sig_rx_start(&rx, M17_FN_END);
	check(rx.state == M17_SIG_PENDING, "first FN 0 with the end bit: not late");

	// FN gap: frame 1 lost. Frame 2 still plays
	m17_sig_rx_start(&rx, 0);
	plain(0, p);
	m17_sig_rx_frame(&rx, 0, p);
	plain(2, p);
	check(m17_sig_rx_frame(&rx, 2, p) == M17_SIG_FRAME_DATA, "after a gap: still played");
	check(rx.state == M17_SIG_INCOMPLETE, "FN gap: INCOMPLETE");
	rx_tail(sig_rs, 0, &tails);
	check(rx.state == M17_SIG_INCOMPLETE && !m17_sig_rx_ready(&rx), "FN gap: INCOMPLETE at the end, never ready");

	// The first frame lost although the stream was started at 0 (the LSF came, frame 0 did not)
	m17_sig_rx_start(&rx, 0);
	plain(1, p);
	m17_sig_rx_frame(&rx, 1, p);
	check(rx.state == M17_SIG_INCOMPLETE, "frame 0 lost: INCOMPLETE");

	// Every data frame lost, the tail received: INCOMPLETE, never ready (a hash of the tag and the authentication
	// frame alone would only fail the verify, shown as invalid)
	m17_sig_rx_start(&rx, 0);
	tails = 0;
	rx_tail(sig_rs, 0, &tails);
	check(tails == 5 && rx.state == M17_SIG_INCOMPLETE && !m17_sig_rx_ready(&rx),
	      "every data frame lost: INCOMPLETE, never ready");
	check(eq_hex(rx.auth, sizeof rx.auth, sig_pa), "every data frame lost: the signer is still known");

	// A repeated frame and a frame out of order are gaps too: the hash can't be rebuilt
	m17_sig_rx_start(&rx, 0);
	plain(0, p);
	m17_sig_rx_frame(&rx, 0, p);
	m17_sig_rx_frame(&rx, 0, p);
	check(rx.state == M17_SIG_INCOMPLETE, "repeated FN: INCOMPLETE");

	// Each tail frame lost in turn
	for (unsigned part = 0; part < 5; part++) {
		char what[64];
		m17_sig_rx_start(&rx, 0);
		for (unsigned i = 0; i < 3; i++) {
			plain(i, p);
			m17_sig_rx_frame(&rx, (uint16_t)i, p);
		}
		rx_tail(sig_rs, 1u << part, &tails);
		snprintf(what, sizeof what, "tail part %u lost: never ready", part);
		check(!m17_sig_rx_ready(&rx), what);
		// The last one lost: the stream simply stops; the caller sees PENDING, not ready (the tail never came)
		snprintf(what, sizeof what, "tail part %u lost: %s", part, part < 4 ? "INCOMPLETE" : "still pending");
		check(rx.state == (part < 4 ? M17_SIG_INCOMPLETE : M17_SIG_PENDING), what);
	}

	// A stream that ends without its tail: its last data frame carries the end bit, as an unsigned one would
	m17_sig_rx_start(&rx, 0);
	for (unsigned i = 0; i < 3; i++) {
		plain(i, p);
		data = m17_sig_rx_frame(&rx, (uint16_t)(i | (i == 2 ? M17_FN_END : 0)), p);
	}
	check(data == M17_SIG_FRAME_DATA, "the end-bit data frame is played");
	check(rx.state == M17_SIG_INCOMPLETE && !m17_sig_rx_ready(&rx), "no tail: INCOMPLETE");

	// ... or one that just stops after its data (signal lost): pending, never ready
	m17_sig_rx_start(&rx, 0);
	for (unsigned i = 0; i < 3; i++) {
		plain(i, p);
		m17_sig_rx_frame(&rx, (uint16_t)i, p);
	}
	check(rx.state == M17_SIG_PENDING && !m17_sig_rx_ready(&rx), "cut off after the data: not ready");

	// Data after the tail began
	m17_sig_rx_start(&rx, 0);
	plain(0, p);
	m17_sig_rx_frame(&rx, 0, p);
	m17_auth_make(N0CALL, W1AW, TS_20261118, p);
	m17_sig_rx_frame(&rx, M17_FN_AUTH, p);
	check(m17_sig_rx_frame(&rx, 1, p) == M17_SIG_FRAME_DATA && rx.state == M17_SIG_INCOMPLETE,
	      "data after the authentication frame: INCOMPLETE");

	// FNs in the tail's range that are none of the five: a corrupt FN. Never played, and the stream is lost
	{
		static const uint16_t odd[] = { 0x7FFF, 0xFFFB, 0xFFFC, 0xFFFD, 0xFFFE };
		for (unsigned i = 0; i < sizeof odd / sizeof odd[0]; i++) {
			m17_sig_rx_start(&rx, 0);
			plain(0, p);
			m17_sig_rx_frame(&rx, 0, p);
			check(m17_sig_rx_frame(&rx, odd[i], p) == M17_SIG_FRAME_TAIL, "odd tail FN: not played");
			check(rx.state == M17_SIG_INCOMPLETE, "odd tail FN: INCOMPLETE");
		}
	}

	// LATE stays LATE, whatever else goes missing
	m17_sig_rx_start(&rx, 5);
	plain(7, p);
	m17_sig_rx_frame(&rx, 7, p);
	m17_sig_rx_frame(&rx, 0x7FFD, p);
	check(rx.state == M17_SIG_LATE, "LATE is final");
}

// The fingerprint of the M17-SIG public key. Expected value derived outside this code with Python's hashlib: y ends in
// 0x70, even, so the compressed key is 02 || x, and
//   hashlib.sha256(bytes.fromhex("02" + Qx)).digest()[:8].hex() = 62320e4e537b9c98
static void test_key_fp(void)
{
	uint8_t q[33], fp[M17_KEY_FP_BYTES], fp3[M17_KEY_FP_BYTES];

	q[0] = 0x02;
	hex(sig_qx, &q[1], 32);
	m17_key_fp(q, fp);
	check(eq_hex(fp, sizeof fp, "62320E4E537B9C98"), "fingerprint of the M17-SIG public key");
	q[0] = 0x03;
	m17_key_fp(q, fp3);
	check(memcmp(fp, fp3, sizeof fp) != 0, "the other y: another fingerprint");
}

int main(void)
{
	test_tag_auth();
	test_hash_input();
	test_tx();
	test_tx_aes();
	test_rx_whole();
	test_rx_aes();
	test_rx_conclusions();
	test_key_fp();
	if (failed) {
		printf("m17SigTest: %u of %u checks failed\n", failed, checks);
		return 1;
	}
	printf("m17SigTest: %u checks passed\n", checks);
	return 0;
}
