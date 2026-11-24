// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The memory the air-framed modes run in (P25, NXDN, M17): one runs at a time, so they share one block. On a platform
// whose DMR vocoder lends its working memory while no DMR voice runs (HAL_VOCODER_LEND_BYTES, hal_vocoder_t.lend), a
// mode that doesn't use that vocoder (P25 and M17: their codecs are in the tree) runs in the loan; the others (NXDN,
// whose voice is the vocoder's) and every mode on a platform without a loan run in a static arena here, sized for the
// largest of them by a union of their memory types. This replaces each mode keeping its own block and NXDN borrowing
// P25's (docs/spec/air-interface.md 7). Radio task only.
#ifndef RADIO_AIR_MEM_H
#define RADIO_AIR_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// mode: the radio_mode_t that will run in it; len: what it needs. NULL: none (the vocoder refused its loan, or the
// arena is in use: one mode at a time). The block is 8-byte aligned and its contents undefined
void *radio_air_mem_get(uint8_t mode, size_t len);
// mode stops: its block goes back (a lending vocoder rebuilds its state at its next call)
void radio_air_mem_put(uint8_t mode);
// True when the block mode got came from the vocoder's loan (the stats' mem_lent)
bool radio_air_mem_lent(uint8_t mode);
// The static arena's size in this build (0 when every built mode runs in the loan), for the debug link and tests
size_t radio_air_mem_arena(void);

#endif
