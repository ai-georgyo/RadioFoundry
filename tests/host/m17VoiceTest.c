// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The M17 voice path (core/m17/m17_voice.c) on the real Codec 2: the engine's hooks feed stream frames, the audio
// callback is driven block by block, and what it plays and sends is compared sample for sample and bit for bit with
// the codec run directly. Priming, the 3200 and 1600 cadence, underruns, overflow, concealment of bad frames, calls
// ending and cut off, transmit framing in both modes and the filler when the encoder is late, the scratch's loan with
// the stream stopped and running and when it is refused, the codec bench, the mixer and the speaker route. With a
// speech file (argv[1], s16le 8 kHz: tests/data/voice) the speech goes through both directions and the host's time per
// block is printed.
//
// The file under test is included, as p256Test does p256.c: the checks read its bench frames and its memory.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "../../core/m17/m17_voice.c"
#include "codec2/c2_int.h"

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

// A clock that moves 7 us between any two reads: every timed block measures 7
static uint32_t fake_us;
static uint32_t fake_clock(void) { return fake_us += 7; }

static m17_voice_mem_t vmem;
#define MIC_UNITY	5	// radio/mic_gain.h's unity step

// ── Reference codec, run directly ──
static c2_enc_state_t ref_enc;
static c2_dec_state_t ref_dec;
static c2_scratch_t ref_scr;

// A vowel-like signal: harmonics of f0 with a falling spectrum, the phase running on from block to block
static long sig_n;
static void vowel(int16_t *pcm, double f0, double amp)
{
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++, sig_n++) {
		double v = 0;
		for (int h = 1; h * f0 < 3800; h++)
			v += sin(2 * M_PI * h * f0 * sig_n / 8000.0 + h * h * 0.3) / (1 + 0.3 * h);
		v *= amp;
		pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}

// n stream frames of a vowel in mode, through the reference encoder
#define MAXF	64
static uint8_t frames[MAXF][M17_STREAM_BYTES];
static void make_frames(c2_mode_t mode, int n, double f0)
{
	int16_t pcm[AUDIO_BLOCK_SAMPLES];
	uint8_t bits[C2_FRAME_BYTES];

	c2_enc_init(&ref_enc, mode);
	sig_n = 0;
	for (int i = 0; i < n; i++) {
		memset(frames[i], 0, M17_STREAM_BYTES);
		for (int b = 0; b < 2; b++) {
			vowel(pcm, f0, 3000);
			if (c2_enc_block(&ref_enc, &ref_scr, pcm, bits))
				memcpy(frames[i] + (mode == C2_MODE_3200 ? b * C2_FRAME_BYTES : 0), bits, C2_FRAME_BYTES);
		}
	}
}

static int16_t out[AUDIO_BLOCK_SAMPLES];

// One audio block with a silent mic; true when the output is not silent
static bool block(void)
{
	int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 };
	memset(out, 0x55, sizeof out);
	m17_voice_audio_block(mic, out, NULL);
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		if (out[i] != 0)
			return true;
	return false;
}

static bool silent(const int16_t *p)
{
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		if (p[i] != 0)
			return false;
	return true;
}

// The reference's next block: a new frame first when bits / lost say so (lost: NULL bits, bad)
static void ref_block(int16_t *pcm, const uint8_t *bits, bool frame, bool bad)
{
	if (frame)
		c2_dec_frame(&ref_dec, bits, bad);
	c2_dec_block(&ref_dec, &ref_scr, pcm);
}

// One block through the voice path against the reference's next block
static bool block_eq(const uint8_t *bits, bool frame, bool bad)
{
	int16_t want[AUDIO_BLOCK_SAMPLES];

	ref_block(want, bits, frame, bad);
	block();
	return memcmp(want, out, sizeof want) == 0;
}

// A stream frame's two blocks, against the reference playing it in mode
static bool stream_eq(c2_mode_t mode, const uint8_t *f, bool bad)
{
	bool ok = block_eq(f, true, bad);
	if (mode == C2_MODE_3200)
		return block_eq(f + C2_FRAME_BYTES, true, bad) && ok;
	return block_eq(NULL, false, false) && ok;
}

static void begin(void)
{
	m17_voice_end();
	m17_voice_reset_stats();
	m17_voice_set_mixer(NULL);
	m17_voice_set_mic_gain(MIC_UNITY);
	running = spk_on = false;
	starts = stops = 0;
	m17_voice_begin(&vmem);
}

static void testConstants(void)
{
	const uint8_t *s = m17_voice_silence(C2_MODE_3200);
	CHECK(!memcmp(s, c2_silence_3200, 8) && !memcmp(s + 8, c2_silence_3200, 8), "3200 silence is not the codec's twice");
	s = m17_voice_silence(C2_MODE_1600);
	static const uint8_t zero[8];
	CHECK(!memcmp(s, c2_silence_1600, 8) && !memcmp(s + 8, zero, 8), "1600 silence is not the codec's and zeros");
	// The bench's frames: silence, voiced vowels, unvoiced hiss (as their comments say)
	static const uint8_t v3200[BENCH_FRAMES] = { 0, 3, 3, 3, 3, 0, 0, 0 };	// voiced[0..1] as bits
	static const uint8_t v1600[BENCH_FRAMES] = { 0, 15, 15, 15, 15, 0, 0, 8 };	// voiced[0..3]
	CHECK(!memcmp(bench_3200[0], c2_silence_3200, 8) && !memcmp(bench_1600[0], c2_silence_1600, 8),
	      "the bench does not start with silence");
	for (int i = 0; i < BENCH_FRAMES; i++) {
		c2_index_t ix;
		c2_unpack(C2_MODE_3200, bench_3200[i], &ix);
		CHECK((ix.voiced[0] | ix.voiced[1] << 1) == v3200[i], "bench 3200 frame %d voicing", i);
		c2_unpack(C2_MODE_1600, bench_1600[i], &ix);
		CHECK((ix.voiced[0] | ix.voiced[1] << 1 | ix.voiced[2] << 2 | ix.voiced[3] << 3) == v1600[i],
		      "bench 1600 frame %d voicing", i);
		CHECK(i == 0 || ix.e[0] > 10, "bench 1600 frame %d is near silence", i);
	}
}

