// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 frame codecs, META, packet mode and BERT (core/m17): every known answer of docs/spec/m17.md 11 the codecs
// touch (dibits, syncs, preambles, EoT, randomizer, CRC, sizes, puncturing, Golay, interleaver, LICH, TYPE, GNSS, the
// packet's text block, PRBS9), each frame type built again here from the spec's rules alone and compared bit for bit,
// and round trips through bit errors and soft noise within each code's correction capacity (the capacity computed
// here from the punctured code's minimum distance). Packet chunking for every data length, a packet longer than the
// receive buffer, the BERT receiver's lock, count and loss. Addresses are numeric (m17_addr.c is the codeplug's).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "m17/m17.h"
#include "m17/m17_frame.h"
#include "m17/m17_meta.h"
#include "m17/m17_packet.h"
#include "m17/m17_bert.h"
#include "air/air_syms.h"
#include "fec/conv.h"
#include "fec/golay.h"
#include "hal_air.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; if (failures < 60) { printf("FAIL %s:%d: ", __FILE__, \
	__LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32_t)(rng_state >> 16);
}

static unsigned rndn(unsigned n)
{
	return rng() % n;
}

static void fill(uint8_t *p, size_t n)
{
	while (n--)
		*p++ = (uint8_t)rng();
}

static double gauss(void)
{
	double u = (rng() + 1.0) / 4294967297.0, v = (rng() + 1.0) / 4294967297.0;

	return sqrt(-2.0 * log(u)) * cos(6.283185307179586 * v);
}

static uint8_t bget(const uint8_t *p, unsigned i)
{
	return (uint8_t)((p[i >> 3] >> (7 - (i & 7))) & 1);
}

static void bput(uint8_t *p, unsigned i, uint8_t b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7));

	p[i >> 3] = (uint8_t)(b ? (p[i >> 3] | m) : (p[i >> 3] & ~m));
}

// --- The transmit chain again, from m17.md 2 alone ---
static const fec_conv_code_t code = { 0x19, 0x17 };	// G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4

// The patterns as the spec writes them
static uint8_t p1[61], p2[12], p3[8];
static void patterns_make(void)
{
	p1[0] = 1;
	for (unsigned k = 0; k < 15; k++)
		for (unsigned j = 0; j < 4; j++)
			p1[1 + 4 * k + j] = (uint8_t)("1011"[j] - '0');
	for (unsigned i = 0; i < 12; i++)
		p2[i] = i < 11;
	for (unsigned i = 0; i < 8; i++)
		p3[i] = i < 7;
}

typedef struct {
	const char *name;
	unsigned t1, off, keep;		// type 1 bits, first type 3 place, bits sent
	const uint8_t *pat;
	unsigned plen;
	bool (*kept)(uint16_t);
} unit_ref_t;

static unit_ref_t u_lsf = { "LSF", 240, 0, 368, p1, 61, m17_p1_kept };
static unit_ref_t u_str = { "stream", 144, 96, 272, p2, 12, m17_p2_kept };
static unit_ref_t u_pkt = { "packet", 206, 0, 368, p3, 8, m17_p3_kept };
static unit_ref_t u_bert = { "BERT", 197, 0, 368, p2, 12, m17_p2_kept };

// Type 1 bits to type 3 bits at t3[off..]: flush, code, puncture (and BERT: its 369th bit dropped)
static void ref_unit(const unit_ref_t *u, const uint8_t *t1, uint8_t *t3)
{
	uint8_t in[32] = { 0 }, enc[64];
	unsigned n = u->t1 + 4, y = 0;

	for (unsigned i = 0; i < u->t1; i++)
		bput(in, i, bget(t1, i));
	fec_conv_encode(&code, in, (uint16_t)n, enc);
	for (unsigned j = 0; j < 2 * n; j++)
		if (u->pat[j % u->plen] && y < u->keep)
			bput(t3, u->off + y++, bget(enc, j));
	CHECK(y == u->keep, "%s: %u bits kept", u->name, y);
}

// Type 3 to the 48 bytes: interleave (type 4 place π(x) takes type 3 bit x), randomize, behind the sync
static void ref_frame(uint16_t sync, const uint8_t t3[46], uint8_t out[48])
{
	memset(out, 0, 48);
	out[0] = (uint8_t)(sync >> 8);
	out[1] = (uint8_t)sync;
	for (unsigned x = 0; x < 368; x++)
		bput(out + 2, m17_interleave((uint16_t)x), bget(t3, x));
	for (unsigned i = 0; i < 46; i++)
		out[2 + i] ^= m17_rand_seq[i];
}

// And back: a frame's type 3 bits
static void ref_t3(const uint8_t frame[48], uint8_t t3[46])
{
	uint8_t pay[46];

	for (unsigned i = 0; i < 46; i++)
		pay[i] = (uint8_t)(frame[2 + i] ^ m17_rand_seq[i]);
	memset(t3, 0, 46);
	for (unsigned x = 0; x < 368; x++)
		bput(t3, x, bget(pay, m17_interleave((uint16_t)x)));
}

static void ref_lich(const uint8_t lich[6], uint8_t t3[46])
{
	for (unsigned w = 0; w < 4; w++) {
		uint16_t d = 0;
		uint32_t cw;

		for (unsigned b = 0; b < 12; b++)
			d = (uint16_t)(d << 1 | bget(lich, 12 * w + b));
		cw = fec_golay24_encode(d);
		for (unsigned b = 0; b < 24; b++)
			bput(t3, 24 * w + b, (uint8_t)((cw >> (23 - b)) & 1));
	}
}

// --- The channel ---
// A frame's symbols as the framer hands them: hard dibits and soft values (+3 = AIR_SOFT_OUTER) with Gaussian noise
// of sigma (in soft units) on each, sliced again for the hard dibits
static void channel(const uint8_t frame[48], double sigma, uint8_t hard[48], int8_t soft[192])
{
	for (uint16_t i = 0; i < 192; i++) {
		double v = air_sym_level(air_sym_get(frame, i)) * (AIR_SOFT_OUTER / 3) + sigma * gauss();
		int s = (int)lrint(v);
		uint8_t d;

		s = s > 127 ? 127 : s < -127 ? -127 : s;
		soft[i] = (int8_t)s;
		d = s >= 2 * AIR_SOFT_OUTER / 3 ? 1 : s >= 0 ? 0 : s >= -2 * AIR_SOFT_OUTER / 3 ? 2 : 3;
		if (i < 8)
			d = air_sym_get(frame, i);	// the sync as found
		air_sym_put(hard, i, d);
	}
}

// Hard errors: payload bits flipped (frame bits 16..383), soft values the corrupted symbols' clean levels
static void flip_bits(uint8_t frame[48], const uint16_t *at, unsigned n)
{
	for (unsigned k = 0; k < n; k++)
		bput(frame, 16u + at[k], (uint8_t)(bget(frame, 16u + at[k]) ^ 1));
}

static void soft_clean(const uint8_t frame[48], int8_t soft[192])
{
	for (uint16_t i = 0; i < 192; i++)
		soft[i] = (int8_t)(air_sym_level(air_sym_get(frame, i)) * (AIR_SOFT_OUTER / 3));
}

// n distinct payload places, chosen among the type 3 places [lo, hi)
static void pick(uint16_t *at, unsigned n, unsigned lo, unsigned hi)
{
	for (unsigned k = 0; k < n; k++) {
		bool dup;

		do {
			at[k] = m17_interleave((uint16_t)(lo + rndn(hi - lo)));
			dup = false;
			for (unsigned j = 0; j < k; j++)
				dup |= at[j] == at[k];
		} while (dup);
	}
}

// n errors among the type 3 places [lo, hi), on symbols not yet in used[]. Hard: any bits (Hamming errors). Soft: one
// adjacent-level slip a symbol (+3 <-> +1, +1 <-> -1, -1 <-> -3: one bit of the Gray code), its soft value just past
// the threshold it crossed, as noise leaves it. Every other bit's soft value is at least 32, a slipped one 8: within
// the Hamming capacity the soft decoder is as sure to correct them as the hard one
static void inject(uint8_t frame[48], int8_t *soft, uint8_t used[192], unsigned n, unsigned lo, unsigned hi)
{
	for (unsigned k = 0; k < n; k++) {
		uint16_t p, sym;
		uint8_t d;

		for (;;) {
			p = m17_interleave((uint16_t)(lo + rndn(hi - lo)));
			sym = (uint16_t)(8 + p / 2);
			d = air_sym_get(frame, sym);
			if (used[sym] || (soft && !(p & 1) && (d & 1)))	// soft: no sign flip of an outer symbol
				continue;
			break;
		}
		used[sym] = 1;
		if (soft) {
			uint8_t nd = (uint8_t)(d ^ (p & 1 ? 1 : 2));
			int l0 = air_sym_level(d) * (AIR_SOFT_OUTER / 3), l1 = air_sym_level(nd) * (AIR_SOFT_OUTER / 3);

			air_sym_put(frame, sym, nd);
			soft[sym] = (int8_t)((l0 + l1) / 2 + (l1 > l0 ? 8 : -8));
		} else {
			bput(frame, 16u + p, (uint8_t)(bget(frame, 16u + p) ^ 1));
		}
	}
}

