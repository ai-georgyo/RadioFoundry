// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 voice: engine stream frames <-> Codec 2 <-> PCM blocks (m17_voice.h; docs/spec/m17.md, the voice path)
#include "config.h"
#if CONFIG_M17	// the whole file
#include "m17/m17_voice.h"
#include "m17/m17_engine.h"

#include <string.h>

#if (M17_VOICE_QUEUE & (M17_VOICE_QUEUE - 1)) != 0 || M17_VOICE_QUEUE > 128 || M17_VOICE_QUEUE <= M17_VOICE_PRIME
#error "M17_VOICE_QUEUE must be a power of two up to 128, above M17_VOICE_PRIME"
#endif
#if (M17_VOICE_TX_QUEUE & (M17_VOICE_TX_QUEUE - 1)) != 0 || M17_VOICE_TX_QUEUE > 128
#error "M17_VOICE_TX_QUEUE must be a power of two up to 128"
#endif
// A stream frame is two codec frames (3200) or one and its data half (1600); a block is a 3200 frame
_Static_assert(M17_STREAM_BYTES == 2 * C2_FRAME_BYTES && AUDIO_BLOCK_SAMPLES == C2_BLOCK_SAMPLES,
	       "a stream frame is two codec frames, a block one 3200 frame");
// A call's sequence carries its mode in bit 0 (rx_start)
_Static_assert(C2_MODE_3200 == 0 && C2_MODE_1600 == 1, "the codec modes are 0 and 1");

#define LOAD(x)		__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

#define BENCH_PHASE(b)	((uint8_t)((b) & 3u))
#define BENCH_MODE(b)	((c2_mode_t)(((b) >> 2) & 1u))

static const hal_audio_t *aud;
static uint32_t (*clock_us)(void);
static m17_voice_stats_t stats;
static m17_voice_mixer_t mixer;
static uint16_t mic_gain_q8 = 256;	// m17_voice_set_mic_gain: the mic samples times this / 256 before encoding

// vs.cb_primed: a new call (zero, as m17_voice_begin leaves it), the same call priming again after the encoder or the
// bench had the decoder (counted already), primed
enum { PRIME_NEW, PRIME_AGAIN, PRIME_DONE };

// What one run of M17 keeps (m17_voice_begin zeroes it), in one struct: each separate static would cost the code an
// address of its own. Who writes each field is said beside it
static struct {
	m17_voice_mem_t *mem;		// radio task: set before the stream starts, cleared after it stops
	bool streaming;			// radio task only

	// Receive queue: producer the provider's task (head), consumer the audio callback (tail). Call boundaries are
	// written by the provider's task only: a start steps the sequence by 2 with the call's codec mode in bit 0, an
	// end copies it
	uint8_t rx_head, rx_tail;
	uint8_t rx_seq, rx_end_seq;

	// Transmit. The radio task counts transmissions (odd while one is on) and sets the mode before the count; the
	// queue's producer is the audio callback (head), its consumer the provider's task (tail). Frames of an earlier
	// transmission are dropped by their count
	uint8_t tx_gen, tx_mode;
	uint8_t tx_head, tx_tail;

	// The scratch's loan. The radio task steps lend_req to odd to ask and back to even to return; the callback copies
	// an odd lend_req to lend_ack once it has stopped using the codec (lent while the two are equal). lent_out: the
	// radio task holds the pointer (radio task only)
	uint8_t lend_req, lend_ack;
	bool lent_out;

	// The bench: phase | mode << 2 (radio task)
	uint8_t bench_ctl;

	// Audio callback state
	uint8_t cb_tx_gen;		// the transmission the encoder runs for (even: none)
	uint8_t cb_tx_mode;
	bool cb_tx_half;		// 3200: the stream frame's first codec frame is in ...
	bool cb_tx_room;		// ... the head slot (false: the queue was full, the frame is dropped)
	uint8_t cb_seq;			// the call the decoder runs for
	bool cb_live, cb_playing, cb_speaker, cb_mixing;
	uint8_t cb_primed;		// PRIME_*: a call that primes again after the encoder or the bench is counted once
	bool cb_half;			// the stream frame at the tail (or a lost 1600 frame) has its second block to play
	bool cb_lost;			// ... and it is a lost 1600 frame's
	bool cb_dec_stale;		// the encoder or the bench used the state: the decoder starts over
	// The stream just (re)started: the speaker route is what its last user left, which may be a beep's own stream
	// that had it on. The first block sends cb_speaker whatever it was, or the route (and the amplifier) stays on
	bool cb_speaker_unsent;
	uint8_t cb_bench;		// the bench phase and mode the state is set up for (0: none)
	uint8_t cb_bench_i;		// the decode phase's next frame ...
	bool cb_bench_half;		// ... and 1600: the frame given last has its second block to play
} vs;

