// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What runs while P25 is on, as one block (radio_p25.c takes it from radio_air_mem.h): the engine, the voice path's
// queue and decoder memory and, on a trunked channel, the follower with its set-up. Only radio_p25.c and
// radio_air_mem.c include it (it pulls the engine's, the follower's and the voice path's headers in); the arena is
// sized by it on a platform without a lending vocoder.
#ifndef RADIO_P25_MEM_H
#define RADIO_P25_MEM_H

#include "config.h"
#if CONFIG_P25
#include <stdbool.h>
#include <stdint.h>
#include "radio/radio_p25.h"	// the view's trunking states
#include "p25/p25_engine.h"
#include "p25/p25_codeplug.h"
#if CONFIG_IMBE
#include "p25/p25Voice.h"
#endif
#if CONFIG_P25_TRUNK
#include "p25/p25_follow.h"

// A trunked channel's follower (docs/spec/p25-trunking.md 7) and what it runs on. The provider's task hands it units
// and calls, the radio task ticks it and carries out its retunes; the two never run it at once (radio_p25.c enter_*)
typedef struct {
	p25_follow_t f;
	p25_follow_cfg_t cfg;
	p25_follow_cc_t cc[P25_CP_CTL_MAX];
	p25_follow_iden_t iden[P25_FOLLOW_IDENS];
	// The last retune the follower asked (its tune hook, from either task): want moves past done until the radio
	// task has carried it out
	volatile uint8_t want, done;
	uint8_t what;
	uint32_t hz, id;
	bool owed;			// the receiver is on the new frequency, the follower not told yet
	volatile uint8_t in_prov, in_radio;	// which task is inside the follower
	// The inbound burst the follower asked last (its send hook, from either task), as want / done above
	volatile uint8_t burst_want, burst_done;
	uint32_t burst_hz;
	uint8_t burst[P25_TSBK_BYTES];
	// PTT and the end of our transmission, for the follower at the radio task's next turn inside it
	bool ptt, talk_end;
	uint8_t ptt_seq, ptt_done;
	bool nogo_kept;			// this system's control channels are in nogo_hz (radio_p25.c)
} p25_trunk_t;
_Static_assert(P25_CP_IDEN_MAX == P25_FOLLOW_IDENS, "one identifier plan size");
_Static_assert(RADIO_P25_TRUNK_HUNT == P25_FOLLOW_HUNT && RADIO_P25_TRUNK_CONTROL == P25_FOLLOW_CONTROL &&
	       RADIO_P25_TRUNK_VOICE == P25_FOLLOW_VOICE, "the view's trunking states are the follower's");
#endif

// What runs while P25 is on: the engine and, with the decoder, the voice path's queue and decoder memory. On a
// trunked channel the follower too, and its set-up as the codeplug gives it, read before the voice path starts
typedef struct {
	p25_engine_t eng;
#if CONFIG_P25_TRUNK
	p25_trunk_t trunk;
	union {
#if CONFIG_IMBE
		p25_voice_mem_t voice;
#endif
		p25_trunk_cfg_t load;
	};
#elif CONFIG_IMBE
	p25_voice_mem_t voice;
#endif
} p25_mem_t;
#endif

#endif
