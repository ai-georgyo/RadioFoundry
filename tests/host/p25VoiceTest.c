// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The P25 voice path (core/p25/p25Voice.c) on a made-up PCM stream: the engine's hooks feed frames a LDU at a time,
// the audio callback is driven block by block. Playback waits for a whole LDU and its lead-in, plays every frame in
// order, conceals a late LDU through the decoder and mutes; bad frames, overflow, a call that ends early, the speaker
// route, the mixer and the stream's start and stop; the provider's task running between the callback's loads.
//
// The file under test is included, as m17VoiceTest does m17_voice.c: the load-order checks run the provider inside
// the callback's loads, and the checks read its queue.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

// Each load the file under test makes calls load_seen first (the inner name is not expanded again: the builtin)
static void load_seen(const volatile void *p);
#define __atomic_load_n(p, o) (load_seen(p), __atomic_load_n(p, o))
#include "../../core/p25/p25Voice.c"
#undef __atomic_load_n

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ── The board's PCM stream ──
static bool running, spk_on;
static int starts, stops;
static bool fake_start(audio_block_cb_t cb, void *ctx)
{
	(void)cb;
	(void)ctx;
	running = true;
	starts++;
	return true;
}
static void fake_stop(void) { running = false; stops++; }
static int spk_calls;
static void fake_speaker(bool on) { spk_on = on; spk_calls++; }
static const hal_audio_t audio = { .start = fake_start, .stop = fake_stop, .speaker_pcm = fake_speaker };

static p25_voice_mem_t vmem;
static const p25_engine_hooks_t *hk;

// A voiced frame: pitch code 60, mid gain, every band voiced, the rest of the bits varied by n
static void frame(uint8_t f[P25_IMBE_BYTES], int n)
{
	for (int i = 0; i < P25_IMBE_BYTES; i++)
		f[i] = (uint8_t)(0x35 * (i + 1) + 7 * n);
	f[0] = 60;
}

static void ldu(uint8_t errs)
{
	uint8_t f[P25_IMBE_BYTES];
	for (int i = 0; i < P25_VOICE_FRAMES; i++) {
		frame(f, i);
		hk->rx_voice(f, errs, NULL);
	}
}

// One audio block; true when it is not silent
static bool block(void)
{
	int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 }, spk[AUDIO_BLOCK_SAMPLES];
	memset(spk, 0x55, sizeof spk);
	p25VoiceAudioBlock(mic, spk, NULL);
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		if (spk[i] != 0)
			return true;
	return false;
}

static void begin(void)
{
	p25VoiceEnd();
	p25VoiceResetStats();
	running = spk_on = false;
	starts = stops = 0;
	p25VoiceBegin(&vmem);
}

// n blocks; how many were not silent
static int play(int n)
{
	int heard = 0;
	for (int i = 0; i < n; i++)
		heard += block();
	return heard;
}

// ── The provider's task between two of the callback's loads ──
// Armed, load_seen counts the callback's loads of the call counters and the queue head, and runs race_fn (the
// provider's hooks) just before the race_at-th: whatever order the callback reads them in, the provider can run
// between any two
static bool race_armed;
static int race_at, race_loads;
static void (*race_fn)(void);

static void load_seen(const volatile void *p)
{
	if (!race_armed || (p != &rx_seq && p != &rx_end_seq && p != &rx_head))
		return;
	if (++race_loads == race_at) {
		race_armed = false;
		race_fn();
	}
}

// One block with fn run before its at-th load; true when it is not silent
static bool race_block(int at, void (*fn)(void))
{
	race_fn = fn;
	race_at = at;
	race_loads = 0;
	race_armed = true;
	bool heard = block();
	CHECK(!race_armed, "the callback made %d loads, not %d", race_loads, at);
	race_armed = false;
	return heard;
}

// A new call of three frames, start to end
static void race_new_call(void)
{
	uint8_t f[P25_IMBE_BYTES];
	hk->rx_start(NULL, NULL);
	for (int i = 0; i < 3; i++) {
		frame(f, i);
		hk->rx_voice(f, 0, NULL);
	}
	hk->rx_end(NULL);
}

// The playing call's last frame, then its end
static void race_last_frame(void)
{
	uint8_t f[P25_IMBE_BYTES];
	frame(f, 9);
	hk->rx_voice(f, 0, NULL);
	hk->rx_end(NULL);
}