// Priming, cadence and concealment in one mode: every block equals the codec run directly
static void testPlayback(c2_mode_t mode)
{
	const char *mn = mode == C2_MODE_3200 ? "3200" : "1600";
	m17_voice_stats_t s;
	int bad_blocks = 0;

	make_frames(mode, 12, 140);
	begin();
	m17_voice_poll();
	CHECK(!running, "%s: the stream started before any call", mn);
	CHECK(!block(), "%s: sound before any call", mn);
	m17_voice_rx_start(mode);
	m17_voice_poll();
	CHECK(running && starts == 1, "%s: the stream did not start with the call", mn);
	c2_dec_init(&ref_dec, mode);
	// One frame is not enough: priming waits for M17_VOICE_PRIME
	m17_voice_rx_frame(frames[0], 0);
	CHECK(!block() && !spk_on, "%s: played before %d frames were in", mn, M17_VOICE_PRIME);
	m17_voice_rx_frame(frames[1], 3);
	// Two blocks a stream frame, one codec frame a block (3200) or one a stream frame (1600)
	CHECK(stream_eq(mode, frames[0], false), "%s: frame 0 is not the codec's", mn);
	CHECK(spk_on, "%s: the speaker route is off while the call plays", mn);
	m17_voice_rx_frame(frames[2], 0);
	CHECK(stream_eq(mode, frames[1], false), "%s: frame 1", mn);
	CHECK(stream_eq(mode, frames[2], false), "%s: frame 2", mn);
	m17_voice_stats(&s);
	CHECK(s.rx_in == 3 && s.rx_calls == 1 && s.rx_underruns == 0, "%s: in %u calls %u underruns %u", mn,
	      (unsigned)s.rx_in, (unsigned)s.rx_calls, (unsigned)s.rx_underruns);
	CHECK(s.rx_frames == (mode == C2_MODE_3200 ? 6u : 3u), "%s: codec frames played %u", mn, (unsigned)s.rx_frames);
	CHECK(s.rx_peak > 1000, "%s: peak %u", mn, (unsigned)s.rx_peak);
	CHECK(s.dec_blocks == 6, "%s: dec blocks %u", mn, (unsigned)s.dec_blocks);
	// Nothing in time: the lost-frame path, block by block (3200) or a lost frame's two blocks (1600), then mute
	bool muted = false;
	for (int i = 0; i < 8; i++) {
		bool f = mode == C2_MODE_3200 || (i & 1) == 0;
		CHECK(block_eq(NULL, f, true), "%s: concealed block %d is not the codec's", mn, i);
		muted = silent(out);
	}
	CHECK(muted, "%s: concealment did not mute", mn);
	m17_voice_stats(&s);
	CHECK(s.rx_underruns == 8 && s.rx_concealed == 8, "%s: underruns %u concealed %u", mn, (unsigned)s.rx_underruns,
	      (unsigned)s.rx_concealed);
	// The late frame plays at once (the call stays primed); then bad frames, concealed through the decoder
	m17_voice_rx_frame(frames[3], 0);
	CHECK(stream_eq(mode, frames[3], false), "%s: the late frame", mn);
	m17_voice_rx_frame(frames[4], M17_ERRS_BAD);
	m17_voice_rx_frame(frames[5], M17_ERRS_BAD);
	CHECK(stream_eq(mode, frames[4], true), "%s: a bad frame is not concealed as the codec does", mn);
	CHECK(stream_eq(mode, frames[5], true), "%s: the second bad frame", mn);
	bad_blocks = 4;
	m17_voice_rx_frame(frames[6], 0);
	m17_voice_rx_frame(frames[7], 0);
	m17_voice_rx_end();
	CHECK(stream_eq(mode, frames[6], false), "%s: the good frame after bad ones", mn);
	CHECK(stream_eq(mode, frames[7], false), "%s: the last frame", mn);
	CHECK(spk_on, "%s: the route went off before the queue ran dry", mn);
	CHECK(!block() && !spk_on, "%s: the route stayed on after the call", mn);
	m17_voice_stats(&s);
	CHECK(s.rx_bad == 2 && s.rx_concealed == 8u + bad_blocks, "%s: bad %u concealed %u", mn, (unsigned)s.rx_bad,
	      (unsigned)s.rx_concealed);
	CHECK(s.rx_frames == (mode == C2_MODE_3200 ? 12u : 6u), "%s: frames %u", mn, (unsigned)s.rx_frames);
	m17_voice_end();
	CHECK(!running && stops == 1 && !spk_on, "%s: end did not stop the stream", mn);
}

