// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 on a channel (m17_engine.h): the session, the call layer's receive side, the radio core's calls. The air mode
// and its head hook are m17_rx.c's, the transmit side m17_tx.c's. docs/spec/m17.md, the engine's section.
//
// Receive, frame by frame in the provider's task, on what the head hook decoded into the frame buffer: an LSF starts
// a stream or a packet; a stream frame with nothing in hand starts a late one, rebuilt from the LICH. Each stream frame
// gives its LICH chunk (TYPE from chunk 2, DST and SRC once chunks 0-2 are in, the whole LSF and its META once a
// superframe's six are in and the CRC holds), its FN, the signature hash (before decryption: the hash is over what was
// sent), then decryption and the voice hook if the squelch passes. A packet's frames go into pkt_hold while it is
// free, BERT frames to the BERT receiver. A reception ends at the end bit, the EoT, a lost signal, the next one, or
// our own transmission.
#include "config.h"

#if CONFIG_M17

#include <stddef.h>
#include <string.h>

#include "m17_engine_int.h"
#include "../crash/crash_secret.h"

#if __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(m17_engine_t) <= M17_ENGINE_BUDGET, "the M17 engine grew past its budget (radio_m17_mem.h)");
#endif

#if CONFIG_M17_SIG
// rs_state says a received signed stream's check waits while u holds a packet's or a BERT run's state: those must
// never reach it (m17_engine_sig_take reads it from the radio core's task whatever is being received)
#define RS_AT	(offsetof(m17_engine_t, u.stream.rs_state) - offsetof(m17_engine_t, u))
#if CONFIG_M17_PACKET
_Static_assert(RS_AT >= sizeof(((m17_engine_t *)0)->u.pkt), "a packet's state reaches rs_state");
#endif
#if CONFIG_M17_BERT
_Static_assert(RS_AT >= sizeof(((m17_engine_t *)0)->u.bert), "a BERT run's state reaches rs_state");
#endif
#endif

#define WHY_WAIT	0xFFu	// the squelch can't decide yet (no TYPE, no DST, an AES nonce not in)

static const air_session_ops_t ops = { m17_tx_fill, NULL };

uint32_t m17_engine_fit(const hal_air_t *air)
{
	return air_session_fit(air, &m17_air_mode);
}

static void view_rx(m17_engine_t *e)
{
	m17_view_begin(e);
	e->view.rx = e->call;
	m17_view_end(e);
}

// --- The squelch ---
static bool key_scr_ok(const m17_engine_cfg_t *g, uint8_t sub)
{
#if CONFIG_M17_SCRAMBLER
	bool ok;

	// (an all-zero seed makes no key stream: refused there). The seed's bytes pass through a register: secret work
	crash_secret_enter();
	ok = m17_scr_key_ok(g->key, g->key_len);
	crash_secret_leave();
	return ok && sub == g->key_len - 1u;
#else
	(void)g;
	(void)sub;
	return false;
#endif
}

static bool key_aes_ok(const m17_engine_cfg_t *g, uint8_t sub)
{
#if CONFIG_M17_AES
	return m17_aes_sub(g->key_len) == sub;
#else
	(void)g;
	(void)sub;
	return false;
#endif
}