// 16 STREAM bytes of silence: c2_silence_3200 twice; c2_silence_1600 and the data half zero (m17VoiceTest checks them
// against the codec's)
static const uint8_t silence_3200[M17_STREAM_BYTES] = {
	0x01, 0x00, 0x09, 0x43, 0x9C, 0xE4, 0x21, 0x08, 0x01, 0x00, 0x09, 0x43, 0x9C, 0xE4, 0x21, 0x08,
};
static const uint8_t silence_1600[M17_STREAM_BYTES] = {
	0x01, 0x00, 0x04, 0x00, 0x25, 0x75, 0xDD, 0xF2, 0, 0, 0, 0, 0, 0, 0, 0,
};

// The decode bench's frames, played in turn: silence, then voiced and unvoiced speech as this tree's encoder made them
// from synthetic input (a 100 Hz and a 220 Hz vowel, each starting and steady, and a hiss; m17VoiceTest checks each
// one's voicing). The low vowel has the most harmonics, though the decoder's cost hardly depends on that
// (docs/spec/codec2.md 17.6)
#define BENCH_FRAMES	8
static const uint8_t bench_3200[BENCH_FRAMES][C2_FRAME_BYTES] = {
	{ 0x01, 0x00, 0x09, 0x43, 0x9C, 0xE4, 0x21, 0x08 },	// silence
	{ 0xCD, 0xD9, 0x8D, 0x73, 0x5E, 0xE4, 0x27, 0x2B },	// 100 Hz vowel, starting: voiced
	{ 0xCD, 0xC5, 0x8D, 0x73, 0x5E, 0xE4, 0x27, 0x2B },	// ... steady
	{ 0xD0, 0xC9, 0xCC, 0x73, 0x5E, 0xFC, 0xAF, 0x29 },	// 220 Hz vowel, starting
	{ 0xD0, 0xC1, 0xCC, 0x73, 0x5E, 0xFC, 0xAF, 0x29 },	// ... steady
	{ 0x19, 0x47, 0x0F, 0xC3, 0xDA, 0xA5, 0x25, 0x2A },	// hiss: unvoiced
	{ 0x0F, 0xC1, 0x78, 0x73, 0xDC, 0xE7, 0x2F, 0x6F },
	{ 0x02, 0x45, 0x6A, 0x5A, 0x9C, 0xC5, 0xE5, 0x89 },
};
static const uint8_t bench_1600[BENCH_FRAMES][C2_FRAME_BYTES] = {
	{ 0x01, 0x00, 0x04, 0x00, 0x25, 0x75, 0xDD, 0xF2 },	// silence
	{ 0xC0, 0x67, 0x37, 0x60, 0x55, 0x67, 0x44, 0x57 },	// 100 Hz vowel, starting: voiced
	{ 0xCD, 0xC7, 0x37, 0x10, 0x55, 0x67, 0x44, 0x57 },	// ... steady
	{ 0xD0, 0xEF, 0x43, 0x22, 0xC4, 0x67, 0x4C, 0x56 },	// 220 Hz vowel, starting
	{ 0xD0, 0xC3, 0x43, 0x02, 0xC4, 0x67, 0x4C, 0x56 },	// ... steady
	{ 0x00, 0x64, 0x21, 0x64, 0x8A, 0xCC, 0xEF, 0xB2 },	// hiss, starting: unvoiced
	{ 0x0F, 0xC0, 0x35, 0x14, 0x8A, 0x4D, 0xEF, 0x92 },	// ... steady
	{ 0x1D, 0xC5, 0x17, 0x17, 0x8A, 0xCD, 0xEE, 0xB2 },	// ... its last 10 ms frame voiced
};

// --- the engine's hooks (provider's task) ---