static void testBadFirstAndOverflow(void)
{
	m17_voice_stats_t s;

	make_frames(C2_MODE_3200, 8, 120);
	begin();
	m17_voice_rx_start(C2_MODE_3200);
	m17_voice_poll();
	// Bad frames with nothing good before them: silence
	m17_voice_rx_frame(frames[0], M17_ERRS_BAD);
	m17_voice_rx_frame(frames[1], M17_ERRS_BAD);
	for (int i = 0; i < 4; i++)
		CHECK(!block(), "a bad frame with nothing to hold sounded");
	m17_voice_stats(&s);
	CHECK(s.rx_bad == 2 && s.rx_concealed == 4 && s.rx_frames == 0, "bad %u concealed %u frames %u",
	      (unsigned)s.rx_bad, (unsigned)s.rx_concealed, (unsigned)s.rx_frames);
	// The first good frame then plays as from a fresh decoder
	c2_dec_init(&ref_dec, C2_MODE_3200);
	m17_voice_rx_frame(frames[2], 0);
	CHECK(stream_eq(C2_MODE_3200, frames[2], false), "the first good frame after bad ones is not as from init");
	// More than the queue holds: the rest is dropped and counted, what was queued plays in order
	for (int i = 3; i < 3 + M17_VOICE_QUEUE + 2; i++)
		m17_voice_rx_frame(frames[i], 0);
	m17_voice_stats(&s);
	CHECK(s.rx_overflows == 2, "overflows %u", (unsigned)s.rx_overflows);
	for (int i = 3; i < 3 + M17_VOICE_QUEUE; i++)
		CHECK(stream_eq(C2_MODE_3200, frames[i], false), "queued frame %d after an overflow", i);
}

static void testCalls(void)
{
	m17_voice_stats_t s;
	bool heard = false;

	make_frames(C2_MODE_1600, 8, 180);
	begin();
	// A call of one frame that ends: it plays without waiting for a second
	m17_voice_rx_start(C2_MODE_1600);
	m17_voice_rx_frame(frames[0], 0);
	m17_voice_rx_end();
	m17_voice_poll();
	for (int i = 0; i < 2; i++)
		heard |= block();
	CHECK(heard, "a short call was not played");
	CHECK(!block() && !spk_on, "the short call did not end");
	m17_voice_stats(&s);
	uint32_t short_peak = s.rx_peak;
	// A call that ends before any frame: nothing; the peak stays the last call's that played
	m17_voice_rx_start(C2_MODE_3200);
	m17_voice_rx_end();
	CHECK(!block() && !spk_on, "an empty call sounded");
	m17_voice_stats(&s);
	CHECK(short_peak > 1000 && s.rx_peak == short_peak, "an empty call changed the peak: %u, was %u",
	      (unsigned)s.rx_peak, (unsigned)short_peak);
	// A call cut off by the next (a squelch decision): its frames are dropped, the next starts over in its own mode
	m17_voice_rx_start(C2_MODE_1600);
	m17_voice_rx_frame(frames[0], 0);
	m17_voice_rx_frame(frames[1], 0);
	m17_voice_rx_frame(frames[2], 0);
	block();
	block();
	block();	// the middle of a stream frame
	m17_voice_rx_start(C2_MODE_3200);
	for (int i = 0; i < 3; i++)
		CHECK(!block(), "the cut call's frames played in the next");
	m17_voice_stats(&s);
	CHECK(s.rx_calls == 2, "calls %u", (unsigned)s.rx_calls);
	CHECK(vs.rx_tail == vs.rx_head, "the cut call's frames still queued");
	// Frames with no call open start one, in the last call's mode
	m17_voice_rx_end();
	make_frames(C2_MODE_3200, 4, 180);
	c2_dec_init(&ref_dec, C2_MODE_3200);
	m17_voice_rx_frame(frames[0], 0);
	m17_voice_rx_frame(frames[1], 0);
	CHECK(stream_eq(C2_MODE_3200, frames[0], false), "frames without a start did not play in the last mode");
	m17_voice_stats(&s);
	CHECK(s.rx_calls == 3, "calls %u", (unsigned)s.rx_calls);
	// Without memory (M17 not running) the hooks and the callback are safe and silent
	m17_voice_end();
	m17_voice_rx_frame(frames[0], 0);
	uint8_t f[M17_STREAM_BYTES];
	CHECK(!m17_voice_tx_frame(f), "a tx frame with M17 off");
	CHECK(!block(), "sound after the end");
	m17_voice_tx(true, C2_MODE_3200);
	CHECK(!running, "a transmission started the stream with M17 off");
}

// ── Transmit ──
static void mic_blocks(int n, double f0, double amp, bool check_spk)
{
	int16_t mic[AUDIO_BLOCK_SAMPLES], spk[AUDIO_BLOCK_SAMPLES];

	for (int i = 0; i < n; i++) {
		vowel(mic, f0, amp);
		memset(spk, 0x55, sizeof spk);
		m17_voice_audio_block(mic, spk, NULL);
		if (check_spk && !silent(spk)) {
			CHECK(false, "speaker not silent while transmitting");
			return;
		}
	}
}

// The reference's stream frames for the same signal (from sample 0)
static void ref_tx(c2_mode_t mode, int n, double f0, double amp)
{
	long save = sig_n;
	sig_n = 0;
	c2_enc_init(&ref_enc, mode);
	int16_t pcm[AUDIO_BLOCK_SAMPLES];
	uint8_t bits[C2_FRAME_BYTES];
	for (int i = 0; i < n; i++) {
		memset(frames[i], 0, M17_STREAM_BYTES);
		for (int b = 0; b < 2; b++) {
			vowel(pcm, f0, amp);
			if (c2_enc_block(&ref_enc, &ref_scr, pcm, bits))
				memcpy(frames[i] + (mode == C2_MODE_3200 ? b * C2_FRAME_BYTES : 0), bits, C2_FRAME_BYTES);
		}
	}
	sig_n = save;
}