// Why the transmission in hand does not play: M17_WHY_NONE when it does, WHY_WAIT while it can't be told. In order:
// the CAN (squelch CAN and addressed), the address (addressed: DST is us, broadcast, or what we call), a key for its
// encryption, a voice data type (streams)
static uint8_t why_not(const m17_engine_t *e)
{
	const m17_engine_cfg_t *g = &e->cfg;
	const m17_call_t *c = &e->call;
	uint16_t t = c->type;

	if (!(c->flags & M17_CALL_TYPE))
		return WHY_WAIT;
	if (g->squelch != M17_SQUELCH_ANY) {
		if (g->rx_can != M17_CAN_ANY && M17_TYPE_CAN(t) != g->rx_can)
			return M17_WHY_CAN;
		if (g->squelch == M17_SQUELCH_ADDRESSED) {
			if (!(c->flags & M17_CALL_LSF) && !c->src)
				return WHY_WAIT;
			if (c->dst != M17_ADDR_BROADCAST && c->dst != g->src && (!g->dst || c->dst != g->dst))
				return M17_WHY_ADDRESSED;
		}
	}
	if (c->flags & M17_CALL_PACKET)
		return M17_WHY_NONE;
	switch (M17_TYPE_ENC(t)) {
	case M17_ENC_NONE:
		break;
	case M17_ENC_SCRAMBLER:
		if (!key_scr_ok(g, (uint8_t)M17_TYPE_SUB(t)))
			return M17_WHY_SCR;
		break;
	case M17_ENC_AES:
		if (!key_aes_ok(g, (uint8_t)M17_TYPE_SUB(t)))
			return M17_WHY_ENC;
		if (!(c->flags & M17_CALL_LSF))
			return WHY_WAIT;	// the nonce is the LSF's META: a late joiner waits for the whole LSF
		break;
	default:
		return M17_WHY_ENC;
	}
	if (M17_TYPE_DATA(t) != M17_DATA_VOICE && M17_TYPE_DATA(t) != M17_DATA_VOICE_DATA)
		return M17_WHY_DATA;
	return M17_WHY_NONE;
}

// The squelch again, after anything it reads changed: the view, the counters (once for each new reason), and for a
// stream the hooks (start when it begins to play, update while it plays, end when it stops)
static void rx_judge(m17_engine_t *e)
{
	uint8_t why = why_not(e), shown = why == WHY_WAIT ? M17_WHY_NONE : why;
	bool play = why == M17_WHY_NONE, was = e->view.rx_played;

	if (shown != M17_WHY_NONE && shown != e->view.rx_why) {
		if (shown == M17_WHY_CAN || shown == M17_WHY_ADDRESSED)
			e->stats.can_rejects++;
		else if (shown == M17_WHY_ENC || shown == M17_WHY_SCR)
			e->stats.enc_skipped++;
	}
	m17_view_begin(e);
	e->view.rx = e->call;
	e->view.rx_why = shown;
	e->view.rx_played = play;
	m17_view_end(e);
	if (m17_rx_now(e) != M17_K_STREAM)
		return;
	if (play && !was) {
		if (e->hooks.rx_start)
			e->hooks.rx_start(&e->call, e->ctx);
	} else if (play) {
		if (e->hooks.rx_update)
			e->hooks.rx_update(&e->call, e->ctx);
	} else if (was) {
		if (e->hooks.rx_end)
			e->hooks.rx_end(e->ctx);
	}
}

// --- A reception's start and end ---
#if CONFIG_M17_SIG
// A signed stream ended: what the radio core verifies waits in u.stream until it takes it. A stream that stopped
// before its last signature frame is still PENDING in m17_sig.c's eyes (it can't see the end): incomplete here
static void sig_conclude(m17_engine_t *e)
{
	m17_sig_rx_t *r = &e->u.stream.sig.rx;

	if (!(e->call.flags & M17_CALL_TYPE) || !(e->call.type & M17_TYPE_SIGNED))
		return;
	if (r->state == M17_SIG_PENDING && !m17_sig_rx_ready(r))
		r->state = M17_SIG_INCOMPLETE;
	e->call.sig = r->state;
	m17_barrier();
	e->u.stream.rs_state = M17_RS_CHECK;
	m17_view_begin(e);
	e->view.sig_seq++;
	m17_view_end(e);
}
#endif