void m17_voice_rx_start(c2_mode_t mode)
{
	STORE(vs.rx_seq, (uint8_t)(((vs.rx_seq + 2u) & 0xFEu) | ((unsigned)mode & 1u)));
}

void m17_voice_rx_frame(const uint8_t stream[M17_STREAM_BYTES], uint8_t errs)
{
	if (LOAD(vs.mem) == NULL) {
		return;
	}
	if (vs.rx_end_seq == vs.rx_seq) {
		m17_voice_rx_start((c2_mode_t)(vs.rx_seq & 1u));	// frames with no call open: a new call, in the last mode
	}
	stats.rx_in++;
	if (errs == M17_ERRS_BAD) {
		stats.rx_bad++;
	}
	uint8_t h = vs.rx_head;
	if ((uint8_t)(h - LOAD(vs.rx_tail)) >= M17_VOICE_QUEUE) {
		stats.rx_overflows++;
		return;
	}
	m17_voice_entry_t *e = &vs.mem->q[h & (M17_VOICE_QUEUE - 1)];
	e->seq = vs.rx_seq;
	e->errs = errs;
	memcpy(e->stream, stream, M17_STREAM_BYTES);
	STORE(vs.rx_head, (uint8_t)(h + 1));
}

void m17_voice_rx_end(void)
{
	STORE(vs.rx_end_seq, vs.rx_seq);
}

#if CONFIG_NET_M17
bool m17_voice_rx_open(void)
{
	return LOAD(vs.rx_end_seq) != LOAD(vs.rx_seq);
}
#endif

bool m17_voice_tx_frame(uint8_t stream[M17_STREAM_BYTES])
{
	if (LOAD(vs.mem) == NULL) {
		return false;
	}
	uint8_t g = LOAD(vs.tx_gen), t = vs.tx_tail, h = LOAD(vs.tx_head);

	while (t != h && vs.mem->tq[t & (M17_VOICE_TX_QUEUE - 1)].gen != g) {
		t++;	// left over from an earlier transmission
	}
	if (t == h) {
		STORE(vs.tx_tail, t);
		return false;	// the engine sends m17_voice_silence and counts filler
	}
	memcpy(stream, vs.mem->tq[t & (M17_VOICE_TX_QUEUE - 1)].stream, M17_STREAM_BYTES);
	STORE(vs.tx_tail, (uint8_t)(t + 1));
	stats.tx_frames++;
	return true;
}

const uint8_t *m17_voice_silence(c2_mode_t mode)
{
	return mode == C2_MODE_1600 ? silence_1600 : silence_3200;
}

// --- audio callback (audio task) ---

static void speaker(bool on)
{
	if (vs.cb_speaker != on || LOAD(vs.cb_speaker_unsent)) {
		vs.cb_speaker = on;
		STORE(vs.cb_speaker_unsent, false);
		if (aud != NULL && aud->speaker_pcm != NULL) {
			aud->speaker_pcm(on);
		}
	}
}

static void silence(int16_t *spk)
{
	memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
}

static uint32_t t_start(void)
{
	return clock_us != NULL ? clock_us() : 0;
}

// One timed codec block into a direction's counters
static void t_end(uint32_t t0, uint32_t *max, uint32_t *sum, uint32_t *blocks)
{
	if (clock_us != NULL) {
		uint32_t dt = clock_us() - t0;

		if (dt > *max) {
			*max = dt;
		}
		*sum += dt;
	}
	(*blocks)++;
}

static void dec_timed_end(uint32_t t0)
{
	t_end(t0, &stats.dec_us_max, &stats.dec_us_sum, &stats.dec_blocks);
}

static void enc_timed_end(uint32_t t0)
{
	t_end(t0, &stats.enc_us_max, &stats.enc_us_sum, &stats.enc_blocks);
}

static void peak(const int16_t *spk)
{
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		uint32_t a = (uint32_t)(spk[i] < 0 ? -spk[i] : spk[i]);

		if (a > stats.rx_peak) {
			stats.rx_peak = a;
		}
	}
}

// Drop what is queued (frames nobody will play)
static void rx_drop(void)
{
	STORE(vs.rx_tail, LOAD(vs.rx_head));
}