static void testLoadOrder(void)
{
	p25_voice_stats_t s;

	for (int at = 1; at <= 3; at++) {
		// A new call starts while the callback reads: its frames are its own, not the last call's
		begin();
		hk->rx_start(NULL, NULL);
		ldu(0);
		hk->rx_end(NULL);
		play(P25_VOICE_LEAD + P25_VOICE_FRAMES + 1);
		CHECK(!spk_on, "load %d: the first call did not end", at);
		int heard = race_block(at, race_new_call);
		heard += play(P25_VOICE_LEAD + 3 + 2);
		p25VoiceGetStats(&s);
		CHECK(heard > 0 && s.rx_frames == P25_VOICE_FRAMES + 3 && s.rx_calls == 2,
		      "load %d: a call started between the loads lost its frames: heard %d, frames %u, calls %u", at,
		      heard, (unsigned)s.rx_frames, (unsigned)s.rx_calls);
		CHECK(!spk_on && rx_tail == rx_head, "load %d: the second call did not end", at);

		// The playing call's last frame and its end come while the callback reads: the frame still plays
		begin();
		hk->rx_start(NULL, NULL);
		ldu(0);
		play(P25_VOICE_LEAD + P25_VOICE_FRAMES);
		CHECK(spk_on && rx_tail == rx_head, "load %d: the call is not playing with its queue empty", at);
		heard = race_block(at, race_last_frame);
		heard += play(3);
		p25VoiceGetStats(&s);
		CHECK(s.rx_frames == P25_VOICE_FRAMES + 1 && heard > 0,
		      "load %d: the frame before an end read with it did not play: frames %u, heard %d", at,
		      (unsigned)s.rx_frames, heard);
		CHECK(!spk_on && rx_tail == rx_head, "load %d: the call did not end", at);
	}
	p25VoiceEnd();
}

static void testPlayback(void)
{
	p25_voice_stats_t s;
	bool heard = false;

	begin();
	p25VoicePoll();
	CHECK(!running, "the stream started before any call");
	CHECK(!block(), "sound before any call");
	hk->rx_start(NULL, NULL);
	p25VoicePoll();
	CHECK(running && starts == 1, "the stream did not start with the call");
	// Eight frames are not a LDU: nothing plays yet
	uint8_t f[P25_IMBE_BYTES];
	for (int i = 0; i < P25_VOICE_FRAMES - 1; i++) {
		frame(f, i);
		hk->rx_voice(f, 0, NULL);
	}
	CHECK(!block() && !spk_on, "played before a whole LDU was in");
	frame(f, 8);
	hk->rx_voice(f, 0, NULL);
	// The lead-in: the speaker route on, silence
	for (int i = 0; i < P25_VOICE_LEAD; i++)
		CHECK(!block(), "lead-in block %d not silent", i);
	CHECK(spk_on, "the speaker route is off while the call plays");
	for (int i = 0; i < P25_VOICE_FRAMES; i++)
		heard |= block();
	p25VoiceGetStats(&s);
	CHECK(heard, "a voiced LDU played silent");
	CHECK(s.rx_frames == 9 && s.rx_in == 9 && s.rx_calls == 1, "frames %u in %u calls %u", (unsigned)s.rx_frames,
	      (unsigned)s.rx_in, (unsigned)s.rx_calls);
	CHECK(s.rx_queue_max == 9, "queue max %u", (unsigned)s.rx_queue_max);
	CHECK(s.rx_peak > 0, "no peak");
	// The next LDU is late: the decoder conceals, then mutes
	for (int i = 0; i < MBE_REPEAT_MAX + 2; i++)
		block();
	p25VoiceGetStats(&s);
	CHECK(s.rx_underruns == MBE_REPEAT_MAX + 2 && s.rx_concealed == MBE_REPEAT_MAX + 2,
	      "underruns %u concealed %u", (unsigned)s.rx_underruns, (unsigned)s.rx_concealed);
	CHECK(s.rx_underrun_at == 9, "underrun at %u", (unsigned)s.rx_underrun_at);
	CHECK(!block(), "concealment did not mute");
	// The LDU arrives after all: it plays at once (the call is primed), then the end turns the route off
	ldu(0);
	hk->rx_end(NULL);
	heard = false;
	for (int i = 0; i < P25_VOICE_FRAMES; i++)
		heard |= block();
	CHECK(heard, "the late LDU did not play");
	CHECK(spk_on, "route off before the queue ran dry");
	CHECK(!block() && !spk_on, "the route stayed on after the call");
	p25VoiceGetStats(&s);
	CHECK(s.rx_frames == 18, "frames %u", (unsigned)s.rx_frames);
	p25VoiceEnd();
	CHECK(!running && stops == 1 && !spk_on, "end did not stop the stream");
}

