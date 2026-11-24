// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 frame codecs (m17.md 1.3, 2): each frame type between its contents (type 1 bits) and the 48 bytes on air (the
// sync word, then 46 bytes of type 4 bits), both ways. The shared FEC does the codes: the K = 5 convolutional code and
// its Viterbi decoder (core/fec/conv.h, G1 = 0x19, G2 = 0x17 as NXDN's), the (24,12) Golay code (core/fec/golay.h: the
// spec's P matrix), the CRC (core/fec/crc.h); here are M17's puncturing, interleaver and randomizer around them.
//
// Receive works on a frame as the framer hands it over (air_framer.h): hard dibits from symbol 0 (the sync) and, where
// the provider has them, one soft value per symbol (+3 = AIR_SOFT_OUTER). Bits are soft through de-randomizing,
// de-interleaving and de-puncturing (punctured positions are erasures, soft 0) into the Viterbi decoder; without soft
// values the hard dibits stand in. No buffer of 368 bits is kept: each coded bit is read from its interleaved symbol.
// Pure functions on the caller's memory; reentrant. Built with CONFIG_M17.
#ifndef RFOUNDRY_M17_FRAME_H
#define RFOUNDRY_M17_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "m17.h"

// Type 1 sizes (m17.md 2.3)
#define M17_LSF_T1_BITS		240u
#define M17_STREAM_T1_BITS	144u	// FN 16 + STREAM 128
#define M17_PACKET_T1_BITS	206u	// chunk 200, EOF, counter 5
#define M17_BERT_T1_BITS	197u
#define M17_FLUSH_BITS		4u
#define M17_LICH_T2_BITS	96u	// four Golay words
#define M17_PACKET_CHUNK	25u
#define M17_PACKET_FRAME_BYTES	26u	// the chunk and byte 25 (EOF bit 7, counter or byte count in bits 6-2)
#define M17_BERT_BYTES		25u	// 197 bits, MSB first, the rest 0

// Viterbi memory for the longest unit (the LSF: 244 input bits): de-punctured soft values and the decisions. The
// engine keeps one (provider's task); a frame's decode uses it only inside the call
typedef struct {
	int8_t soft[2 * (M17_LSF_T1_BITS + M17_FLUSH_BITS)];
	uint16_t dec[M17_LSF_T1_BITS + M17_FLUSH_BITS];
} m17_fec_scratch_t;

// --- Building blocks (exposed for the host tests and the bench tool) ---
// The randomizer sequence (m17.md 1.3): payload byte i ^= m17_rand_seq[i]
extern const uint8_t m17_rand_seq[M17_PAYLOAD_BYTES];
// The interleaver π(x) = (45x + 92x^2) mod 368 (m17.md 2.7); its own inverse
static inline uint16_t m17_interleave(uint16_t x)
{
	return (uint16_t)((45u * x + 92u * (((uint32_t)x * x) % 368u)) % 368u);
}
// CRC-16 of m17.md 2.8 (poly 0x5935, init 0xFFFF) over n bytes; over data with its CRC appended it is 0
uint16_t m17_crc(const uint8_t *p, size_t n);
// Puncture patterns (m17.md 2.5): whether coded bit i (G1, G2, G1, ... from 0) of a unit is sent
bool m17_p1_kept(uint16_t i);
bool m17_p2_kept(uint16_t i);
bool m17_p3_kept(uint16_t i);

// --- Transmit: contents to the 48 bytes on air ---
void m17_frame_lsf(const uint8_t lsf[M17_LSF_BYTES], uint8_t out[M17_FRAME_BYTES]);
// The LICH of chunk cnt (0..5) of lsf
void m17_lich_make(const uint8_t lsf[M17_LSF_BYTES], uint8_t cnt, uint8_t lich[M17_LICH_BYTES]);
void m17_frame_stream(const uint8_t lich[M17_LICH_BYTES], uint16_t fn, const uint8_t stream[M17_STREAM_BYTES],
		      uint8_t out[M17_FRAME_BYTES]);
void m17_frame_packet(const uint8_t chunk[M17_PACKET_FRAME_BYTES], uint8_t out[M17_FRAME_BYTES]);
void m17_frame_bert(const uint8_t bits[M17_BERT_BYTES], uint8_t out[M17_FRAME_BYTES]);
// The 192 symbols before an LSF or a BERT frame, and the end of transmission: 48 bytes each
void m17_preamble(bool bert, uint8_t out[M17_FRAME_BYTES]);
void m17_eot(uint8_t out[M17_FRAME_BYTES]);

// --- Receive: a frame's symbols (hard: 192 dibits from the sync on; soft: 192 values or NULL) to its contents ---
// Each returns the coded bits the decoder corrected (the Viterbi count, or the Golay corrections), or -1 when the
// frame is not trusted (LSF: CRC; LICH: a Golay word beyond repair)
int m17_frame_lsf_get(const uint8_t *hard, const int8_t *soft, m17_fec_scratch_t *s, uint8_t lsf[M17_LSF_BYTES]);
int m17_frame_lich_get(const uint8_t *hard, const int8_t *soft, uint8_t lich[M17_LICH_BYTES]);
int m17_frame_stream_get(const uint8_t *hard, const int8_t *soft, m17_fec_scratch_t *s, uint16_t *fn,
			 uint8_t stream[M17_STREAM_BYTES]);
int m17_frame_packet_get(const uint8_t *hard, const int8_t *soft, m17_fec_scratch_t *s,
			 uint8_t chunk[M17_PACKET_FRAME_BYTES]);
int m17_frame_bert_get(const uint8_t *hard, const int8_t *soft, m17_fec_scratch_t *s, uint8_t bits[M17_BERT_BYTES]);
// The LICH's fields
static inline uint8_t m17_lich_cnt(const uint8_t lich[M17_LICH_BYTES])
{
	return (uint8_t)(lich[5] >> 5);
}
// How far 24 symbols at hard (from a frame's start) are from the end-of-transmission pattern: errors in symbols
uint8_t m17_eot_errs(const uint8_t *hard);

#endif
