// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What runs while M17 is on, as one block (radio_m17.c takes it from radio_air_mem.h): the engine, and the voice path's
// queues, Codec 2 states and scratch. Only radio_m17.c and radio_air_mem.c include it (it pulls the engine's and the
// codec's headers in).
//
// Its size is a budget (docs/spec/m17.md, the memory section): it must fit what the board's DMR vocoder lends
// (HAL_VOCODER_LEND_BYTES, asserted) and should stay within the shared air arena that P25 already sizes on a radio
// without one (radio_air_mem.c), so turning M17 on costs that radio no RAM. tests/host/radioCoreTest.c prints and checks
// it for the host's layout; the assert below checks it in every firmware build.
#ifndef RADIO_M17_MEM_H
#define RADIO_M17_MEM_H

#include "config.h"
#if CONFIG_M17
#include "m17/m17_engine.h"
#include "m17/m17_voice.h"

typedef struct {
	m17_engine_t eng;
	m17_voice_mem_t voice;
} m17_mem_t;

// The budget: what the plan allows. The HR_C7000 lends 7,168 bytes. On an STM32 radio the shared arena is the largest
// mode's block: P25's, 6,368 bytes on the STM32F4, so M17 may grow that arena by at most 128 bytes (measured
// 6,448 on ARM and 6,412 on C-SKY with every M17 switch on: 80 bytes over P25's). Growing past the budget is a plan
// change. Checked on the 32-bit targets; the host's 64-bit pointers make its layout larger
#define RADIO_M17_MEM_BUDGET	6496u
#if __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(m17_mem_t) <= RADIO_M17_MEM_BUDGET, "M17's memory grew past its budget (docs/spec/m17.md, memory)");
#endif
#ifdef HAL_VOCODER_LEND_BYTES
_Static_assert(sizeof(m17_mem_t) <= HAL_VOCODER_LEND_BYTES, "M17 does not fit the memory the vocoder lends");
#endif
#endif

#endif
