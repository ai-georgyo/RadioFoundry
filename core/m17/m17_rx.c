// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 receive (m17_engine.h): the air mode, its six sync patterns and the head hook. An 8-symbol sync also turns up
// in payload and noise, and the LSF and packet words (like stream and BERT) are two symbols apart, so a sync says
// little: the head hook decides what a frame is from the whole frame. At 24 symbols it looks for the end of
// transmission; at 192 it decodes the frame as the kind the transmission in hand expects (after an LSF: stream or
// packet by its TYPE; in a BERT run: BERT), and by the sync's kind only to start one or when that decode fails. The
// decode stays in the frame buffer for the call layer (m17_engine.c), which never decodes a frame twice.
// docs/spec/m17.md, the engine's section.
#include "config.h"

#if CONFIG_M17

#include <string.h>

#include "m17_engine_int.h"
#include "../air/air_syms.h"
#include "../fec/golay.h"
#include "../../hal/hal_radio.h"

// The end of transmission: 24 symbols of 555D within this many symbol errors. An 8-symbol sync is 4 symbols from
// 555D, so a frame whose sync held (1 error at most) is 3 or more off in its first 8 and needs nearly all of the 16
// random symbols after it to match; random symbols are within 4 of 24 about 3 times in a billion
#define EOT_ERRS_MAX	4u
// Corrected coded bits (the Viterbi count) a frame may have and still count as its kind. Random symbols decode with
// about 14 % of the sent bits corrected, and in 100,000 tries never under 26 of a stream's 272, 33 of a packet's 368,
// 37 of BERT's; a frame the decoder still gets right has up to about 40 (the frame codecs on random symbols and on
// frames with noise; tests/host/m17EngineTest.c then measures the frame loss end to end). A frame with its
// sync alone to show must stay well under random; one where the transmission in hand has its next frame due may come
// closer (a packet's CRC and BERT's count judge the rest)
#define LSF_ERRS_MAX		48	// and the CRC
#define STREAM_ERRS_MAX		24	// with the LICH's Golay words; while a stream runs, either one, or ...
#define STREAM_ERRS_LICH	32	// ... the LICH with this many (random symbols: 2 in 1,000; see CHASE_BITS), or ...
#define STREAM_ERRS_LOOSE	40	// ... the FN due (the one after the last, counting the frames lost since)
#define PKT_ERRS_MAX		48	// and byte 25 as the packet in hand allows
#define BERT_ERRS_START		30
#define BERT_ERRS_MAX		48
// Stream frames in a row handed over untrusted while a stream runs (the voice path plays them as lost): one lost in a
// fade costs 40 ms of speech, not the call
#define COAST_MAX	2u
// The LICH retry: the least reliable bits of a Golay word that failed, every pattern of flips among them. The
// extended Golay code's covering radius is 4: one flip brings any word within 3 of a code word, so the retry decodes
// every word, and only the counter (6, 7 are none) turns a LICH down. Random symbols: 8 % of LICHs hold on hard
// decisions, 76 % with the retry; with at most STREAM_ERRS_LICH corrected bits, 3 in 10,000 and 2 in 1,000
#define CHASE_BITS	4u
#define LOW_MID		(2 * AIR_SOFT_OUTER / 3)	// m17_frame.c's: the soft value of an outer / inner decision

static uint8_t kind_of(uint8_t sync_id)
{
	static const uint8_t k[] = { M17_K_LSF, M17_K_BERT, M17_K_STREAM, M17_K_PACKET, M17_K_LSF, M17_K_BERT };

	return sync_id < sizeof k ? k[sync_id] : M17_K_NONE;
}

// --- The LICH from soft values ---
// LICH coded bit x (0..95, type 3 places: the LICH is not punctured) as a soft value, positive for 1: m17_frame.c's
// reading of payload bit π(x), sign or outer, de-randomized
static int lich_soft(const int8_t *soft, uint16_t x)
{
	uint16_t p = m17_interleave(x);
	int v = soft[M17_SYNC_SYMS + (p >> 1)];

	v = (p & 1u) ? (v < 0 ? -v : v) - LOW_MID : -v;
	return ((m17_rand_seq[p >> 3] >> (7u - (p & 7u))) & 1u) ? -v : v;
}