// The reception in hand is over (whatever ended it)
static void rx_finish(m17_engine_t *e)
{
	uint8_t k = m17_rx_now(e), flags = 0;
	bool played = e->view.rx_played;

	if (k == M17_K_NONE)
		return;
	if (k == M17_K_STREAM) {
#if CONFIG_M17_SIG
		sig_conclude(e);
#endif
#if CONFIG_M17_SCRAMBLER
		crypto_wipe(&e->u.stream.scr, sizeof e->u.stream.scr);
#endif
		if (played && e->hooks.rx_end)
			e->hooks.rx_end(e->ctx);
	}
#if CONFIG_M17_PACKET
	if (k == M17_K_PACKET && !(e->rx_state & M17_RXS_PKT_END))
		e->stats.pkt_bad++;	// its frames stopped before the last
#endif
	if (k == M17_K_BERT)
		flags = M17_RXS_BERT_KEPT;
	e->rx_state = flags;
	e->rx_expect = M17_K_NONE;
	m17_view_begin(e);
	e->view.rx = e->call;
	e->view.rx_active = false;
	e->view.rx_played = false;
	e->view.rx_why = M17_WHY_NONE;
	m17_view_end(e);
}

// A new reception of kind k: the one in hand ends, u starts over
static void rx_begin(m17_engine_t *e, uint8_t k)
{
	bool bert_kept = m17_rx_now(e) == M17_K_NONE && (e->rx_state & M17_RXS_BERT_KEPT);
	int16_t rssi = e->call.rssi_dbm;

	rx_finish(e);
	// (a signed stream's check not taken by now is lost with u: the radio core takes it within a tick or two)
	if (!(k == M17_K_BERT && bert_kept))
		m17_u_reset(e);
	memset(&e->call, 0, sizeof e->call);
	e->call.rssi_dbm = rssi;
	memset(e->lich_lsf, 0, sizeof e->lich_lsf);
	memset(e->rx_meta, 0, sizeof e->rx_meta);
	e->lich_have = 0;
	e->rx_state = k;
	e->rx_expect = k;
	m17_view_begin(e);
	m17_text_clear(&e->text);
	e->view.rx = e->call;
	e->view.rx_active = true;
	e->view.rx_played = false;
	e->view.rx_why = M17_WHY_NONE;
	e->view.rx_calls++;
	m17_view_end(e);
}

// The framer still locked: it is moving to another sync near this one's place, or a new transmission's start
// (air_framer.c), and the head hook sees that frame next. The reception waits for it (M17_RXS_RELOCK, m17_rx.c)
void m17_eng_rx_lost(m17_engine_t *e)
{
	if (m17_rx_now(e) != M17_K_NONE && air_framer_locked(&e->session.fr)) {
		e->rx_state |= M17_RXS_RELOCK;
		return;
	}
	rx_finish(e);
}

// Whatever is coming in ends: our transmission starts (provider's task, before u is the transmitter's: nothing is
// kept), or the head hook says so (a retune, a relock that came to nothing)
void m17_eng_rx_stop(m17_engine_t *e, bool keep_bert)
{
	rx_finish(e);
	if (!keep_bert)
		e->rx_state = 0;
}

// --- META (m17.md 4) ---
// A GNSS META into the call's position. A field whose validity bit is 0 is ignored (m17.md 4.2): one with a position
// is the fix now, whole; one without (altitude, velocity or radius alone) only adds its fields to a position already
// in, and is no position itself. So M17_CALL_GNSS means a position came, and a real one is never overwritten by zeros
static void gnss_take(m17_call_t *c, const m17_gnss_t *g)
{
	m17_gnss_t *f = &c->gnss;

	if (g->valid & M17_GNSS_VALID_POS) {
		*f = *g;
		c->flags |= M17_CALL_GNSS;
		return;
	}
	if (!(c->flags & M17_CALL_GNSS))
		return;
	f->source = g->source;
	f->station = g->station;
	if (g->valid & M17_GNSS_VALID_ALT)
		f->alt = g->alt;
	if (g->valid & M17_GNSS_VALID_VEL) {
		f->bearing = g->bearing;
		f->speed = g->speed;
	}
	if (g->valid & M17_GNSS_VALID_RADIUS)
		f->radius = g->radius;
	f->valid |= g->valid;
}