// The call stops playing; the stream frame half played is dropped. The speaker route goes off with it unless a beep
// holds it: the mixer's end turns it off then, and an off here would toggle the amplifier under the beep within one
// block (the mixer turns it on again after rx_block)
static void rx_stop_playing(void)
{
	if (vs.cb_half && !vs.cb_lost) {
		STORE(vs.rx_tail, (uint8_t)(vs.rx_tail + 1));
	}
	vs.cb_half = vs.cb_lost = false;
	if (vs.cb_playing) {
		STORE(vs.cb_playing, false);
		if (!vs.cb_mixing) {
			speaker(false);
		}
	}
}

static void rx_block(int16_t *spk)
{
	c2_dec_state_t *dec = &vs.mem->st.dec;
	// The provider writes a call's start, then its frames, then its end; read in the reverse order, every frame
	// counted here belongs to the call read (or an earlier one), and an end read has all of its call's frames in
	uint8_t end_seq = LOAD(vs.rx_end_seq), h = LOAD(vs.rx_head), seq = LOAD(vs.rx_seq);

	if (seq != vs.cb_seq) {
		// A new call: the decoder starts over in its mode (what is left of the last call is dropped below). The
		// speaker route may be off (an end, a stop): sent again when the call plays. Not while a beep holds it: the
		// beep's end turns it off
		rx_stop_playing();
		vs.cb_seq = seq;
		vs.cb_live = true;
		vs.cb_primed = PRIME_NEW;
		vs.cb_half = vs.cb_lost = vs.cb_dec_stale = false;
		if (!vs.cb_mixing) {
			vs.cb_speaker = false;
		}
		c2_dec_init(dec, (c2_mode_t)(seq & 1u));
	} else if (vs.cb_dec_stale) {
		// The encoder or the bench had the state: a call still coming in primes again from a decoder at its start
		vs.cb_dec_stale = false;
		rx_stop_playing();
		if (vs.cb_primed == PRIME_DONE) {
			vs.cb_primed = PRIME_AGAIN;
		}
		c2_dec_init(dec, (c2_mode_t)(seq & 1u));
	}
	// Drop entries of earlier calls
	uint8_t t = vs.rx_tail;
	while (t != h && vs.mem->q[t & (M17_VOICE_QUEUE - 1)].seq != vs.cb_seq) {
		t++;
	}
	STORE(vs.rx_tail, t);

	bool ended = end_seq == vs.cb_seq;
	bool m1600 = (vs.cb_seq & 1u) != 0;
	uint8_t queued = (uint8_t)(h - t);
	if (!vs.cb_live) {
		silence(spk);
		return;
	}
	if (vs.cb_primed != PRIME_DONE) {
		// Frames come one per 40 ms, steady: M17_VOICE_PRIME in hand covers the provider's jitter. A call that ends
		// with fewer plays what it has
		if (queued < M17_VOICE_PRIME && !(ended && queued > 0)) {
			if (ended) {
				vs.cb_live = false;	// a call that ended before playing anything
			}
			silence(spk);
			return;
		}
		STORE(vs.cb_playing, true);	// the radio task reads it (m17_voice_scratch_borrow)
		if (vs.cb_primed == PRIME_NEW) {
			// Once a call: one that primes again after a transmission or the bench is the same call, its peak kept
			stats.rx_calls++;
			stats.rx_peak = 0;	// the latest call that played: one that never does leaves it
		}
		vs.cb_primed = PRIME_DONE;
		speaker(true);
	}
	uint32_t t0;
	if (vs.cb_half) {
		// The stream frame's second block: 3200 its second codec frame, 1600 the frame's second half
		vs.cb_half = false;
		t0 = t_start();
		if (vs.cb_lost) {
			vs.cb_lost = false;
			c2_dec_block(dec, &vs.mem->scratch, spk);
			dec_timed_end(t0);
			stats.rx_underruns++;
			stats.rx_concealed++;
			return;
		}
		const m17_voice_entry_t *e = &vs.mem->q[t & (M17_VOICE_QUEUE - 1)];
		bool bad = e->errs == M17_ERRS_BAD;
		if (!m1600) {
			c2_dec_frame(dec, e->stream + C2_FRAME_BYTES, bad);
		}
		c2_dec_block(dec, &vs.mem->scratch, spk);
		dec_timed_end(t0);
		STORE(vs.rx_tail, (uint8_t)(t + 1));
		if (bad) {
			stats.rx_concealed++;
			return;
		}
		if (!m1600) {
			stats.rx_frames++;
		}
		peak(spk);
		return;
	}
	if (queued == 0) {
		if (ended) {
			vs.cb_live = false;
			STORE(vs.cb_playing, false);
			speaker(false);
			silence(spk);
			return;
		}
		// Nothing in time: the decoder's lost-frame path (hold the last parameters, fading, then mute). 3200 looks
		// again next block; a lost 1600 frame is two blocks long
		t0 = t_start();
		c2_dec_frame(dec, NULL, true);
		c2_dec_block(dec, &vs.mem->scratch, spk);
		dec_timed_end(t0);
		stats.rx_underruns++;
		stats.rx_concealed++;
		vs.cb_half = vs.cb_lost = m1600;
		return;
	}
	// A stream frame's first block. It stays at the tail until its second block has played
	const m17_voice_entry_t *e = &vs.mem->q[t & (M17_VOICE_QUEUE - 1)];
	bool bad = e->errs == M17_ERRS_BAD;
	t0 = t_start();
	c2_dec_frame(dec, e->stream, bad);
	c2_dec_block(dec, &vs.mem->scratch, spk);
	dec_timed_end(t0);
	vs.cb_half = true;
	if (bad) {
		stats.rx_concealed++;
		return;
	}
	stats.rx_frames++;
	peak(spk);
}

