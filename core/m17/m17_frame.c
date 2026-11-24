// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 frame codecs (m17_frame.h; docs/spec/m17.md 1.2-1.3, 2, 3, 5.1). A coded unit (LSF, stream part, packet, BERT)
// goes through the shared convolutional code and then, bit by bit, through its puncture pattern straight to its
// interleaved place in the payload: no buffer of type 3 bits either way. Receive reads each kept coded bit from its
// symbol (soft, or the hard dibit) as it de-punctures into the caller's scratch.
#include "config.h"

#if CONFIG_M17

#include <string.h>

#include "m17/m17_frame.h"
#include "air/air_syms.h"
#include "fec/conv.h"
#include "fec/crc.h"
#include "fec/golay.h"
#include "../../hal/hal_air.h"

#define M17_CRC_POLY	0x5935u
#define M17_CRC_INIT	0xFFFFu
// A bit's size without soft values: every hard bit as sure as any other (the decoder only compares sums)
#define HARD_SOFT	(AIR_SOFT_OUTER / 3)
// The second bit of a dibit says outer (1) or inner (0): its soft value is the symbol's size against the midpoint
// between +1 and +3
#define LOW_MID		(2 * AIR_SOFT_OUTER / 3)

// The code (m17.md 2.4): G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4, G1's bit first
static const fec_conv_code_t code = { 0x19, 0x17 };

const uint8_t m17_rand_seq[M17_PAYLOAD_BYTES] = {
	0xD6, 0xB5, 0xE2, 0x30, 0x82, 0xFF, 0x84, 0x62, 0xBA, 0x4E, 0x96, 0x90, 0xD8, 0x98, 0xDD, 0x5D,
	0x0C, 0xC8, 0x52, 0x43, 0x91, 0x1D, 0xF8, 0x6E, 0x68, 0x2F, 0x35, 0xDA, 0x14, 0xEA, 0xCD, 0x76,
	0x19, 0x8D, 0xD5, 0x80, 0xD1, 0x33, 0x87, 0x13, 0x57, 0x18, 0x2D, 0x29, 0x78, 0xC3,
};

uint16_t m17_crc(const uint8_t *p, size_t n)
{
	return fec_crc16(M17_CRC_INIT, M17_CRC_POLY, p, n);
}

// P1 (61): a 1, then 1011 fifteen times: positions 2, 6, ... 58 dropped
bool m17_p1_kept(uint16_t i)
{
	uint16_t k = (uint16_t)(i % 61u);

	return k == 0 || (k & 3u) != 2u;
}

// P2 (12): eleven 1s, then a 0
bool m17_p2_kept(uint16_t i)
{
	return i % 12u != 11u;
}

// P3 (8): seven 1s, then a 0
bool m17_p3_kept(uint16_t i)
{
	return (i & 7u) != 7u;
}

// --- The interleaver, walked: π(x + 1) - π(x) = 137 + 184 x (mod 368), so consecutive places need no multiply ---
typedef struct {
	uint16_t p;	// π(x)
	uint16_t d;	// π(x + 1) - π(x)
} ilv_t;

static void ilv_at(ilv_t *it, uint16_t x)
{
	it->p = m17_interleave(x);
	it->d = (uint16_t)((137u + 184u * x) % M17_PAYLOAD_BITS);
}

static void ilv_next(ilv_t *it)
{
	it->p = (uint16_t)(it->p + it->d);
	if (it->p >= M17_PAYLOAD_BITS)
		it->p = (uint16_t)(it->p - M17_PAYLOAD_BITS);
	it->d = (uint16_t)(it->d + 184u);
	if (it->d >= M17_PAYLOAD_BITS)
		it->d = (uint16_t)(it->d - M17_PAYLOAD_BITS);
}

// --- One convolutionally coded unit ---
typedef struct {
	uint16_t t1;			// type 1 bits, without the flush
	uint16_t off;			// its first type 3 place (the stream part follows the LICH's 96)
	uint16_t t3;			// coded bits sent: BERT's P2 keeps 369, the last is dropped
	bool (*kept)(uint16_t i);
} unit_t;

static const unit_t u_lsf = { M17_LSF_T1_BITS, 0, M17_PAYLOAD_BITS, m17_p1_kept };
static const unit_t u_stream = { M17_STREAM_T1_BITS, M17_LICH_T2_BITS, M17_PAYLOAD_BITS - M17_LICH_T2_BITS,
				 m17_p2_kept };