static void meta_take(m17_engine_t *e, uint16_t type, const uint8_t meta[M17_META_BYTES])
{
	m17_gnss_t g;
	uint64_t a;

	if (M17_TYPE_ENC(type) != M17_ENC_NONE)
		return;		// AES: the nonce; scrambler: not defined
	switch (M17_TYPE_SUB(type)) {
	case M17_META_TEXT:
		m17_view_begin(e);
		if (m17_text_add(&e->text, meta)) {
			e->call.flags |= M17_CALL_TEXT;
			e->view.text_seq++;
		}
		m17_view_end(e);
		break;
	case M17_META_GNSS:
		if (m17_gnss_decode(meta, &g))
			gnss_take(&e->call, &g);
		break;
	case M17_META_EXTCALL:
		a = m17_extcall_1(meta);
		if (a != M17_ADDR_RESERVED) {
			e->call.ext1 = a;
			e->call.ext2 = m17_extcall_2(meta);
			e->call.flags |= M17_CALL_EXTCALL;
		}
		break;
	default:
		break;
	}
}

// --- Packets ---
#if CONFIG_M17_PACKET
// The packet starts: kept in pkt_hold when the squelch passes and the radio core took the one before; else only its
// CRC is checked (cap 0), so a whole one can be counted as dropped
static void pkt_start(m17_engine_t *e)
{
	bool keep = e->view.rx_played && !e->pkt_hold.full;

	m17_pkt_rx_start(&e->u.pkt.rx, keep ? e->pkt_hold.buf : NULL, keep ? M17_ENGINE_PKT_KEEP : 0);
}

static void rx_packet(m17_engine_t *e)
{
	m17_pkt_rx_t *r = &e->u.pkt.rx;
	uint8_t res, tl;

	if (m17_rx_now(e) != M17_K_PACKET)
		return;
	e->stats.packet++;
	res = m17_pkt_rx_frame(r, e->frame);
	if (res == M17_PKT_MORE)
		return;
	e->rx_state |= M17_RXS_PKT_END;
	if (res != M17_PKT_DONE) {
		e->stats.pkt_bad++;
	} else if (!e->view.rx_played) {
		// squelched (counted as such when it started)
	} else if (!r->cap) {
		e->stats.pkt_dropped++;		// the one before still waits
	} else {
		m17_packet_info_t *i = &e->pkt_hold.info;
		uint32_t type = m17_pkt_type_get(r->buf, r->kept, &tl);

		if (tl == 0) {
			e->stats.pkt_bad++;	// a data type no sender writes
			rx_finish(e);
			return;
		}

		i->src = e->call.src;
		i->dst = e->call.dst;
		i->type = type;
		i->len = (uint16_t)(r->kept - tl);
		i->cut = r->cut;
		memmove(e->pkt_hold.buf, e->pkt_hold.buf + tl, i->len);
		m17_barrier();
		e->pkt_hold.full = 1;
		e->stats.pkt_rx++;
		m17_view_begin(e);
		e->view.pkt_seq++;
		m17_view_end(e);
	}
	rx_finish(e);
}
#endif

// --- Streams ---
// An LSF: a stream or a packet starts (a new transmission, whatever was in hand)
static void rx_lsf(m17_engine_t *e)
{
	const uint8_t *lsf = e->frame;
	uint16_t t = m17_lsf_type(lsf);
	bool stream = (t & M17_TYPE_STREAM) != 0;

	e->stats.lsf_ok++;
	rx_begin(e, stream ? M17_K_STREAM : M17_K_PACKET);
	e->call.dst = m17_addr_get(lsf + M17_LSF_DST);
	e->call.src = m17_addr_get(lsf + M17_LSF_SRC);
	e->call.type = t;
	e->call.flags = (uint8_t)(M17_CALL_TYPE | M17_CALL_LSF | (stream ? 0u : M17_CALL_PACKET));
	if (stream)
		e->rx_state |= M17_RXS_FN0;	// (call.fn is 0 now, which the view shows: the first frame's is 0 too)
	e->call.errs = (uint8_t)e->cur_errs;
	memcpy(e->lich_lsf, lsf, M17_LSF_BYTES);
	memcpy(e->rx_meta, lsf + M17_LSF_META, M17_META_BYTES);
	meta_take(e, t, e->rx_meta);
#if CONFIG_M17_SIG
	if (stream && (t & M17_TYPE_SIGNED)) {
		m17_sig_rx_start(&e->u.stream.sig.rx, 0);
		e->call.sig = M17_SIG_PENDING;
	}
#endif
	rx_judge(e);
#if CONFIG_M17_PACKET
	if (!stream)
		pkt_start(e);
#endif
}