static void testTransmit(c2_mode_t mode)
{
	const char *mn = mode == C2_MODE_3200 ? "3200" : "1600";
	uint8_t f[M17_STREAM_BYTES];
	m17_voice_stats_t s;

	ref_tx(mode, 20, 130, 4000);
	begin();
	CHECK(!m17_voice_tx_frame(f), "%s: a frame before any transmission", mn);
	m17_voice_tx(true, mode);
	CHECK(running && starts == 1, "%s: the transmission did not start the stream", mn);
	m17_voice_tx(true, mode);
	CHECK(starts == 1, "%s: a second start", mn);
	// The encoder is late (no block yet, half a stream frame): the engine sends its filler
	CHECK(!m17_voice_tx_frame(f), "%s: a frame before the encoder made one", mn);
	sig_n = 0;
	mic_blocks(1, 130, 4000, true);
	CHECK(!m17_voice_tx_frame(f), "%s: a frame after one block", mn);
	mic_blocks(1, 130, 4000, true);
	// Each stream frame as the codec makes it: two 3200 frames, or a 1600 frame and 8 zero bytes
	CHECK(m17_voice_tx_frame(f) && !memcmp(f, frames[0], sizeof f), "%s: stream frame 0 is not the codec's", mn);
	CHECK(!m17_voice_tx_frame(f), "%s: a second frame from two blocks", mn);
	for (int i = 1; i < 10; i++) {
		mic_blocks(2, 130, 4000, true);
		CHECK(m17_voice_tx_frame(f) && !memcmp(f, frames[i], sizeof f), "%s: stream frame %d", mn, i);
	}
	m17_voice_stats(&s);
	CHECK(s.tx_frames == 10 && s.tx_encoded == 20 && s.tx_dropped == 0, "%s: tx counts %u %u %u", mn,
	      (unsigned)s.tx_frames, (unsigned)s.tx_encoded, (unsigned)s.tx_dropped);
	CHECK(s.enc_blocks == 20 && s.dec_blocks == 0, "%s: enc blocks %u dec %u", mn, (unsigned)s.enc_blocks,
	      (unsigned)s.dec_blocks);
	CHECK(s.tx_mic_peak > 4000 && s.tx_mic_peak < 32768, "%s: mic peak %u", mn, (unsigned)s.tx_mic_peak);
	// The engine not taking them: the queue fills, the rest are dropped, the encoder keeps running; what was
	// queued is sent in order
	mic_blocks(2 * (M17_VOICE_TX_QUEUE + 2), 130, 4000, true);
	m17_voice_stats(&s);
	CHECK(s.tx_encoded == 20u + 2 * (M17_VOICE_TX_QUEUE + 2) && s.tx_dropped == 2,
	      "%s: full queue: %u encoded, %u dropped", mn, (unsigned)s.tx_encoded, (unsigned)s.tx_dropped);
	for (int i = 10; i < 10 + M17_VOICE_TX_QUEUE; i++)
		CHECK(m17_voice_tx_frame(f) && !memcmp(f, frames[i], sizeof f), "%s: queued frame %d", mn, i);
	CHECK(!m17_voice_tx_frame(f), "%s: more than the queue held", mn);
	// The end: what is still queued is not sent, in this transmission or the next; a half-made frame is dropped
	mic_blocks(3, 130, 4000, true);
	m17_voice_tx(false, mode);
	CHECK(!m17_voice_tx_frame(f), "%s: a frame after the end", mn);
	m17_voice_tx(true, mode);
	CHECK(!m17_voice_tx_frame(f), "%s: a frame of the last transmission in a new one", mn);
	// The new transmission starts the encoder over
	sig_n = 0;
	mic_blocks(2, 130, 4000, true);
	CHECK(m17_voice_tx_frame(f) && !memcmp(f, frames[0], sizeof f), "%s: the next transmission's first frame", mn);
	m17_voice_tx(false, mode);
}

// 3200: the queue full when a stream frame's first codec frame is made, room when the second is: the frame is dropped
// (its first half went nowhere) and the next one is whole
static void testTransmitEdge(void)
{
	uint8_t f[M17_STREAM_BYTES];
	m17_voice_stats_t s;

	ref_tx(C2_MODE_3200, 8, 110, 5000);
	begin();
	m17_voice_tx(true, C2_MODE_3200);
	sig_n = 0;
	mic_blocks(2 * M17_VOICE_TX_QUEUE, 110, 5000, true);	// frames 0..3 queued, full
	mic_blocks(1, 110, 5000, true);				// frame 4's first half: no room
	CHECK(m17_voice_tx_frame(f) && !memcmp(f, frames[0], sizeof f), "frame 0");
	mic_blocks(1, 110, 5000, true);				// frame 4's second half: room, but dropped
	m17_voice_stats(&s);
	CHECK(s.tx_dropped == 1, "dropped %u", (unsigned)s.tx_dropped);
	mic_blocks(2, 110, 5000, true);				// frame 5
	for (int i = 1; i < 4; i++)
		CHECK(m17_voice_tx_frame(f) && !memcmp(f, frames[i], sizeof f), "frame %d", i);
	CHECK(m17_voice_tx_frame(f) && !memcmp(f, frames[5], sizeof f), "the frame after a dropped half");
	CHECK(!m17_voice_tx_frame(f), "a frame from the dropped half");
	m17_voice_tx(false, C2_MODE_3200);
	// The mic gain: 6 dB up (three steps) doubles what the encoder sees
	m17_voice_reset_stats();
	m17_voice_tx(true, C2_MODE_1600);
	sig_n = 0;
	mic_blocks(2, 125, 1000, true);
	m17_voice_stats(&s);
	uint32_t unity = s.tx_mic_peak;
	m17_voice_tx(false, C2_MODE_1600);
	m17_voice_set_mic_gain(8);
	m17_voice_tx(true, C2_MODE_1600);
	sig_n = 0;
	mic_blocks(2, 125, 1000, true);
	m17_voice_stats(&s);
	CHECK(unity > 1000 && s.tx_mic_peak >= 2 * unity - 16 && s.tx_mic_peak <= 2 * unity, "mic peak %u, with gain %u",
	      (unsigned)unity, (unsigned)s.tx_mic_peak);
	m17_voice_set_mic_gain(MIC_UNITY);
	m17_voice_tx(false, C2_MODE_1600);
	// Reception after it: a call plays from a fresh decoder (the encoder had the shared state)
	make_frames(C2_MODE_3200, 3, 150);
	c2_dec_init(&ref_dec, C2_MODE_3200);
	m17_voice_rx_start(C2_MODE_3200);
	m17_voice_rx_frame(frames[0], 0);
	m17_voice_rx_frame(frames[1], 0);
	CHECK(stream_eq(C2_MODE_3200, frames[0], false), "a call after a transmission is not from a fresh decoder");
	// A transmission while a call plays: the call stops, the speaker with it. What was received before it is
	// dropped: after the transmission the call's old frames are not heard (here the call ended meanwhile)
	m17_voice_rx_frame(frames[2], 0);
	m17_voice_stats(&s);
	uint32_t calls = s.rx_calls;
	m17_voice_tx(true, C2_MODE_3200);
	mic_blocks(1, 150, 1000, true);
	CHECK(!spk_on, "the speaker route stayed on while transmitting");
	m17_voice_rx_end();
	m17_voice_tx(false, C2_MODE_3200);
	for (int i = 0; i < 4; i++)
		CHECK(!block() && !spk_on, "the call's frames from before the transmission played after it (block %d)", i);
	m17_voice_stats(&s);
	CHECK(s.rx_calls == calls && vs.rx_tail == vs.rx_head, "calls %u, was %u", (unsigned)s.rx_calls,
	      (unsigned)calls);
	m17_voice_end();
}

