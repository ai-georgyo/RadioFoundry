// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 on a channel (m17.md 5, 8, 9; the engine's section of m17.md says more): the call layer on an air interface
// session (core/air/air_session.h). The radio core begins it on an M17 channel and ends it on leaving; in between it
// receives streams, packets and BERT, hands voice to the voice path through hooks, and transmits when the radio core
// says so: a stream (PTT), a packet (an SMS, sent on its own), a BERT run (the test mode).
//
// Receive: the air mode's head hook (m17_rx.c) confirms a short sync from the whole frame (LSF CRC, LICH Golay, the
// end-of-transmission pattern) because 8-symbol syncs also turn up in payload; the frame is decoded, a late joiner
// rebuilds the LSF from six LICH chunks, and TYPE decides the codec, the squelch and the decryption.
// Transmit: preamble, LSF, frames pulled from the voice path (two Codec 2 3200 frames, or one 1600 frame and 8 data
// bytes, per 40 ms), EoT. A signed stream ends with sig_tail frames of silence, the authentication frame and the four
// signature frames; the signature itself is the radio core's to compute (m17_engine_sig_want / _give), in its task.
//
// No RTOS call: the radio core's task and the provider's task use it as air_session.h describes. The hooks run in the
// provider's task. Every secret in it (the key, the scrambler state, AES round keys) is wiped at m17_engine_end.
//
// Who writes what. The radio core's task writes cfg (m17_engine_set, not while transmitting), the transmit request and
// the packet to send (only while no transmission is prepared), gnss_in (gnss_seq), and the signature it gives (rs,
// rs_state, only while rs_state says wanted). The provider's task owns everything else while the engine runs: the
// receive fields, the transmit progress, the per-transmission union u (one transmission at a time: the one being
// received, or the one being sent, never both: the radio is simplex) and the work union. The received packet waits in
// pkt_hold, outside u, so a stream heard or sent before the radio core takes it can't overwrite it: the provider's task
// writes pkt_hold only while full is 0, the radio core's task reads it only while full is 1 and clears full when done.
// A packet received while one still waits is dropped and counted (pkt_dropped).
//
// Stream length. An encrypted (scrambler or AES) or signed stream ends with the end bit at FN 7FFA at the latest
// (M17_FN_ENC_LAST) whatever the TX time-out: AES's counter and the scrambler's late-join seek repeat past 7FFF, and a
// signed stream's tail takes 7FFB-7FFE (m17.md 6, 7). An unsigned clear stream wraps its FN at 7FFF as the spec says.
// The radio core shows the limit as the time-out in force (radio_m17_view_t.tot_s).
//
// The engine's size is a budget (radio_m17_mem.h): sizeof(m17_engine_t) <= M17_ENGINE_BUDGET, asserted in m17_engine.c.
// Built with CONFIG_M17.
#ifndef RFOUNDRY_M17_ENGINE_H
#define RFOUNDRY_M17_ENGINE_H

#include <stddef.h>
#include "config.h"
#include "m17.h"
#include "m17_frame.h"
#include "m17_meta.h"
#include "m17_crypt.h"
#if CONFIG_M17_PACKET
#include "m17_packet.h"
#endif
#if CONFIG_M17_BERT
#include "m17_bert.h"
#endif
#if CONFIG_M17_SIG
#include "m17_sig.h"
#endif
#include "../air/air_session.h"

#define M17_CAN_ANY		0xFFu	// rx_can: every CAN
#define M17_ERRS_BAD		0xFFu	// rx_voice errs: the frame could not be trusted (play as lost)
#define M17_ENGINE_PKT_KEEP	144u	// bytes of a received packet kept (data type and payload: an SMS's type byte and
					// the 143 bytes the inbox keeps; the CRC is checked over all of it)
#define M17_ENGINE_BUDGET	2560u	// bytes, the engine's part of m17_mem_t (ARM layout; C-SKY is smaller)
#define M17_FN_ENC_LAST		M17_FN_DATA_MAX_SIGNED	// 7FFA: an encrypted or signed stream's last data frame

// Which calls play (m17_engine_cfg_t.squelch). 0, a zeroed parameter block's, is the CAN squelch
enum {
	M17_SQUELCH_CAN,	// the CAN is ours
	M17_SQUELCH_ANY,	// everything
	M17_SQUELCH_ADDRESSED,	// the CAN is ours and DST is us, broadcast, or what we call (a reflector, a group)
};