static const unit_t u_packet = { M17_PACKET_T1_BITS, 0, M17_PAYLOAD_BITS, m17_p3_kept };
static const unit_t u_bert = { M17_BERT_T1_BITS, 0, M17_PAYLOAD_BITS, m17_p2_kept };

#define UNIT_IN_BYTES	((M17_LSF_T1_BITS + M17_FLUSH_BITS + 7u) / 8u)	// the longest unit, flush included: 31

static uint8_t bit_get(const uint8_t *p, uint16_t i)
{
	return (uint8_t)((p[i >> 3] >> (7u - (i & 7u))) & 1u);
}

// t1 to type 4 bits at their places in pay (46 bytes, zero there), before the randomizer
static void unit_put(const unit_t *u, const uint8_t *t1, uint8_t *pay)
{
	uint8_t in[UNIT_IN_BYTES], enc[2 * UNIT_IN_BYTES];
	uint16_t n = (uint16_t)(u->t1 + M17_FLUSH_BITS), y = 0;
	ilv_t it;

	// The contents, then zeros: the bits past t1 in its last byte and the four flush bits
	memset(in, 0, sizeof in);
	memcpy(in, t1, (size_t)(u->t1 + 7u) / 8u);
	if (u->t1 & 7u)
		in[u->t1 >> 3] &= (uint8_t)(0xFFu << (8u - (u->t1 & 7u)));
	fec_conv_encode(&code, in, n, enc);
	ilv_at(&it, u->off);
	for (uint16_t j = 0; j < 2u * n && y < u->t3; j++) {
		if (!u->kept(j))
			continue;
		if (bit_get(enc, j))
			pay[it.p >> 3] |= (uint8_t)(0x80u >> (it.p & 7u));
		ilv_next(&it);
		y++;
	}
}

// The sync word, then the payload randomized (m17.md 1.3)
static void frame_finish(uint16_t sync, uint8_t out[M17_FRAME_BYTES])
{
	out[0] = (uint8_t)(sync >> 8);
	out[1] = (uint8_t)sync;
	for (uint8_t i = 0; i < M17_PAYLOAD_BYTES; i++)
		out[2 + i] ^= m17_rand_seq[i];
}

void m17_frame_lsf(const uint8_t lsf[M17_LSF_BYTES], uint8_t out[M17_FRAME_BYTES])
{
	memset(out, 0, M17_FRAME_BYTES);
	unit_put(&u_lsf, lsf, out + 2);
	frame_finish(M17_SYNC_LSF, out);
}

// LICH_CNT outside 0..5 is never sent: taken modulo 6
void m17_lich_make(const uint8_t lsf[M17_LSF_BYTES], uint8_t cnt, uint8_t lich[M17_LICH_BYTES])
{
	cnt = (uint8_t)(cnt % M17_LICH_CHUNKS);
	memcpy(lich, lsf + 5u * cnt, 5);
	lich[5] = (uint8_t)(cnt << 5);
}

// The LICH's four Golay words at type 3 places 0..95: LICH bits 0-11 the first word's data (bit 11 first), each word
// sent from bit 23 (m17.md 5.1, 13)
static void lich_put(const uint8_t lich[M17_LICH_BYTES], uint8_t *pay)
{
	ilv_t it;

	ilv_at(&it, 0);
	for (uint8_t w = 0; w < 4; w++) {
		uint16_t d = 0;
		uint32_t cw;

		for (uint8_t b = 0; b < 12; b++)
			d = (uint16_t)(d << 1 | bit_get(lich, (uint16_t)(12u * w + b)));
		cw = fec_golay24_encode(d);
		for (uint8_t b = 24; b-- > 0;) {
			if ((cw >> b) & 1u)
				pay[it.p >> 3] |= (uint8_t)(0x80u >> (it.p & 7u));
			ilv_next(&it);
		}
	}
}

void m17_frame_stream(const uint8_t lich[M17_LICH_BYTES], uint16_t fn, const uint8_t stream[M17_STREAM_BYTES],
		      uint8_t out[M17_FRAME_BYTES])
{
	uint8_t t1[M17_STREAM_T1_BITS / 8];

	t1[0] = (uint8_t)(fn >> 8);
	t1[1] = (uint8_t)fn;
	memcpy(t1 + 2, stream, M17_STREAM_BYTES);
	memset(out, 0, M17_FRAME_BYTES);
	lich_put(lich, out + 2);
	unit_put(&u_stream, t1, out + 2);
	frame_finish(M17_SYNC_STREAM, out);
}