// --- The minimum distance of a terminated, punctured unit: the lightest kept output of any non-zero input ---
// Shortest paths over (time, state) that leave state 0 at some time and return to it (the tail makes every path end
// there); a coded bit's weight counts only where it is sent
static unsigned min_distance(const unit_ref_t *u)
{
	unsigned n = u->t1 + 4, best = 9999;
	uint8_t keep[600];
	unsigned y = 0;

	for (unsigned j = 0; j < 2 * n; j++) {
		keep[j] = u->pat[j % u->plen] && y < u->keep;
		y += keep[j];
	}
	for (unsigned t0 = 0; t0 < u->t1; t0++) {
		unsigned w[16], nw[16];

		for (unsigned s = 0; s < 16; s++)
			w[s] = 9999;
		// The input 1 at t0 from state 0
		{
			unsigned r = 1, g1 = __builtin_parity(r & code.g1), g2 = __builtin_parity(r & code.g2);

			w[1] = g1 * keep[2 * t0] + g2 * keep[2 * t0 + 1];
		}
		for (unsigned t = t0 + 1; t < n; t++) {
			for (unsigned s = 0; s < 16; s++)
				nw[s] = 9999;
			for (unsigned s = 0; s < 16; s++) {
				if (w[s] >= best)
					continue;
				for (unsigned b = 0; b < 2 && (t < u->t1 || b == 0); b++) {
					unsigned r = (s << 1 | b) & 0x1F, ns = r & 0xF;
					unsigned c = w[s] + __builtin_parity(r & code.g1) * keep[2 * t] +
						     __builtin_parity(r & code.g2) * keep[2 * t + 1];

					if (ns == 0) {
						if (c < best)
							best = c;
					} else if (c < nw[ns]) {
						nw[ns] = c;
					}
				}
			}
			memcpy(w, nw, sizeof w);
		}
	}
	return best;
}

// --- Known answers ---
static void test_symbols(void)
{
	static const int8_t b4[4] = { -1, -3, 3, 1 };
	static const struct { uint16_t w; int8_t s[8]; } syncs[] = {
		{ M17_SYNC_LSF, { 3, 3, 3, 3, -3, -3, 3, -3 } },
		{ M17_SYNC_BERT, { -3, 3, -3, -3, 3, 3, 3, 3 } },
		{ M17_SYNC_STREAM, { -3, -3, -3, -3, 3, 3, -3, 3 } },
		{ M17_SYNC_PACKET, { 3, -3, 3, 3, -3, -3, -3, -3 } },
		{ M17_EOT_WORD, { 3, 3, 3, 3, 3, 3, -3, 3 } },
	};
	uint8_t b = 0xB4, f[48], pre[48], h[48];

	for (uint16_t i = 0; i < 4; i++)
		CHECK(air_sym_level(air_sym_get(&b, i)) == b4[i], "B4 symbol %u", i);
	CHECK(M17_SYNC_LSF == 0x55F7 && M17_SYNC_BERT == 0xDF55 && M17_SYNC_STREAM == 0xFF5D &&
	      M17_SYNC_PACKET == 0x75FF && M17_EOT_WORD == 0x555D, "sync words");
	for (unsigned k = 0; k < 5; k++) {
		uint8_t w[2] = { (uint8_t)(syncs[k].w >> 8), (uint8_t)syncs[k].w };

		for (uint16_t i = 0; i < 8; i++)
			CHECK(air_sym_level(air_sym_get(w, i)) == syncs[k].s[i], "sync %04X symbol %u", syncs[k].w, i);
	}
	// LSF = stream negated, BERT = packet negated (every dibit's sign bit flipped)
	CHECK((M17_SYNC_LSF ^ M17_SYNC_STREAM) == 0xAAAA && (M17_SYNC_BERT ^ M17_SYNC_PACKET) == 0xAAAA, "negations");
	// Each frame starts with its sync
	memset(h, 0, sizeof h);
	m17_frame_lsf(h, f);
	CHECK(f[0] == 0x55 && f[1] == 0xF7, "LSF sync");
	m17_frame_stream(h, 0, h, f);
	CHECK(f[0] == 0xFF && f[1] == 0x5D, "stream sync");
	m17_frame_packet(h, f);
	CHECK(f[0] == 0x75 && f[1] == 0xFF, "packet sync");
	m17_frame_bert(h, f);
	CHECK(f[0] == 0xDF && f[1] == 0x55, "BERT sync");
	// Preambles: 77... and DD..., alternating outer symbols, the last opposite the sync's first
	m17_preamble(false, pre);
	for (unsigned i = 0; i < 48; i++)
		CHECK(pre[i] == 0x77, "LSF preamble byte %u", i);
	for (uint16_t i = 0; i < 192; i++)
		CHECK(air_sym_level(air_sym_get(pre, i)) == (i & 1 ? -3 : 3), "LSF preamble symbol %u", i);
	CHECK(air_sym_level(air_sym_get(pre, 191)) == -syncs[0].s[0], "LSF preamble into its sync");
	m17_preamble(true, pre);
	for (unsigned i = 0; i < 48; i++)
		CHECK(pre[i] == 0xDD, "BERT preamble byte %u", i);
	CHECK(air_sym_level(air_sym_get(pre, 191)) == -syncs[1].s[0], "BERT preamble into its sync");
	// EoT: 555D 24 times; the error count per symbol
	m17_eot(f);
	for (unsigned i = 0; i < 48; i += 2)
		CHECK(f[i] == 0x55 && f[i + 1] == 0x5D, "EoT bytes %u", i);
	CHECK(m17_eot_errs(f) == 0, "EoT clean");
	for (unsigned n = 1; n <= 24; n++) {
		uint8_t e[48];

		memcpy(e, f, 48);
		for (uint16_t i = 0; i < n; i++)
			air_sym_put(e, i, (uint8_t)(air_sym_get(e, i) ^ (1 + rndn(3))));
		CHECK(m17_eot_errs(e) == n, "EoT with %u symbol errors: %u", n, m17_eot_errs(e));
	}
	CHECK(m17_eot_errs(pre) > 4, "a preamble is no EoT");
	printf("  dibits, syncs, preambles, EoT\n");
}

static void test_randomizer_crc(void)
{
	static const uint8_t seq[46] = {
		0xD6, 0xB5, 0xE2, 0x30, 0x82, 0xFF, 0x84, 0x62, 0xBA, 0x4E, 0x96, 0x90, 0xD8, 0x98, 0xDD, 0x5D,
		0x0C, 0xC8, 0x52, 0x43, 0x91, 0x1D, 0xF8, 0x6E, 0x68, 0x2F, 0x35, 0xDA, 0x14, 0xEA, 0xCD, 0x76,
		0x19, 0x8D, 0xD5, 0x80, 0xD1, 0x33, 0x87, 0x13, 0x57, 0x18, 0x2D, 0x29, 0x78, 0xC3,
	};
	uint8_t all[256], buf[900];

	CHECK(memcmp(m17_rand_seq, seq, 46) == 0, "randomizer sequence");
	CHECK(m17_rand_seq[0] == 0xD6 && m17_rand_seq[1] == 0xB5 && m17_rand_seq[45] == 0xC3, "randomizer ends");
	for (unsigned i = 0; i < 256; i++)
		all[i] = (uint8_t)i;
	CHECK(m17_crc(all, 0) == 0xFFFF, "CRC empty %04X", m17_crc(all, 0));
	CHECK(m17_crc((const uint8_t *)"A", 1) == 0x206E, "CRC A");
	CHECK(m17_crc((const uint8_t *)"123456789", 9) == 0x772B, "CRC 123456789");
	CHECK(m17_crc(all, 256) == 0x1C31, "CRC 00..FF");
	for (unsigned rep = 0; rep < 500; rep++) {
		unsigned n = rndn(826);
		uint16_t c;

		fill(buf, n);
		c = m17_crc(buf, n);
		buf[n] = (uint8_t)(c >> 8);
		buf[n + 1] = (uint8_t)c;
		CHECK(m17_crc(buf, n + 2) == 0, "CRC residue, %u bytes", n);
	}
	printf("  randomizer, CRC FFFF 206E 772B 1C31, residue 0\n");
}

static void test_sizes_puncture(void)
{
	const unit_ref_t *u[4] = { &u_lsf, &u_str, &u_pkt, &u_bert };
	static const unsigned t2[4] = { 488, 296, 420, 402 }, sent[4] = { 368, 272, 368, 369 };
	unsigned g1_drop = 0, g2_drop = 0;

	CHECK(M17_LSF_T1_BITS == 240 && M17_STREAM_T1_BITS == 144 && M17_PACKET_T1_BITS == 206 &&
	      M17_BERT_T1_BITS == 197, "type 1 sizes");
	for (unsigned k = 0; k < 4; k++) {
		unsigned n = 0;

		CHECK(2 * (u[k]->t1 + M17_FLUSH_BITS) == t2[k], "%s type 2: %u", u[k]->name, 2 * (u[k]->t1 + 4));
		for (uint16_t j = 0; j < t2[k]; j++) {
			n += u[k]->kept(j);
			CHECK(u[k]->kept(j) == (u[k]->pat[j % u[k]->plen] != 0), "%s pattern at %u", u[k]->name, j);
		}
		CHECK(n == sent[k], "%s punctured: %u", u[k]->name, n);
	}
	// P1 drops 2, 6, ... 58 of each 61; as many G1 as G2 bits over the LSF
	for (uint16_t j = 0; j < 61; j++)
		CHECK(m17_p1_kept(j) == !(j >= 2 && (j - 2) % 4 == 0), "P1 at %u", j);
	for (uint16_t j = 0; j < 488; j++)
		if (!m17_p1_kept(j)) {
			if (j & 1)
				g2_drop++;
			else
				g1_drop++;
		}
	CHECK(g1_drop == 60 && g2_drop == 60, "P1 drops G1 %u G2 %u", g1_drop, g2_drop);
	CHECK(!m17_p2_kept(11) && m17_p2_kept(10) && !m17_p2_kept(23) && m17_p2_kept(12), "P2");
	CHECK(!m17_p3_kept(7) && m17_p3_kept(6) && !m17_p3_kept(15) && m17_p3_kept(8), "P3");
	CHECK(M17_PAYLOAD_BITS == 368 && M17_FRAME_SYMS == 192 && M17_LICH_T2_BITS == 96, "geometry");
	printf("  sizes 240/144/206/197 -> 488/296/420/402 -> 368/272/368/369\n");
}

