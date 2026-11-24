// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 BERT (m17.md 9): the PRBS9 bit stream a BERT transmission carries, 197 bits a frame, and the receiver that locks
// to it and counts bit errors (synchronizing, counting, losing lock as spec G.2 describes). Pure functions. Built with
// CONFIG_M17_BERT.
#ifndef RFOUNDRY_M17_BERT_H
#define RFOUNDRY_M17_BERT_H

#include <stdbool.h>
#include <stdint.h>
#include "m17_frame.h"

#define M17_PRBS9_START		1u	// "state of 1": cell 1, the register's least significant bit (m17.md 13)

typedef struct {
	uint16_t reg;		// cells 1..9 in bits 0..8
} m17_prbs9_t;

static inline void m17_prbs9_init(m17_prbs9_t *g)
{
	g->reg = M17_PRBS9_START;
}
// The next bit: cell 5 XOR cell 9, which shifts into cell 1 (m17.md 9)
static inline uint8_t m17_prbs9_bit(m17_prbs9_t *g)
{
	uint8_t b = (uint8_t)(((g->reg >> 4) ^ (g->reg >> 8)) & 1u);

	g->reg = (uint16_t)(((g->reg << 1) | b) & 0x1FFu);
	return b;
}
// A frame's 197 bits, MSB first (the rest of the 25 bytes 0)
void m17_bert_next(m17_prbs9_t *g, uint8_t bits[M17_BERT_BYTES]);

enum { M17_BERT_SYNC, M17_BERT_COUNT };
typedef struct {
	m17_prbs9_t gen;
	uint8_t state;		// M17_BERT_*
	uint8_t run;		// synchronizing: matches in a row (18 locks)
	uint8_t win_bits, win_errs;	// counting: the next place in win, the errors in it (more than 18: lock lost)
	uint8_t win[16];		// counting: the last 128 bits, 1 where one was wrong (a sliding window)
	uint32_t bits, errs;	// counted while locked
	uint32_t frames, locks, losses;
} m17_bert_rx_t;

void m17_bert_rx_init(m17_bert_rx_t *r);
// A received frame's 197 bits (m17_frame_bert_get)
void m17_bert_rx_frame(m17_bert_rx_t *r, const uint8_t bits[M17_BERT_BYTES]);

#endif