static void testBadAndOverflow(void)
{
	p25_voice_stats_t s;

	begin();
	hk->rx_start(NULL, NULL);
	ldu(IMBE_ERRS(11, 2, 0));	// frames the decoder does not trust
	for (int i = 0; i < P25_VOICE_LEAD + P25_VOICE_FRAMES; i++)
		CHECK(!block(), "a bad frame with nothing to repeat sounded");
	p25VoiceGetStats(&s);
	CHECK(s.rx_bad == 9 && s.rx_concealed == 9 && s.rx_frames == 0, "bad %u concealed %u frames %u",
	      (unsigned)s.rx_bad, (unsigned)s.rx_concealed, (unsigned)s.rx_frames);
	// Corrected bits are counted on good frames
	ldu(2);
	p25VoiceGetStats(&s);
	CHECK(s.rx_bit_errors == 18, "bit errors %u", (unsigned)s.rx_bit_errors);
	// More than the queue holds: the rest is dropped and counted
	for (int i = 0; i < 4; i++)
		ldu(0);
	p25VoiceGetStats(&s);
	CHECK(s.rx_queue_max == P25_VOICE_QUEUE, "queue max %u", (unsigned)s.rx_queue_max);
	CHECK(s.rx_overflows == 5 * 9 - P25_VOICE_QUEUE, "overflows %u", (unsigned)s.rx_overflows);
}

static void testShortCallAndNewCall(void)
{
	p25_voice_stats_t s;
	uint8_t f[P25_IMBE_BYTES];
	bool heard = false;

	begin();
	// A call of three frames that ends: they play without waiting for a LDU
	hk->rx_start(NULL, NULL);
	for (int i = 0; i < 3; i++) {
		frame(f, i);
		hk->rx_voice(f, 0, NULL);
	}
	hk->rx_end(NULL);
	for (int i = 0; i < P25_VOICE_LEAD + 3; i++)
		heard |= block();
	CHECK(heard, "a short call was not played");
	CHECK(!block() && !spk_on, "the short call did not end");
	// A call that ends before any frame: nothing
	hk->rx_start(NULL, NULL);
	hk->rx_end(NULL);
	CHECK(!block() && !spk_on, "an empty call sounded");
	// Frames left of a call that was cut off are dropped when the next starts
	hk->rx_start(NULL, NULL);
	ldu(0);
	block();
	hk->rx_start(NULL, NULL);	// (the engine starts a new call without an end: a squelch decision)
	for (int i = 0; i < 3; i++)
		CHECK(!block(), "the cut call's frames played in the next");
	p25VoiceGetStats(&s);
	CHECK(s.rx_calls == 2, "calls %u", (unsigned)s.rx_calls);
	// Frames with no call open start one
	hk->rx_end(NULL);
	ldu(0);
	heard = false;
	for (int i = 0; i < P25_VOICE_LEAD + 9; i++)
		heard |= block();
	CHECK(heard, "frames without a start did not play");
}

static bool mixing;
static bool mix(int16_t *spk)
{
	if (mixing)
		spk[0] = 1000;
	return mixing;
}

static void testMixerAndTx(void)
{
	uint8_t f[P25_IMBE_BYTES];
	p25_voice_stats_t s;

	begin();
	p25VoiceSetMixer(mix);
	mixing = true;
	CHECK(block() && spk_on, "the mixer's sound is not routed");
	mixing = false;
	CHECK(!block() && !spk_on, "the route stayed on after the mixer");
	p25VoiceSetMixer(NULL);
	// No transmission: the transmit hook has nothing (the engine sends its own silence)
	CHECK(!hk->tx_voice(f, NULL), "a tx frame with no transmission on");
	p25VoiceGetStats(&s);
	CHECK(s.tx_frames == 0, "tx frames %u", (unsigned)s.tx_frames);
	// Without memory (P25 not running) the callback is silent and safe
	p25VoiceEnd();
	CHECK(!block(), "sound after the end");
}

// A vowel-like mic block: harmonics of 125 Hz (a 64-sample period), the phase running on from block to block
static int mic_n;
static void mic_block(int16_t *mic, int amp)
{
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++, mic_n++) {
		double v = 0;
		for (int h = 1; h <= 20; h++)
			v += sin(2 * M_PI * h * mic_n / 64.0 + h * h * 0.3) / (1 + 0.2 * h);
		mic[i] = (int16_t)(amp * v);
	}
}