static void test_golay_interleaver(void)
{
	static const uint16_t data[5] = { 0x800, 0x001, 0x123, 0xFFF, 0x000 };
	static const uint32_t word[5] = { 0x800C75, 0x0018EB, 0x1230AC, 0xFFFFFF, 0x000000 };
	static const uint16_t rows[12] = { 0xC75, 0x63B, 0xF68, 0x7B4, 0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E,
					   0x8EB };
	static const uint16_t fixed[8] = { 0, 23, 92, 115, 184, 207, 276, 299 };
	uint8_t seen[368] = { 0 };
	unsigned nfixed = 0;

	for (unsigned k = 0; k < 5; k++)
		CHECK(fec_golay24_encode(data[k]) == word[k], "Golay %03X: %06X", data[k], fec_golay24_encode(data[k]));
	for (unsigned k = 0; k < 12; k++)
		CHECK((fec_golay24_encode((uint16_t)(0x800u >> k)) & 0xFFF) == rows[k], "Golay P row %u", k);
	// Bits 11-1 are data(x) x^11 mod g(x), g = C75 (x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1); bit 0 even parity
	for (uint32_t dw = 0; dw < 4096; dw++) {
		uint32_t r = dw << 11, cw = fec_golay24_encode((uint16_t)dw);

		for (unsigned b = 22; b >= 11; b--)
			if ((r >> b) & 1)
				r ^= 0xC75u << (b - 11);
		CHECK(((cw >> 1) & 0x7FF) == r && __builtin_parity(cw) == 0 && cw >> 12 == dw, "Golay %03X from g(x)", dw);
	}
	CHECK(m17_interleave(0) == 0 && m17_interleave(1) == 137 && m17_interleave(2) == 90 &&
	      m17_interleave(3) == 227 && m17_interleave(47) == 367 && m17_interleave(367) == 47, "π points");
	for (uint16_t x = 0; x < 368; x++) {
		uint16_t p = m17_interleave(x);

		CHECK(p < 368 && m17_interleave(p) == x, "π(π(%u))", x);
		seen[p]++;
		if (p == x) {
			CHECK(nfixed < 8 && fixed[nfixed] == x, "fixed point %u", x);
			nfixed++;
		}
	}
	CHECK(nfixed == 8, "fixed points: %u", nfixed);
	for (unsigned i = 0; i < 368; i++)
		CHECK(seen[i] == 1, "π a permutation at %u", i);
	printf("  Golay words and P rows, π points, involution, fixed points\n");
}

static void test_type_lsf(void)
{
	uint8_t lsf[30], meta[14], lich[6];

	CHECK(m17_type(true, M17_DATA_VOICE, M17_ENC_NONE, M17_META_TEXT, 0, false) == 0x0005, "TYPE voice");
	CHECK(m17_type(true, M17_DATA_VOICE_DATA, M17_ENC_NONE, M17_META_TEXT, 0, false) == 0x0007, "TYPE voice+data");
	CHECK(m17_type(true, M17_DATA_DATA, M17_ENC_NONE, M17_META_TEXT, 0, false) == 0x0003, "TYPE data");
	CHECK(m17_type(true, M17_DATA_VOICE, M17_ENC_NONE, M17_META_TEXT, 15, false) == 0x0785, "TYPE CAN 15");
	CHECK(m17_type(true, M17_DATA_VOICE, M17_ENC_AES, M17_AES_128, 0, false) == 0x0015, "TYPE AES-128");
	CHECK(m17_type(true, M17_DATA_VOICE, M17_ENC_SCRAMBLER, M17_SCR_16, 0, false) == 0x002D, "TYPE scrambler 16");
	CHECK(m17_type(true, M17_DATA_VOICE, M17_ENC_NONE, M17_META_GNSS, 0, true) == 0x0825, "TYPE GNSS signed");
	CHECK(m17_type(false, 0, M17_ENC_NONE, M17_META_TEXT, 0, false) == 0x0000, "TYPE packet");
	CHECK(M17_TYPE_CAN(0x0785) == 15 && M17_TYPE_DATA(0x0007) == 3 && M17_TYPE_ENC(0x002D) == 1 &&
	      M17_TYPE_SUB(0x002D) == 1 && (0x0825 & M17_TYPE_SIGNED), "TYPE fields");
	// The LSF: N0CALL to W1AW (m17.md 11), fields where 3 puts them, the CRC over 28 bytes
	fill(meta, 14);
	m17_lsf_make(lsf, 0x0000001680B7ull, 0x00004B13D106ull, 0x0005, meta);
	CHECK(m17_addr_get(lsf) == 0x0000001680B7ull && m17_addr_get(lsf + 6) == 0x00004B13D106ull, "LSF addresses");
	CHECK(lsf[5] == 0xB7 && lsf[11] == 0x06 && m17_lsf_type(lsf) == 0x0005 && !memcmp(lsf + 14, meta, 14),
	      "LSF layout");
	CHECK(m17_lsf_crc_ok(lsf) && m17_crc(lsf, 30) == 0, "LSF CRC");
	CHECK((uint16_t)(lsf[28] << 8 | lsf[29]) == m17_crc(lsf, 28), "LSF CRC big endian");
	lsf[3] ^= 0x10;
	CHECK(!m17_lsf_crc_ok(lsf), "LSF CRC catches");
	m17_lsf_make(lsf, M17_ADDR_BROADCAST, 1, 0x0005, NULL);
	for (unsigned i = 14; i < 28; i++)
		CHECK(lsf[i] == 0, "no META: zeros");
	CHECK(m17_addr_get(lsf) == 0xFFFFFFFFFFFFull, "broadcast");
	// LICH_CNT 0..5: LSF bits 40 n .. 40 n + 39
	fill(lsf, 30);
	for (uint8_t n = 0; n < 6; n++) {
		m17_lich_make(lsf, n, lich);
		for (unsigned i = 0; i < 40; i++)
			CHECK(bget(lich, i) == bget(lsf, 40u * n + i), "LICH %u bit %u", n, i);
		CHECK(m17_lich_cnt(lich) == n && (lich[5] & 0x1F) == 0, "LICH_CNT %u", n);
	}
	printf("  TYPE words, LSF layout and CRC, LICH chunks\n");
}

// --- Every frame type against the spec's chain, and the LICH's layout ---
static void test_frames_ref(void)
{
	for (unsigned rep = 0; rep < 200; rep++) {
		uint8_t t1[32], t3[46], ref[48], f[48], lsf[30], lich[6], st[16];
		uint16_t fn = (uint16_t)rng();

		fill(lsf, 30);
		m17_frame_lsf(lsf, f);
		memset(t3, 0, 46);
		ref_unit(&u_lsf, lsf, t3);
		ref_frame(M17_SYNC_LSF, t3, ref);
		CHECK(!memcmp(f, ref, 48), "LSF frame against the spec's chain");

		m17_lich_make(lsf, (uint8_t)(rep % 6), lich);
		fill(st, 16);
		m17_frame_stream(lich, fn, st, f);
		memset(t3, 0, 46);
		t1[0] = (uint8_t)(fn >> 8);
		t1[1] = (uint8_t)fn;
		memcpy(t1 + 2, st, 16);
		ref_lich(lich, t3);
		ref_unit(&u_str, t1, t3);
		ref_frame(M17_SYNC_STREAM, t3, ref);
		CHECK(!memcmp(f, ref, 48), "stream frame against the spec's chain");
		// The LICH: four Golay words at type 3 places 0..95, LICH bit 0 the first word's data bit 11
		ref_t3(f, t3);
		for (unsigned w = 0; w < 4; w++) {
			uint32_t cw = 0;

			for (unsigned b = 0; b < 24; b++)
				cw = cw << 1 | bget(t3, 24 * w + b);
			CHECK(fec_golay24_encode((uint16_t)(cw >> 12)) == cw, "LICH word %u a code word", w);
			for (unsigned b = 0; b < 12; b++)
				CHECK(((cw >> (23 - b)) & 1) == bget(lich, 12 * w + b), "LICH bit %u", 12 * w + b);
		}

		fill(t1, 26);
		m17_frame_packet(t1, f);
		memset(t3, 0, 46);
		ref_unit(&u_pkt, t1, t3);
		ref_frame(M17_SYNC_PACKET, t3, ref);
		CHECK(!memcmp(f, ref, 48), "packet frame against the spec's chain");

		fill(t1, 25);
		m17_frame_bert(t1, f);
		memset(t3, 0, 46);
		ref_unit(&u_bert, t1, t3);
		ref_frame(M17_SYNC_BERT, t3, ref);
		CHECK(!memcmp(f, ref, 48), "BERT frame against the spec's chain");
	}
	printf("  LSF, stream (LICH), packet and BERT frames bit for bit against the spec's chain\n");
}

// --- Round trips ---
static m17_fec_scratch_t scratch;

typedef struct {
	unsigned ok, runs;
} rate_t;