// One mic block into the transmission g: encoded, and a stream frame queued for the engine once complete
static void tx_block(uint8_t g, const int16_t *mic)
{
	int16_t scaled[AUDIO_BLOCK_SAMPLES];
	uint8_t bits[C2_FRAME_BYTES];
	uint16_t gain = LOAD(mic_gain_q8);
	uint32_t pk = 0;

	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		int32_t v = gain == 256 ? mic[i] : ((int32_t)mic[i] * gain) / 256;

		v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
		scaled[i] = (int16_t)v;
		if ((uint32_t)(v < 0 ? -v : v) > pk) {
			pk = (uint32_t)(v < 0 ? -v : v);
		}
	}
	if (pk > stats.tx_mic_peak) {
		stats.tx_mic_peak = pk;
	}
	uint32_t t0 = t_start();
	bool done = c2_enc_block(&vs.mem->st.enc, &vs.mem->scratch, scaled, bits);
	enc_timed_end(t0);
	stats.tx_encoded++;
	if (!done) {
		return;		// 1600: the frame's first half
	}
	// The slot at the head is the producer's until it is published: the first 3200 frame waits in it for the second.
	// A full queue (the engine not taking frames yet, or ending) drops the stream frame; the encoder keeps running
	uint8_t h = vs.tx_head;
	m17_voice_tx_entry_t *e = &vs.mem->tq[h & (M17_VOICE_TX_QUEUE - 1)];
	bool room = (uint8_t)(h - LOAD(vs.tx_tail)) < M17_VOICE_TX_QUEUE;
	if (vs.cb_tx_mode == C2_MODE_3200) {
		if (!vs.cb_tx_half) {
			vs.cb_tx_half = true;
			vs.cb_tx_room = room;
			if (room) {
				memcpy(e->stream, bits, C2_FRAME_BYTES);
			}
			return;
		}
		vs.cb_tx_half = false;
		room = vs.cb_tx_room;
	}
	if (!room) {
		stats.tx_dropped++;
		return;
	}
	if (vs.cb_tx_mode == C2_MODE_3200) {
		memcpy(e->stream + C2_FRAME_BYTES, bits, C2_FRAME_BYTES);
	} else {
		memcpy(e->stream, bits, C2_FRAME_BYTES);
		memset(e->stream + C2_FRAME_BYTES, 0, C2_FRAME_BYTES);	// the data half: nothing to send
	}
	e->gen = g;
	STORE(vs.tx_head, (uint8_t)(h + 1));
}