// One Golay word from its soft values: as sliced, and when that is beyond the code, Chase's second algorithm: every
// pattern of flips among the CHASE_BITS least reliable bits is decoded and the code word nearest the soft values (the
// sizes of the bits it disagrees with, summed) is taken. The extended code corrects 3 and detects 4; a fourth or fifth
// error among the weak bits is then corrected too. Returns the bits corrected, -1 when no pattern decodes
static int golay_soft(const int8_t *soft, uint8_t w, uint16_t *data)
{
	int8_t mag[24];
	uint8_t low[CHASE_BITS];
	uint32_t hard = 0, best_cost = UINT32_MAX;
	int best = -1;

	for (uint8_t b = 0; b < 24; b++) {
		int v = lich_soft(soft, (uint16_t)(24u * w + b));

		hard = hard << 1 | (v > 0 ? 1u : 0u);
		mag[b] = (int8_t)(v < 0 ? (v < -127 ? 127 : -v) : (v > 127 ? 127 : v));
	}
	best = fec_golay24_decode(hard, data);
	if (best >= 0)
		return best;
	// The weakest bits, by selection (a few dozen compares)
	for (uint8_t i = 0; i < CHASE_BITS; i++) {
		uint8_t at = 0;
		int8_t m = 127;

		for (uint8_t b = 0; b < 24; b++) {
			bool taken = false;

			for (uint8_t j = 0; j < i; j++)
				taken |= low[j] == b;
			if (!taken && mag[b] <= m) {
				m = mag[b];
				at = b;
			}
		}
		low[i] = at;
	}
	for (uint8_t f = 1; f < 1u << CHASE_BITS; f++) {
		uint32_t cw = hard, diff, cost = 0;
		uint16_t d;

		for (uint8_t i = 0; i < CHASE_BITS; i++)
			if ((f >> i) & 1u)
				cw ^= 1ul << (23u - low[i]);
		if (fec_golay24_decode(cw, &d) < 0)
			continue;
		diff = fec_golay24_encode(d) ^ hard;
		for (uint8_t b = 0; b < 24; b++)
			if ((diff >> (23u - b)) & 1u)
				cost += (uint8_t)mag[b];
		if (cost < best_cost) {
			best_cost = cost;
			*data = d;
			best = 0;
			for (uint32_t x = diff; x; x &= x - 1u)
				best++;
		}
	}
	return best;
}

// The LICH: hard decisions first (m17_frame.c). With soft values and retry, a failure is decoded again word by word
// from the soft values (*hard false). Only while a transmission runs: the retry finds a code word near almost any 24
// bits, so a frame that only has a sync to show for itself must pass on hard decisions. noinline: its frame (the
// retry's bit sizes) would otherwise sit under the Viterbi decoder's, the provider task's deepest call
static __attribute__((noinline)) int lich_get(const air_frame_t *f, bool retry, uint8_t lich[M17_LICH_BYTES],
					       bool *hard)
{
	int n = m17_frame_lich_get(f->hard, f->soft, lich), fixed = 0;

	*hard = true;
	if (n >= 0 || !retry || !f->soft)
		return n;
	*hard = false;
	memset(lich, 0, M17_LICH_BYTES);
	for (uint8_t w = 0; w < 4; w++) {
		uint16_t d = 0;
		int k = golay_soft(f->soft, w, &d);

		if (k < 0)
			return -1;
		fixed += k;
		for (uint8_t b = 0; b < 12; b++)
			air_bit_put(lich, (uint16_t)(12u * w + b), (uint8_t)((d >> (11u - b)) & 1u));
	}
	return m17_lich_cnt(lich) < M17_LICH_CHUNKS ? fixed : -1;
}

// --- The frame as one kind ---
// Decode f as kind k into out (the frame buffer's layout, m17_engine_int.h). expected: the transmission in hand calls
// for this kind (looser: the frame is where one is due); else the frame has its sync alone to show and must pass in
// full. Returns the decoder's count, or -1 when the frame is not of this kind
static int decode_as(m17_engine_t *e, const air_frame_t *f, uint8_t k, bool expected, uint8_t out[M17_LSF_BYTES])
{
	int errs, lich;
	uint16_t fn;
	bool lich_hard;

	switch (k) {
	case M17_K_LSF:
		errs = m17_frame_lsf_get(f->hard, f->soft, &e->work.fec, out);
		return errs >= 0 && errs <= LSF_ERRS_MAX ? errs : -1;
	case M17_K_STREAM:
		lich = lich_get(f, expected, &out[M17_F_LICH], &lich_hard);
		errs = m17_frame_stream_get(f->hard, f->soft, &e->work.fec, &fn, &out[M17_F_STREAM]);
		out[M17_F_FN] = (uint8_t)(fn >> 8);
		out[M17_F_FN + 1] = (uint8_t)fn;
		out[M17_F_LICH_OK] = lich < 0 ? 0u : lich_hard ? 1u : 2u;
		if (!expected ? lich < 0 || errs > STREAM_ERRS_MAX :
		    errs > STREAM_ERRS_MAX && !(lich >= 0 && errs <= STREAM_ERRS_LICH) &&
		    !(errs <= STREAM_ERRS_LOOSE && (fn & M17_FN_MASK) == m17_fn_due(e)))
			return -1;
		return errs + (lich > 0 ? lich : 0);
#if CONFIG_M17_PACKET
	case M17_K_PACKET: {
		uint8_t b;

		// Without its LSF a packet frame is no use (no source, no type): only inside a packet transmission
		if (!expected)
			return -1;
		errs = m17_frame_packet_get(f->hard, f->soft, &e->work.fec, out);
		b = out[M17_PACKET_CHUNK];
		// Byte 25 as the packet in hand allows: the last frame's byte count, or the frame counter due
		if (errs > PKT_ERRS_MAX)
			return -1;
		if ((b & M17_PKT_EOF) ? M17_PKT_COUNT(b) == 0 || M17_PKT_COUNT(b) > M17_PACKET_CHUNK :
					M17_PKT_COUNT(b) != e->u.pkt.rx.next)
			return -1;
		return errs;
	}
#endif
#if CONFIG_M17_BERT
	case M17_K_BERT:
		errs = m17_frame_bert_get(f->hard, f->soft, &e->work.fec, out);
		return errs <= (expected ? BERT_ERRS_MAX : BERT_ERRS_START) ? errs : -1;
#endif
	default:
		return -1;
	}
}