// One frame of each kind through hard errors; t: errors the unit is sure to correct
static void test_round_trips(void)
{
	const unit_ref_t *u[4] = { &u_lsf, &u_str, &u_pkt, &u_bert };
	unsigned d[4], t[4];

	for (unsigned k = 0; k < 4; k++) {
		d[k] = min_distance(u[k]);
		t[k] = (d[k] - 1) / 2;
	}
	printf("  minimum distance: LSF %u, stream %u, packet %u, BERT %u (corrects %u/%u/%u/%u)\n", d[0], d[1], d[2],
	       d[3], t[0], t[1], t[2], t[3]);
	// P1 leaves the LSF the weakest (rate 244/368); the other three correct 2 errors or more
	CHECK(d[0] == 4 && d[1] == 6 && d[2] == 5 && d[3] == 5, "minimum distances");
	for (unsigned rep = 0; rep < 4000; rep++) {
		uint8_t lsf[30], out[30], f[48], lich[6], li[6], st[16], so[16], ch[26], co[26], bb[25], bo[25], meta[14];
		uint8_t used[192];
		int8_t soft[192], *sp = rep & 1 ? soft : NULL;
		uint16_t fn = (uint16_t)rng(), fo;
		unsigned e;
		int r;

		// LSF, up to its capacity
		fill(meta, 14);
		m17_lsf_make(lsf, ((uint64_t)rng() << 16) ^ rng(), rng(), (uint16_t)rng(), meta);
		m17_frame_lsf(lsf, f);
		soft_clean(f, soft);
		memset(used, 0, sizeof used);
		e = rndn(t[0] + 1);
		inject(f, sp, used, e, 0, 368);
		r = m17_frame_lsf_get(f, sp, &scratch, out);
		CHECK(r == (int)e && !memcmp(out, lsf, 30), "LSF with %u errors (%s): %d", e, sp ? "soft" : "hard", r);

		// Stream: the LICH words up to 3 errors each, the stream part up to its capacity
		m17_lich_make(lsf, (uint8_t)(rep % 6), lich);
		fill(st, 16);
		m17_frame_stream(lich, fn, st, f);
		soft_clean(f, soft);
		memset(used, 0, sizeof used);
		{
			unsigned el = 0;

			for (unsigned w = 0; w < 4; w++) {
				unsigned n = rndn(4);

				inject(f, sp, used, n, 24 * w, 24 * w + 24);
				el += n;
			}
			e = rndn(t[1] + 1);
			inject(f, sp, used, e, 96, 368);
			r = m17_frame_lich_get(f, sp, li);
			CHECK(r == (int)el && !memcmp(li, lich, 6), "LICH with %u errors: %d", el, r);
			r = m17_frame_stream_get(f, sp, &scratch, &fo, so);
			CHECK(r == (int)e && fo == fn && !memcmp(so, st, 16), "stream with %u errors: %d", e, r);
		}

		// Packet frame (byte 25 bits 1-0 are not sent: they come back 0)
		fill(ch, 26);
		m17_frame_packet(ch, f);
		soft_clean(f, soft);
		memset(used, 0, sizeof used);
		e = rndn(t[2] + 1);
		inject(f, sp, used, e, 0, 368);
		r = m17_frame_packet_get(f, sp, &scratch, co);
		ch[25] &= 0xFC;
		CHECK(r == (int)e && !memcmp(co, ch, 26), "packet with %u errors: %d", e, r);

		// BERT frame (197 bits: the last byte's low 3 bits are not sent)
		fill(bb, 25);
		m17_frame_bert(bb, f);
		soft_clean(f, soft);
		memset(used, 0, sizeof used);
		e = rndn(t[3] + 1);
		inject(f, sp, used, e, 0, 368);
		r = m17_frame_bert_get(f, sp, &scratch, bo);
		bb[24] &= 0xF8;
		CHECK(r == (int)e && !memcmp(bo, bb, 25), "BERT with %u errors: %d", e, r);
	}
	// Past the Hamming capacity soft values still pay: the LSF with 4 slips (its distance), every one corrected
	{
		unsigned ok = 0;

		for (unsigned rep = 0; rep < 1000; rep++) {
			uint8_t lsf[30], out[30], f[48], meta[14], used[192] = { 0 };
			int8_t soft[192];

			fill(meta, 14);
			m17_lsf_make(lsf, rng(), rng(), 5, meta);
			m17_frame_lsf(lsf, f);
			soft_clean(f, soft);
			inject(f, soft, used, 4, 0, 368);
			ok += m17_frame_lsf_get(f, soft, &scratch, out) == 4 && !memcmp(out, lsf, 30);
		}
		CHECK(ok == 1000, "LSF with 4 slips, soft: %u of 1000", ok);
	}
	// A LICH word with 4 errors is detected, not miscorrected; LICH_CNT 6 and 7 are refused
	for (unsigned rep = 0; rep < 300; rep++) {
		uint8_t lsf[30], f[48], lich[6], li[6], st[16] = { 0 };
		uint16_t at[4];

		fill(lsf, 30);
		m17_lich_make(lsf, (uint8_t)(rep % 6), lich);
		m17_frame_stream(lich, 0, st, f);
		pick(at, 4, 24 * (rep % 4), 24 * (rep % 4) + 24);
		flip_bits(f, at, 4);
		CHECK(m17_frame_lich_get(f, NULL, li) == -1, "LICH word with 4 errors");
		lich[5] = (uint8_t)((6 + rep % 2) << 5);
		m17_frame_stream(lich, 0, st, f);
		CHECK(m17_frame_lich_get(f, NULL, li) == -1 && m17_lich_cnt(li) == 6 + rep % 2, "LICH_CNT %u", 6 + rep % 2);
	}
	// Far past the capacity the LSF's CRC says so (no false LSF in many tries)
	{
		uint8_t lsf[30], out[30], f[48], meta[14];
		unsigned bad = 0, false_ok = 0;

		for (unsigned rep = 0; rep < 2000; rep++) {
			uint16_t at[60];

			fill(lsf, 30);
			fill(meta, 14);
			m17_lsf_make(lsf, rng(), rng(), 5, meta);
			m17_frame_lsf(lsf, f);
			pick(at, 60, 0, 368);
			flip_bits(f, at, 60);
			if (m17_frame_lsf_get(f, NULL, &scratch, out) < 0)
				bad++;
			else
				false_ok += memcmp(out, lsf, 30) != 0;
		}
		CHECK(bad > 1900 && false_ok == 0, "LSF with 60 errors: %u refused, %u false", bad, false_ok);
	}
	printf("  round trips within capacity: LSF, stream with LICH, packet, BERT; hard and soft\n");
}

// Gaussian noise: soft beats hard; and the frame error rate at a moderate noise is small
static void test_noise(void)
{
	static const double sig[4] = { 14.0, 20.0, 28.0, 36.0 };

	for (unsigned k = 0; k < 4; k++) {
		unsigned soft_ok = 0, hard_ok = 0, lich_soft = 0, lich_hard = 0, sym_err = 0, n = 1500;

		for (unsigned rep = 0; rep < n; rep++) {
			uint8_t lsf[30], f[48], h[48], lich[6], li[6], st[16], so[16], meta[14];
			int8_t soft[192];
			uint16_t fo;

			fill(lsf, 30);
			fill(meta, 14);
			m17_lsf_make(lsf, rng(), rng(), 5, meta);
			m17_lich_make(lsf, (uint8_t)(rep % 6), lich);
			fill(st, 16);
			m17_frame_stream(lich, (uint16_t)rep, st, f);
			channel(f, sig[k], h, soft);
			for (uint16_t i = 8; i < 192; i++)
				sym_err += air_sym_get(h, i) != air_sym_get(f, i);
			soft_ok += m17_frame_stream_get(h, soft, &scratch, &fo, so) >= 0 && fo == rep && !memcmp(so, st, 16);
			hard_ok += m17_frame_stream_get(h, NULL, &scratch, &fo, so) >= 0 && fo == rep && !memcmp(so, st, 16);
			lich_soft += m17_frame_lich_get(h, soft, li) >= 0 && !memcmp(li, lich, 6);
			lich_hard += m17_frame_lich_get(h, NULL, li) >= 0 && !memcmp(li, lich, 6);
		}
		printf("  noise sigma %.0f (+3 = %d): symbol errors %.2f%%, stream frames soft %u hard %u, LICH %u/%u of %u\n",
		       sig[k], AIR_SOFT_OUTER, 100.0 * sym_err / (n * 184.0), soft_ok, hard_ok, lich_soft, lich_hard, n);
		CHECK(soft_ok >= hard_ok, "soft no worse than hard at sigma %.0f", sig[k]);
		CHECK(lich_soft == lich_hard, "the LICH decodes the provider's hard dibits either way");
		if (k == 0)
			CHECK(soft_ok == n && lich_soft >= n - 3, "sigma 14: every stream frame, LICH %u", lich_soft);
		if (k == 1)
			CHECK(soft_ok >= n - n / 100, "sigma 20: 99%% of the stream frames");
		if (k == 3)
			CHECK(soft_ok > hard_ok + n / 20, "sigma 36: soft decisions pay");
	}
	// LSF, packet and BERT frames through the same noise: soft and hard
	for (unsigned k = 0; k < 2; k++) {
		static const double sg[2] = { 16.0, 24.0 };
		unsigned ok[2][3] = { { 0 } }, n = 1000;

		for (unsigned rep = 0; rep < n; rep++) {
			uint8_t lsf[30], out[30], f[48], h[48], ch[26], co[26], bb[25], bo[25], meta[14];
			int8_t soft[192];

			fill(meta, 14);
			m17_lsf_make(lsf, rng(), rng(), 5, meta);
			fill(ch, 26);
			ch[25] &= 0xFC;
			fill(bb, 25);
			bb[24] &= 0xF8;
			for (unsigned m = 0; m < 2; m++) {
				int8_t *sp = m ? NULL : soft;

				m17_frame_lsf(lsf, f);
				channel(f, sg[k], h, soft);
				ok[m][0] += m17_frame_lsf_get(h, sp, &scratch, out) >= 0 && !memcmp(out, lsf, 30);
				m17_frame_packet(ch, f);
				channel(f, sg[k], h, soft);
				m17_frame_packet_get(h, sp, &scratch, co);
				ok[m][1] += !memcmp(co, ch, 26);
				m17_frame_bert(bb, f);
				channel(f, sg[k], h, soft);
				m17_frame_bert_get(h, sp, &scratch, bo);
				ok[m][2] += !memcmp(bo, bb, 25);
			}
		}
		printf("  noise sigma %.0f: LSF %u/%u, packet %u/%u, BERT %u/%u of %u (soft/hard)\n", sg[k], ok[0][0],
		       ok[1][0], ok[0][1], ok[1][1], ok[0][2], ok[1][2], n);
		for (unsigned j = 0; j < 3; j++)
			CHECK(ok[0][j] >= ok[1][j], "soft no worse, sigma %.0f", sg[k]);
		if (k == 0)
			CHECK(ok[0][0] >= n - n / 100 && ok[0][1] >= n - n / 100 && ok[0][2] >= n - n / 100, "sigma 16 frames");
	}
}