// One block of the codec bench: the codec's cost alone, nothing sent, nothing heard
static void bench_block(uint8_t b, const int16_t *mic, int16_t *spk)
{
	c2_mode_t mode = BENCH_MODE(b);
	uint8_t bits[C2_FRAME_BYTES];

	if (b != vs.cb_bench) {
		vs.cb_bench = b;
		vs.cb_bench_i = 0;
		vs.cb_bench_half = false;
		if (BENCH_PHASE(b) == M17_VOICE_BENCH_ENC) {
			c2_enc_init(&vs.mem->st.enc, mode);
		} else {
			c2_dec_init(&vs.mem->st.dec, mode);
		}
	}
	uint32_t t0 = t_start();
	if (BENCH_PHASE(b) == M17_VOICE_BENCH_ENC) {
		c2_enc_block(&vs.mem->st.enc, &vs.mem->scratch, mic, bits);
		enc_timed_end(t0);
		silence(spk);
		return;
	}
	// 3200 a frame each block; 1600 a frame every second block, as a call plays them
	if (!vs.cb_bench_half) {
		const uint8_t (*seq)[C2_FRAME_BYTES] = mode == C2_MODE_1600 ? bench_1600 : bench_3200;

		c2_dec_frame(&vs.mem->st.dec, seq[vs.cb_bench_i], false);
		vs.cb_bench_i = (uint8_t)((vs.cb_bench_i + 1u) % BENCH_FRAMES);
		vs.cb_bench_half = mode == C2_MODE_1600;
	} else {
		vs.cb_bench_half = false;
	}
	c2_dec_block(&vs.mem->st.dec, &vs.mem->scratch, spk);
	dec_timed_end(t0);
	silence(spk);
}

// The scratch's loan, at the top of the block: acknowledged only while idle (no call playing, nothing encoded, no
// bench). True while lent
static bool lent(uint8_t r, uint8_t g, uint8_t b)
{
	if ((r & 1u) == 0) {
		return false;
	}
	if (vs.lend_ack != r && !vs.cb_playing && (g & 1u) == 0 && BENCH_PHASE(b) == M17_VOICE_BENCH_OFF) {
		STORE(vs.lend_ack, r);
	}
	return vs.lend_ack == r;
}

void m17_voice_audio_block(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)ctx;
	if (LOAD(vs.mem) == NULL) {
		silence(spk);
		return;
	}
	uint8_t g = LOAD(vs.tx_gen), b = LOAD(vs.bench_ctl);

	if (lent(LOAD(vs.lend_req), g, b)) {
		// No codec state touched: silence, and what is received is dropped (a call starting now loses those frames)
		rx_drop();
		silence(spk);
	} else if ((g & 1u) != 0) {
		if (g != vs.cb_tx_gen) {
			vs.cb_tx_gen = g;
			vs.cb_tx_mode = LOAD(vs.tx_mode);
			vs.cb_tx_half = false;
			stats.tx_mic_peak = 0;
			c2_enc_init(&vs.mem->st.enc, (c2_mode_t)vs.cb_tx_mode);
		}
		// What was received before is stale once the transmission ends (a call still on air primes again on new
		// frames): dropped, as in the bench
		rx_stop_playing();
		rx_drop();
		vs.cb_dec_stale = true;
		vs.cb_bench = 0;		// the bench, if it runs after, starts over
		tx_block(g, mic);
		silence(spk);
	} else if (BENCH_PHASE(b) != M17_VOICE_BENCH_OFF) {
		vs.cb_tx_gen = g;
		rx_stop_playing();
		vs.cb_dec_stale = true;
		rx_drop();
		bench_block(b, mic, spk);
	} else {
		vs.cb_tx_gen = g;
		vs.cb_bench = 0;
		rx_block(spk);
	}
	m17_voice_mixer_t mix = LOAD(mixer);
	if (mix != NULL && mix(spk)) {
		vs.cb_mixing = true;
		speaker(true);
	} else if (vs.cb_mixing) {
		vs.cb_mixing = false;
		if (!vs.cb_playing) {
			speaker(false);
		}
	}
	if (LOAD(vs.cb_speaker_unsent)) {
		speaker(vs.cb_speaker);
	}
}

// --- radio task ---

static void stream_start(void)
{
	STORE(vs.cb_speaker_unsent, true);	// before the callback can run
	vs.streaming = aud->start(m17_voice_audio_block, NULL);
}

void m17_voice_init(const hal_audio_t *audio, uint32_t (*now_us)(void))
{
	aud = audio;
	clock_us = now_us;
}

void m17_voice_begin(m17_voice_mem_t *m)
{
	// The stream is stopped (m17_voice_end) and the engine not started: nothing else touches this state now. Zero is
	// every count, queue and flag at its start, and the 3200 mode
	memset(&vs, 0, sizeof(vs));
	STORE(vs.mem, m);
}

