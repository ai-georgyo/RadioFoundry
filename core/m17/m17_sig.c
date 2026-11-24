// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 stream signatures (docs/spec/m17.md 7): the stream hash on both sides, the authentication frame's fields, the
// signature frames' halves, a public key's fingerprint, and what a receiver can conclude before any curve work. The
// verify itself is p256_verify's, in the radio core's task. Built with CONFIG_M17_SIG
#include "config.h"

#if CONFIG_M17_SIG

#include <string.h>
#include "m17_sig.h"

// got: the signature frames in bits 0-3, the authentication frame in bit 4
#define GOT_AUTH	0x10u
#define GOT_ALL		0x1Fu

_Static_assert(M17_AUTH_BYTES == M17_STREAM_BYTES, "the authentication frame is one STREAM field");
_Static_assert(M17_SIG_RS_BYTES == 4u * M17_STREAM_BYTES, "r || s fills the four signature frames");
_Static_assert(M17_HASH_BYTES == SHA256_LEN, "H is a SHA-256 digest");
_Static_assert(sizeof M17_SIG_TAG - 1u == M17_SIG_TAG_LEN, "the tag is hashed without its terminator");

void m17_key_fp(const uint8_t q_c[33], uint8_t fp[M17_KEY_FP_BYTES])
{
	sha256_t h;
	uint8_t d[SHA256_LEN];

	sha256_init(&h);
	sha256_update(&h, q_c, 33u);
	sha256_final(&h, d);
	memcpy(fp, d, M17_KEY_FP_BYTES);
}

void m17_auth_make(uint64_t signer, uint64_t dst, uint32_t ts, uint8_t out[M17_AUTH_BYTES])
{
	m17_addr_put(&out[0], signer);
	m17_addr_put(&out[M17_ADDR_BYTES], dst);
	out[12] = (uint8_t)(ts >> 24);
	out[13] = (uint8_t)(ts >> 16);
	out[14] = (uint8_t)(ts >> 8);
	out[15] = (uint8_t)ts;
}

void m17_auth_read(const uint8_t a[M17_AUTH_BYTES], uint64_t *signer, uint64_t *dst, uint32_t *ts)
{
	*signer = m17_addr_get(&a[0]);
	*dst = m17_addr_get(&a[M17_ADDR_BYTES]);
	*ts = (uint32_t)a[12] << 24 | (uint32_t)a[13] << 16 | (uint32_t)a[14] << 8 | a[15];
}

// One frame into the hash: its FN as sent (big endian, the end bit as it is), then its 16 STREAM bytes (m17.md 7.2)
static void hash_frame(sha256_t *h, uint16_t fn, const uint8_t stream[M17_STREAM_BYTES])
{
	const uint8_t b[2] = { (uint8_t)(fn >> 8), (uint8_t)fn };

	sha256_update(h, b, sizeof b);
	sha256_update(h, stream, M17_STREAM_BYTES);
}

static void hash_start(sha256_t *h)
{
	sha256_init(h);
	sha256_update(h, M17_SIG_TAG, M17_SIG_TAG_LEN);
}

// --- Transmit ---

void m17_sig_tx_start(m17_sig_tx_t *t)
{
	hash_start(&t->h);
	t->frames = 0;
}

void m17_sig_tx_frame(m17_sig_tx_t *t, uint16_t fn, const uint8_t stream[M17_STREAM_BYTES])
{
	hash_frame(&t->h, fn, stream);
	t->frames++;
}

void m17_sig_tx_auth(m17_sig_tx_t *t, const uint8_t auth[M17_AUTH_BYTES], uint8_t hash[M17_HASH_BYTES])
{
	hash_frame(&t->h, M17_FN_AUTH, auth);
	sha256_final(&t->h, hash);
}

// --- Receive ---

void m17_sig_rx_start(m17_sig_rx_t *r, uint16_t first_fn)
{
	memset(r, 0, sizeof *r);
	hash_start(&r->h);
	// The end bit doesn't make a first frame late: only frames missed before it do
	r->state = (first_fn & M17_FN_MASK) ? M17_SIG_LATE : M17_SIG_PENDING;
}

// A tail FN's place: 0 the authentication frame, 1-4 the signature frames; -1 for an FN in the tail's range
// (7FFB-7FFF with or without the end bit) that is none of the five, which only a corrupt FN gives
static int tail_part(uint16_t fn)
{
	if (fn == M17_FN_AUTH) {
		return 0;
	}
	for (uint8_t i = 0; i < 4u; i++) {
		if (fn == m17_sig_fn(i)) {
			return 1 + i;
		}
	}
	return -1;
}

static void lost(m17_sig_rx_t *r)
{
	if (r->state == M17_SIG_PENDING) {
		r->state = M17_SIG_INCOMPLETE;
	}
}

// Conclusions come as soon as something is known to be missing, so the stream's end needs no call of its own: a data
// frame off the expected FN (a gap, a repeat, or data after the tail began), a data frame with the end bit (the tail
// cut off), a tail frame whose predecessors in the tail are not all in or that follows no data frame, an FN in the
// tail's range that is none of the five. LATE and INCOMPLETE are final. What this can't see: the last data frame lost (the next FN is 7FFB either way;
// the check then fails), and a stream that simply stops before its last frame (the caller finds the state still
// PENDING with m17_sig_rx_ready false: the tail never came)
uint8_t m17_sig_rx_frame(m17_sig_rx_t *r, uint16_t fn, const uint8_t stream[M17_STREAM_BYTES])
{
	// Tail parts that must be in when part p arrives: none for the authentication frame, then each one before it
	static const uint8_t before[5] = { 0x00u, GOT_AUTH, GOT_AUTH | 0x01u, GOT_AUTH | 0x03u, GOT_AUTH | 0x07u };
	uint16_t n = fn & M17_FN_MASK;
	int p;

	if (n < M17_FN_AUTH) {
		if (r->state == M17_SIG_PENDING) {
			if (r->got || n != r->next_fn || (fn & M17_FN_END)) {
				lost(r);
			} else {
				hash_frame(&r->h, fn, stream);
				r->next_fn++;
			}
		}
		return M17_SIG_FRAME_DATA;
	}
	p = tail_part(fn);
	if (p < 0) {
		lost(r);
		return M17_SIG_FRAME_TAIL;
	}
	// The tail's parts in order, after at least one data frame: next_fn 0 here is a stream started at FN 0 whose every
	// data frame was lost (one that really has none starts at 7FFB, so is LATE already). Hashing on would make a lost
	// stream look like a bad signature
	if ((r->got & before[p]) != before[p] || r->next_fn == 0) {
		lost(r);
	}
	if (p == 0) {
		// The first copy only: the hash is closed by it, and a later one could disagree with what was hashed
		if (!(r->got & GOT_AUTH)) {
			if (r->state == M17_SIG_PENDING) {
				hash_frame(&r->h, fn, stream);
				sha256_final(&r->h, r->hash);
			}
			memcpy(r->auth, stream, M17_AUTH_BYTES);
			r->got |= GOT_AUTH;
		}
	} else {
		memcpy(&r->rs[(unsigned)(p - 1) * M17_STREAM_BYTES], stream, M17_STREAM_BYTES);
		r->got |= (uint8_t)(1u << (p - 1));
	}
	return M17_SIG_FRAME_TAIL;
}

bool m17_sig_rx_ready(const m17_sig_rx_t *r)
{
	return r->state == M17_SIG_PENDING && r->got == GOT_ALL;
}

#endif