// --- META ---
static void test_text(void)
{
	uint8_t meta[14], txt[80];
	char out[M17_TEXT_MAX + 1];
	m17_text_t t;

	memset(txt, 'x', sizeof txt);
	CHECK(m17_text_blocks(txt, 0) == 0 && m17_text_blocks((const uint8_t *)"a", 1) == 1, "blocks 0, 1");
	CHECK(m17_text_blocks(txt, 13) == 1 && m17_text_blocks(txt, 14) == 2 && m17_text_blocks(txt, 39) == 3, "13 14 39");
	memset(txt, 'x', sizeof txt);
	CHECK(m17_text_blocks(txt, 52) == 4 && m17_text_blocks(txt, 80) == 4, "52, longer cut");
	// A packet carries one block: control byte 11 (m17.md 11)
	m17_text_meta((const uint8_t *)"Hello", 5, 0, meta);
	CHECK(meta[0] == 0x11 && !memcmp(meta + 1, "Hello        ", 13), "one block: 11, padded with spaces");
	m17_text_meta(txt, 0, 0, meta);
	for (unsigned i = 0; i < 14; i++)
		CHECK(meta[i] == 0, "no text: 00");
	// Control bytes of 2, 3 and 4 blocks
	for (unsigned nb = 1; nb <= 4; nb++) {
		static const uint8_t hi[5] = { 0, 0x1, 0x3, 0x7, 0xF };

		for (uint8_t i = 0; i < nb; i++) {
			m17_text_meta(txt, 13 * nb, i, meta);
			CHECK(meta[0] == (uint8_t)(hi[nb] << 4 | 1u << i), "control %u blocks, block %u: %02X", nb, i, meta[0]);
		}
		m17_text_meta(txt, 13 * nb, (uint8_t)nb, meta);
		CHECK(meta[0] == (uint8_t)(hi[nb] << 4 | 1u), "block index past the last wraps");
	}
	// The cut never splits a UTF-8 character: a two-byte one at 51-52, a three-byte one at 50-52, a four at 49-52
	{
		static const unsigned starts[3] = { 51, 50, 49 };
		static const uint8_t lead[3] = { 0xC3, 0xE2, 0xF0 };
		static const unsigned keep[3] = { 51, 50, 49 };

		for (unsigned k = 0; k < 3; k++) {
			memset(txt, 'a', sizeof txt);
			txt[starts[k]] = lead[k];
			for (unsigned j = starts[k] + 1; j < starts[k] + 2 + k; j++)
				txt[j] = 0x80 | (uint8_t)j;
			m17_text_meta(txt, starts[k] + 2 + k, 3, meta);
			CHECK(meta[0] == 0xF8, "4 blocks");
			CHECK(!memcmp(meta + 1, txt + 39, keep[k] - 39), "the last block's text");
			for (unsigned j = keep[k] - 39; j < 13; j++)
				CHECK(meta[1 + j] == ' ', "the character cut is left out, %u", k);
		}
		// One that fits exactly is kept
		memset(txt, 'a', sizeof txt);
		txt[50] = 0xC3;
		txt[51] = 0xA9;
		m17_text_meta(txt, 60, 3, meta);
		CHECK(meta[12] == 0xC3 && meta[13] == 0xA9, "a character ending at 52 kept");
	}
	// Collecting: blocks out of order, a repeat, then done; trailing spaces gone
	{
		const char *msg = "The quick brown fox jumps over the lazy";	// 39 bytes: 3 blocks
		size_t n = strlen(msg);

		m17_text_clear(&t);
		CHECK(!m17_text_done(&t) && m17_text_get(&t, out) == 0 && out[0] == 0, "empty");
		m17_text_meta((const uint8_t *)msg, n, 2, meta);
		CHECK(m17_text_add(&t, meta) && !m17_text_done(&t), "block 3");
		CHECK(m17_text_get(&t, out) == 13 && !strcmp(out, msg + 26), "a part: '%s'", out);
		CHECK(!m17_text_add(&t, meta), "the same block again: nothing new");
		m17_text_meta((const uint8_t *)msg, n, 0, meta);
		CHECK(m17_text_add(&t, meta), "block 1");
		CHECK(!m17_text_done(&t), "two of three");
		m17_text_meta((const uint8_t *)msg, n, 1, meta);
		CHECK(m17_text_add(&t, meta) && m17_text_done(&t), "complete");
		CHECK(m17_text_get(&t, out) == n && !strcmp(out, msg), "text '%s'", out);
		CHECK(t.want == 0x7 && t.have == 0x7, "nibbles");
		// A missing middle block is left out
		m17_text_clear(&t);
		m17_text_meta((const uint8_t *)msg, n, 0, meta);
		m17_text_add(&t, meta);
		m17_text_meta((const uint8_t *)msg, n, 2, meta);
		m17_text_add(&t, meta);
		CHECK(m17_text_get(&t, out) == 13 + strlen(msg + 26) && !memcmp(out, msg, 13) && !strcmp(out + 13, msg + 26),
		      "block 2 missing: '%s'", out);
		// Short text padded with spaces comes back without them; zero padding too
		m17_text_clear(&t);
		m17_text_meta((const uint8_t *)"CQ CQ", 5, 0, meta);
		m17_text_add(&t, meta);
		CHECK(m17_text_done(&t) && m17_text_get(&t, out) == 5 && !strcmp(out, "CQ CQ"), "'%s'", out);
		memset(meta, 0, 14);
		meta[0] = 0x11;
		memcpy(meta + 1, "AB", 2);
		m17_text_clear(&t);
		m17_text_add(&t, meta);
		CHECK(m17_text_get(&t, out) == 2 && !strcmp(out, "AB"), "zero padding");
		// A changed block replaces the old one and is news
		memcpy(meta + 1, "AC", 2);
		CHECK(m17_text_add(&t, meta) && m17_text_get(&t, out) == 2 && !strcmp(out, "AC"), "changed block");
	}
	// Control bytes that are not a block of a message
	{
		static const uint8_t bad[] = { 0x00, 0x10, 0x13, 0x12, 0x34, 0x24, 0x21, 0x5F, 0xF3, 0xF0, 0x0F, 0x3C, 0x78 };

		for (unsigned k = 0; k < sizeof bad; k++) {
			m17_text_clear(&t);
			memset(meta, 'z', 14);
			meta[0] = bad[k];
			CHECK(!m17_text_add(&t, meta) && t.want == 0 && t.have == 0, "control %02X refused", bad[k]);
		}
	}
	printf("  text: blocks, control bytes (11 for a packet), the UTF-8 cut, collecting\n");
}