// A late joiner learned TYPE (from chunk 2, or from the whole LSF when chunk 2 held only on the soft retry): a signed
// stream's check starts here, LATE unless this is frame 0
static void type_learned(m17_engine_t *e, uint16_t fn)
{
#if CONFIG_M17_SIG
	if (e->call.type & M17_TYPE_SIGNED) {
		m17_sig_rx_start(&e->u.stream.sig.rx, fn);
		e->call.sig = e->u.stream.sig.rx.state;
	}
#else
	(void)e;
	(void)fn;
#endif
}

// A stream frame's LICH chunk (m17.md 5.1, 5.3): TYPE from chunk 2 (hard: its Golay words held on hard decisions),
// DST and SRC once chunks 0-2 are in, and at a
// superframe's end, with all six in and the CRC holding, the LSF itself: its META (which may change there), and for a
// late joiner everything. Chunks 2-5 are collected again each superframe (META and CRC change together); 0 and 1 hold
static void lich_take(m17_engine_t *e, const uint8_t *lich, bool hard, uint16_t fn)
{
	uint8_t cnt = m17_lich_cnt(lich);
	bool changed = false;

	memcpy(e->lich_lsf + 5u * cnt, lich, 5);
	e->lich_have |= (uint8_t)(1u << cnt);
	// TYPE decides the decryption and the squelch: from a chunk the soft retry found only once the CRC holds
	if (cnt == 2 && hard && !(e->call.flags & M17_CALL_TYPE)) {
		e->call.type = m17_lsf_type(e->lich_lsf);
		e->call.flags |= M17_CALL_TYPE;
		changed = true;
		type_learned(e, fn);
	}
	if ((e->lich_have & 0x07u) == 0x07u && !(e->call.flags & M17_CALL_LSF) && !e->call.src) {
		e->call.dst = m17_addr_get(e->lich_lsf + M17_LSF_DST);
		e->call.src = m17_addr_get(e->lich_lsf + M17_LSF_SRC);
		changed = true;
	}
	if (cnt == M17_LICH_CHUNKS - 1u && e->lich_have == 0x3Fu) {
		if (m17_lsf_crc_ok(e->lich_lsf)) {
			bool had_type = (e->call.flags & M17_CALL_TYPE) != 0;

			e->call.dst = m17_addr_get(e->lich_lsf + M17_LSF_DST);
			e->call.src = m17_addr_get(e->lich_lsf + M17_LSF_SRC);
			e->call.type = m17_lsf_type(e->lich_lsf);	// the META type may change at a boundary
			e->call.flags |= M17_CALL_TYPE | M17_CALL_LSF;
			if (!had_type)
				type_learned(e, fn);
			memcpy(e->rx_meta, e->lich_lsf + M17_LSF_META, M17_META_BYTES);
			meta_take(e, e->call.type, e->rx_meta);
			changed = true;
		}
		e->lich_have &= 0x03u;
	}
	if (changed)
		rx_judge(e);
}

// Decrypt a frame (m17.md 6) with the channel's key. The scrambler runs on from frame to frame; after a gap in the FNs,
// a late join or a new key it seeks to the frame's place from the seed (its matrices in the work union). Secret work
// (crash_secret.h), as m17_tx.c's encryption: a crash record made meanwhile keeps no register or stack word
static void rx_decrypt(m17_engine_t *e, uint16_t fn, uint8_t stream[M17_STREAM_BYTES])
{
	switch (M17_TYPE_ENC(e->call.type)) {
#if CONFIG_M17_SCRAMBLER
	case M17_ENC_SCRAMBLER:
		crash_secret_enter();
		if (!(e->rx_state & M17_RXS_SCR))
			m17_scr_seek(&e->u.stream.scr, e->cfg.key, e->cfg.key_len, fn, &e->work.scr);
		m17_scr_frame(&e->u.stream.scr, stream);
		crash_secret_leave();
		e->rx_state |= M17_RXS_SCR;
		break;
#endif
#if CONFIG_M17_AES
	case M17_ENC_AES:
		crash_secret_enter();
		aes_init(&e->work.aes, e->cfg.key, e->cfg.key_len);
		m17_aes_frame(&e->work.aes, e->rx_meta, fn, stream);
		aes_wipe(&e->work.aes);
		crash_secret_leave();
		break;
#endif
	default:
		break;
	}
}

