// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS speaker routing (docs/spec/hardware-cs7000p.md 8): what the expander and the modem are told for
// each state of the two sources. Pure: tests/cs7000pSpeakerTest.c checks every case. Unverified on a radio.
//
// Everything the speaker plays leaves the HR_C6000's line out: this board has no analog path from the
// discriminator. In FM receive the chip plays its own FM audio there through a feed-through (4:0x36) that does not
// follow the squelch, and the PCM stream (beeps) reaches the same output by another way. So the amplifier is on
// while either source wants the speaker, and the feed-through is open only for FM audio alone: PCM wins, a beep is
// never mixed with the receiver's noise. The line-out amplifier ahead of the volume pot is on the receive rail:
// nothing plays while transmitting or asleep.
//
// The accessory speaker stays disconnected: what its detect line means is in no document.
#ifndef SPEAKER_CS7000P_H
#define SPEAKER_CS7000P_H

#include <stdint.h>
#include <stdbool.h>
#include "exp_cs7000p.h"

typedef struct {
	uint32_t set, clear;	// expander bits (cs7000p_exp_set)
	bool feed;		// the modem's FM audio feed-through to its line out
} cs7000p_speaker_t;

// fm: the core wants received audio on the speaker (FM squelch open, a call playing). pcm: the PCM route is on (a
// beep). fm_rx: the modem is in FM receive, where its line out carries its own FM audio.
// On: the amplifier powered and the internal speaker connected. Off: the amplifier off; the speaker stays as it is
static inline cs7000p_speaker_t cs7000p_speaker_lines(bool fm, bool pcm, bool fm_rx)
{
	cs7000p_speaker_t s;
	bool on = fm || pcm;
	s.set = on ? EXP_AMP : 0u;
	s.clear = on ? EXP_SPK_INT_OFF : EXP_AMP;
	s.feed = fm && !pcm && fm_rx;
	return s;
}

// audio_cs7000p.c tells radio_cs7000p.c, which owns the routing, about the PCM route (the audio task)
void cs7000p_speaker_pcm(bool on);

#endif