static void test_gnss(void)
{
	uint8_t meta[14];
	m17_gnss_t g, o;
	int32_t lat, lon, alt;
	uint16_t spd;

	// The bit layout of 4.2, one known pattern
	memset(&g, 0, sizeof g);
	g.source = 0;
	g.station = M17_STATION_HANDHELD;
	g.valid = 0xF;
	g.radius = 3;
	g.bearing = 270;
	g.lat = 0x123456;
	g.lon = -2;
	g.alt = 0xABCD;
	g.speed = 0x9EF;
	m17_gnss_encode(&g, meta);
	{
		static const uint8_t want[14] = { 0x02, 0xF7, 0x0E, 0x12, 0x34, 0x56, 0xFF, 0xFF, 0xFE, 0xAB, 0xCD, 0x9E,
						  0xF0, 0x00 };

		CHECK(!memcmp(meta, want, 14), "GNSS layout %02X %02X %02X ... %02X %02X", meta[0], meta[1], meta[2],
		      meta[11], meta[12]);
	}
	CHECK(m17_gnss_decode(meta, &o) && !memcmp(&o, &g, sizeof g), "GNSS decode");
	// Limits of m17.md 11
	g.alt = 0;
	m17_gnss_to_fix(&g, NULL, NULL, &alt, NULL);
	CHECK(alt == -5000, "altitude 0: -500 m (%d dm)", alt);
	g.alt = 65535;
	m17_gnss_to_fix(&g, NULL, NULL, &alt, NULL);
	CHECK(alt == 322675, "altitude 65535: 32267.5 m (%d dm)", alt);
	g.speed = 4095;
	m17_gnss_to_fix(&g, NULL, NULL, NULL, &spd);
	CHECK(spd == 20475, "speed 4095: 2047.5 km/h");
	g.lat = 8388607;
	g.lon = 8388607;
	m17_gnss_to_fix(&g, &lat, &lon, NULL, NULL);
	CHECK(lat == 900000000 && lon == 1800000000, "+90 +180: %d %d", lat, lon);
	g.lat = -8388607;
	g.lon = -8388607;
	m17_gnss_to_fix(&g, &lat, &lon, NULL, NULL);
	CHECK(lat == -900000000 && lon == -1800000000, "-90 -180");
	// -8388608 is never sent: encoded as -8388607, read as -8388607
	g.lat = -8388608;
	m17_gnss_encode(&g, meta);
	CHECK(meta[3] == 0x80 && meta[4] == 0x00 && meta[5] == 0x01, "-8388608 clamped");
	meta[5] = 0x00;
	m17_gnss_decode(meta, &o);
	CHECK(o.lat == -8388607, "-8388608 read as -8388607");
	// Radius from the error: 1 m, 2 m, 3 m, 128 m, 500 m -> 0, 1, 2, 7, 7
	{
		static const uint32_t err[6] = { 1, 2, 3, 128, 500, 129 };
		static const uint8_t n[6] = { 0, 1, 2, 7, 7, 7 };

		for (unsigned k = 0; k < 6; k++) {
			m17_gnss_from_fix(&g, 0, 0, 0, false, 0, 0, false, err[k], M17_STATION_MOBILE);
			CHECK(g.radius == n[k] && (g.valid & M17_GNSS_VALID_RADIUS), "radius %u m: %u", err[k], g.radius);
		}
		m17_gnss_from_fix(&g, 0, 0, 0, false, 0, 0, false, 0, M17_STATION_MOBILE);
		CHECK(g.valid == M17_GNSS_VALID_POS && g.station == M17_STATION_MOBILE && g.source == M17_GNSS_SRC_CLIENT,
		      "unknown radius: not valid");
	}
	// Fields without their validity bit go out as zeros and come back as zero / the tree's "none"
	memset(&g, 0, sizeof g);
	g.valid = M17_GNSS_VALID_POS;
	g.lat = 1000;
	g.lon = -1000;
	g.alt = 1234;
	g.speed = 99;
	g.bearing = 99;
	g.radius = 5;
	m17_gnss_encode(&g, meta);
	CHECK(meta[1] == 0x80 && meta[2] == 0 && meta[9] == 0 && meta[10] == 0 && meta[11] == 0 && meta[12] == 0,
	      "invalid fields zero");
	CHECK(m17_gnss_decode(meta, &o) && o.alt == 0 && o.speed == 0 && o.bearing == 0 && o.radius == 0, "decoded 0");
	m17_gnss_to_fix(&o, &lat, &lon, &alt, &spd);
	CHECK(alt == INT32_MIN && spd == 0xFFFF, "none: altitude, speed");
	memset(meta, 0, 14);
	CHECK(!m17_gnss_decode(meta, &o), "nothing valid");
	// A bearing over 359 makes the velocity invalid
	memset(&g, 0, sizeof g);
	g.valid = M17_GNSS_VALID_VEL;
	g.bearing = 200;
	g.speed = 10;
	m17_gnss_encode(&g, meta);
	CHECK(m17_gnss_decode(meta, &o) && o.bearing == 200 && o.speed == 10, "bearing 200");
	meta[1] |= 1;	// 200 + 256 = 456
	CHECK(!m17_gnss_decode(meta, &o) && o.speed == 0 && o.bearing == 0, "bearing 456 refused");
	// Reserved bits 100-111 sent as 0 and ignored
	memset(&g, 0xFF, sizeof g);
	g.valid = 0xF;
	g.bearing = 359;
	g.speed = 4095;
	g.lat = 8388607;
	g.lon = -8388607;
	m17_gnss_encode(&g, meta);
	CHECK((meta[12] & 0x0F) == 0 && meta[13] == 0 && (meta[1] & 0x0E) == 0x0E, "reserved 0");
	meta[12] |= 0x0F;
	meta[13] = 0xFF;
	CHECK(m17_gnss_decode(meta, &o) && o.speed == 4095 && o.bearing == 359 && o.source == 15 && o.station == 15,
	      "reserved ignored");
	// From and to the tree's units: random fixes come back within a step (1.2 m, 2.4 m, 0.5 m, 0.5 km/h)
	for (unsigned rep = 0; rep < 20000; rep++) {
		int32_t la = (int32_t)(rng() % 1800000001u) - 900000000, lo = (int32_t)(rng() % 3600000001u) - 1800000000;
		int32_t am = (int32_t)rndn(33000) - 600;
		uint16_t sp = (uint16_t)rndn(2100), co = (uint16_t)rndn(400);
		int32_t la2, lo2, ad;
		uint16_t s10;

		if (rep == 0)
			la = 900000000, lo = -1800000000;
		m17_gnss_from_fix(&g, la, lo, am, true, sp, co, true, 1 + rndn(300), M17_STATION_HANDHELD);
		m17_gnss_encode(&g, meta);
		CHECK(m17_gnss_decode(meta, &o) && !memcmp(&o, &g, sizeof g), "fix round trip");
		CHECK(g.lat == (int32_t)lrint(la * 8388607.0 / 900000000.0), "latitude %d: %d", la, g.lat);
		CHECK(g.lon == (int32_t)lrint(lo * 8388607.0 / 1800000000.0), "longitude %d: %d", lo, g.lon);
		m17_gnss_to_fix(&o, &la2, &lo2, &ad, &s10);
		CHECK(labs((long)la2 - la) <= 54 && labs((long)lo2 - lo) <= 108, "position back %d %d -> %d %d", la, lo, la2,
		      lo2);
		CHECK(ad == (am < -500 ? -5000 : am > 32267 ? 322675 : am * 10), "altitude %d m: %d dm", am, ad);
		CHECK(s10 == (sp > 2047 ? 20475 : sp * 10), "speed %u: %u", sp, s10);
		CHECK(o.bearing == (co > 359 ? 0 : co), "bearing %u", co);
	}
	// Extended callsign: two addresses, then zeros
	memset(meta, 0, 14);
	m17_addr_put(meta, 0x00004B13D106ull);
	m17_addr_put(meta + 6, 0x1202BCCECAEDull);
	CHECK(m17_extcall_1(meta) == 0x00004B13D106ull && m17_extcall_2(meta) == 0x1202BCCECAEDull, "extended callsign");
	printf("  GNSS: layout, limits, radius, validity, units; extended callsign\n");
}

