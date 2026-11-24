// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 BERT (m17_bert.h; docs/spec/m17.md 9): PRBS9 frames and the receiver of spec G.2.
#include "config.h"

#if CONFIG_M17_BERT

#include <string.h>

#include "m17/m17_bert.h"

#define LOCK_RUN	18u	// matches in a row that lock: twice the register
#define WIN_BITS	128u	// the window lock is lost in (a power of 2: the ring's index wraps by mask) ...
#define WIN_ERRS	18u	// ... with more errors than this

_Static_assert(sizeof(((m17_bert_rx_t *)0)->win) * 8u == WIN_BITS, "the BERT loss window is 128 bits");

void m17_bert_next(m17_prbs9_t *g, uint8_t bits[M17_BERT_BYTES])
{
	memset(bits, 0, M17_BERT_BYTES);
	for (uint16_t i = 0; i < M17_BERT_T1_BITS; i++)
		if (m17_prbs9_bit(g))
			bits[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
}

void m17_bert_rx_init(m17_bert_rx_t *r)
{
	memset(r, 0, sizeof *r);
	m17_prbs9_init(&r->gen);
	r->state = M17_BERT_SYNC;
}

// Synchronizing: the register is the generator run as a self-synchronizing descrambler (each received bit shifts in),
// so 9 good bits after any error give it the sender's state. Counting: the register runs free and predicts. The loss
// window slides: win holds the last 128 counted bits' errors, so a burst across any 128 bits is seen (m17.md 9)
void m17_bert_rx_frame(m17_bert_rx_t *r, const uint8_t bits[M17_BERT_BYTES])
{
	r->frames++;
	for (uint16_t i = 0; i < M17_BERT_T1_BITS; i++) {
		uint8_t b = (uint8_t)((bits[i >> 3] >> (7u - (i & 7u))) & 1u), m, *w;

		if (r->state == M17_BERT_SYNC) {
			uint8_t want = (uint8_t)(((r->gen.reg >> 4) ^ (r->gen.reg >> 8)) & 1u);

			r->run = b == want ? (uint8_t)(r->run + 1u) : 0;
			r->gen.reg = (uint16_t)(((r->gen.reg << 1) | b) & 0x1FFu);
			if (r->run >= LOCK_RUN) {
				r->state = M17_BERT_COUNT;
				r->locks++;
				r->win_bits = 0;
				r->win_errs = 0;
				memset(r->win, 0, sizeof r->win);
			}
			continue;
		}
		r->bits++;
		// The bit 128 back leaves the window, this one enters it
		w = &r->win[r->win_bits >> 3];
		m = (uint8_t)(0x80u >> (r->win_bits & 7u));
		if (*w & m)
			r->win_errs--;
		if (b != m17_prbs9_bit(&r->gen)) {
			r->errs++;
			r->win_errs++;
			*w |= m;
		} else {
			*w &= (uint8_t)~m;
		}
		r->win_bits = (uint8_t)((r->win_bits + 1u) & (WIN_BITS - 1u));
		if (r->win_errs > WIN_ERRS) {
			r->state = M17_BERT_SYNC;
			r->losses++;
			r->run = 0;
		}
	}
}

#endif
