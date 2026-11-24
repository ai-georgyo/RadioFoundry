// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 transmit (m17_engine.h): a stream, a packet or a BERT run, frame by frame into the session's queue. The radio
// core's task prepares (the LSF and the preamble, and while priming only those: a reception may still hold u); from
// the first data frame on everything runs in the provider's task. docs/spec/m17.md, the engine's section.
//
// A stream frame: the voice path's 16 STREAM bytes (or silence when it has none ready before the queue runs short),
// META changed at superframe boundaries (with encryption none: a new position at most every 5 s, else the next text
// block), encrypted, hashed when signed, the LICH chunk, the frame codec. The FN limit: an encrypted or signed stream
// ends by FN 7FFA (M17_FN_ENC_LAST) whatever the operator does; an unsigned clear one wraps at 7FFF.
#include "config.h"

#if CONFIG_M17

#include <string.h>

#include "m17_engine_int.h"
#include "../crash/crash_secret.h"

// GNSS META goes at least this many superframes after the one before (21 x 240 ms: at most once every 5 s, m17.md
// 4.2), counted from frame to frame (tx_gnss_wait), so the FN's wrap at 7FFF changes nothing; a new position waits
// at most that long after the radio core gave it
#define GNSS_SUPERFRAMES	21u

static uint16_t lead_syms(const m17_engine_t *e)
{
	const hal_air_t *air = e->session.air;

	return (uint16_t)(((uint32_t)air->tx_lead_ms * M17_SYM_RATE) / 1000u + air->tx_chunk_syms);
}

// The queue would run short unless something goes in now
static bool tx_short(const m17_engine_t *e)
{
	return air_session_tx_queued(&e->session) < lead_syms(e);
}

static uint16_t tx_type(const m17_engine_t *e)
{
	return m17_lsf_type(e->tx_lsf);
}

// The META of the next superframe, encryption none: a new position (GNSS_SUPERFRAMES apart), else the next block of
// the text, else none (text with control byte 00). Returns the META type
static uint8_t meta_next(m17_engine_t *e, uint8_t meta[M17_META_BYTES])
{
	const m17_engine_cfg_t *c = &e->cfg;
	uint8_t blocks, seq = e->gnss_seq;

	// gnss_seq odd: the radio core is in the middle of writing it (this task preempted it, m17_engine_gnss); moved
	// after the copy: written meanwhile. Either way the position goes at a later turn
	if (c->gnss && !(seq & 1u) && seq != e->gnss_done && e->tx_gnss_wait == 0u) {
		m17_gnss_t g;

		m17_barrier();
		g = e->gnss_in;
		m17_barrier();
		if (seq == e->gnss_seq) {
			e->gnss_done = seq;
			e->tx_gnss_wait = GNSS_SUPERFRAMES;
			m17_gnss_encode(&g, meta);
			return M17_META_GNSS;
		}
	}
	blocks = c->text && c->text_len ? m17_text_blocks(c->text, c->text_len) : 0;
	if (blocks) {
		m17_text_meta(c->text, c->text_len, e->tx_text_block, meta);
		e->tx_text_block = (uint8_t)((e->tx_text_block + 1u) % blocks);
		return M17_META_TEXT;
	}
	memset(meta, 0, M17_META_BYTES);
	return M17_META_TEXT;
}