// --- Packet ---
static void test_packet(void)
{
	uint8_t o[4], data[M17_PKT_MAX], buf[M17_PKT_MAX], ch[26];
	uint8_t len;
	unsigned frames_seen[34] = { 0 };

	// The data type: UTF-8's form, every value of 21 bits
	for (uint32_t v = 0; v <= 0x1FFFFF; v++) {
		uint8_t n = m17_pkt_type_put(v, o), l;
		uint32_t back = m17_pkt_type_get(o, n, &l);

		if (n != (v < 0x80 ? 1 : v < 0x800 ? 2 : v < 0x10000 ? 3 : 4) || l != n || back != v) {
			CHECK(0, "type %X: %u bytes, back %X", v, n, back);
			break;
		}
	}
	CHECK(m17_pkt_type_put(0x200000, o) == 0, "past 21 bits");
	CHECK(m17_pkt_type_put(M17_PKT_SMS, o) == 1 && o[0] == 0x05, "SMS is 05");
	{
		static const uint8_t bad[][4] = { { 0xC0, 0x80 }, { 0xC1, 0xBF }, { 0xE0, 0x80, 0x80 }, { 0xE0, 0x9F, 0xBF },
						  { 0xF0, 0x80, 0x80, 0x80 }, { 0xF0, 0x8F, 0xBF, 0xBF }, { 0x80 },
						  { 0xBF }, { 0xF8, 0x88, 0x80, 0x80 }, { 0xFF }, { 0xC2, 0x00 },
						  { 0xE1, 0x80, 0xC0 } };
		static const uint8_t blen[] = { 2, 2, 3, 3, 4, 4, 1, 1, 4, 1, 2, 3 };

		for (unsigned k = 0; k < sizeof blen; k++)
			CHECK(m17_pkt_type_get(bad[k], blen[k], &len) == 0 && len == 0, "malformed type %u", k);
		CHECK(m17_pkt_type_get((const uint8_t *)"\xE2\x82", 2, &len) == 0 && len == 0, "cut short");
		CHECK(m17_pkt_type_get(o, 0, &len) == 0 && len == 0, "nothing");
		CHECK(m17_pkt_type_get((const uint8_t *)"\x05hi", 3, &len) == 5 && len == 1, "one byte of more");
	}
	// Every length 1..823: frames, byte 25, padding, and back through the receiver
	for (uint16_t n = 1; n <= M17_PKT_DATA_MAX; n++) {
		m17_pkt_tx_t tx;
		m17_pkt_rx_t rx;
		unsigned frames = 0, bytes = 0;
		uint8_t res = M17_PKT_MORE;

		fill(data, n);
		m17_pkt_tx_start(&tx, data, n);
		m17_pkt_rx_start(&rx, buf, sizeof buf);
		while (m17_pkt_tx_frame(&tx, ch)) {
			bool last = (ch[25] & M17_PKT_EOF) != 0;
			unsigned use = last ? M17_PKT_COUNT(ch[25]) : 25;

			CHECK((ch[25] & 3) == 0, "byte 25 bits 1-0");
			if (!last)
				CHECK(M17_PKT_COUNT(ch[25]) == frames, "length %u frame %u counter", n, frames);
			else
				CHECK(use >= 1 && use <= 25 && use == (unsigned)(n + 2 - 25 * frames), "length %u count %u", n, use);
			for (unsigned i = use; i < 25; i++)
				CHECK(ch[i] == 0, "padding");
			frames++;
			bytes += 25;
			CHECK(res == M17_PKT_MORE, "frame after the end");
			res = m17_pkt_rx_frame(&rx, ch);
			CHECK(res == (last ? M17_PKT_DONE : M17_PKT_MORE), "length %u frame %u: %u", n, frames, res);
		}
		CHECK(frames == m17_pkt_frames(n) && frames >= 1 && frames <= 33, "length %u: %u frames", n, frames);
		frames_seen[frames]++;
		CHECK(bytes >= 25 && bytes <= 825 && bytes % 25 == 0, "bytes %u", bytes);
		CHECK(rx.kept == n && !rx.cut && rx.len == n + 2 && !memcmp(buf, data, n), "length %u received", n);
		CHECK(!m17_pkt_tx_frame(&tx, ch), "all out");
		CHECK(m17_pkt_rx_frame(&rx, ch) == M17_PKT_BAD, "a frame after the end");
	}
	for (unsigned f = 1; f <= 33; f++)
		CHECK(frames_seen[f] > 0, "%u frames seen", f);
	CHECK(m17_pkt_frames(823) == 33 && m17_pkt_frames(1) == 1 && m17_pkt_frames(23) == 1 && m17_pkt_frames(24) == 2,
	      "frame counts");
	// Through the air with errors: every frame of a long packet
	{
		m17_pkt_tx_t tx;
		m17_pkt_rx_t rx;
		uint8_t f[48], co[26], res = 0;
		uint16_t at[3];
		unsigned frames = 0;

		fill(data, 823);
		m17_pkt_tx_start(&tx, data, 823);
		m17_pkt_rx_start(&rx, buf, sizeof buf);
		while (m17_pkt_tx_frame(&tx, ch)) {
			m17_frame_packet(ch, f);
			pick(at, 2, 0, 368);
			flip_bits(f, at, 2);
			CHECK(m17_frame_packet_get(f, NULL, &scratch, co) == 2 && !memcmp(co, ch, 26), "packet frame");
			res = m17_pkt_rx_frame(&rx, co);
			frames++;
		}
		CHECK(frames == 33 && res == M17_PKT_DONE && rx.kept == 823 && !memcmp(buf, data, 823), "823 bytes on air");
	}
	// Bigger than the buffer: the start kept, cut, the CRC still over all of it
	{
		m17_pkt_tx_t tx;
		m17_pkt_rx_t rx;
		uint8_t small[40], res = 0;

		fill(data, 823);
		m17_pkt_tx_start(&tx, data, 823);
		m17_pkt_rx_start(&rx, small, sizeof small);
		while (m17_pkt_tx_frame(&tx, ch))
			res = m17_pkt_rx_frame(&rx, ch);
		CHECK(res == M17_PKT_DONE && rx.cut && rx.kept == 40 && !memcmp(small, data, 40) && rx.len == 825,
		      "cut: %u kept", rx.kept);
		// A byte far past the buffer corrupted: the CRC fails
		m17_pkt_tx_start(&tx, data, 823);
		m17_pkt_rx_start(&rx, small, sizeof small);
		for (unsigned k = 0; m17_pkt_tx_frame(&tx, ch); k++) {
			if (k == 20)
				ch[7] ^= 1;
			res = m17_pkt_rx_frame(&rx, ch);
		}
		CHECK(res == M17_PKT_BAD, "CRC past the buffer");
		// Exactly the buffer: not cut; one more: cut, and kept stops before the CRC
		for (uint16_t n = 38; n <= 42; n++) {
			fill(data, n);
			m17_pkt_tx_start(&tx, data, n);
			m17_pkt_rx_start(&rx, small, sizeof small);
			while (m17_pkt_tx_frame(&tx, ch))
				res = m17_pkt_rx_frame(&rx, ch);
			CHECK(res == M17_PKT_DONE && rx.kept == (n < 40 ? n : 40) && rx.cut == (n > 40) &&
			      !memcmp(small, data, rx.kept), "length %u into 40", n);
		}
	}
	// The CRC's two bytes split over the last two frames (24 + 25 k data bytes), or alone in the last (23 + 25 k)
	{
		static const uint16_t lens[4] = { 24, 49, 23, 48 };

		for (unsigned k = 0; k < 4; k++) {
			m17_pkt_tx_t tx;
			m17_pkt_rx_t rx;
			uint8_t res = 0, last = 0;

			fill(data, lens[k]);
			m17_pkt_tx_start(&tx, data, lens[k]);
			m17_pkt_rx_start(&rx, buf, sizeof buf);
			while (m17_pkt_tx_frame(&tx, ch)) {
				last = ch[25];
				res = m17_pkt_rx_frame(&rx, ch);
			}
			CHECK(res == M17_PKT_DONE && rx.kept == lens[k] && M17_PKT_COUNT(last) == (k < 2 ? 1 : 25),
			      "length %u: last count %u", lens[k], M17_PKT_COUNT(last));
		}
	}
	// Receiver refusals: counter out of order, a missing frame, too many frames, byte counts 0 and 26-31, a CRC error
	{
		m17_pkt_rx_t rx;

		m17_pkt_rx_start(&rx, buf, sizeof buf);
		memset(ch, 0, 26);
		ch[25] = 1 << 2;
		CHECK(m17_pkt_rx_frame(&rx, ch) == M17_PKT_BAD, "first frame counter 1");
		CHECK(m17_pkt_rx_frame(&rx, ch) == M17_PKT_BAD, "after BAD: still BAD until restarted");
		m17_pkt_rx_start(&rx, buf, sizeof buf);
		ch[25] = 0;
		CHECK(m17_pkt_rx_frame(&rx, ch) == M17_PKT_MORE, "frame 0");
		ch[25] = 2 << 2;
		CHECK(m17_pkt_rx_frame(&rx, ch) == M17_PKT_BAD, "frame 1 missing");
		m17_pkt_rx_start(&rx, buf, sizeof buf);
		for (unsigned k = 0; k < 32; k++) {
			ch[25] = (uint8_t)(k << 2);
			CHECK(m17_pkt_rx_frame(&rx, ch) == M17_PKT_MORE, "frame %u", k);
		}
		ch[25] = 0;
		CHECK(m17_pkt_rx_frame(&rx, ch) == M17_PKT_BAD, "a 33rd frame without EOF");
		for (unsigned c = 0; c < 32; c++) {
			if (c >= 1 && c <= 25)
				continue;
			m17_pkt_rx_start(&rx, buf, sizeof buf);
			ch[25] = (uint8_t)(M17_PKT_EOF | c << 2);
			CHECK(m17_pkt_rx_frame(&rx, ch) == M17_PKT_BAD, "byte count %u", c);
		}
		// Too short to hold a type and the CRC: 1 or 2 bytes in all
		for (unsigned c = 1; c <= 2; c++) {
			uint16_t crc = m17_crc(ch, 0);

			m17_pkt_rx_start(&rx, buf, sizeof buf);
			memset(ch, 0, 26);
			ch[0] = (uint8_t)(crc >> 8);
			ch[1] = (uint8_t)crc;
			ch[25] = (uint8_t)(M17_PKT_EOF | c << 2);
			CHECK(m17_pkt_rx_frame(&rx, ch) == M17_PKT_BAD, "%u bytes in all", c);
		}
		// A one-byte packet (type only) is the shortest good one
		{
			m17_pkt_tx_t tx;

			data[0] = M17_PKT_RAW;
			m17_pkt_tx_start(&tx, data, 1);
			m17_pkt_rx_start(&rx, buf, sizeof buf);
			CHECK(m17_pkt_tx_frame(&tx, ch) && ch[25] == (M17_PKT_EOF | 3 << 2), "1 byte: count 3");
			CHECK(m17_pkt_rx_frame(&rx, ch) == M17_PKT_DONE && rx.kept == 1, "1 byte received");
			m17_pkt_tx_start(&tx, data, 1);
			m17_pkt_tx_frame(&tx, ch);
			ch[2] ^= 0x40;
			m17_pkt_rx_start(&rx, buf, sizeof buf);
			CHECK(m17_pkt_rx_frame(&rx, ch) == M17_PKT_BAD, "CRC error");
		}
	}
	// An SMS (type 5, text and its zero, the CRC over both)
	{
		static const char sms[] = "\x05Hello M17";	// and the terminating zero
		m17_pkt_tx_t tx;
		m17_pkt_rx_t rx;
		uint8_t res = 0;

		m17_pkt_tx_start(&tx, (const uint8_t *)sms, sizeof sms);
		m17_pkt_rx_start(&rx, buf, sizeof buf);
		while (m17_pkt_tx_frame(&tx, ch))
			res = m17_pkt_rx_frame(&rx, ch);
		CHECK(res == M17_PKT_DONE && rx.kept == sizeof sms && m17_pkt_type_get(buf, rx.kept, &len) == M17_PKT_SMS &&
		      len == 1 && !strcmp((const char *)buf + 1, "Hello M17"), "SMS");
	}
	// A data length past the limit is cut to 823
	{
		m17_pkt_tx_t tx;

		m17_pkt_tx_start(&tx, data, 900);
		CHECK(tx.len == M17_PKT_DATA_MAX, "length limit");
	}
	printf("  packet: data types, every length 1..823 (1 to 33 frames), on air, cut, refusals, SMS\n");
}