// What the frame is (M17_K_NONE: not ours), with its decode in out and its count in *errs (-1: an untrusted stream
// frame). By rx_expect while a transmission runs, by the sync's kind to start one or when the expected decode fails
static uint8_t decode(m17_engine_t *e, const air_frame_t *f, uint8_t out[M17_LSF_BYTES], int *errs)
{
	uint8_t exp = e->rx_expect;
	uint8_t sk = (f->flags & AIR_FRAME_NO_SYNC) ? M17_K_NONE : kind_of(f->sync_id);

	if (exp != M17_K_NONE) {
		*errs = decode_as(e, f, exp, true, out);
		if (*errs >= 0) {
			e->rx_state = (uint8_t)(e->rx_state & ~M17_RXS_COAST);
			return exp;
		}
		if (sk != M17_K_NONE && sk != exp) {
			*errs = decode_as(e, f, sk, false, out);
			if (*errs >= 0) {
				e->stats.expect_miss++;
				return sk;
			}
		}
		// (not on a relock: that frame is where the framer moved to, not where the stream's next is due)
		if (exp == M17_K_STREAM && !m17_rx_relock(e) &&
		    (e->rx_state & M17_RXS_COAST) < COAST_MAX * M17_RXS_COAST_1) {
			// The frame where the stream's next one is due, beyond repair: handed over as lost
			e->rx_state = (uint8_t)(e->rx_state + M17_RXS_COAST_1);
			memset(out, 0, M17_F_LICH_OK + 1u);
			*errs = -1;
			return M17_K_STREAM;
		}
	} else if (sk != M17_K_NONE) {
		*errs = decode_as(e, f, sk, false, out);
		if (*errs >= 0)
			return sk;
	}
	if (sk == M17_K_LSF)
		e->stats.lsf_bad++;
	return M17_K_NONE;
}

// The soft values' low bits are the receiver's noise: a running mix of them (and of the frame times) for the radio
// core's entropy pool
static void noise_mix(m17_engine_t *e, const air_frame_t *f)
{
	uint32_t n = e->view.noise ^ f->t_us;

	if (f->soft)
		for (uint16_t i = 0; i < M17_FRAME_SYMS; i++)
			n = (n << 7 | n >> 25) ^ (uint8_t)f->soft[i];
	m17_view_begin(e);
	e->view.noise = n;
	m17_view_end(e);
}

static uint16_t rx_head(const air_frame_t *f, void *ctx)
{
	m17_engine_t *e = ctx;
	uint8_t tx = (uint8_t)air_session_tx_state(&e->session), out[M17_LSF_BYTES], k;
	int errs = -1;

	// Simplex: nothing comes in while we send (a provider that delivers anyway is not heard)
	if (tx == AIR_TX_ON || tx == AIR_TX_ENDING)
		return 0;
	if (f->nsyms < M17_FRAME_SYMS) {
		if (m17_eot_errs(f->hard) <= EOT_ERRS_MAX) {
			e->stats.eot++;
			// The framer lets go: its rx_lost ends the transmission, or here when it had just moved (no
			// rx_lost follows from a lock the head hook never agreed to)
			if (m17_rx_relock(e))
				m17_eng_rx_stop(e, true);
			return 0;
		}
		return AIR_HEAD_AGAIN | M17_FRAME_SYMS;
	}
	noise_mix(e, f);
	// A new lock with a reception still in hand and no word of a relock: the framer started over without losing a
	// signal (a retune), so what comes now is another transmission
	if (!e->session.fr.confirmed && m17_rx_now(e) != M17_K_NONE && !m17_rx_relock(e))
		m17_eng_rx_stop(e, true);
	k = decode(e, f, out, &errs);
	// After a relock: the frame goes on with the reception in hand, or that reception is over
	if (m17_rx_relock(e)) {
		if (k == m17_rx_now(e))
			e->rx_state = (uint8_t)(e->rx_state & ~M17_RXS_RELOCK);
		else
			m17_eng_rx_stop(e, true);
	}
	if (k == M17_K_NONE) {
		e->stats.head_rejects++;
		return 0;
	}
	// The symbols are read: the decode goes in their place
	memcpy(e->frame, out, sizeof out);
	e->rx_kind = k;
	e->cur_errs = (int8_t)(errs > 127 ? 127 : errs);
	return M17_FRAME_SYMS;
}

