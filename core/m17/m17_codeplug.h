// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 channels in the codeplug (docs/spec/m17-codeplug.md): kind 6 and system type 6 (docs/spec/channel-modes.md 4).
// The codec from the stored bytes to plain structs the radio core sets the M17 engine up from: a channel's parameter
// block, the radio-wide record (key 0x0A06: our callsign, the META text), an M17 system's traffic keys, signing key
// (its system secret) and the station keys of its entries. Bytes in, values out: no engine here. Every block reads
// safely when zeroed or short. The Codeplug Editor's module (site/static/js/codeplug/mode_m17.js) reads and writes the
// same bytes, and both are tested against tests/vectors/channel-modes.txt. Built with CONFIG_M17.
#ifndef RFOUNDRY_M17_CODEPLUG_H
#define RFOUNDRY_M17_CODEPLUG_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "codeplug/codeplug.h"
#include "m17.h"
#include "m17_meta.h"

// --- A channel's parameter block (m17-codeplug.md 2) ---
#define M17_CP_CONV_LEN		12u
// byte 0 (bits 3-7 reserved: written 0, kept as found; signing, the position and the call text are the station's
// choices, in the radio-wide record)
#define M17_CP_SQUELCH(b)	((b) & 3u)	// M17_SQUELCH_* (m17_engine.h); 3 reads as 0
#define M17_CP_1600		0x04u		// voice + data: Codec 2 1600, else 3200
// byte 1: CAN in bits 0-3; byte 2: TX CAN in bits 0-3, valid with bit 7 (clear: the CAN)
#define M17_CP_TX_CAN_SET	0x80u
// bytes 3-8: destination, 48 bits big endian; all zero is broadcast
// byte 9: transmit encryption, M17_ENC_* (0 none; a value the build lacks reads as none); bytes 10-11: traffic key ID

typedef struct {
	uint8_t squelch;	// M17_SQUELCH_*
	bool mode1600;
	uint8_t can;		// 0..15
	uint8_t tx_can;		// 0..15 (the CAN when not set)
	uint64_t dst;		// M17_ADDR_BROADCAST for a stored zero (the only place broadcast is spelled as zeros);
				// encoding stores M17_ADDR_BROADCAST and 0 alike as zeros
	uint8_t tx_enc;		// M17_ENC_*
	uint16_t key_id;
} m17_conv_t;

// Decode len bytes (a short block, or none, takes the defaults); never fails: a value out of range reads as its default
void m17_conv_decode(const uint8_t *b, int len, m17_conv_t *out);
// The block for c: a TX CAN equal to the CAN is stored as "not set", broadcast as zeros
void m17_conv_encode(const m17_conv_t *c, uint8_t out[M17_CP_CONV_LEN]);

// --- The radio-wide record, key 0x0A00 + 6 (m17-codeplug.md 3) ---
#define M17_CP_WIDE_LEN		60u
// bytes 0-5 our address (0: none, the codeplug's radio name stands in where it encodes), byte 6 flags (below; bits 2-7
// reserved), byte 7 our GNSS station type (M17_STATION_*), bytes 8-59 the call text sent as META (UTF-8, zero padded;
// sent whenever it is not empty)
#define M17_CP_WIDE_SIGN	0x01u	// Sign M17 calls (needs a usable signing key: radio_m17_key)
#define M17_CP_WIDE_GNSS	0x02u	// Send position in M17 calls (with a fix, at most every 5 s)
typedef struct {
	uint64_t src;		// 0: none
	bool sign, gnss;	// M17_CP_WIDE_*
	uint8_t station;	// M17_STATION_*
	uint8_t text_len;
	uint8_t text[M17_TEXT_MAX];
} m17_wide_t;

void m17_wide_decode(const uint8_t *b, int len, m17_wide_t *out);
void m17_wide_encode(const m17_wide_t *w, uint8_t out[M17_CP_WIDE_LEN]);
// The record as stored (codeplug_mode_settings), defaults when absent; and written back (codeplug_kv_write: a storage
// job's work, the Settings row that types our callsign). False when the write failed
void m17_wide_load(m17_wide_t *out);
bool m17_wide_save(const m17_wide_t *w);
// Our address: the record's, else the radio name (codeplug.h) where it is a valid callsign, else 0 (PTT refused).
// bad (may be NULL): the record holds bytes that are not a standard address (a CPS wrote text, or an extended or
// broadcast value): the address is then 0 and the UI says "M17 callsign not valid"
uint64_t m17_own_addr(bool *bad);

// --- What a channel's call is set up from ---
typedef struct {
	m17_conv_t conv;
	uint64_t src;		// ours (m17_own_addr)
	uint8_t system;		// CODEPLUG_SYSTEM_NONE or an M17 system
	bool tx_allowed;	// the system allows transmit (no system: true)
} m17_conv_cfg_t;
// Channel number's set-up from its tag, the radio-wide record and its record ch; false when it isn't a live M17 channel
bool m17_conv_load(uint16_t number, const channel_t *ch, m17_conv_cfg_t *out);
// Write ch at number as an M17 channel with conv's block (channel-modes.md 2.4), as nxdn_conv_save does: the base
// record in its safe stand-in shape, the tag kept where it is M17 already
bool m17_conv_save(uint16_t number, const channel_t *ch, const m17_conv_t *conv);

// --- An M17 system (m17-codeplug.md 4): keys ---
// Traffic key algorithm ids (channel-modes.md 5.4, this type's numbering)
enum { M17_CP_ALG_SCRAMBLER = 1, M17_CP_ALG_AES = 2 };
#define M17_CP_ENTRY_STATION	128u	// entry class: a station's public key (6-byte address, 33-byte compressed key)
#define M17_CP_STATION_LEN	39u

// The traffic key for the channel's encryption (enc M17_ENC_SCRAMBLER or AES, key_id): its length (1-3 for a
// scrambler seed, 16 / 24 / 32 for AES), 0 when missing or of the wrong length. A secret: the caller wipes key
uint8_t m17_system_key(uint8_t system, uint8_t enc, uint16_t key_id, uint8_t key[32]);
// The system secret (key 0x0300 + n, m17-codeplug.md 4): bytes 0-31 the signing key d (big endian), bytes 32-64 its
// public key Q compressed (02 / 03 || X), which the Codeplug Editor writes with d. A short record (32 bytes, d only)
// is valid: the radio derives Q at its key check
#define M17_CP_SECRET_LEN	65u
// The system's signing key d (a secret: the caller wipes d, inside crash_secret_enter / _leave) and, when stored, Q
// compressed (q_c may be NULL; *has_q false when the record has d only). False when missing or d is not a valid key
bool m17_system_signing_key(uint8_t system, uint8_t d[32], uint8_t q_c[33], bool *has_q);
// The M17 system whose signing key a channel's system means: system itself when it is an M17 system with a secret,
// else the first M17 system that has one (the VFO, a channel on no system). CODEPLUG_SYSTEM_NONE: none
uint8_t m17_signing_system(uint8_t system);
// Public keys of station addr, in turn: the i-th one found in system's entries (then every other M17 system's), as
// 33 bytes compressed; false past the last
bool m17_station_key(uint8_t system, uint64_t addr, uint8_t i, uint8_t key[33]);
// Every station key in turn, any system (the Station Keys list): the i-th class-128 entry of an M17 system, in key
// order. False past the last
bool m17_station_key_at(uint16_t i, uint64_t *addr, uint8_t key[33]);
// A station key is on file for addr (any M17 system): the "Not signed" marker's question, a linear search
bool m17_station_known(uint64_t addr);

#endif