// One block while transmitting: the speaker must stay silent
static void tx_blocks(int n, int amp)
{
	int16_t mic[AUDIO_BLOCK_SAMPLES], spk[AUDIO_BLOCK_SAMPLES];

	for (int i = 0; i < n; i++) {
		mic_block(mic, amp);
		memset(spk, 0x55, sizeof spk);
		p25VoiceAudioBlock(mic, spk, NULL);
		for (int k = 0; k < AUDIO_BLOCK_SAMPLES; k++)
			if (spk[k] != 0) {
				CHECK(false, "speaker not silent while transmitting");
				return;
			}
	}
}

static void testTransmit(void)
{
	static mbe_params_t pr;
	imbe_state_t st;
	uint8_t f[P25_IMBE_BYTES];
	p25_voice_stats_t s;
	int got = 0, voiced = 0, pitch_ok = 0;

	begin();
	CHECK(!running, "the stream runs before anything happened");
	p25VoiceTx(true);
	CHECK(running && starts == 1, "the transmission did not start the stream");
	p25VoiceTx(true);
	CHECK(starts == 1, "a second start");
	// The engine takes each frame as it is made: all of them arrive, and they say what the mic heard
	imbe_state_reset(&st);
	for (int i = 0; i < 30; i++) {
		tx_blocks(1, 6000);
		while (hk->tx_voice(f, NULL)) {
			got++;
			if (imbe_unpack(&st, f, &pr) == IMBE_FRAME_OK && i >= 4) {
				// (125 Hz: w0 = 2 pi / 64 in the core's units, 32 harmonics by the standard's count)
				pitch_ok += pr.L >= 28 && pr.L <= 30;
				for (int k = 1; k <= 6; k++)
					voiced += pr.v[k] != 0;
			}
		}
	}
	CHECK(got == 30, "frames taken %d of 30", got);
	CHECK(pitch_ok >= 24, "the pitch was found in %d of 26 frames", pitch_ok);
	CHECK(voiced >= 24 * 6, "low harmonics voiced %d of %d", voiced, 26 * 6);
	p25VoiceGetStats(&s);
	CHECK(s.tx_frames == 30 && s.tx_encoded == 30 && s.tx_dropped == 0, "tx counts %u %u %u", (unsigned)s.tx_frames,
	      (unsigned)s.tx_encoded, (unsigned)s.tx_dropped);
	CHECK(s.tx_mic_peak > 6000 && s.tx_mic_peak < 32768, "mic peak %u", (unsigned)s.tx_mic_peak);
	// The engine not taking them: the queue fills, the rest are dropped, the encoder keeps running
	tx_blocks(P25_VOICE_TX_QUEUE + 3, 6000);
	p25VoiceGetStats(&s);
	CHECK(s.tx_encoded == 30 + P25_VOICE_TX_QUEUE + 3 && s.tx_dropped == 3, "full queue: %u encoded, %u dropped",
	      (unsigned)s.tx_encoded, (unsigned)s.tx_dropped);
	// The end: what is still queued is not sent, in this transmission or the next
	p25VoiceTx(false);
	CHECK(!hk->tx_voice(f, NULL), "a frame after the end");
	p25VoiceTx(true);
	CHECK(!hk->tx_voice(f, NULL), "a frame of the last transmission in a new one");
	// An all-zero mic (the route off) gives silence frames
	{
		int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 }, spk[AUDIO_BLOCK_SAMPLES];
		p25VoiceAudioBlock(mic, spk, NULL);
	}
	CHECK(hk->tx_voice(f, NULL) && memcmp(f, imbe_silence, sizeof f) == 0, "a zero block is not a silence frame");
	p25VoiceGetStats(&s);
	CHECK(s.tx_silent == 1, "silent frames %u", (unsigned)s.tx_silent);
	// The mic gain: 6 dB up (three steps) doubles what the encoder sees
	p25VoiceTx(false);
	p25VoiceTx(true);
	mic_n = 0;
	tx_blocks(2, 1000);
	p25VoiceGetStats(&s);
	uint32_t unity = s.tx_mic_peak;
	p25VoiceTx(false);
	p25VoiceSetMicGain(8);
	p25VoiceTx(true);
	mic_n = 0;
	tx_blocks(2, 1000);
	p25VoiceGetStats(&s);
	CHECK(unity > 1000 && s.tx_mic_peak >= 2 * unity - 16 && s.tx_mic_peak <= 2 * unity, "mic peak %u, with gain %u",
	      (unsigned)unity, (unsigned)s.tx_mic_peak);
	p25VoiceSetMicGain(5);
	p25VoiceTx(false);
	// Reception after it: a call plays as before (the decoder starts over)
	hk->rx_start(NULL, NULL);
	ldu(0);
	for (int i = 0; i < P25_VOICE_LEAD; i++)
		CHECK(!block(), "sound in the lead");
	CHECK(block(), "no sound in a call after a transmission");
	p25VoiceEnd();
	p25VoiceTx(true);	// not running: nothing happens
	CHECK(!running, "a transmission started the stream with P25 off");
}