// A transmission while a call is still on air: after it the call primes again from a fresh decoder, but it is the same
// call, counted once in rx_calls and its peak kept (the bench's decoder reset takes the same path)
static void testTxDuringCall(void)
{
	m17_voice_stats_t s;
	bool played = false;

	make_frames(C2_MODE_3200, 6, 180);
	begin();
	m17_voice_rx_start(C2_MODE_3200);
	for (int i = 0; i < 3; i++)
		m17_voice_rx_frame(frames[i], 0);
	for (int i = 0; i < 4; i++)
		played |= block();
	m17_voice_stats(&s);
	uint32_t pk = s.rx_peak;
	CHECK(played && spk_on && s.rx_calls == 1 && pk > 1000, "the call does not play: calls %u, peak %u",
	      (unsigned)s.rx_calls, (unsigned)pk);
	m17_voice_tx(true, C2_MODE_3200);
	mic_blocks(2, 150, 1000, true);
	m17_voice_tx(false, C2_MODE_3200);
	for (int i = 3; i < 6; i++)
		m17_voice_rx_frame(frames[i], 0);
	played = false;
	for (int i = 0; i < 6; i++)
		played |= block();
	CHECK(played && spk_on, "the call did not play again after the transmission");
	m17_voice_stats(&s);
	CHECK(s.rx_calls == 1 && s.rx_peak >= pk, "the call counted %u times, peak %u, was %u", (unsigned)s.rx_calls,
	      (unsigned)s.rx_peak, (unsigned)pk);
	m17_voice_rx_end();
	m17_voice_end();
}

// ── The scratch's loan ──
static void testBorrowStopped(void)
{
	size_t len = 0;
	m17_voice_stats_t s;

	begin();
	void *p = m17_voice_scratch_borrow(&len);
	CHECK(p == &vmem.scratch && len == sizeof(c2_scratch_t), "not granted at once with the stream stopped");
	CHECK(m17_voice_scratch_borrow(NULL) == p, "a second borrow while lent");
	m17_voice_stats(&s);
	CHECK(s.lent == 1, "lent %u", (unsigned)s.lent);
	// A call heard meanwhile: the stream waits for the return
	m17_voice_rx_start(C2_MODE_3200);
	m17_voice_poll();
	CHECK(!running, "the stream started while the scratch was lent");
	m17_voice_scratch_return();
	m17_voice_poll();
	CHECK(running, "the stream did not start after the return");
	m17_voice_end();
	// No audio at all: granted at once too
	m17_voice_init(NULL, NULL);
	begin();
	CHECK(m17_voice_scratch_borrow(&len) != NULL, "not granted without audio");
	m17_voice_scratch_return();
	m17_voice_init(&audio, fake_clock);
	// Nothing without memory
	m17_voice_end();
	CHECK(m17_voice_scratch_borrow(&len) == NULL, "granted with M17 off");
}