static void rx_stream(m17_engine_t *e)
{
	uint8_t *fr = e->frame, *stream = fr + M17_F_STREAM;
	uint16_t fn = (uint16_t)(fr[M17_F_FN] << 8 | fr[M17_F_FN + 1]);
	bool trusted = e->cur_errs >= 0, tail = false;

	if (m17_rx_now(e) != M17_K_STREAM) {
		rx_begin(e, M17_K_STREAM);
		e->call.flags |= M17_CALL_LATE;
	}
	e->stats.stream++;
	if (!trusted) {
		// Lost in a fade: its place in the stream is all that is known. The scrambler seeks at the next good
		// one
		e->stats.lich_bad++;
		e->rx_state = (uint8_t)(e->rx_state & ~M17_RXS_SCR);
		if (e->view.rx_played && e->hooks.rx_voice)
			e->hooks.rx_voice(stream, M17_ERRS_BAD, e->ctx);
		return;
	}
	if ((fn & M17_FN_MASK) != m17_fn_due(e))
		e->rx_state = (uint8_t)(e->rx_state & ~M17_RXS_SCR);	// not the frame due: seek
	e->rx_state = (uint8_t)(e->rx_state & ~M17_RXS_FN0);
	e->call.fn = fn;
	e->call.errs = (uint8_t)e->cur_errs;
	if (fr[M17_F_LICH_OK]) {
		e->stats.lich_ok++;
		lich_take(e, fr + M17_F_LICH, fr[M17_F_LICH_OK] == 1u, fn);
	} else {
		e->stats.lich_bad++;
	}
#if CONFIG_M17_SIG
	// The hash is over the frame as sent: before decryption
	if ((e->call.flags & M17_CALL_TYPE) && (e->call.type & M17_TYPE_SIGNED))
		tail = m17_sig_rx_frame(&e->u.stream.sig.rx, fn, stream) == M17_SIG_FRAME_TAIL;
#endif
	if (!tail && e->view.rx_played) {
		rx_decrypt(e, fn, stream);
		if (e->hooks.rx_voice)
			e->hooks.rx_voice(stream, (uint8_t)e->cur_errs, e->ctx);	// 0..127 here
		crypto_wipe(stream, M17_STREAM_BYTES);
	}
	view_rx(e);
	if (fn & M17_FN_END)
		rx_finish(e);
}

// --- BERT ---
#if CONFIG_M17_BERT
static void rx_bert(m17_engine_t *e, const air_frame_t *f)
{
	m17_bert_rx_t *r = &e->u.bert.rx;
	// A run picked up again after a loss counts on, unless this frame opens a new run (its preamble)
	bool resume = m17_rx_now(e) == M17_K_NONE && (e->rx_state & M17_RXS_BERT_KEPT) &&
		      f->sync_id != M17_SID_PRE_BERT;

	if (m17_rx_now(e) != M17_K_BERT) {
		if (!resume)
			e->rx_state = 0;
		rx_begin(e, M17_K_BERT);
		if (!resume)
			m17_bert_rx_init(r);
	}
	e->stats.bert++;
	m17_bert_rx_frame(r, e->frame);
	m17_view_begin(e);
	e->view.bert_frames = r->frames;
	e->view.bert_bits = r->bits;
	e->view.bert_errs = r->errs;
	e->view.bert_locks = r->locks;
	e->view.bert_losses = r->losses;
	e->view.bert_state = r->state;
	m17_view_end(e);
}
#endif