bool m17_engine_tx_prepare(m17_engine_t *e, const m17_tx_req_t *req)
{
	const m17_engine_cfg_t *c = &e->cfg;
	uint8_t meta[M17_META_BYTES], enc = M17_ENC_NONE, sub = M17_META_TEXT;
	uint64_t dst = c->dst;
	uint16_t type = 0;

	if (!air_session_is_open(&e->session) || !(e->session.fit & AIR_FIT_TX) || !c->src || c->tx_inhibit)
		return false;
	if (air_session_tx_state(&e->session) == AIR_TX_PRIMING)
		return air_session_tx_prime(&e->session);	// prepared already: the queue had no room for it all
	if (air_session_tx_state(&e->session) != AIR_TX_IDLE)
		return false;
	memset(meta, 0, sizeof meta);
	switch (req->kind) {
	case M17_TX_STREAM:
		if (!c->silence)
			return false;
#if !CONFIG_M17_SIG
		if (c->sign)
			return false;
#endif
		switch (c->tx_enc) {
		case M17_ENC_NONE:
			break;
#if CONFIG_M17_SCRAMBLER
		case M17_ENC_SCRAMBLER: {
			bool ok;

			// Checked as it is: no register made from the seed on the radio task's stack (m17.md 16); its bytes
			// pass through a register, so secret work all the same
			crash_secret_enter();
			ok = m17_scr_key_ok(c->key, c->key_len);
			crash_secret_leave();
			if (!ok)
				return false;
			enc = M17_ENC_SCRAMBLER;
			sub = (uint8_t)(c->key_len - 1u);
			break;
		}
#endif
#if CONFIG_M17_AES
		case M17_ENC_AES:
			sub = m17_aes_sub(c->key_len);
			if (sub == 0xFFu)
				return false;
			enc = M17_ENC_AES;
			m17_aes_nonce(req->ts, req->rnd, meta);
			break;
#endif
		default:
			return false;	// never in the clear when the channel asks for what the build or key can't do
		}
		// A position given before this transmission is news to it: the LSF can carry it. gnss_done is the count
		// before the one in force (gnss_seq even here: this task writes it; 0 never given, 2 follows 0xFE)
		e->gnss_done = e->gnss_seq == 0u ? 0u : e->gnss_seq == 2u ? 0xFEu : (uint8_t)(e->gnss_seq - 2u);
		e->tx_gnss_wait = 0;
		e->tx_text_block = 0;
		if (enc == M17_ENC_NONE)
			sub = meta_next(e, meta);
		type = m17_type(true, c->voice == M17_DATA_VOICE_DATA ? M17_DATA_VOICE_DATA : M17_DATA_VOICE, enc, sub,
				c->tx_can, c->sign);
		break;
#if CONFIG_M17_PACKET
	case M17_TX_PACKET:
		if (!e->pkt_data || !e->pkt_len)
			return false;
		dst = e->pkt_dst;
		type = m17_type(false, 0, M17_ENC_NONE, M17_META_TEXT, c->tx_can, false);
		break;
#endif
#if CONFIG_M17_BERT
	case M17_TX_BERT:
		break;
#endif
	default:
		return false;
	}
	e->txr = *req;
	if (req->kind != M17_TX_BERT)
		m17_lsf_make(e->tx_lsf, dst, c->src, type, meta);
	e->tx_fn = 0;
	e->tx_lich_cnt = 0;
	e->tx_tail_left = 0;
	e->tx_stop = false;
	m17_preamble(req->kind == M17_TX_BERT, e->tx_out);
	e->tx_at = 0;
	e->tx_phase = M17_TXP_PREAMBLE;
	return air_session_tx_prime(&e->session);
}

bool m17_engine_tx_go(m17_engine_t *e)
{
	if (!air_session_tx_go(&e->session))
		return false;
	e->view.tx_calls++;
	return true;
}

void m17_engine_tx_stop(m17_engine_t *e)
{
	e->tx_stop = true;
}

void m17_engine_tx_abort(m17_engine_t *e)
{
	air_session_tx_abort(&e->session);
}

air_tx_state_t m17_engine_tx_state(const m17_engine_t *e)
{
	return air_session_tx_state(&e->session);
}

// A sent packet's data is the caller's again: the next packet is loaded afresh
void m17_engine_tx_clear(m17_engine_t *e)
{
	air_session_tx_clear(&e->session);
	if (air_session_tx_state(&e->session) != AIR_TX_IDLE)
		return;
	if (e->txr.kind == M17_TX_PACKET)
		e->pkt_data = NULL;
	e->tx_phase = M17_TXP_IDLE;
}

#if CONFIG_M17_PACKET
bool m17_engine_packet_load(m17_engine_t *e, uint64_t dst, const uint8_t *data, uint16_t len)
{
	if (!data || !len || len > M17_PKT_DATA_MAX || air_session_tx_state(&e->session) != AIR_TX_IDLE)
		return false;
	e->pkt_data = data;
	e->pkt_len = len;
	e->pkt_dst = dst;
	return true;
}
#endif

// --- The frames (provider's task) ---
static void phase(m17_engine_t *e, uint8_t p)
{
	e->tx_phase = p;
}

static void frame_eot(m17_engine_t *e)
{
	m17_eot(e->tx_out);
	phase(e, M17_TXP_EOT);
#if CONFIG_M17_SCRAMBLER
	if (e->txr.kind == M17_TX_STREAM)
		crypto_wipe(&e->u.stream.scr, sizeof e->u.stream.scr);
#endif
}