// A transmission while a call plays: the call stops, the speaker route with it, and what was queued before is not
// heard after it; the call, still on air, plays again once a whole LDU is in
static void testTxDuringCall(void)
{
	p25_voice_stats_t s;

	begin();
	hk->rx_start(NULL, NULL);
	ldu(0);
	CHECK(play(P25_VOICE_LEAD + 3) > 0 && spk_on, "the call does not play");
	p25VoiceGetStats(&s);
	uint32_t pk = s.rx_peak;
	p25VoiceTx(true);
	tx_blocks(3, 6000);
	CHECK(!spk_on, "the speaker route stayed on while transmitting");
	CHECK(rx_tail == rx_head, "frames from before the transmission still queued during it");
	p25VoiceTx(false);
	CHECK(play(2 * P25_VOICE_FRAMES) == 0 && !spk_on, "the call's frames from before the transmission played after it");
	p25VoiceGetStats(&s);
	CHECK(s.rx_frames == 3, "frames %u", (unsigned)s.rx_frames);
	// Frames before a whole LDU is in do not play: the call primes again
	uint8_t f[P25_IMBE_BYTES];
	for (int i = 0; i < P25_VOICE_FRAMES - 1; i++) {
		frame(f, i);
		hk->rx_voice(f, 0, NULL);
	}
	CHECK(play(P25_VOICE_LEAD + 1) == 0 && !spk_on, "the call played after the transmission before a whole LDU");
	hk->rx_voice(f, 0, NULL);
	CHECK(play(P25_VOICE_LEAD + P25_VOICE_FRAMES) > 0 && spk_on, "the call did not play again after the transmission");
	p25VoiceGetStats(&s);
	CHECK(s.rx_frames == 3 + P25_VOICE_FRAMES, "frames %u", (unsigned)s.rx_frames);
	// Still one call: priming again after the transmission doesn't count it twice or clear its peak
	CHECK(s.rx_calls == 1 && pk > 0 && s.rx_peak >= pk, "the call counted %u times, peak %u, was %u",
	      (unsigned)s.rx_calls, (unsigned)s.rx_peak, (unsigned)pk);
	hk->rx_end(NULL);
	play(2);
	CHECK(!spk_on, "the route stayed on after the call");

	// A call that ends during the transmission: nothing of it after, the route stays off
	begin();
	hk->rx_start(NULL, NULL);
	ldu(0);
	CHECK(play(P25_VOICE_LEAD + 2) > 0 && spk_on, "the call does not play");
	p25VoiceTx(true);
	tx_blocks(2, 6000);
	ldu(0);
	hk->rx_end(NULL);
	tx_blocks(1, 6000);
	p25VoiceTx(false);
	CHECK(play(P25_VOICE_LEAD + P25_VOICE_FRAMES) == 0 && !spk_on,
	      "a call that ended during the transmission was heard after it");
	// A call that starts during it plays once the transmission is over
	p25VoiceTx(true);
	tx_blocks(1, 6000);
	hk->rx_start(NULL, NULL);
	tx_blocks(1, 6000);
	p25VoiceTx(false);
	ldu(0);
	hk->rx_end(NULL);
	CHECK(play(P25_VOICE_LEAD + P25_VOICE_FRAMES + 1) > 0 && !spk_on,
	      "a call that started during the transmission did not play after it, or the route stayed on");
	p25VoiceGetStats(&s);
	CHECK(s.rx_frames == 2 + P25_VOICE_FRAMES, "frames %u", (unsigned)s.rx_frames);
	p25VoiceEnd();
}