// m17_call_t.flags
#define M17_CALL_TYPE		0x01u	// TYPE is known (the LSF, or the LICH chunk that carries it)
#define M17_CALL_LSF		0x02u	// the whole LSF is in and its CRC holds (DST, SRC known)
#define M17_CALL_LATE		0x04u	// joined without the LSF frame: rebuilt from the LICH
#define M17_CALL_TEXT		0x08u	// META text has come (m17_engine_text)
#define M17_CALL_GNSS		0x10u	// a position has come (gnss)
#define M17_CALL_EXTCALL	0x20u	// an extended callsign has come (ext1, ext2)
#define M17_CALL_PACKET		0x40u	// a packet transmission, not a stream

// A transmission heard: the stream now or the last one
typedef struct {
	uint64_t src, dst;
	uint64_t ext1, ext2;		// extended callsign META: the originator, the reflector (0 none)
	m17_gnss_t gnss;
	uint16_t type;			// TYPE (m17.md 3.1)
	uint16_t fn;			// the latest frame number
	uint8_t flags;			// M17_CALL_*
	uint8_t sig;			// M17_SIG_* (m17_sig.h; M17_SIG_NONE without CONFIG_M17_SIG)
	uint8_t errs;			// corrected bits of the latest frame, saturating
	int16_t rssi_dbm;
} m17_call_t;

typedef struct {
	// Receive, provider's task. start: a stream we play began (TYPE known: the codec mode is M17_TYPE_DATA(c->type));
	// update: its fields filled in or META changed; voice: a frame's 16 STREAM bytes, decrypted, with errs the bits
	// the decoder corrected or M17_ERRS_BAD; end: the stream ended (EoT, the last frame, or lost)
	void (*rx_start)(const m17_call_t *c, void *ctx);
	void (*rx_update)(const m17_call_t *c, void *ctx);
	void (*rx_voice)(const uint8_t stream[M17_STREAM_BYTES], uint8_t errs, void *ctx);
	void (*rx_end)(void *ctx);
	// Transmit, provider's task: the next frame's 16 STREAM bytes from the encoder; false when none is ready (the
	// engine sends cfg.silence and counts it as filler). NULL: every frame is silence
	bool (*tx_voice)(uint8_t stream[M17_STREAM_BYTES], void *ctx);
} m17_engine_hooks_t;

typedef struct {
	uint64_t src;		// ours: the source of what we send. 0: no callsign (the radio core refuses PTT first)
	uint64_t dst;		// what PTT calls; M17_ADDR_BROADCAST for everyone
	uint8_t rx_can;		// 0..15, or M17_CAN_ANY
	uint8_t tx_can;
	uint8_t squelch;	// M17_SQUELCH_*
	uint8_t voice;		// what PTT sends: M17_DATA_VOICE (Codec 2 3200) or M17_DATA_VOICE_DATA (1600 + data)
	bool tx_inhibit;
	bool sign;		// what we send is signed (CONFIG_M17_SIG; the radio core gives the signature)
	uint8_t sig_tail;	// frames of silence after PTT release in a signed stream: the signature's time
	uint8_t tx_enc;		// M17_ENC_* PTT sends with: NONE unless the channel opts in (and the build has it)
	uint8_t key_len;	// the channel's traffic key: a scrambler seed (1-3 bytes) or an AES key (16, 24, 32);
	uint8_t key[32];	// ... received streams of that kind are decrypted with it. A secret
	uint8_t text_len;	// META text we send, one block a superframe in turn (m17.md 5.3); 0 none
	const uint8_t *text;	// ... the caller's (radio_m17.c's copy of the radio-wide record), unchanged while the
				// engine runs except through m17_engine_set
	bool gnss;		// we send our position as META when the radio core gives one (m17_engine_gnss)
	const uint8_t *silence;	// 16 STREAM bytes of encoded silence in the voice mode
} m17_engine_cfg_t;

// m17_tx_req_t.kind. A BERT run (PTT on the BER test page, or the debug link's run) ends like a stream: at tx_stop, at
// the next frame boundary
enum { M17_TX_STREAM, M17_TX_PACKET, M17_TX_BERT };