// The first data frame: u becomes the transmitter's (a reception in hand ends first)
static void data_start(m17_engine_t *e)
{
	m17_eng_rx_stop(e, false);
	m17_u_reset(e);
	switch (e->txr.kind) {
	case M17_TX_STREAM:
#if CONFIG_M17_SCRAMBLER
		if (M17_TYPE_ENC(tx_type(e)) == M17_ENC_SCRAMBLER) {
			crash_secret_enter();	// the seed through registers into the state
			m17_scr_init(&e->u.stream.scr, e->cfg.key, e->cfg.key_len);
			crash_secret_leave();
		}
#endif
#if CONFIG_M17_SIG
		m17_sig_tx_start(&e->u.stream.sig.tx);
		e->u.stream.rs_state = M17_RS_NONE;
#endif
		break;
#if CONFIG_M17_PACKET
	case M17_TX_PACKET:
		m17_pkt_tx_start(&e->u.pkt.tx, e->pkt_data, e->pkt_len);
		break;
#endif
#if CONFIG_M17_BERT
	case M17_TX_BERT:
		m17_prbs9_init(&e->u.bert.tx);
		break;
#endif
	default:
		break;
	}
	phase(e, M17_TXP_DATA);
}

// Secret work (crash_secret.h): the scrambler's state and AES's round keys go through registers and the stack here,
// 25 times a second, in the provider's task. A crash record made meanwhile keeps none of it; a plain frame keeps all
static void tx_encrypt(m17_engine_t *e, uint16_t fn, uint8_t stream[M17_STREAM_BYTES])
{
	switch (M17_TYPE_ENC(tx_type(e))) {
#if CONFIG_M17_SCRAMBLER
	case M17_ENC_SCRAMBLER:
		crash_secret_enter();
		m17_scr_frame(&e->u.stream.scr, stream);
		crash_secret_leave();
		break;
#endif
#if CONFIG_M17_AES
	case M17_ENC_AES:
		crash_secret_enter();
		aes_init(&e->work.aes, e->cfg.key, e->cfg.key_len);
		m17_aes_frame(&e->work.aes, e->tx_lsf + M17_LSF_META, fn, stream);
		aes_wipe(&e->work.aes);
		crash_secret_leave();
		break;
#endif
	default:
		(void)fn;
		(void)stream;
		break;
	}
}

// One stream frame into tx_out with the next LICH chunk
static void frame_stream(m17_engine_t *e, uint16_t fn, const uint8_t stream[M17_STREAM_BYTES])
{
	uint8_t lich[M17_LICH_BYTES];

	m17_lich_make(e->tx_lsf, e->tx_lich_cnt, lich);
	e->tx_lich_cnt = (uint8_t)((e->tx_lich_cnt + 1u) % M17_LICH_CHUNKS);
	m17_frame_stream(lich, fn, stream, e->tx_out);
}

// A superframe boundary: the LSF the LICH carries gets the next META (encryption none; the META type bits with it)
static void meta_turn(m17_engine_t *e)
{
	uint8_t meta[M17_META_BYTES], sub;
	uint16_t t = tx_type(e);

	if (e->tx_lich_cnt != 0 || e->tx_fn == 0 || M17_TYPE_ENC(t) != M17_ENC_NONE)
		return;
	if (e->tx_gnss_wait)
		e->tx_gnss_wait--;
	sub = meta_next(e, meta);
	t = (uint16_t)((t & ~(3u << 5)) | (uint16_t)sub << 5);
	m17_lsf_make(e->tx_lsf, m17_addr_get(e->tx_lsf + M17_LSF_DST), m17_addr_get(e->tx_lsf + M17_LSF_SRC), t, meta);
}