// The speaker route as calls start: a beep that ends in the block where a call is first seen, and a playing call cut
// off by one that ends before it plays, leave it off
static void testRouteAtNewCall(void)
{
	begin();
	p25VoiceSetMixer(mix);
	mixing = true;
	CHECK(block() && spk_on, "the mixer is not heard");
	hk->rx_start(NULL, NULL);
	mixing = false;
	CHECK(!block() && !spk_on, "the route stayed on after a beep that ended as a call started");
	hk->rx_end(NULL);
	block();
	CHECK(!spk_on, "the route came on for an empty call");
	p25VoiceSetMixer(NULL);
	// A beep that goes on while a call starts keeps the route, and its end turns it off
	p25VoiceSetMixer(mix);
	mixing = true;
	block();
	hk->rx_start(NULL, NULL);
	CHECK(block() && spk_on, "the route went off under a beep as a call started");
	mixing = false;
	CHECK(!block() && !spk_on, "the route stayed on after the beep");
	hk->rx_end(NULL);
	p25VoiceSetMixer(NULL);
	// A playing call cut off by a new one that ends before it plays
	hk->rx_start(NULL, NULL);
	ldu(0);
	CHECK(play(P25_VOICE_LEAD + 2) > 0 && spk_on, "the call does not play");
	hk->rx_start(NULL, NULL);
	CHECK(!block() && !spk_on, "the route stayed on after the call was cut off");
	hk->rx_end(NULL);
	CHECK(!block() && !spk_on, "the route came on for the empty call");
	// A playing call cut off by a new one while a beep plays: the route stays on through the block, not off and on
	// again under the beep, and the beep's end turns it off
	hk->rx_start(NULL, NULL);
	ldu(0);
	CHECK(play(P25_VOICE_LEAD + 2) > 0 && spk_on, "the call does not play");
	p25VoiceSetMixer(mix);
	mixing = true;
	block();
	int calls = spk_calls;
	hk->rx_start(NULL, NULL);
	block();
	CHECK(spk_on && spk_calls == calls, "the route toggled under a beep as a new call cut one off (%d calls)",
	      spk_calls - calls);
	mixing = false;
	block();
	CHECK(!spk_on, "the route stayed on after the beep");
	hk->rx_end(NULL);
	p25VoiceSetMixer(NULL);
	p25VoiceEnd();
}

// The peak is the latest call's that played: a call that is seen but never plays leaves it
static void testPeak(void)
{
	p25_voice_stats_t s;

	begin();
	hk->rx_start(NULL, NULL);
	ldu(0);
	hk->rx_end(NULL);
	play(P25_VOICE_LEAD + P25_VOICE_FRAMES + 1);
	p25VoiceGetStats(&s);
	uint32_t pk = s.rx_peak;
	CHECK(pk > 0, "no peak");
	hk->rx_start(NULL, NULL);
	hk->rx_end(NULL);
	play(2);
	p25VoiceGetStats(&s);
	CHECK(s.rx_peak == pk, "an empty call changed the peak: %u, was %u", (unsigned)s.rx_peak, (unsigned)pk);
	// A call of silence frames, not yet primed: the peak stays the last call's until it plays, then is its own
	hk->rx_start(NULL, NULL);
	for (int i = 0; i < P25_VOICE_FRAMES - 1; i++)
		hk->rx_voice(imbe_silence, 0, NULL);
	play(2);
	p25VoiceGetStats(&s);
	CHECK(s.rx_peak == pk, "a call not playing yet changed the peak: %u, was %u", (unsigned)s.rx_peak, (unsigned)pk);
	hk->rx_voice(imbe_silence, 0, NULL);
	play(P25_VOICE_LEAD + P25_VOICE_FRAMES);
	p25VoiceGetStats(&s);
	CHECK(s.rx_peak < pk, "the peak was not reset as the call played: %u, was %u", (unsigned)s.rx_peak, (unsigned)pk);
	p25VoiceEnd();
}

// A beep's own stream left the speaker route on and the transmission took the running stream over (#705): the
// first block gives the route back
static void testRouteTakeover(void)
{
	begin();
	spk_on = true;		// the beep's callback routed the speaker
	p25VoiceTx(true);
	block();
	CHECK(!spk_on, "the beep's route left on after the takeover");
	p25VoiceTx(false);
	p25VoiceEnd();
}

int main(void)
{
	p25VoiceInit(&audio);
	hk = p25VoiceHooks();
	testPlayback();
	testBadAndOverflow();
	testShortCallAndNewCall();
	testMixerAndTx();
	testTransmit();
	testRouteTakeover();
	testLoadOrder();
	testTxDuringCall();
	testRouteAtNewCall();
	testPeak();
	printf("p25VoiceTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
