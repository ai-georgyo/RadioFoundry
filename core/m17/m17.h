// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 (docs/spec/m17.md; the M17 specification v2.1.1): the protocol's numbers, the link setup frame and its TYPE
// word, addresses. Section numbers in comments are m17.md's ("m17.md 3.1") unless they say "spec".
//
// Layout of core/m17 (each file's header says more):
//   m17.h          this: geometry, sync words, LSF, TYPE, addresses (m17_addr.c)
//   m17_frame.h    the frame codecs: randomizer, interleaver, convolutional code and puncturing, LICH Golay, CRC
//   m17_meta.h     META: text, GNSS, extended callsign
//   m17_packet.h   packet mode: data type, chunks, CRC; SMS
//   m17_bert.h     BERT: PRBS9 generator and receiver
//   m17_crypt.h    scrambler and the AES-CTR frame layout (CONFIG_M17_SCRAMBLER, CONFIG_M17_AES)
//   m17_sig.h      stream signatures: the stream hash, the authentication frame (CONFIG_M17_SIG)
//   m17_engine.h   the call layer on an air session: receive, transmit, the air_mode_t
//   m17_voice.h    voice between the engine and the PCM audio, through Codec 2 (core/codec2)
//   m17_codeplug.h kind 6 / system type 6 in the codeplug (docs/spec/m17-codeplug.md)
// No RTOS or board call anywhere in core/m17: the radio core (core/radio/radio_m17.c) runs it.
#ifndef RFOUNDRY_M17_H
#define RFOUNDRY_M17_H

#include <stdbool.h>
#include <stdint.h>

// --- Physical layer (m17.md 1) ---
#define M17_SYM_RATE		4800u
#define M17_FRAME_SYMS		192u	// sync burst + payload: 40 ms, every frame type
#define M17_FRAME_BYTES		48u	// as packed dibits (hal_air.h): the 2 sync bytes, then the 46 payload bytes
#define M17_SYNC_SYMS		8u
#define M17_PAYLOAD_BITS	368u	// type 4 bits behind a sync
#define M17_PAYLOAD_BYTES	46u
#define M17_PREAMBLE_SYMS	192u
#define M17_EOT_SYMS		192u
#define M17_FRAME_MS		40u

// Sync words as on air (16 bits, MSB first: the air_sync_t bytes as written, m17.md 1.2)
#define M17_SYNC_LSF		0x55F7u
#define M17_SYNC_BERT		0xDF55u
#define M17_SYNC_STREAM		0xFF5Du
#define M17_SYNC_PACKET		0x75FFu
#define M17_EOT_WORD		0x555Du	// 24 times: the end of a transmission
#define M17_PREAMBLE_LSF	0x77u	// +3 -3 ...: the preamble byte before an LSF
#define M17_PREAMBLE_BERT	0xDDu	// -3 +3 ...: before BERT

// --- The link setup frame (m17.md 3) ---
#define M17_ADDR_BYTES		6u
#define M17_META_BYTES		14u
#define M17_LSD_BYTES		28u	// DST, SRC, TYPE, META
#define M17_LSF_BYTES		30u	// ... and the CRC over them
#define M17_LSF_DST		0u	// byte offsets
#define M17_LSF_SRC		6u
#define M17_LSF_TYPE		12u
#define M17_LSF_META		14u
#define M17_LSF_CRC		28u

// TYPE (m17.md 3.1), a 16-bit big-endian word, bit 0 the least significant
#define M17_TYPE_STREAM		0x0001u			// else packet
#define M17_TYPE_DATA(t)	(((t) >> 1) & 3u)	// stream: M17_DATA_*
#define M17_TYPE_ENC(t)		(((t) >> 3) & 3u)	// M17_ENC_*
#define M17_TYPE_SUB(t)		(((t) >> 5) & 3u)	// encryption subtype, or the META type when ENC is none
#define M17_TYPE_CAN(t)		(((t) >> 7) & 0xFu)
#define M17_TYPE_SIGNED		0x0800u			// stream: ends with the authentication and signature frames
enum { M17_DATA_RESERVED, M17_DATA_DATA, M17_DATA_VOICE, M17_DATA_VOICE_DATA };	// voice: Codec 2 3200; voice + data: 1600
enum { M17_ENC_NONE, M17_ENC_SCRAMBLER, M17_ENC_AES, M17_ENC_OTHER };
enum { M17_META_TEXT, M17_META_GNSS, M17_META_EXTCALL, M17_META_RESERVED };	// subtype with M17_ENC_NONE
enum { M17_SCR_8, M17_SCR_16, M17_SCR_24 };					// subtype with M17_ENC_SCRAMBLER
enum { M17_AES_128, M17_AES_192, M17_AES_256 };					// subtype with M17_ENC_AES
#define M17_CAN_MAX		15u