#if CONFIG_M17_SIG
// The operator let go of a signed stream (or the FN limit came): the tail is fixed now, so the hash can be final while
// it goes out. Its frames of silence are hashed ahead as they will be sent, encrypted, the scrambler put back after;
// then the authentication frame (signer: our address, the destination, the timestamp) closes the hash. H waits in rs
// for the radio core (m17_engine_sig_want), the signature comes back there (sig_give). The tail's last data frame is
// 7FFA at the latest
static void tail_fix(m17_engine_t *e)
{
	uint8_t st[M17_STREAM_BYTES], auth[M17_AUTH_BYTES];
	uint16_t n = e->cfg.sig_tail;
	bool secret = M17_TYPE_ENC(tx_type(e)) != M17_ENC_NONE;
#if CONFIG_M17_SCRAMBLER
	m17_scr_t keep;
#endif

	// Secret work while the scrambler's state is copied on this task's stack (and the tail encrypted ahead)
	if (secret)
		crash_secret_enter();
#if CONFIG_M17_SCRAMBLER
	keep = e->u.stream.scr;
#endif
	if (e->tx_fn + n > M17_FN_AUTH)
		n = (uint16_t)(M17_FN_AUTH - e->tx_fn);
	e->tx_tail_left = (uint8_t)n;
	// The authentication frame's timestamp is when it is made (m17.md 7.1): key-up's plus the frames sent before it,
	// 40 ms each. From here on txr.ts is that one (the AES nonce took key-up's into the LSF at the prepare); 0 stays
	// "no clock"
	if (e->txr.ts != 0)
		e->txr.ts += (uint32_t)(((uint32_t)e->tx_fn + n) * M17_FRAME_MS / 1000u);
	for (uint16_t i = 0; i < n; i++) {
		uint16_t fn = (uint16_t)(e->tx_fn + i);

		memcpy(st, e->cfg.silence, M17_STREAM_BYTES);
		tx_encrypt(e, fn, st);
		m17_sig_tx_frame(&e->u.stream.sig.tx, fn, st);
	}
#if CONFIG_M17_SCRAMBLER
	e->u.stream.scr = keep;
	crypto_wipe(&keep, sizeof keep);
#endif
	if (secret)
		crash_secret_leave();
	m17_auth_make(e->cfg.src, m17_addr_get(e->tx_lsf + M17_LSF_DST), e->txr.ts, auth);
	m17_sig_tx_auth(&e->u.stream.sig.tx, auth, e->u.stream.rs);
	m17_barrier();
	e->u.stream.rs_state = M17_RS_WANTED;
	m17_view_begin(e);
	e->view.sig_seq++;
	m17_view_end(e);
	phase(e, M17_TXP_TAIL);
}

// The authentication frame, then the four signature frames from r || s; zeros when the signature did not come before
// the queue would run short (receivers then report a failed check). False: waiting for it
static bool frame_sig(m17_engine_t *e)
{
	uint8_t st[M17_STREAM_BYTES];
	uint8_t i = e->tx_tail_left;

	if (e->tx_phase == M17_TXP_TAIL) {
		m17_auth_make(e->cfg.src, m17_addr_get(e->tx_lsf + M17_LSF_DST), e->txr.ts, st);
		frame_stream(e, M17_FN_AUTH, st);
		phase(e, M17_TXP_SIG);
		return true;
	}
	if (i == 0 && e->u.stream.rs_state != M17_RS_GIVEN) {
		if (e->u.stream.rs_state == M17_RS_WANTED && !tx_short(e))
			return false;
		e->u.stream.rs_state = M17_RS_LATE;
		e->stats.sig_late++;
	}
	if (e->u.stream.rs_state == M17_RS_GIVEN)
		memcpy(st, e->u.stream.rs + M17_STREAM_BYTES * i, M17_STREAM_BYTES);
	else
		memset(st, 0, sizeof st);
	frame_stream(e, m17_sig_fn(i), st);
	e->tx_tail_left = (uint8_t)(i + 1u);
	return true;
}
#endif