// One transmission's own values (m17_engine_tx_prepare)
typedef struct {
	uint8_t kind;			// M17_TX_*
	uint32_t ts;			// seconds since 2020-01-01 UTC at key-up: the AES nonce's, and the authentication
					// frame's plus the frames sent before it (m17_tx.c); 0: no clock
	uint8_t rnd[10];		// AES: the nonce's 80 bits, never repeated under one key (entropy_nonce: random bytes
					// and the radio's persistent counter, entropy.h)
} m17_tx_req_t;

// view.tx_phase: where a transmission is (the UI's "SIGNING" while the tail goes out)
enum { M17_TXP_IDLE, M17_TXP_PREAMBLE, M17_TXP_LSF, M17_TXP_DATA, M17_TXP_TAIL, M17_TXP_SIG, M17_TXP_EOT };

// view.rx_why: why the transmission heard does not play (0: it plays, or nothing is heard)
enum { M17_WHY_NONE, M17_WHY_CAN, M17_WHY_ADDRESSED, M17_WHY_ENC, M17_WHY_SCR, M17_WHY_DATA };

typedef struct {
	uint32_t frames;		// frames handed over by the framer (head hook agreed)
	uint32_t lsf_ok, lsf_bad;	// LSF frames: CRC held / failed
	uint32_t lich_ok, lich_bad;	// stream frames' LICH: Golay held / failed
	uint32_t stream, packet, bert;	// frames of each kind decoded
	uint32_t fec_bits;		// bits the decoders corrected
	uint32_t can_rejects;		// streams not played: another CAN, or not addressed to us (squelch)
	uint32_t eot;			// ends of transmission seen
	uint32_t head_rejects;		// syncs the head hook turned down (payload, noise)
	uint32_t enc_skipped;		// streams encrypted with what we have no key for (not played)
	uint32_t pkt_rx, pkt_bad, pkt_tx;	// packets received whole, dropped (counter, CRC), sent
	uint32_t pkt_dropped;		// packets received whole while the one before still waited (pkt_hold full)
	uint32_t sig_late;		// signed transmissions whose signature was not ready in time (zeros sent)
	uint32_t fn_limit;		// encrypted or signed streams ended at M17_FN_ENC_LAST
	uint32_t expect_miss;		// frames whose expected kind failed to decode and were decoded by their sync instead
} m17_engine_stats_t;

// A received packet (m17_engine_packet_take)
typedef struct {
	uint64_t src, dst;
	uint32_t type;			// M17_PKT_*
	uint16_t len;			// bytes of payload copied (after the data type)
	bool cut;			// the packet was longer than M17_ENGINE_PKT_KEEP: the payload's start only
} m17_packet_info_t;

#if CONFIG_M17_SIG
// What the radio core checks after a signed stream (m17_engine_sig_take)
typedef struct {
	uint8_t state;			// M17_SIG_PENDING: ready to verify; else the conclusion already (INCOMPLETE, LATE)
	uint8_t hash[M17_HASH_BYTES];
	uint8_t rs[M17_SIG_RS_BYTES];
	uint64_t signer, dst, src;	// src: the stream's SRC, or the extended callsign's originator (m17.md 7.4)
	uint32_t ts;
} m17_sig_check_t;
#endif

typedef struct {
	bool rx_active;			// a transmission is being received (whether or not it plays)
	bool rx_played;			// ... and passes the squelch
	m17_call_t rx;			// its fields; kept after it ends
	uint8_t tx;			// air_tx_state_t
	uint8_t tx_phase;		// M17_TXP_*
	uint8_t rx_why;			// M17_WHY_*
	uint8_t text_seq;		// bumps when the META text changed (m17_engine_text)
	uint8_t pkt_seq;		// bumps when a packet waits (m17_engine_packet_take)
	uint8_t sig_seq;		// bumps when a signed stream waits to be checked (m17_engine_sig_take), or the
					// transmitter waits for a signature (m17_engine_sig_want)
	uint32_t rx_calls, tx_calls, tx_filler;
	uint32_t noise;			// a running mix of the soft values received: the radio core stirs it into its entropy
#if CONFIG_M17_BERT
	uint32_t bert_frames, bert_bits, bert_errs, bert_locks, bert_losses;
	uint8_t bert_state;		// M17_BERT_*
#endif
} m17_engine_view_t;