static inline uint16_t m17_type(bool stream, uint8_t data, uint8_t enc, uint8_t sub, uint8_t can, bool sig)
{
	return (uint16_t)((stream ? M17_TYPE_STREAM : 0u) | ((data & 3u) << 1) | ((enc & 3u) << 3) | ((sub & 3u) << 5) |
			  ((can & 0xFu) << 7) | (sig ? M17_TYPE_SIGNED : 0u));
}

// --- Addresses (m17.md 10) ---
#define M17_ADDR_RESERVED	0x000000000000ull
#define M17_ADDR_STD_MAX	0xEE6B27FFFFFFull	// the last standard (base-40) address, "........."
#define M17_ADDR_BROADCAST	0xFFFFFFFFFFFFull
#define M17_CALL_MAX		9u			// characters of a standard address
#define M17_ADDR_TEXT		13u			// text buffer: 9 characters, or 12 hex digits, and the NUL

// What an address is (m17_addr_text)
enum { M17_ADDR_KIND_RESERVED, M17_ADDR_KIND_STD, M17_ADDR_KIND_EXTENDED, M17_ADDR_KIND_BROADCAST };

static inline uint64_t m17_addr_get(const uint8_t p[M17_ADDR_BYTES])
{
	uint64_t a = 0;

	for (uint8_t i = 0; i < M17_ADDR_BYTES; i++)
		a = (a << 8) | p[i];
	return a;
}

static inline void m17_addr_put(uint8_t p[M17_ADDR_BYTES], uint64_t a)
{
	for (uint8_t i = M17_ADDR_BYTES; i-- > 0;) {
		p[i] = (uint8_t)a;
		a >>= 8;
	}
}

// m17_addr.c. Text to an address: letters are upper-cased first; trailing spaces don't count (m17.md 10). False for
// empty text (all spaces: the reserved address), more than 9 characters, or a character outside A-Z 0-9 - / . and
// space; *addr is then 0
bool m17_addr_from_text(const char *text, uint64_t *addr);
// An address as text: a standard one as its characters (no trailing spaces), an extended or reserved one as 12 hex
// digits (the spec gives them no encoding), broadcast as the empty string (the UI shows its own word for it). Returns
// M17_ADDR_KIND_*
uint8_t m17_addr_text(uint64_t addr, char out[M17_ADDR_TEXT]);
// The character of a base-40 value and back (0 = space; m17.md 10's table), for an editor that filters what is typed
static inline char m17_addr_char(uint8_t v)
{
	return v == 0 ? ' ' : v <= 26 ? (char)('A' + v - 1) : v <= 36 ? (char)('0' + v - 27) :
	       v == 37 ? '-' : v == 38 ? '/' : v == 39 ? '.' : ' ';
}
uint8_t m17_addr_value(char c);		// 0 for space and for a character outside the alphabet (case-insensitive)

// --- LSF fields ---
static inline uint16_t m17_lsf_type(const uint8_t lsf[M17_LSF_BYTES])
{
	return (uint16_t)((lsf[M17_LSF_TYPE] << 8) | lsf[M17_LSF_TYPE + 1]);
}

// Build an LSF: the fields, then the CRC (m17_frame.h m17_crc) into bytes 28-29
void m17_lsf_make(uint8_t lsf[M17_LSF_BYTES], uint64_t dst, uint64_t src, uint16_t type,
		  const uint8_t meta[M17_META_BYTES]);
// The CRC over the first 28 bytes holds
bool m17_lsf_crc_ok(const uint8_t lsf[M17_LSF_BYTES]);

// --- Stream frame numbers (m17.md 5.2, 7.1) ---
#define M17_FN_END		0x8000u	// the last frame of a transmission
#define M17_FN_MASK		0x7FFFu
#define M17_FN_DATA_MAX_SIGNED	0x7FFAu	// a signed stream's last data frame number at most
#define M17_FN_AUTH		0x7FFBu
#define M17_FN_SIG1		0x7FFCu
#define M17_FN_SIG2		0x7FFDu
#define M17_FN_SIG3		0x7FFEu
#define M17_FN_SIG4		0xFFFFu	// end bit set

#define M17_STREAM_BYTES	16u	// a stream frame's STREAM field
#define M17_LICH_BYTES		6u	// 40 bits of LSF, LICH_CNT, reserved (m17.md 5.1)
#define M17_LICH_CHUNKS		6u	// LICH_CNT 0..5: a superframe

#endif
