// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS speaker routing's truth table (speaker_cs7000p.h): every combination of the two sources and
// the modem's mode, on every state the two expander bits can be in.
#include <stdio.h>
#include "speaker_cs7000p.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t apply(uint32_t shadow, cs7000p_speaker_t s)
{
	return (shadow & ~s.clear) | s.set;
}

int main(void)
{
	// The eight cases, written out: fm, pcm, fm_rx -> amplifier, feed-through
	static const struct {
		bool fm, pcm, fm_rx, amp, feed;
	} want[8] = {
		{ false, false, false, false, false },	// nothing to play, the modem in DMR or transmitting
		{ false, false, true, false, false },	// squelch shut in FM: silent, and the feed-through shut
		{ false, true, false, true, false },	// a beep in DMR
		{ false, true, true, true, false },	// a beep with the squelch shut: no receiver noise under it
		{ true, false, false, true, false },	// a call playing in DMR: the PCM path's audio
		{ true, false, true, true, true },	// FM audio: the only case that opens the feed-through
		{ true, true, false, true, false },
		{ true, true, true, true, false },	// a beep over FM audio: PCM wins
	};
	unsigned seen = 0;
	for (unsigned i = 0; i < 8; i++) {
		cs7000p_speaker_t s = cs7000p_speaker_lines(want[i].fm, want[i].pcm, want[i].fm_rx);
		seen |= 1u << ((want[i].fm ? 4u : 0u) | (want[i].pcm ? 2u : 0u) | (want[i].fm_rx ? 1u : 0u));
		CHECK(s.feed == want[i].feed);
		CHECK(((s.set & EXP_AMP) != 0u) == want[i].amp);
		CHECK(((s.clear & EXP_AMP) != 0u) == !want[i].amp);
		// Only the amplifier and the internal speaker's switch, and never a bit both ways
		CHECK(((s.set | s.clear) & ~(EXP_AMP | EXP_SPK_INT_OFF)) == 0u && (s.set & s.clear) == 0u);
		// The accessory speaker is never connected, the internal one never disconnected
		CHECK((s.clear & EXP_SPK_EXT_OFF) == 0u && (s.set & EXP_SPK_INT_OFF) == 0u);
		// No transmit bit, no rail, not the supply hold
		CHECK(((s.set | s.clear) & (EXP_TX_BITS | EXP_POWER_HOLD | EXP_RX_RAIL | EXP_SYNTH_RAIL)) == 0u);
		// On every state the two bits can be in, and from the reset word
		for (unsigned b = 0; b < 4; b++) {
			uint32_t shadow = (EXP_RESET_WORD & ~(EXP_AMP | EXP_SPK_INT_OFF)) | ((b & 1u) ? EXP_AMP : 0u) |
					  ((b & 2u) ? EXP_SPK_INT_OFF : 0u);
			uint32_t after = apply(shadow, s);
			CHECK(((after & EXP_AMP) != 0u) == want[i].amp);
			if (want[i].amp) {
				CHECK((after & EXP_SPK_INT_OFF) == 0u);	// an amplifier that is on has its speaker
			} else {
				CHECK(((after ^ shadow) & EXP_SPK_INT_OFF) == 0u);	// off leaves the switch alone
			}
			CHECK((after & EXP_SPK_EXT_OFF) != 0u);
			CHECK(((after ^ shadow) & ~(EXP_AMP | EXP_SPK_INT_OFF)) == 0u);
		}
	}
	CHECK(seen == 0xFFu);	// every combination once
	// From reset: both speakers disconnected and the amplifier off, and "off" keeps it so
	CHECK(apply(EXP_RESET_WORD, cs7000p_speaker_lines(false, false, true)) == EXP_RESET_WORD);
	CHECK(apply(EXP_RESET_WORD, cs7000p_speaker_lines(true, false, true)) ==
	      ((EXP_RESET_WORD & ~EXP_SPK_INT_OFF) | EXP_AMP));
	printf("# cs7000pSpeakerTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