typedef struct {
	air_session_t session;
	m17_engine_cfg_t cfg;
	m17_engine_hooks_t hooks;
	void *ctx;
	// Receive (provider's task)
	m17_call_t call;		// the transmission in hand
	m17_text_t text;		// its META text so far
	uint8_t rx_state;		// the receiver's own states (m17_rx.c)
	uint8_t rx_kind;		// the kind of the frame in hand, as decoded
	// What the next frame is expected to be while a transmission runs: after an LSF, stream or packet by its TYPE;
	// in a BERT run, BERT; none while searching. The LSF/packet and stream/BERT sync words are 2 symbols apart
	// (m17.md 1.2), so with one symbol error a frame can match both: the head hook decodes it as this kind and falls
	// back to its sync's kind only when that decode fails (expect_miss), or when no transmission runs
	uint8_t rx_expect;
	uint8_t lich_have;		// LICH chunks of the current LSF collected (bit n: chunk n)
	uint8_t lich_lsf[M17_LSF_BYTES];	// ... being put together
	uint8_t rx_meta[M17_META_BYTES];	// the META in force
	// The head hook's decode, kept for rx_frame (no second decode): the results are written in place over the
	// frame's own buffer (frame[] below: the LSF's 30 bytes, or FN and STREAM, or a packet chunk, or BERT bits) once
	// its symbols are read, and cur_errs holds the decoder's count (-1: not trusted)
	int8_t cur_errs;
	// Transmit (provider's task once it runs)
	m17_tx_req_t txr;
	uint8_t tx_lsf[M17_LSF_BYTES];
	uint8_t tx_out[M17_FRAME_BYTES];	// the frame being sent
	uint8_t tx_phase;		// preamble, LSF, frames, tail, signature, EoT
	uint8_t tx_lich_cnt;
	uint8_t tx_text_block;
	uint8_t tx_tail_left;
	uint16_t tx_fn;
	uint16_t tx_at;			// symbols of tx_out already queued
	volatile bool tx_stop;		// the radio core let go: end at the next frame boundary
	uint8_t tx_gnss_wait;		// superframes before GNSS META may go again (m17_tx.c)
	// The packet M17_TX_PACKET sends (m17_engine_packet_load): the caller's buffer, untouched by the engine, the
	// caller's until AIR_TX_DONE (radio_m17.c keeps it in its static pending send)
	const uint8_t *pkt_data;
	uint16_t pkt_len;
	uint64_t pkt_dst;
	volatile uint8_t gnss_seq;	// m17_engine_gnss wrote gnss_in: a sequence lock, odd while it writes
	uint8_t gnss_done;
	m17_gnss_t gnss_in;
	// One transmission's state, by what it is (only one is on air at a time)
	union {
		struct {
#if CONFIG_M17_SCRAMBLER
			m17_scr_t scr;
#endif
#if CONFIG_M17_SIG
			union {
				m17_sig_rx_t rx;
				m17_sig_tx_t tx;
			} sig;
			uint8_t rs[M17_SIG_RS_BYTES];	// the signature given (m17_engine_sig_give)
			volatile uint8_t rs_state;	// none, wanted, given
#endif
			uint8_t pad;
		} stream;
#if CONFIG_M17_PACKET
		struct {
			m17_pkt_rx_t rx;	// receiving: keeps into pkt_hold.buf (only while pkt_hold.full is 0)
			m17_pkt_tx_t tx;	// sending: reads pkt_data
		} pkt;
#endif
#if CONFIG_M17_BERT
		struct {
			m17_bert_rx_t rx;
			m17_prbs9_t tx;
		} bert;
#endif
	} u;
#if CONFIG_M17_PACKET
	// A received packet waiting for the radio core (m17_engine_packet_take), outside u: see "Who writes what" above
	struct {
		m17_packet_info_t info;
		uint8_t buf[M17_ENGINE_PKT_KEEP];
		volatile uint8_t full;	// 1: buf and info hold a packet the radio core has not taken
	} pkt_hold;
#endif
	volatile bool recheck;		// m17_engine_set changed what passes the squelch
	volatile uint8_t seq;		// odd while the provider's task writes view
	m17_engine_view_t view;
	m17_engine_stats_t stats;
	uint8_t frame[M17_FRAME_BYTES];	// the framer's frame buffer ...
	int8_t soft[M17_FRAME_SYMS];	// ... and its soft values
	// Working memory inside one frame's decode or build (provider's task), never across frames
	union {
		m17_fec_scratch_t fec;
#if CONFIG_M17_AES
		aes_ctx_t aes;		// the key schedule, expanded for the frame and wiped after it
#endif
#if CONFIG_M17_SCRAMBLER
		m17_scr_work_t scr;	// a late joiner's seek (m17_scr_seek)
#endif
	} work;
} m17_engine_t;