static void testBorrowRunning(void)
{
	size_t len = 0;
	m17_voice_stats_t s;
	static m17_voice_mem_t before;

	make_frames(C2_MODE_3200, 8, 140);
	begin();
	m17_voice_rx_start(C2_MODE_3200);
	m17_voice_poll();
	m17_voice_rx_end();
	block();
	CHECK(running, "no stream");
	// The first call posts the request; the callback answers at the top of its next block; a later call gets it
	CHECK(m17_voice_scratch_borrow(&len) == NULL, "granted at once with the stream running");
	CHECK(m17_voice_scratch_borrow(&len) == NULL, "granted before the callback answered");
	block();
	void *p = m17_voice_scratch_borrow(&len);
	CHECK(p == &vmem.scratch && len == sizeof(c2_scratch_t), "not granted after the callback answered");
	m17_voice_stats(&s);
	CHECK(s.lent == 1, "lent %u", (unsigned)s.lent);
	// While lent the callback plays silence and touches no codec state; a call starting then loses its frames
	memset(&vmem.scratch, 0xA5, sizeof vmem.scratch);
	memcpy(&before, &vmem, sizeof before);
	m17_voice_rx_start(C2_MODE_3200);
	m17_voice_rx_frame(frames[0], 0);
	m17_voice_rx_frame(frames[1], 0);
	for (int i = 0; i < 4; i++)
		CHECK(!block(), "sound while lent");
	CHECK(!memcmp(&before.st, &vmem.st, sizeof before.st) &&
	      !memcmp(&before.scratch, &vmem.scratch, sizeof before.scratch),
	      "the callback touched the codec while lent");
	CHECK(vs.rx_tail == vs.rx_head && !spk_on, "frames kept, or the speaker on, while lent");
	// Back: the call plays again once primed, from a fresh decoder
	m17_voice_scratch_return();
	c2_dec_init(&ref_dec, C2_MODE_3200);
	m17_voice_rx_frame(frames[2], 0);
	CHECK(!block(), "played before priming again");
	m17_voice_rx_frame(frames[3], 0);
	CHECK(stream_eq(C2_MODE_3200, frames[2], false), "the call after the return is not from a fresh decoder");
	m17_voice_stats(&s);
	CHECK(s.rx_calls == 1, "calls %u", (unsigned)s.rx_calls);
	// Refused while the call plays: nothing posted, the callback never lends it
	CHECK(m17_voice_scratch_borrow(&len) == NULL, "lent while a call plays");
	CHECK((vs.lend_req & 1u) == 0, "a request posted while a call plays");
	m17_voice_rx_end();
	block();
	block();
	CHECK(!block() && !spk_on, "the call did not end");
	// A request the callback has not answered yet: a transmission withdraws it, the encoder runs
	CHECK(m17_voice_scratch_borrow(&len) == NULL, "granted at once");
	m17_voice_tx(true, C2_MODE_3200);
	mic_blocks(2, 140, 3000, true);
	m17_voice_stats(&s);
	CHECK(s.tx_encoded == 2 && (vs.lend_req & 1u) == 0, "the transmission was held by a withdrawn request");
	// Refused while the encoder runs
	CHECK(m17_voice_scratch_borrow(&len) == NULL && (vs.lend_req & 1u) == 0, "lent while encoding");
	m17_voice_tx(false, C2_MODE_3200);
	block();
	CHECK(vs.lend_ack != vs.lend_req || (vs.lend_req & 1u) == 0, "a withdrawn request was lent later");
	// Refused while the bench runs; and a request the callback answered but nobody took is withdrawn by the bench
	m17_voice_bench(M17_VOICE_BENCH_DEC, C2_MODE_3200);
	CHECK(m17_voice_scratch_borrow(&len) == NULL && (vs.lend_req & 1u) == 0, "lent while the bench runs");
	m17_voice_bench(M17_VOICE_BENCH_OFF, C2_MODE_3200);
	block();
	CHECK(m17_voice_scratch_borrow(&len) == NULL, "granted at once");
	block();	// answered
	m17_voice_bench(M17_VOICE_BENCH_ENC, C2_MODE_3200);
	block();
	m17_voice_stats(&s);
	CHECK(s.enc_blocks == 3 && (vs.lend_req & 1u) == 0, "the bench was held by an answered request (%u)",
	      (unsigned)s.enc_blocks);
	m17_voice_bench(M17_VOICE_BENCH_OFF, C2_MODE_3200);
	// A scratch handed out stays lent through a transmission (the caller broke the rule): nothing encodes, nothing
	// is corrupted
	block();
	CHECK(m17_voice_scratch_borrow(&len) == NULL, "granted at once");
	block();
	CHECK(m17_voice_scratch_borrow(&len) == p, "not granted");
	m17_voice_tx(true, C2_MODE_3200);
	m17_voice_reset_stats();
	mic_blocks(2, 140, 3000, true);
	m17_voice_stats(&s);
	CHECK(s.tx_encoded == 0, "encoded on a lent scratch");
	m17_voice_scratch_return();
	mic_blocks(2, 140, 3000, true);
	m17_voice_stats(&s);
	CHECK(s.tx_encoded == 2, "the encoder did not resume after the return");
	m17_voice_tx(false, C2_MODE_3200);
	// A return, then a new request before the callback saw the return: answered again, not taken as the old answer
	block();
	CHECK(m17_voice_scratch_borrow(&len) == NULL, "granted at once");
	block();
	CHECK(m17_voice_scratch_borrow(&len) == p, "not granted");
	m17_voice_scratch_return();
	CHECK(m17_voice_scratch_borrow(&len) == NULL, "granted on the old answer");
	block();
	CHECK(m17_voice_scratch_borrow(&len) == p, "the new request was not answered");
	m17_voice_scratch_return();
	m17_voice_end();
}