// --- BERT ---
static void test_bert(void)
{
	m17_prbs9_t g;
	m17_bert_rx_t r;
	uint8_t b[25], first[6] = { 0 };
	static const uint8_t want[6] = { 0x08, 0xC2, 0x72, 0xAC, 0x37, 0xA6 };
	uint16_t start;
	unsigned period = 0;

	m17_prbs9_init(&g);
	for (unsigned i = 0; i < 48; i++)
		first[i >> 3] |= (uint8_t)(m17_prbs9_bit(&g) << (7 - (i & 7)));
	CHECK(!memcmp(first, want, 6), "PRBS9 first bytes %02X %02X %02X %02X %02X %02X", first[0], first[1], first[2],
	      first[3], first[4], first[5]);
	m17_prbs9_init(&g);
	start = g.reg;
	do {
		m17_prbs9_bit(&g);
		period++;
	} while (g.reg != start && period < 1000);
	CHECK(period == 511, "PRBS9 period %u", period);
	// Frames continue the generator: 197 bits each, the rest 0
	{
		m17_prbs9_t a, c;

		m17_prbs9_init(&a);
		m17_prbs9_init(&c);
		for (unsigned k = 0; k < 5; k++) {
			m17_bert_next(&a, b);
			for (unsigned i = 0; i < 200; i++)
				CHECK(bget(b, i) == (i < 197 ? m17_prbs9_bit(&c) : 0), "BERT frame %u bit %u", k, i);
		}
	}
	// Clean frames: lock after 18 bits, every later bit counted, no error
	m17_prbs9_init(&g);
	m17_bert_rx_init(&r);
	for (unsigned k = 0; k < 10; k++) {
		m17_bert_next(&g, b);
		m17_bert_rx_frame(&r, b);
	}
	CHECK(r.state == M17_BERT_COUNT && r.locks == 1 && r.losses == 0 && r.frames == 10, "locked");
	CHECK(r.bits == 1970 - 18 && r.errs == 0, "counted %u bits, %u errors", r.bits, r.errs);
	// Errors counted one for one (spread out: below the loss threshold)
	{
		unsigned flipped = 0;

		for (unsigned k = 0; k < 50; k++) {
			unsigned at[3] = { rndn(65), 65 + rndn(66), 131 + rndn(66) };

			m17_bert_next(&g, b);
			for (unsigned e = 0; e < 3; e++) {
				b[at[e] >> 3] ^= (uint8_t)(0x80 >> (at[e] & 7));
				flipped++;
			}
			m17_bert_rx_frame(&r, b);
		}
		CHECK(r.state == M17_BERT_COUNT && r.losses == 0, "still locked");
		CHECK(r.errs == flipped && r.bits == 1970 - 18 + 50 * 197, "errors %u of %u", r.errs, flipped);
	}
	// Exactly: a known number of errors in one frame
	{
		uint32_t e0 = r.errs, b0 = r.bits;

		m17_bert_next(&g, b);
		for (unsigned i = 0; i < 197; i += 20)
			b[i >> 3] ^= (uint8_t)(0x80 >> (i & 7));
		m17_bert_rx_frame(&r, b);
		CHECK(r.errs - e0 == 10 && r.bits - b0 == 197, "10 errors counted: %u", r.errs - e0);
	}
	// Garbage: lock lost, nothing more counted until it locks again
	{
		uint32_t bits0;

		fill(b, 25);
		m17_bert_rx_frame(&r, b);
		CHECK(r.state == M17_BERT_SYNC && r.losses == 1, "lock lost");
		bits0 = r.bits;
		for (unsigned k = 0; k < 3; k++) {
			fill(b, 25);
			m17_bert_rx_frame(&r, b);
		}
		CHECK(r.bits == bits0 && r.state == M17_BERT_SYNC, "nothing counted while synchronizing");
		// The sender went on meanwhile: the receiver finds it again without a reset
		for (unsigned k = 0; k < 3; k++) {
			m17_bert_next(&g, b);
			m17_bert_rx_frame(&r, b);
		}
		CHECK(r.state == M17_BERT_COUNT && r.locks >= 2, "locked again: %u locks", r.locks);
	}
	// More than 18 errors in 128 bits loses lock; 18 do not
	{
		// From the lock: n errors in the first 128 counted bits
		for (unsigned n = 17; n <= 20; n++) {
			m17_prbs9_init(&g);
			m17_bert_rx_init(&r);
			m17_bert_next(&g, b);
			// bits 0..17 lock; 18..145 are the window; put n errors at 20, 26, ...
			for (unsigned i = 0; i < n; i++) {
				unsigned at = 20 + 6 * i;

				b[at >> 3] ^= (uint8_t)(0x80 >> (at & 7));
			}
			m17_bert_rx_frame(&r, b);
			// (after a loss the rest of the frame locks again)
			CHECK(r.losses == (n > 18) && r.locks == 1u + (n > 18), "%u errors in the window: losses %u", n,
			      r.losses);
		}
	}
	// The window slides (m17.md 9: more than 18 errors within any 128 bits). Errors are placed by the stream's bit
	// index g (frame g / 197, bit g % 197); a clean start locks at g = 17 and counts from g = 18
	{
		static const struct { unsigned first, every, burst, step, frames; } runs[] = {
			{ 200, 128, 18, 1, 30 },	// 18 in a row every 128 bits: no 128 bits hold more than 18
			{ 200, 127, 18, 1, 30 },	// ... every 127: some 128 bits hold 19
			{ 400, 0, 18, 7, 6 },		// 18 spread over 120 bits, once
		};

		for (unsigned k = 0; k < 3; k++) {
			unsigned flipped = 0;

			m17_prbs9_init(&g);
			m17_bert_rx_init(&r);
			for (unsigned f = 0; f < runs[k].frames; f++) {
				m17_bert_next(&g, b);
				for (unsigned i = 0; i < 197; i++) {
					unsigned gi = 197 * f + i, rel;

					if (gi < runs[k].first)
						continue;
					rel = runs[k].every ? (gi - runs[k].first) % runs[k].every : gi - runs[k].first;
					if (rel % runs[k].step == 0 && rel / runs[k].step < runs[k].burst) {
						b[i >> 3] ^= (uint8_t)(0x80 >> (i & 7));
						flipped++;
					}
				}
				m17_bert_rx_frame(&r, b);
			}
			if (k == 1)
				CHECK(r.losses >= 1, "18 errors every 127 bits: lock lost (%u)", r.losses);
			else
				CHECK(r.losses == 0 && r.locks == 1 && r.errs == flipped, "%u: 18 in 128 bits keep the lock", k);
		}
		// 19 errors in 37 bits, starting anywhere: lock lost exactly once, wherever 128-bit boundaries from the
		// lock fall (a block window misses a burst split across two blocks)
		for (unsigned s = 150; s < 700; s++) {
			m17_prbs9_init(&g);
			m17_bert_rx_init(&r);
			for (unsigned f = 0; f < 5; f++) {
				m17_bert_next(&g, b);
				for (unsigned i = 0; i < 197; i++) {
					unsigned gi = 197 * f + i;

					if (gi >= s && gi < s + 37 && (gi - s) % 2 == 0)
						b[i >> 3] ^= (uint8_t)(0x80 >> (i & 7));
				}
				m17_bert_rx_frame(&r, b);
			}
			CHECK(r.losses == 1 && r.locks == 2 && r.state == M17_BERT_COUNT, "19 errors from bit %u: %u losses",
			      s, r.losses);
		}
	}
	// Lock from any generator state (the receiver never reset with the sender)
	{
		unsigned locked = 0;

		for (unsigned rep = 0; rep < 100; rep++) {
			g.reg = (uint16_t)(1 + rndn(511));
			m17_bert_rx_init(&r);
			m17_bert_next(&g, b);
			m17_bert_rx_frame(&r, b);
			m17_bert_next(&g, b);
			m17_bert_rx_frame(&r, b);
			// Predictions are right from bit 9 on (the register then holds received bits only): the lock comes
			// 18 matches after the last wrong one, at bit 26 at the latest
			locked += r.state == M17_BERT_COUNT && r.errs == 0 && r.bits >= 2 * 197 - 27 && r.bits <= 2 * 197 - 18;
		}
		CHECK(locked == 100, "locked from any state: %u", locked);
	}
	// On air: BERT frames with noise into the receiver
	{
		uint8_t f[48], h[48], bo[25];
		int8_t soft[192];

		m17_prbs9_init(&g);
		m17_bert_rx_init(&r);
		for (unsigned k = 0; k < 100; k++) {
			m17_bert_next(&g, b);
			m17_frame_bert(b, f);
			channel(f, 16.0, h, soft);
			m17_frame_bert_get(h, soft, &scratch, bo);
			m17_bert_rx_frame(&r, bo);
		}
		printf("  BERT on air, sigma 16: %u bits, %u errors, %u locks\n", r.bits, r.errs, r.locks);
		CHECK(r.state == M17_BERT_COUNT && r.bits > 19000 && r.errs < r.bits / 1000, "BERT on air");
	}
	printf("  PRBS9 08 C2 72 AC 37 A6, period 511; BERT lock, count, loss\n");
}

// The host time of one stream frame's decode with soft values: the stream part and the LICH
static void test_timing(void)
{
	uint8_t lsf[30], f[48], h[48], lich[6], st[16];
	int8_t soft[192];
	uint16_t fn;
	struct timespec a, b;
	unsigned n = 20000;
	volatile int sink = 0;
	double us;

	fill(lsf, 30);
	m17_lich_make(lsf, 0, lich);
	fill(st, 16);
	m17_frame_stream(lich, 1, st, f);
	channel(f, 20.0, h, soft);
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (unsigned i = 0; i < n; i++) {
		sink += m17_frame_stream_get(h, soft, &scratch, &fn, st);
		sink += m17_frame_lich_get(h, soft, lich);
	}
	clock_gettime(CLOCK_MONOTONIC, &b);
	us = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 1e3 / n;
	printf("  host time: one stream frame (stream part and LICH, soft) %.2f us\n", us);
	(void)sink;
}

int main(void)
{
	patterns_make();
	test_symbols();
	test_randomizer_crc();
	test_sizes_puncture();
	test_golay_interleaver();
	test_type_lsf();
	test_frames_ref();
	test_round_trips();
	test_noise();
	test_text();
	test_gnss();
	test_packet();
	test_bert();
	test_timing();
	printf("m17Test: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