// The next stream frame into tx_out. False: nothing yet (the voice path has no frame and the queue can wait)
static bool next_stream(m17_engine_t *e)
{
	uint16_t t = tx_type(e), fn = e->tx_fn;
	bool limited = M17_TYPE_ENC(t) != M17_ENC_NONE || (t & M17_TYPE_SIGNED), last = false;
	uint8_t st[M17_STREAM_BYTES];

#if CONFIG_M17_SIG
	if (e->tx_phase == M17_TXP_SIG && e->tx_tail_left == 4u) {
		frame_eot(e);
		return true;
	}
	if (e->tx_phase == M17_TXP_SIG || (e->tx_phase == M17_TXP_TAIL && e->tx_tail_left == 0))
		return frame_sig(e);
#endif
	if (e->tx_phase == M17_TXP_DATA) {
		// The FN limit: a signed stream's tail must end by 7FFA, an encrypted one's last frame is 7FFA
		bool at_limit = limited &&
				((t & M17_TYPE_SIGNED) ? fn + e->cfg.sig_tail >= M17_FN_AUTH : fn >= M17_FN_ENC_LAST);

		if (e->tx_stop || at_limit) {
			// Counted once: the limit latches the stop, so a frame the encoder is still late with (false
			// below, called again) comes back here as stopped
			if (at_limit && !e->tx_stop) {
				e->stats.fn_limit++;
				e->tx_stop = true;
			}
#if CONFIG_M17_SIG
			if (t & M17_TYPE_SIGNED) {
				tail_fix(e);
				if (e->tx_tail_left == 0)
					return frame_sig(e);
			} else
#endif
			{
				last = true;
			}
		}
	}
	if (e->tx_phase == M17_TXP_DATA) {
		if (!e->hooks.tx_voice || !e->hooks.tx_voice(st, e->ctx)) {
			if (e->hooks.tx_voice && !tx_short(e))
				return false;	// the encoder's frame may still come in time
			if (e->hooks.tx_voice) {
				m17_view_begin(e);
				e->view.tx_filler++;
				m17_view_end(e);
			}
			memcpy(st, e->cfg.silence, M17_STREAM_BYTES);
		}
		meta_turn(e);	// (once the frame is sure to go: it moves the text on)
	} else {
		// The tail: silence, as hashed ahead
		memcpy(st, e->cfg.silence, M17_STREAM_BYTES);
		e->tx_tail_left--;
	}
	if (last)
		fn |= M17_FN_END;
	tx_encrypt(e, fn, st);
#if CONFIG_M17_SIG
	if ((t & M17_TYPE_SIGNED) && e->tx_phase == M17_TXP_DATA)
		m17_sig_tx_frame(&e->u.stream.sig.tx, fn, st);
#endif
	frame_stream(e, fn, st);
	crypto_wipe(st, sizeof st);
	e->tx_fn = (uint16_t)((e->tx_fn + 1u) & M17_FN_MASK);	// a clear unsigned stream wraps at 7FFF
	if (last)
		phase(e, M17_TXP_EOT);	// the EoT follows this frame (m17_tx_fill)
	return true;
}

static bool next_frame(m17_engine_t *e)
{
	switch (e->txr.kind) {
	case M17_TX_STREAM:
		return next_stream(e);
#if CONFIG_M17_PACKET
	case M17_TX_PACKET: {
		uint8_t chunk[M17_PACKET_FRAME_BYTES];

		// The packet goes out whole whenever the stop came
		if (!m17_pkt_tx_frame(&e->u.pkt.tx, chunk)) {
			e->stats.pkt_tx++;
			frame_eot(e);
		} else {
			m17_frame_packet(chunk, e->tx_out);
		}
		return true;
	}
#endif
#if CONFIG_M17_BERT
	case M17_TX_BERT: {
		uint8_t bits[M17_BERT_BYTES];

		if (e->tx_stop) {
			frame_eot(e);
		} else {
			m17_bert_next(&e->u.bert.tx, bits);
			m17_frame_bert(bits, e->tx_out);
		}
		return true;
	}
#endif
	default:
		frame_eot(e);
		return true;
	}
}

// The session wants symbols: tx_out's rest, then the next frame, while the queue has room. While priming (the radio
// core's task) only the preamble and the LSF go in
void m17_tx_fill(void *ctx)
{
	m17_engine_t *e = ctx;
	bool priming = air_session_tx_state(&e->session) == AIR_TX_PRIMING;

	for (;;) {
		if (e->tx_at < M17_FRAME_SYMS) {
			uint16_t n = (uint16_t)(M17_FRAME_SYMS - e->tx_at), space = air_session_tx_space(&e->session);

			if (n > space)
				n = space;
			if (!n || !air_session_tx(&e->session, e->tx_out, e->tx_at, n))
				return;
			e->tx_at = (uint16_t)(e->tx_at + n);
			continue;
		}
		switch (e->tx_phase) {
		case M17_TXP_PREAMBLE:
			if (e->txr.kind != M17_TX_BERT) {
				m17_frame_lsf(e->tx_lsf, e->tx_out);
				phase(e, M17_TXP_LSF);
				e->tx_at = 0;
				continue;
			}
			// BERT has no LSF: its first frame follows the preamble
			/* fall through */
		case M17_TXP_LSF:
			if (priming)
				return;
			data_start(e);
			break;
		case M17_TXP_EOT:
			// The EoT is out: the transmission ends. (No frame starts with the EoT's word: the last data
			// frame is still in tx_out when the phase came first)
			if (e->tx_out[0] == (uint8_t)(M17_EOT_WORD >> 8) && e->tx_out[1] == (uint8_t)M17_EOT_WORD) {
				air_session_tx_finish(&e->session);
				return;
			}
			frame_eot(e);
			e->tx_at = 0;
			continue;
		default:
			break;
		}
		if (!next_frame(e))
			return;
		e->tx_at = 0;
	}
}

#endif