void m17_eng_rx_frame(m17_engine_t *e, const air_frame_t *f)
{
	uint8_t now = m17_rx_now(e);

	e->stats.frames++;
	if (e->cur_errs > 0)
		e->stats.fec_bits += (uint32_t)e->cur_errs;
	e->call.rssi_dbm = f->rssi_dbm;
	if (e->recheck) {
		// m17_engine_set: the squelch and the keys again (a new scrambler key seeks from the seed)
		e->recheck = false;
		e->rx_state = (uint8_t)(e->rx_state & ~M17_RXS_SCR);
		if (now == M17_K_STREAM || now == M17_K_PACKET)
			rx_judge(e);
	}
	switch (e->rx_kind) {
	case M17_K_LSF:
		rx_lsf(e);
		break;
	case M17_K_STREAM:
		rx_stream(e);
		break;
#if CONFIG_M17_PACKET
	case M17_K_PACKET:
		rx_packet(e);
		break;
#endif
#if CONFIG_M17_BERT
	case M17_K_BERT:
		rx_bert(e, f);
		break;
#endif
	default:
		break;
	}
}

// --- The radio core's calls ---
// The configuration with its traffic key: the copy goes through registers, so it is secret work (crash_secret.h). A
// build without encryption has no key in it
static void cfg_copy(m17_engine_t *e, const m17_engine_cfg_t *cfg)
{
#if CONFIG_M17_SCRAMBLER || CONFIG_M17_AES
	crash_secret_enter();
	e->cfg = *cfg;
	crash_secret_leave();
#else
	e->cfg = *cfg;
#endif
}

bool m17_engine_begin(m17_engine_t *e, const hal_air_t *air, const m17_engine_cfg_t *cfg,
		      const m17_engine_hooks_t *hooks, void *ctx)
{
	memset(e, 0, sizeof *e);
	cfg_copy(e, cfg);
	e->hooks = *hooks;
	e->ctx = ctx;
	if (air_session_open(&e->session, air, &m17_air_mode, e, e->frame, e->soft, M17_FRAME_SYMS, &ops, e))
		return true;
	crypto_wipe(e->cfg.key, sizeof e->cfg.key);
	return false;
}

// The session first, so the provider's task is out of the engine; then every secret: the key, the scrambler state and
// AES round keys (u, work), the frames that held plaintext or a key stream
void m17_engine_end(m17_engine_t *e)
{
	air_session_close(&e->session);
	crypto_wipe(e->cfg.key, sizeof e->cfg.key);
	e->cfg.key_len = 0;
	crypto_wipe(&e->u, sizeof e->u);
	crypto_wipe(&e->work, sizeof e->work);
	crypto_wipe(e->frame, sizeof e->frame);
	crypto_wipe(e->soft, sizeof e->soft);
	crypto_wipe(e->tx_out, sizeof e->tx_out);
	e->rx_state = 0;
	e->rx_expect = M17_K_NONE;
	e->view.rx_active = false;
	e->view.rx_played = false;
}

// The framer starts over; the reception in hand ends with the next lock (the head hook sees a new lock with one in
// hand, m17_rx.c), in the provider's task where the hooks run
void m17_engine_retuned(m17_engine_t *e)
{
	air_session_retuned(&e->session);
}

// Not while transmitting: the frames being built read the key and the text. The provider's task sees recheck at the
// next frame: the squelch again, the scrambler from the seed
void m17_engine_set(m17_engine_t *e, const m17_engine_cfg_t *cfg)
{
	if (air_session_tx_state(&e->session) != AIR_TX_IDLE)
		return;
	cfg_copy(e, cfg);
	e->recheck = true;
}

// A sequence lock: gnss_seq is odd while gnss_in is written, even after (0 stays "never given": 0xFE goes on to 2).
// The provider's task runs above the radio core's, so it can read in the middle of this write, never the other way:
// it sees the odd count, or a count that moved, and takes the position at its next turn (m17_tx.c)
void m17_engine_gnss(m17_engine_t *e, const m17_gnss_t *g)
{
	uint8_t s = e->gnss_seq;

	e->gnss_seq = (uint8_t)(s + 1u);
	m17_barrier();
	e->gnss_in = *g;
	m17_barrier();
	e->gnss_seq = s == 0xFEu ? 2u : (uint8_t)(s + 2u);
}