// Byte 25's bits 1-0 are not sent (unit_put stops at bit 206)
void m17_frame_packet(const uint8_t chunk[M17_PACKET_FRAME_BYTES], uint8_t out[M17_FRAME_BYTES])
{
	memset(out, 0, M17_FRAME_BYTES);
	unit_put(&u_packet, chunk, out + 2);
	frame_finish(M17_SYNC_PACKET, out);
}

void m17_frame_bert(const uint8_t bits[M17_BERT_BYTES], uint8_t out[M17_FRAME_BYTES])
{
	memset(out, 0, M17_FRAME_BYTES);
	unit_put(&u_bert, bits, out + 2);
	frame_finish(M17_SYNC_BERT, out);
}

// The outer symbols alternating, ending opposite the sync's first symbol: +3 -3 (77) before the LSF's +3, -3 +3 (DD)
// before BERT's -3
void m17_preamble(bool bert, uint8_t out[M17_FRAME_BYTES])
{
	memset(out, bert ? M17_PREAMBLE_BERT : M17_PREAMBLE_LSF, M17_FRAME_BYTES);
}

void m17_eot(uint8_t out[M17_FRAME_BYTES])
{
	for (uint8_t i = 0; i < M17_FRAME_BYTES; i += 2) {
		out[i] = (uint8_t)(M17_EOT_WORD >> 8);
		out[i + 1] = (uint8_t)M17_EOT_WORD;
	}
}

// --- Receive ---
// Payload bit p (0..367) as a soft value (conv.h: positive says 1), de-randomized. The first bit of a dibit is its
// sign (1 below zero), the second whether it is an outer symbol
static int8_t pbit_soft(const uint8_t *hard, const int8_t *soft, uint16_t p)
{
	uint16_t s = (uint16_t)(M17_SYNC_SYMS + (p >> 1));
	uint8_t r = bit_get(m17_rand_seq, p);
	int v;

	if (!soft) {
		uint8_t d = air_sym_get(hard, s);
		uint8_t b = (uint8_t)((p & 1u) ? d & 1u : d >> 1);

		return (int8_t)((b ^ r) ? HARD_SOFT : -HARD_SOFT);
	}
	v = soft[s];
	if (p & 1u)
		v = (v < 0 ? -v : v) - LOW_MID;
	else
		v = -v;
	if (r)
		v = -v;
	return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
}

// A unit's kept bits from their symbols, the dropped ones as erasures, through the Viterbi decoder into out
// ((t1 + 4 + 7) / 8 bytes). Returns the decoder's count of corrected coded bits
static int unit_get(const unit_t *u, const uint8_t *hard, const int8_t *soft, m17_fec_scratch_t *s, uint8_t *out)
{
	uint16_t n = (uint16_t)(u->t1 + M17_FLUSH_BITS), y = 0;
	ilv_t it;

	ilv_at(&it, u->off);
	for (uint16_t j = 0; j < 2u * n; j++) {
		if (y < u->t3 && u->kept(j)) {
			s->soft[j] = pbit_soft(hard, soft, it.p);
			ilv_next(&it);
			y++;
		} else {
			s->soft[j] = 0;
		}
	}
	return fec_conv_decode(&code, s->soft, n, s->dec, out);
}

// The LSF is written whatever the CRC says; -1 tells the caller not to use it
int m17_frame_lsf_get(const uint8_t *hard, const int8_t *soft, m17_fec_scratch_t *s, uint8_t lsf[M17_LSF_BYTES])
{
	uint8_t dec[UNIT_IN_BYTES];
	int errs = unit_get(&u_lsf, hard, soft, s, dec);

	memcpy(lsf, dec, M17_LSF_BYTES);
	return m17_lsf_crc_ok(lsf) ? errs : -1;
}

