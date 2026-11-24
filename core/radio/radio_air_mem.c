// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The memory the air-framed modes run in (radio_air_mem.h): the vocoder's loan for the modes whose codec is in the
// tree where the platform's vocoder lends, else one static arena sized for the largest built mode. Radio task only.
#include <stdint.h>
#include "config.h"
#include "board.h"
#include "radio/radio_air_mem.h"
#if CONFIG_P25
#include "radio/radio_p25_mem.h"
#endif
#if CONFIG_NXDN
#include "nxdn/nxdn_engine.h"
#endif
#if CONFIG_M17
#include "radio/radio_m17_mem.h"
#endif

// Which built modes run in the arena: NXDN always (its voice is the vocoder's); P25 and M17 only where no vocoder
// lends its memory
#ifdef HAL_VOCODER_LEND_BYTES
#define ARENA_P25	0
#define ARENA_M17	0
#else
#define ARENA_P25	CONFIG_P25
#define ARENA_M17	CONFIG_M17
#endif
#define ARENA_NXDN	CONFIG_NXDN
#define ARENA		(ARENA_P25 || ARENA_NXDN || ARENA_M17)

#if ARENA
// One mode at a time, so one block: the union of their memory types (8-byte aligned, as their engines need)
static union {
#if ARENA_P25
	p25_mem_t p25;
#endif
#if ARENA_NXDN
	nxdn_engine_t nxdn;
#endif
#if ARENA_M17
	m17_mem_t m17;
#endif
} arena __attribute__((aligned(8)));
#endif

#ifdef HAL_VOCODER_LEND_BYTES
// The modes that run in the vocoder's loan: their codecs are in the tree (P25's IMBE, M17's Codec 2)
#define LOAN_MODES	(CONFIG_P25 + CONFIG_M17)
static bool in_loan(uint8_t mode)
{
	return (CONFIG_P25 && mode == RADIO_MODE_P25) || (CONFIG_M17 && mode == RADIO_MODE_M17);
}
#else
#define LOAN_MODES	0
static bool in_loan(uint8_t mode)
{
	(void)mode;
	return false;
}
#endif

// Who holds a block is kept where two modes can ask for one: the arena, or a loan two modes take. A build whose only
// block is one mode's loan (P25 on a lending vocoder) keeps nothing, as P25 kept nothing before the arena: it asks
// the vocoder each time it starts, and the radio core runs one mode at a time
#define HOLDER		(ARENA || LOAN_MODES > 1)
#if HOLDER
// The mode holding a block (the arena or the loan), plus one; 0: none. One at a time: a second get fails until put
static uint8_t held;
#endif

void *radio_air_mem_get(uint8_t mode, size_t len)
{
	void *m = NULL;

#if HOLDER
	if (held != 0)
		return NULL;
#endif
	if (in_loan(mode)) {
#ifdef HAL_VOCODER_LEND_BYTES
		size_t got = 0;
		if (board.vocoder != NULL && board.vocoder->lend != NULL)
			m = board.vocoder->lend(&got);
		if (m == NULL || got < len || ((uintptr_t)m & 7u) != 0)
			return NULL;
#endif
	} else {
#if ARENA
		if (len > sizeof(arena))
			return NULL;
		m = &arena;
#else
		(void)len;
		return NULL;
#endif
	}
#if HOLDER
	held = (uint8_t)(mode + 1u);
#endif
	return m;
}

void radio_air_mem_put(uint8_t mode)
{
#if HOLDER
	if (held == (uint8_t)(mode + 1u))
		held = 0;
#else
	(void)mode;
#endif
}

bool radio_air_mem_lent(uint8_t mode)
{
#if HOLDER
	return held == (uint8_t)(mode + 1u) && in_loan(mode);
#else
	return in_loan(mode);	// the loan is the only block: a mode that got one got it there
#endif
}

size_t radio_air_mem_arena(void)
{
#if ARENA
	return sizeof(arena);
#else
	return 0;
#endif
}