void m17_engine_view(const m17_engine_t *e, m17_engine_view_t *out)
{
	uint8_t seq;

	// The provider's task writes the view in a few instructions: read again if it did meanwhile
	do {
		seq = e->seq;
		m17_barrier();
		*out = e->view;
		m17_barrier();
	} while ((seq & 1u) || seq != e->seq);
	out->tx = (uint8_t)air_session_tx_state(&e->session);
	out->tx_phase = e->tx_phase;
}

size_t m17_engine_text(const m17_engine_t *e, char out[M17_TEXT_MAX + 1])
{
	m17_text_t t;
	uint8_t seq;

	do {
		seq = e->seq;
		m17_barrier();
		t = e->text;
		m17_barrier();
	} while ((seq & 1u) || seq != e->seq);
	return m17_text_get(&t, out);
}

#if CONFIG_M17_PACKET
uint16_t m17_engine_packet_take(m17_engine_t *e, m17_packet_info_t *info, uint8_t *buf, uint16_t cap)
{
	uint16_t n;

	if (!e->pkt_hold.full)
		return 0;
	m17_barrier();
	*info = e->pkt_hold.info;
	n = info->len < cap ? info->len : cap;
	if (buf && n)
		memcpy(buf, e->pkt_hold.buf, n);
	if (n < info->len) {
		info->len = n;
		info->cut = true;
	}
	m17_barrier();
	e->pkt_hold.full = 0;
	return n;
}
#endif

#if CONFIG_M17_SIG
// u.stream is the transmitter's only from the tail on (before it, a reception may still hold u)
static bool tx_tail(const m17_engine_t *e)
{
	return e->tx_phase == M17_TXP_TAIL || e->tx_phase == M17_TXP_SIG;
}

bool m17_engine_sig_want(const m17_engine_t *e, uint8_t hash[M17_HASH_BYTES])
{
	if (!tx_tail(e) || e->u.stream.rs_state != M17_RS_WANTED)
		return false;
	m17_barrier();
	memcpy(hash, e->u.stream.rs, M17_HASH_BYTES);	// H waits in rs until r || s replaces it
	return true;
}

// Byte by byte through a volatile pointer, so every byte is in before rs_state says GIVEN
void m17_engine_sig_give(m17_engine_t *e, const uint8_t rs[M17_SIG_RS_BYTES])
{
	volatile uint8_t *to = e->u.stream.rs;

	if (!tx_tail(e) || e->u.stream.rs_state != M17_RS_WANTED)
		return;
	if (!rs) {
		e->u.stream.rs_state = M17_RS_LATE;
		return;
	}
	for (uint8_t i = 0; i < M17_SIG_RS_BYTES; i++)
		to[i] = rs[i];
	m17_barrier();
	e->u.stream.rs_state = M17_RS_GIVEN;
}

// Copied, then checked that the provider's task did not start another reception meanwhile (it clears rs_state first,
// and bumps sig_seq for each check it leaves)
bool m17_engine_sig_take(m17_engine_t *e, m17_sig_check_t *out)
{
	const m17_sig_rx_t *r = &e->u.stream.sig.rx;
	uint8_t gen;

	if (e->u.stream.rs_state != M17_RS_CHECK)
		return false;
	gen = e->view.sig_seq;
	m17_barrier();
	out->state = r->state;
	memcpy(out->hash, r->hash, M17_HASH_BYTES);
	memcpy(out->rs, r->rs, M17_SIG_RS_BYTES);
	m17_auth_read(r->auth, &out->signer, &out->dst, &out->ts);
	out->src = (e->call.flags & M17_CALL_EXTCALL) ? e->call.ext1 : e->call.src;
	m17_barrier();
	if (e->u.stream.rs_state != M17_RS_CHECK || e->view.sig_seq != gen)
		return false;
	e->u.stream.rs_state = M17_RS_NONE;
	return true;
}
#endif

#endif