// The provider's hard dibits into the Golay decoder, a hard-decision one: soft values would only repeat the
// slicer's decisions. Also -1 for LICH_CNT 6 or 7, which no sender makes (m17.md 13: discard that LICH)
int m17_frame_lich_get(const uint8_t *hard, const int8_t *soft, uint8_t lich[M17_LICH_BYTES])
{
	ilv_t it;
	int fixed = 0;
	bool bad = false;

	(void)soft;
	memset(lich, 0, M17_LICH_BYTES);
	ilv_at(&it, 0);
	for (uint8_t w = 0; w < 4; w++) {
		uint32_t cw = 0;
		uint16_t d;
		int n;

		for (uint8_t b = 0; b < 24; b++) {
			cw = cw << 1 | (pbit_soft(hard, NULL, it.p) > 0 ? 1u : 0u);
			ilv_next(&it);
		}
		n = fec_golay24_decode(cw, &d);
		if (n < 0) {
			bad = true;
			d = (uint16_t)(cw >> 12);	// the data bits as received: something to look at, never trusted
		} else {
			fixed += n;
		}
		for (uint8_t b = 0; b < 12; b++) {
			uint16_t i = (uint16_t)(12u * w + b);

			if ((d >> (11u - b)) & 1u)
				lich[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
		}
	}
	if (bad || m17_lich_cnt(lich) >= M17_LICH_CHUNKS)
		return -1;
	return fixed;
}

int m17_frame_stream_get(const uint8_t *hard, const int8_t *soft, m17_fec_scratch_t *s, uint16_t *fn,
			 uint8_t stream[M17_STREAM_BYTES])
{
	uint8_t dec[(M17_STREAM_T1_BITS + M17_FLUSH_BITS + 7u) / 8u];
	int errs = unit_get(&u_stream, hard, soft, s, dec);

	*fn = (uint16_t)(dec[0] << 8 | dec[1]);
	memcpy(stream, dec + 2, M17_STREAM_BYTES);
	return errs;
}

int m17_frame_packet_get(const uint8_t *hard, const int8_t *soft, m17_fec_scratch_t *s,
			 uint8_t chunk[M17_PACKET_FRAME_BYTES])
{
	uint8_t dec[(M17_PACKET_T1_BITS + M17_FLUSH_BITS + 7u) / 8u];
	int errs = unit_get(&u_packet, hard, soft, s, dec);

	memcpy(chunk, dec, M17_PACKET_FRAME_BYTES);
	chunk[M17_PACKET_CHUNK] &= 0xFCu;	// bits 1-0 were never sent
	return errs;
}

int m17_frame_bert_get(const uint8_t *hard, const int8_t *soft, m17_fec_scratch_t *s, uint8_t bits[M17_BERT_BYTES])
{
	uint8_t dec[(M17_BERT_T1_BITS + M17_FLUSH_BITS + 7u) / 8u];
	int errs = unit_get(&u_bert, hard, soft, s, dec);

	memcpy(bits, dec, M17_BERT_BYTES);
	bits[M17_BERT_BYTES - 1] &= (uint8_t)(0xFFu << (8u - M17_BERT_T1_BITS % 8u));	// 197 bits: 5 in the last byte
	return errs;
}

// Symbol by symbol against 555D three times
uint8_t m17_eot_errs(const uint8_t *hard)
{
	static const uint8_t eot[2] = { (uint8_t)(M17_EOT_WORD >> 8), (uint8_t)M17_EOT_WORD };
	uint8_t errs = 0;

	for (uint8_t i = 0; i < 24; i++)
		errs = (uint8_t)(errs + (air_sym_get(hard, i) != air_sym_get(eot, (uint16_t)(i & 7u))));
	return errs;
}

// --- The LSF (m17.md 3) ---
void m17_lsf_make(uint8_t lsf[M17_LSF_BYTES], uint64_t dst, uint64_t src, uint16_t type,
		  const uint8_t meta[M17_META_BYTES])
{
	uint16_t crc;

	m17_addr_put(lsf + M17_LSF_DST, dst);
	m17_addr_put(lsf + M17_LSF_SRC, src);
	lsf[M17_LSF_TYPE] = (uint8_t)(type >> 8);
	lsf[M17_LSF_TYPE + 1] = (uint8_t)type;
	if (meta)
		memcpy(lsf + M17_LSF_META, meta, M17_META_BYTES);
	else
		memset(lsf + M17_LSF_META, 0, M17_META_BYTES);
	crc = m17_crc(lsf, M17_LSD_BYTES);
	lsf[M17_LSF_CRC] = (uint8_t)(crc >> 8);
	lsf[M17_LSF_CRC + 1] = (uint8_t)crc;
}

bool m17_lsf_crc_ok(const uint8_t lsf[M17_LSF_BYTES])
{
	return m17_crc(lsf, M17_LSD_BYTES) == (uint16_t)(lsf[M17_LSF_CRC] << 8 | lsf[M17_LSF_CRC + 1]);
}

#endif
