// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the engine's three files share and nothing else uses (m17_engine.h is the interface): the receive kinds, the
// air mode's sync numbers, where the head hook leaves a decoded frame in the frame buffer, the states of
// u.stream.rs_state, and the calls between the files. Built with CONFIG_M17.
#ifndef RFOUNDRY_M17_ENGINE_INT_H
#define RFOUNDRY_M17_ENGINE_INT_H

#include "m17_engine.h"
#include "../crypto/crypto_wipe.h"

// A frame's kind as decoded (rx_kind), what the next one should be (rx_expect), and what is being received (the low
// bits of rx_state)
enum { M17_K_NONE, M17_K_LSF, M17_K_STREAM, M17_K_PACKET, M17_K_BERT };

// The air mode's sync patterns (air_sync_t.id, in the profile's order)
enum { M17_SID_PRE_LSF, M17_SID_PRE_BERT, M17_SID_STREAM, M17_SID_PACKET, M17_SID_LSF, M17_SID_BERT };

// rx_state: the reception in hand (M17_K_*) and the receiver's flags
#define M17_RXS_KIND		0x07u
#define M17_RXS_SCR		0x08u	// the scrambler register is at the frame after call.fn
#define M17_RXS_COAST		0x30u	// untrusted stream frames in a row (m17_rx.c)
#define M17_RXS_COAST_1		0x10u
#define M17_RXS_BERT_KEPT	0x40u	// none in hand: the last reception was a BERT run, lost: picked up again, it
					// counts on
#define M17_RXS_RELOCK		0x40u	// one in hand: the framer moved to another sync (a slipped symbol, a new start)
					// and said lost; the head hook's next decode says whether it goes on
#define M17_RXS_PKT_END		0x80u	// the packet in hand came to its last frame
#define M17_RXS_FN0		0x80u	// the stream in hand: no trusted frame since its LSF, so FN 0 is due (the bit is
					// PKT_END's, which holds only with a packet in hand)

// A frame decoded by the head hook, in place in the frame buffer (m17_engine_t.frame): an LSF's 30 bytes, a packet
// frame's 26 or BERT's 25 bytes from 0; a stream frame as below
#define M17_F_FN		0u	// FN, big endian
#define M17_F_STREAM		2u	// the 16 STREAM bytes
#define M17_F_LICH		18u	// the LICH's 6 bytes ...
#define M17_F_LICH_OK		24u	// ... 1 when its Golay words held, 2 when they did on the soft retry
_Static_assert(M17_F_LICH_OK < M17_FRAME_BYTES && M17_LSF_BYTES <= M17_FRAME_BYTES, "a decoded frame fits its buffer");

#if CONFIG_M17_SIG
// u.stream.rs_state. Transmit: the signature the radio core gives. Receive: a signed stream's check waits for
// m17_engine_sig_take (the frames' results stay in u.stream until it is taken or the next reception starts)
enum { M17_RS_NONE, M17_RS_WANTED, M17_RS_GIVEN, M17_RS_LATE, M17_RS_CHECK };
#endif

// A compiler barrier: what one task wrote is in memory before the flag that hands it to the other (seq, rs_state,
// pkt_hold.full), and the other's copy is not read before the flag. Single core: nothing more is needed
static inline void m17_barrier(void)
{
	__asm__ __volatile__("" ::: "memory");
}

// The view under its sequence counter: odd while the provider's task writes it
static inline void m17_view_begin(m17_engine_t *e)
{
	e->seq++;
	m17_barrier();
}

static inline void m17_view_end(m17_engine_t *e)
{
	m17_barrier();
	e->seq++;
}

static inline uint8_t m17_rx_now(const m17_engine_t *e)
{
	return (uint8_t)(e->rx_state & M17_RXS_KIND);
}

// u starts over for another transmission: a signed stream's check that waits in it is withdrawn first, so
// m17_engine_sig_take, which looks at rs_state again after its copy, never takes a half-wiped one
static inline void m17_u_reset(m17_engine_t *e)
{
#if CONFIG_M17_SIG
	e->u.stream.rs_state = M17_RS_NONE;
	m17_barrier();
#endif
	crypto_wipe(&e->u, sizeof e->u);
}

// The FN the stream's next frame should have: 0 right after its LSF, else the one after the last trusted frame; one
// more for each frame handed over untrusted since (the coast count). The head hook's loose acceptance and the
// scrambler's run from frame to frame both go by it
static inline uint16_t m17_fn_due(const m17_engine_t *e)
{
	uint16_t next = (e->rx_state & M17_RXS_FN0) ? 0u : (uint16_t)(e->call.fn + 1u);

	return (uint16_t)((next + (e->rx_state & M17_RXS_COAST) / M17_RXS_COAST_1) & M17_FN_MASK);
}

// A relock waits (the flag shares its bit with BERT_KEPT, which holds only with nothing in hand)
static inline bool m17_rx_relock(const m17_engine_t *e)
{
	return m17_rx_now(e) != M17_K_NONE && (e->rx_state & M17_RXS_RELOCK);
}

// m17_engine.c: the call layer's receive side (the air mode's rx_frame and rx_lost), and ending the reception in hand
// (keep_bert: a BERT run's counters may go on)
void m17_eng_rx_frame(m17_engine_t *e, const air_frame_t *f);
void m17_eng_rx_lost(m17_engine_t *e);
void m17_eng_rx_stop(m17_engine_t *e, bool keep_bert);
// m17_tx.c: the session's tx_fill
void m17_tx_fill(void *ctx);

#endif
