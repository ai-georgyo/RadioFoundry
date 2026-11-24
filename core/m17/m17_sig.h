// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 stream signatures (m17.md 7): the stream hash over every data frame and the authentication frame, the
// authentication frame's fields, the four signature frames' halves, and what a receiver concludes. The curve work is
// core/crypto/p256.h's and runs in the radio core's task (radio_m17.c), never in the provider's: here is only
// bookkeeping that runs per frame. Pure functions. Built with CONFIG_M17_SIG.
#ifndef RFOUNDRY_M17_SIG_H
#define RFOUNDRY_M17_SIG_H

#include <stdbool.h>
#include <stdint.h>
#include "m17.h"
#include "../support/sha256.h"

#define M17_SIG_TAG		"M17-SIG-v2"	// hashed first, without a terminator
#define M17_SIG_TAG_LEN		10u
#define M17_AUTH_BYTES		16u		// signer, destination, timestamp
#define M17_SIG_RS_BYTES	64u
#define M17_HASH_BYTES		32u

// What a receiver concludes about a stream (debug link m17.sig, the call line's marker)
enum {
	M17_SIG_NONE,		// not a signed stream
	M17_SIG_PENDING,	// signed, still coming or being checked
	M17_SIG_VALID,		// the signature verifies with a key of the signer's
	M17_SIG_INVALID,	// it does not (a corrupt frame looks the same as tampering: never "forged")
	M17_SIG_NO_KEY,		// no public key for the signer
	M17_SIG_INCOMPLETE,	// a data frame missed (an FN gap), or the tail cut short
	M17_SIG_LATE,		// joined after frame 0: the hash can't be rebuilt
};

// A public key's fingerprint, the same on the radio, the debug link (m17.key_fp) and the Codeplug Editor: the first 8
// bytes of SHA-256 over the 33-byte compressed key (02 / 03 || X), shown as 16 hex digits in groups of 4. Compressed
// so a stored key needs no curve work to show it
#define M17_KEY_FP_BYTES	8u
void m17_key_fp(const uint8_t q_c[33], uint8_t fp[M17_KEY_FP_BYTES]);

// The authentication frame's STREAM bytes, and back
void m17_auth_make(uint64_t signer, uint64_t dst, uint32_t ts, uint8_t out[M17_AUTH_BYTES]);
void m17_auth_read(const uint8_t a[M17_AUTH_BYTES], uint64_t *signer, uint64_t *dst, uint32_t *ts);

// --- Transmit: the hash as frames are built; the tail's payloads ---
typedef struct {
	sha256_t h;
	uint16_t frames;	// data frames hashed
} m17_sig_tx_t;

void m17_sig_tx_start(m17_sig_tx_t *t);
// A data frame as it goes out (fn as sent, stream as sent: ciphertext when encrypted)
void m17_sig_tx_frame(m17_sig_tx_t *t, uint16_t fn, const uint8_t stream[M17_STREAM_BYTES]);
// The authentication frame closes the hash: H for the signer
void m17_sig_tx_auth(m17_sig_tx_t *t, const uint8_t auth[M17_AUTH_BYTES], uint8_t hash[M17_HASH_BYTES]);
// Signature frame i (0..3: FN 7FFC, 7FFD, 7FFE, FFFF) of r || s
static inline uint16_t m17_sig_fn(uint8_t i)
{
	return i < 3 ? (uint16_t)(M17_FN_SIG1 + i) : (uint16_t)M17_FN_SIG4;
}

// --- Receive ---
// m17_sig_rx_frame's answer for a frame
enum {
	M17_SIG_FRAME_DATA,	// an ordinary frame: decrypt and play it
	M17_SIG_FRAME_TAIL,	// the authentication or a signature frame: never decrypt or play
};

typedef struct {
	sha256_t h;
	uint16_t next_fn;	// the data FN due
	uint8_t state;		// M17_SIG_* so far
	uint8_t got;		// bits 0-3: signature frames in; bit 4: the authentication frame
	uint8_t auth[M17_AUTH_BYTES];
	uint8_t rs[M17_SIG_RS_BYTES];
	uint8_t hash[M17_HASH_BYTES];	// once the authentication frame is in
} m17_sig_rx_t;

// A signed stream starts (its LSF, or the LICH's, says signed). first_fn: the first frame's FN (not 0: a late join)
void m17_sig_rx_start(m17_sig_rx_t *r, uint16_t first_fn);
// Each stream frame as received, before decrypting; the jump from the last data FN to 7FFB is not a gap
uint8_t m17_sig_rx_frame(m17_sig_rx_t *r, uint16_t fn, const uint8_t stream[M17_STREAM_BYTES]);
// Everything a verifier needs is in: the hash, r || s, the authentication fields. False with the state set otherwise
// (INCOMPLETE, LATE) when the stream ended
bool m17_sig_rx_ready(const m17_sig_rx_t *r);

#endif