// The air mode (m17_rx.c): profile, sync patterns, head hook (docs/spec/m17.md, the engine's section)
extern const air_mode_t m17_air_mode;

// What the board's provider gives M17: AIR_FIT_xxx, 0 = this board can't
uint32_t m17_engine_fit(const hal_air_t *air);
// Start on the tuned channel (the board in its digital mode, its DMR stack stopped). False: no provider, no fit
bool m17_engine_begin(m17_engine_t *e, const hal_air_t *air, const m17_engine_cfg_t *cfg,
		      const m17_engine_hooks_t *hooks, void *ctx);
// Stop: the session closes (a transmission is cut) and every secret is wiped
void m17_engine_end(m17_engine_t *e);
void m17_engine_retuned(m17_engine_t *e);
// What is received and sent changes while running (CAN, squelch, destination, keys, text). Not while transmitting
void m17_engine_set(m17_engine_t *e, const m17_engine_cfg_t *cfg);
// Our position for GNSS META (radio core's task; the next superframe boundary sends it, at most once every 5 s)
void m17_engine_gnss(m17_engine_t *e, const m17_gnss_t *g);

// Transmit (the radio core's task), as nxdn_engine.h: prepare queues the preamble and the LSF (false: refused, nothing
// queued: no source address, tx_inhibit, a packet not loaded); hal_radio_t.tx(); go. stop: the operator let go (a
// stream ends at the next frame boundary, after its tail; a BERT run at the next frame boundary; a packet when its
// frames are out, whenever stop comes). state AIR_TX_DONE: switch the transmitter off, then clear
bool m17_engine_tx_prepare(m17_engine_t *e, const m17_tx_req_t *req);
bool m17_engine_tx_go(m17_engine_t *e);
void m17_engine_tx_stop(m17_engine_t *e);
void m17_engine_tx_abort(m17_engine_t *e);
air_tx_state_t m17_engine_tx_state(const m17_engine_t *e);
void m17_engine_tx_clear(m17_engine_t *e);

// A copy of the state for the radio core's task, and the META text heard (UTF-8, NUL-terminated; returns its length)
void m17_engine_view(const m17_engine_t *e, m17_engine_view_t *out);
size_t m17_engine_text(const m17_engine_t *e, char out[M17_TEXT_MAX + 1]);

#if CONFIG_M17_PACKET
// The packet the next M17_TX_PACKET sends to dst: data type and payload, len <= M17_PKT_DATA_MAX. Only the pointer is
// kept: data stays the caller's, unchanged, until the transmission is AIR_TX_DONE (or the engine ends). Radio core's
// task, not while transmitting. False when len is 0 or too long
bool m17_engine_packet_load(m17_engine_t *e, uint64_t dst, const uint8_t *data, uint16_t len);
// A received packet, once (radio core's task): its fields into info, up to cap payload bytes into buf; returns the
// bytes copied, 0 when none waits. Taking it frees the engine's buffer for the next
uint16_t m17_engine_packet_take(m17_engine_t *e, m17_packet_info_t *info, uint8_t *buf, uint16_t cap);
#endif

#if CONFIG_M17_SIG
// Transmit: true once a signed stream's hash is final (the operator let go and the tail is fixed): hash for the
// signer. The radio core signs (p256_sign) and gives r || s, or NULL when it can't (the tail then carries zeros:
// receivers report a failed check; counted in sig_late). Radio core's task
bool m17_engine_sig_want(const m17_engine_t *e, uint8_t hash[M17_HASH_BYTES]);
void m17_engine_sig_give(m17_engine_t *e, const uint8_t rs[M17_SIG_RS_BYTES]);
// Receive: a signed stream ended; what to verify, once (radio core's task). The radio core's verdict goes in its own
// view, not here
bool m17_engine_sig_take(m17_engine_t *e, m17_sig_check_t *out);
#endif

#endif
