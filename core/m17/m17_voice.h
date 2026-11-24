// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * M17 voice between the engine (m17_engine.h: 16 STREAM bytes every 40 ms) and the board's PCM audio (hal_audio_t),
 * through Codec 2 (core/codec2). Modeled on the P25 voice path (core/p25/p25Voice.h): the codec runs only in the
 * audio block callback, PCM goes through hal_audio_t, no stock codec is needed. m17.md, the voice section.
 *
 *  RX: the engine's rx_voice hook queues each frame's STREAM bytes; the audio callback, every 20 ms, gives the
 *      decoder a codec frame when one is due (3200: one per block, two per stream frame; 1600: one per stream frame,
 *      its data half ignored) and synthesizes a block. Frames come one per 40 ms, steady, so playback starts once
 *      M17_VOICE_PRIME frames are queued. A frame the FEC could not trust, and a block with nothing queued during a
 *      call, go to the decoder as bad: it holds the last parameters, fading, then mutes.
 *  TX: while a transmission is on the callback encodes each mic block; the engine's tx_voice hook takes one stream
 *      frame's bytes at a time (two 3200 frames, or a 1600 frame and 8 zero bytes). With none ready it says so and
 *      the engine sends silence.
 *
 * Threading: m17_voice_begin, _poll, _tx and _end run in the radio core's task; the rx_* / tx_frame calls in the
 * provider's (from the engine's hooks); the codec only in the audio callback. Single producer / single consumer queues
 * and call counters as in p25Voice.c. No RTOS call; the clock for the codec's timing counters is the caller's (init).
 *
 * Memory is the caller's (m17_voice_mem_t, lent: radio_m17_mem.h). Its codec scratch is free between blocks while no
 * voice runs, and the radio core borrows it then for the signature work (m17_voice_scratch_borrow): the callback
 * plays silence and touches no codec state until it is returned.
 */
#ifndef RFOUNDRY_M17_VOICE_H
#define RFOUNDRY_M17_VOICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "hal_audio.h"
#include "m17.h"
#include "../codec2/codec2.h"

// Frames arrive one per 40 ms, in the provider's 30 ms chunks: 4 entries (160 ms) with 2 primed cover that jitter
#define M17_VOICE_QUEUE		4	// stream frames (160 ms), power of two
#define M17_VOICE_PRIME		2	// frames queued before playback starts (80 ms of margin)
#define M17_VOICE_TX_QUEUE	4	// encoded stream frames waiting for the engine (160 ms), power of two

typedef struct {
	uint8_t seq;		// the call it belongs to
	uint8_t errs;		// M17_ERRS_BAD (m17_engine.h): play as lost
	uint8_t stream[M17_STREAM_BYTES];
} m17_voice_entry_t;

typedef struct {
	uint8_t gen;		// the transmission it belongs to
	uint8_t stream[M17_STREAM_BYTES];
} m17_voice_tx_entry_t;

// Everything the voice path needs while M17 runs. A radio transmits or receives: the encoder's and the decoder's
// states share memory (the one that starts is initialized), and they share the scratch
typedef struct {
	m17_voice_entry_t q[M17_VOICE_QUEUE];
	m17_voice_tx_entry_t tq[M17_VOICE_TX_QUEUE];
	union {
		c2_enc_state_t enc;
		c2_dec_state_t dec;
	} st;
	c2_scratch_t scratch;
} m17_voice_mem_t;

typedef struct {
	uint32_t rx_in;		// frames the engine handed in
	uint32_t rx_frames;	// codec frames decoded and played
	uint32_t rx_bad;	// frames the FEC could not trust
	uint32_t rx_concealed;	// blocks the decoder filled from a bad or missing frame
	uint32_t rx_calls;	// calls that started playing
	uint32_t rx_underruns;	// blocks with nothing queued during a call
	uint32_t rx_overflows;	// frames dropped, queue full
	uint32_t rx_peak;	// largest decoded sample (absolute) in the latest call that played
	uint32_t tx_frames;	// stream frames handed to the engine
	uint32_t tx_encoded;	// mic blocks encoded
	uint32_t tx_dropped;	// encoded frames dropped, queue full
	uint32_t tx_mic_peak;	// largest mic sample (absolute, after the gain) in the latest transmission
	uint32_t enc_us_max, dec_us_max;	// the codec's longest block, microseconds (0 without a clock)
	uint32_t enc_us_sum, dec_us_sum;	// ... summed, with the block counts below: the mean
	uint32_t enc_blocks, dec_blocks;
	uint32_t lent;		// times the scratch was lent (m17_voice_scratch_borrow)
} m17_voice_stats_t;