void m17_voice_poll(void)
{
	if (!vs.streaming && aud != NULL && LOAD(vs.mem) != NULL && LOAD(vs.rx_seq) != 0 && (vs.lend_req & 1u) == 0) {
		stream_start();
	}
}

// A request the callback has not answered, or answered with the pointer not yet taken, is withdrawn: nobody holds the
// scratch. One handed out stays lent (the callback stays silent) until it comes back
static void lend_withdraw(void)
{
	if ((vs.lend_req & 1u) != 0 && !vs.lent_out) {
		STORE(vs.lend_req, (uint8_t)(vs.lend_req + 1));
	}
}

void m17_voice_tx(bool on, c2_mode_t mode)
{
	if (LOAD(vs.mem) == NULL || on == ((vs.tx_gen & 1u) != 0)) {
		return;
	}
	if (on) {
		lend_withdraw();
		STORE(vs.tx_mode, (uint8_t)((unsigned)mode & 1u));
	}
	STORE(vs.tx_gen, (uint8_t)(vs.tx_gen + 1));
	if (on && !vs.streaming && aud != NULL) {
		stream_start();
	}
}

void m17_voice_set_mic_gain(uint8_t step)
{
	// 2 dB a step around unity at 5, as the other voice paths'
	static const uint16_t q8[11] = { 81, 102, 128, 161, 203, 256, 322, 406, 511, 643, 810 };

	STORE(mic_gain_q8, q8[step > 10 ? 10 : step]);
}

void m17_voice_end(void)
{
	if (vs.streaming && aud != NULL) {
		aud->stop();
		if (aud->speaker_pcm != NULL) {
			aud->speaker_pcm(false);
		}
	}
	vs.streaming = false;
	vs.lend_req = vs.lend_ack = 0;	// no callback runs now
	vs.lent_out = false;
	vs.bench_ctl = 0;
	STORE(vs.mem, (m17_voice_mem_t *)NULL);
}

bool m17_voice_streaming(void)
{
	return vs.streaming;
}

void m17_voice_set_mixer(m17_voice_mixer_t m)
{
	STORE(mixer, m);
}

void *m17_voice_scratch_borrow(size_t *len)
{
	m17_voice_mem_t *m = LOAD(vs.mem);

	if (m == NULL) {
		return NULL;
	}
	if (!vs.lent_out) {
		uint8_t r = vs.lend_req;

		if ((r & 1u) == 0) {
			if ((vs.tx_gen & 1u) != 0 || BENCH_PHASE(vs.bench_ctl) != M17_VOICE_BENCH_OFF ||
			    (vs.streaming && LOAD(vs.cb_playing))) {
				return NULL;
			}
			r = (uint8_t)(r + 1);
			if (!vs.streaming) {
				// No callback runs, and only this task starts one (m17_voice_poll waits for the return)
				vs.lend_ack = r;
			}
			STORE(vs.lend_req, r);
		}
		if (LOAD(vs.lend_ack) != r) {
			return NULL;	// the callback acknowledges at the top of its next block
		}
		vs.lent_out = true;
		stats.lent++;
	}
	if (len != NULL) {
		*len = sizeof(m->scratch);
	}
	return &m->scratch;
}

void m17_voice_scratch_return(void)
{
	if ((vs.lend_req & 1u) != 0) {
		STORE(vs.lend_req, (uint8_t)(vs.lend_req + 1));
	}
	vs.lent_out = false;
}

void m17_voice_bench(uint8_t phase, c2_mode_t mode)
{
	if (LOAD(vs.mem) == NULL) {
		return;
	}
	if (phase != M17_VOICE_BENCH_ENC && phase != M17_VOICE_BENCH_DEC) {
		STORE(vs.bench_ctl, (uint8_t)M17_VOICE_BENCH_OFF);
		return;
	}
	lend_withdraw();
	STORE(vs.bench_ctl, (uint8_t)(phase | (((unsigned)mode & 1u) << 2)));
	if (!vs.streaming && aud != NULL) {
		stream_start();
	}
}

void m17_voice_stats(m17_voice_stats_t *out)
{
	*out = stats;
}

void m17_voice_reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
}

#endif	// CONFIG_M17