static void rx_frame(const air_frame_t *f, void *ctx)
{
	m17_eng_rx_frame(ctx, f);
}

static void rx_lost(void *ctx)
{
	m17_eng_rx_lost(ctx);
}

// m17.md 1.2: the preamble's last 16 symbols with the LSF or BERT sync (START_ONLY: a transmission starts there), then
// the four 8-symbol syncs; every frame 192 symbols with its sync first, so the grid rule holds (air-interface.md 6).
// The gate words are air_softmodem_sync_prep's (tests/host/airSoftmodemTest.c checks them). One symbol error at most:
// an inverted path turns each sync into another kind's, two symbols from the right one
const air_mode_t m17_air_mode = {
	.name = "M17",
	.profile = {
		.sym_rate = M17_SYM_RATE,
		.levels = 4,
		.shape = AIR_SHAPE_RRC,		// m17.md 1: root raised cosine 0.5
		.shape_param = 50,
		.dev_outer_hz = 2400,
		.timing = AIR_TIMING_CONTINUOUS,
		.bw = RADIO_BW_12K5,		// 9 kHz occupied
		.sync_every_syms = 0,		// no 24-symbol pattern recurs
		.sync_any_syms = M17_FRAME_SYMS,
		.idle = M17_PREAMBLE_LSF,	// +3 -3 ...
		.flags = AIR_PROFILE_SYNC_GRID,
		.verify = AIR_VERIFY_M17,
		.nsyncs = 6,
		.sync = {
			{ .id = M17_SID_PRE_LSF, .nsyms = 24, .max_errs = 1, .flags = AIR_SYNC_START_ONLY,
			  .frame_at = 16, .syms = { 0x77, 0x77, 0x77, 0x77, 0x55, 0xF7 },
			  .gate_up = 0xAA82, .gate_mask = 0xFF8B, .sign_val = 0xAAF2, .sign_mask = 0xFFFF },
			{ .id = M17_SID_PRE_BERT, .nsyms = 24, .max_errs = 1, .flags = AIR_SYNC_START_ONLY,
			  .frame_at = 16, .syms = { 0xDD, 0xDD, 0xDD, 0xDD, 0xDF, 0x55 },
			  .gate_up = 0x5548, .gate_mask = 0xFFE8, .sign_val = 0x554F, .sign_mask = 0xFFFF },
			{ .id = M17_SID_STREAM, .nsyms = 8, .max_errs = 1, .syms = { 0xFF, 0x5D },
			  .gate_up = 0x0009, .gate_mask = 0x000B, .sign_val = 0x000D, .sign_mask = 0x00FF },
			{ .id = M17_SID_PACKET, .nsyms = 8, .max_errs = 1, .syms = { 0x75, 0xFF },
			  .gate_up = 0x0020, .gate_mask = 0x0068, .sign_val = 0x00B0, .sign_mask = 0x00FF },
			{ .id = M17_SID_LSF, .nsyms = 8, .max_errs = 1, .syms = { 0x55, 0xF7 },
			  .gate_up = 0x0002, .gate_mask = 0x000B, .sign_val = 0x00F2, .sign_mask = 0x00FF },
			{ .id = M17_SID_BERT, .nsyms = 8, .max_errs = 1, .syms = { 0xDF, 0x55 },
			  .gate_up = 0x0048, .gate_mask = 0x0068, .sign_val = 0x004F, .sign_mask = 0x00FF },
		},
	},
	.frame_syms = M17_FRAME_SYMS,
	.head_syms = 24,			// the EoT's first three words
	.lost_frames = 4,			// 160 ms without a sync
	.head = rx_head,
	.rx_frame = rx_frame,
	.rx_lost = rx_lost,
};

#endif