// The board's PCM audio (NULL: frames are counted, nothing plays) and a microsecond clock for the codec's timing
// counters (NULL: not timed). Once, at start-up
void m17_voice_init(const hal_audio_t *audio, uint32_t (*now_us)(void));
// Start on mem (radio task, before m17_engine_begin); the stream starts with the first call (m17_voice_poll)
void m17_voice_begin(m17_voice_mem_t *mem);
// Radio task, every tick while M17 runs: starts the audio stream once a call is heard
void m17_voice_poll(void);
// A transmission starts in mode (the stream starts if it was not running; from the next block the mic is encoded) or
// ends (on false the mode is ignored). Radio task, around the engine's transmit calls
void m17_voice_tx(bool on, c2_mode_t mode);
// The mic gain step, as p25VoiceSetMicGain (radio/mic_gain.h). Any task
void m17_voice_set_mic_gain(uint8_t step);
// Stop (radio task, after m17_engine_end): the stream stops and mem is free again
void m17_voice_end(void);
// The voice stream runs (its callback owns hal_audio_t). Radio task only
bool m17_voice_streaming(void);
// Something else to hear while the stream runs (beeps), as p25VoiceSetMixer
typedef bool (*m17_voice_mixer_t)(int16_t *spk);
void m17_voice_set_mixer(m17_voice_mixer_t mixer);

// The engine's hook bodies (provider's task). rx_start: a call in mode (M17_TYPE_DATA: VOICE is 3200, VOICE_DATA 1600)
void m17_voice_rx_start(c2_mode_t mode);
void m17_voice_rx_frame(const uint8_t stream[M17_STREAM_BYTES], uint8_t errs);
void m17_voice_rx_end(void);
#if CONFIG_NET_M17
// A call started and not yet ended (the producer's view: its frames may still be playing). The receive queue's second
// producer (the reflector's path, radio_m17.c) asks it while the first is quiet
bool m17_voice_rx_open(void);
#endif
bool m17_voice_tx_frame(uint8_t stream[M17_STREAM_BYTES]);
// 16 STREAM bytes of silence in a mode (c2_silence_* twice, or once and 8 zero bytes)
const uint8_t *m17_voice_silence(c2_mode_t mode);

// The codec scratch for other work while no voice plays and nothing is encoded (radio task). The stream stopped (no
// callback runs; the radio task alone starts it, so nothing races): granted at once. The stream running: the first
// call posts the request and returns NULL; the callback acknowledges it at the top of its next block, and a later call
// (next tick) gets the scratch. NULL also while a call plays, the encoder runs or the bench is on. While lent the
// callback plays silence and touches no codec state (a call starting then loses its first frames), and
// m17_voice_poll does not start the stream. Returned with m17_voice_scratch_return before any m17_voice_tx(true) or
// m17_voice_bench
void *m17_voice_scratch_borrow(size_t *len);
void m17_voice_scratch_return(void);

// The codec bench (debug link, HIL; no RF), one direction at a time on the one codec state, as a simplex radio runs it:
// M17_VOICE_BENCH_ENC encodes each block of mic samples in mode (the frames are dropped), timing each block into
// enc_us_*; M17_VOICE_BENCH_DEC decodes a fixed frame sequence from flash (c2_silence_* and the bench's own const
// frames, voiced and unvoiced) in mode and times each block into dec_us_*; OFF stops. Changing phase initializes the
// state for it. The speaker stays off. Radio task, while M17 runs and nothing is received or sent
enum { M17_VOICE_BENCH_OFF, M17_VOICE_BENCH_ENC, M17_VOICE_BENCH_DEC };
void m17_voice_bench(uint8_t phase, c2_mode_t mode);

void m17_voice_stats(m17_voice_stats_t *out);
void m17_voice_reset_stats(void);

// The audio block callback (exposed for tests): one 20 ms block in, one out
void m17_voice_audio_block(const int16_t *mic, int16_t *spk, void *ctx);

#endif