// ── The codec bench ──
static void testBench(void)
{
	m17_voice_stats_t s;
	c2_params_t got, want;

	begin();
	m17_voice_bench(M17_VOICE_BENCH_ENC, C2_MODE_3200);
	CHECK(running, "the bench did not start the stream");
	sig_n = 0;
	mic_blocks(5, 100, 4000, false);
	CHECK(!spk_on, "the speaker is on in the bench");
	m17_voice_stats(&s);
	CHECK(s.enc_blocks == 5 && s.enc_us_max == 7 && s.enc_us_sum == 35, "enc bench: %u blocks, max %u sum %u",
	      (unsigned)s.enc_blocks, (unsigned)s.enc_us_max, (unsigned)s.enc_us_sum);
	CHECK(s.dec_blocks == 0 && s.dec_us_max == 0 && s.tx_encoded == 0 && s.tx_frames == 0 && s.rx_frames == 0,
	      "the encode bench filled other stats");
	// The decode phase: the fixed frames in turn from a fresh decoder, the speaker silent
	for (c2_mode_t mode = C2_MODE_3200; mode <= C2_MODE_1600; mode++) {
		const uint8_t (*seq)[C2_FRAME_BYTES] = mode == C2_MODE_1600 ? bench_1600 : bench_3200;
		int blocks = mode == C2_MODE_1600 ? 2 * BENCH_FRAMES + 4 : BENCH_FRAMES + 2;
		bool same = true;

		m17_voice_reset_stats();
		m17_voice_bench(M17_VOICE_BENCH_DEC, mode);
		c2_dec_init(&ref_dec, mode);
		for (int i = 0; i < blocks; i++) {
			int16_t want_pcm[AUDIO_BLOCK_SAMPLES];
			bool frame = mode == C2_MODE_3200 || (i & 1) == 0;
			int k = (mode == C2_MODE_3200 ? i : i / 2) % BENCH_FRAMES;

			ref_block(want_pcm, seq[k], frame, false);
			CHECK(!block(), "sound in the decode bench");
			for (int j = 0; j < 2; j++) {
				c2_dec_params(&vmem.st.dec, (uint8_t)j, &got, NULL);
				c2_dec_params(&ref_dec, (uint8_t)j, &want, NULL);
				same &= got.wo == want.wo && got.e == want.e && got.voiced == want.voiced && got.L == want.L;
			}
		}
		CHECK(same, "decode bench %d: not the frame sequence from a fresh decoder", (int)mode);
		m17_voice_stats(&s);
		CHECK(s.dec_blocks == (uint32_t)blocks && s.dec_us_max == 7 && s.dec_us_sum == 7u * blocks,
		      "dec bench %d: %u blocks, max %u", (int)mode, (unsigned)s.dec_blocks, (unsigned)s.dec_us_max);
		CHECK(s.enc_blocks == 0 && s.rx_frames == 0 && s.rx_calls == 0, "the decode bench filled other stats");
		CHECK(!spk_on, "the speaker is on in the decode bench");
	}
	// Changing phase starts the state over: encode after decode gives what a fresh encoder gives (its frames are
	// dropped; its state equals the reference's after the same blocks, both laid over the same bytes)
	memset(&vmem.st, 0x5A, sizeof vmem.st);
	memset(&ref_enc, 0x5A, sizeof ref_enc);
	m17_voice_bench(M17_VOICE_BENCH_ENC, C2_MODE_1600);
	sig_n = 0;
	mic_blocks(4, 100, 4000, false);
	ref_tx(C2_MODE_1600, 2, 100, 4000);
	CHECK(!memcmp(&vmem.st.enc, &ref_enc, sizeof ref_enc), "the encode bench did not start from a fresh encoder");
	// Received frames during the bench are dropped, nothing plays; off returns to reception
	m17_voice_rx_start(C2_MODE_3200);
	make_frames(C2_MODE_3200, 4, 150);
	m17_voice_rx_frame(frames[0], 0);
	m17_voice_rx_frame(frames[1], 0);
	CHECK(!block() && vs.rx_tail == vs.rx_head, "a call played during the bench");
	m17_voice_bench(M17_VOICE_BENCH_OFF, C2_MODE_3200);
	c2_dec_init(&ref_dec, C2_MODE_3200);
	m17_voice_rx_frame(frames[2], 0);
	m17_voice_rx_frame(frames[3], 0);
	CHECK(stream_eq(C2_MODE_3200, frames[2], false), "reception after the bench is not from a fresh decoder");
	// No clock: blocks counted, no time
	m17_voice_init(&audio, NULL);
	m17_voice_reset_stats();
	m17_voice_bench(M17_VOICE_BENCH_DEC, C2_MODE_1600);
	block();
	block();
	m17_voice_stats(&s);
	CHECK(s.dec_blocks == 2 && s.dec_us_max == 0 && s.dec_us_sum == 0, "no clock: %u blocks, max %u",
	      (unsigned)s.dec_blocks, (unsigned)s.dec_us_max);
	m17_voice_bench(M17_VOICE_BENCH_OFF, C2_MODE_1600);
	m17_voice_init(&audio, fake_clock);
	m17_voice_end();
	// Not running: nothing
	m17_voice_bench(M17_VOICE_BENCH_ENC, C2_MODE_3200);
	CHECK(!running && vs.bench_ctl == 0, "the bench ran with M17 off");
}

// ── The mixer and the speaker route ──
static bool mixing;
static bool mix(int16_t *spk)
{
	if (mixing)
		spk[0] = 1000;
	return mixing;
}

static void testMixer(void)
{
	begin();
	m17_voice_set_mixer(mix);
	mixing = true;
	CHECK(block() && spk_on, "the mixer's sound is not routed");
	mixing = false;
	CHECK(!block() && !spk_on, "the route stayed on after the mixer");
	// A beep during the bench is heard, and the route goes off after it
	m17_voice_bench(M17_VOICE_BENCH_DEC, C2_MODE_3200);
	mixing = true;
	CHECK(block() && spk_on, "the mixer is not heard in the bench");
	mixing = false;
	CHECK(!block() && !spk_on, "the route stayed on after a beep in the bench");
	m17_voice_bench(M17_VOICE_BENCH_OFF, C2_MODE_3200);
	// A beep that ends in the block where a call is first seen: the route goes off with it (the call has not primed)
	mixing = true;
	CHECK(block() && spk_on, "the mixer is not heard");
	m17_voice_rx_start(C2_MODE_3200);
	mixing = false;
	CHECK(!block() && !spk_on, "the route stayed on after a beep that ended as a call started");
	m17_voice_rx_end();
	block();
	CHECK(!spk_on, "the route came on for an empty call");
	// A playing call cut off by a new one while a beep plays: the route stays on through the block, not off and on
	// again under the beep, and the beep's end turns it off
	make_frames(C2_MODE_3200, 4, 180);
	m17_voice_rx_start(C2_MODE_3200);
	for (int i = 0; i < 4; i++)
		m17_voice_rx_frame(frames[i], 0);
	bool played = false;
	for (int i = 0; i < 3; i++)
		played |= block();
	CHECK(played && spk_on, "the call does not play");
	mixing = true;
	block();
	int calls = spk_calls;
	m17_voice_rx_start(C2_MODE_3200);
	block();
	CHECK(spk_on && spk_calls == calls, "the route toggled under a beep as a new call cut one off (%d calls)",
	      spk_calls - calls);
	mixing = false;
	block();
	CHECK(!spk_on, "the route stayed on after the beep");
	m17_voice_rx_end();
	m17_voice_set_mixer(NULL);
	m17_voice_end();
	// A beep's own stream left the speaker route on and the transmission took the running stream over: the first
	// block gives the route back
	begin();
	spk_on = true;
	m17_voice_tx(true, C2_MODE_3200);
	block();
	CHECK(!spk_on, "the beep's route left on after the takeover");
	m17_voice_tx(false, C2_MODE_3200);
	m17_voice_end();
}

// ── Speech through both directions, timed on the host ──
static uint32_t host_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000000u + ts.tv_nsec / 1000);
}

static void testSpeech(const char *path)
{
	FILE *fp = fopen(path, "rb");
	static int16_t pcm[1000 * AUDIO_BLOCK_SAMPLES];
	static uint8_t tx[500][M17_STREAM_BYTES];
	size_t n = 0;
	uint8_t b2[2];

	if (fp == NULL) {
		CHECK(false, "can't read %s", path);
		return;
	}
	while (n < sizeof pcm / sizeof pcm[0] && fread(b2, 1, 2, fp) == 2)
		pcm[n++] = (int16_t)(b2[0] | b2[1] << 8);
	fclose(fp);
	size_t blocks = n / AUDIO_BLOCK_SAMPLES & ~(size_t)1;
	m17_voice_init(&audio, host_us);
	for (c2_mode_t mode = C2_MODE_3200; mode <= C2_MODE_1600; mode++) {
		const char *mn = mode == C2_MODE_3200 ? "3200" : "1600";
		m17_voice_stats_t s;
		size_t nf = 0, bad_tx = 0, bad_rx = 0;
		int16_t spk[AUDIO_BLOCK_SAMPLES];

		// Transmit: the engine takes each stream frame as it is made; the same bits as the codec run directly
		begin();
		m17_voice_tx(true, mode);
		c2_enc_init(&ref_enc, mode);
		uint8_t want[M17_STREAM_BYTES] = { 0 }, bits[C2_FRAME_BYTES];
		for (size_t b = 0; b < blocks; b++) {
			m17_voice_audio_block(pcm + b * AUDIO_BLOCK_SAMPLES, spk, NULL);
			if (c2_enc_block(&ref_enc, &ref_scr, pcm + b * AUDIO_BLOCK_SAMPLES, bits))
				memcpy(want + (mode == C2_MODE_3200 && (b & 1) ? C2_FRAME_BYTES : 0), bits, C2_FRAME_BYTES);
			if (b & 1) {
				if (!m17_voice_tx_frame(tx[nf]) || memcmp(tx[nf], want, sizeof want))
					bad_tx++;
				nf++;
				memset(want, 0, sizeof want);
			}
		}
		m17_voice_tx(false, mode);
		m17_voice_stats(&s);
		CHECK(bad_tx == 0 && nf == blocks / 2, "%s speech: %zu of %zu stream frames not the codec's", mn, bad_tx, nf);
		printf("# m17VoiceTest: %s encode through the voice path: %u blocks, %.1f us mean, %u us max per block\n", mn,
		       (unsigned)s.enc_blocks, s.enc_blocks ? (double)s.enc_us_sum / s.enc_blocks : 0.0,
		       (unsigned)s.enc_us_max);
		m17_voice_end();
		// Receive: one stream frame per two blocks, steady; the same PCM as the codec run directly
		begin();
		m17_voice_rx_start(mode);
		m17_voice_poll();
		c2_dec_init(&ref_dec, mode);
		size_t in = 0;
		for (; in < M17_VOICE_PRIME - 1; in++)
			m17_voice_rx_frame(tx[in], 0);
		for (size_t b = 0; b < 2 * nf; b++) {
			if ((b & 1) == 0 && in < nf)
				m17_voice_rx_frame(tx[in++], 0);
			if (in == nf)
				m17_voice_rx_end();
			int16_t ref[AUDIO_BLOCK_SAMPLES];
			const uint8_t *f = tx[b / 2];
			ref_block(ref, mode == C2_MODE_3200 && (b & 1) ? f + C2_FRAME_BYTES : f,
				  mode == C2_MODE_3200 || (b & 1) == 0, false);
			block();
			if (memcmp(ref, out, sizeof ref))
				bad_rx++;
		}
		CHECK(!block() && !spk_on, "%s speech: the call did not end", mn);
		m17_voice_stats(&s);
		CHECK(bad_rx == 0 && s.rx_underruns == 0 && s.rx_overflows == 0, "%s speech: %zu blocks differ, %u underruns",
		      mn, bad_rx, (unsigned)s.rx_underruns);
		CHECK(s.rx_peak > 3000, "%s speech: peak %u", mn, (unsigned)s.rx_peak);
		printf("# m17VoiceTest: %s decode through the voice path: %u blocks, %.1f us mean, %u us max per block\n", mn,
		       (unsigned)s.dec_blocks, s.dec_blocks ? (double)s.dec_us_sum / s.dec_blocks : 0.0,
		       (unsigned)s.dec_us_max);
		m17_voice_end();
	}
	m17_voice_init(&audio, fake_clock);
}

int main(int argc, char **argv)
{
	m17_voice_init(&audio, fake_clock);
	testConstants();
	testPlayback(C2_MODE_3200);
	testPlayback(C2_MODE_1600);
	testBadFirstAndOverflow();
	testCalls();
	testTransmit(C2_MODE_3200);
	testTransmit(C2_MODE_1600);
	testTransmitEdge();
	testTxDuringCall();
	testBorrowStopped();
	testBorrowRunning();
	testBench();
	testMixer();
	if (argc > 1)
		testSpeech(argv[1]);
	printf("m17VoiceTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
