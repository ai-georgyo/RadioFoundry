// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codec 2 at 3200 and 1600 bit/s (docs/spec/codec2.md), the vocoder M17 carries: one fixed-point implementation for
// every platform (a CK803S has no FPU), on core/dsp (dsp.h: the 512-point real FFT, trigonometry, log tables) and
// core/dsp/fxp.h. Bits are Codec 2's own: a frame from here decodes in any Codec 2 decoder, and the decoder reads any
// encoder's frames with the parameters (voicing, harmonic count, energy, LSPs) decoded exactly (spec 15.1).
//
// Frames and blocks. The audio side works in 20 ms blocks of 160 samples (hal_audio.h AUDIO_BLOCK_SAMPLES). A 3200 frame
// is one block, a 1600 frame two. The work is spread so every block costs about the same: the encoder analyses each
// block as it comes (two 10 ms analysis frames) and packs a frame when it is complete; the decoder takes a frame's bits
// once and synthesizes one block per call.
//
// Memory is the caller's (it is lent: radio_m17_mem.h). The state of one direction is kept between calls; the scratch is
// needed only inside a call and may be used for something else between calls. Encoder and decoder states may share
// memory (a radio transmits or receives): init the one that starts.
//
// No RTOS call, no float, no libm, no 64-bit product or divide per sample (fxp.h rules). Reentrant per state.
// Built with CONFIG_CODEC2 (derived from CONFIG_M17).
#ifndef RFOUNDRY_CODEC2_H
#define RFOUNDRY_CODEC2_H

#include <stdbool.h>
#include <stdint.h>

#define C2_FRAME_BYTES		8	// both modes: 64 bits, MSB of byte 0 first (spec 4.1)
#define C2_BLOCK_SAMPLES	160	// one call's samples, 20 ms at 8 kHz

typedef enum {
	C2_MODE_3200 = 0,	// 20 ms frames: one per block
	C2_MODE_1600 = 1,	// 40 ms frames: one per two blocks
} c2_mode_t;

// Sizes are part of the contract: the M17 memory plan (docs/spec/m17.md, radio_m17_mem.h) counts on them. An
// implementation lays its own structs over these and asserts they fit; growing one is a change to that plan
#define C2_ENC_STATE_BYTES	1216	// analysis buffer, pitch estimator history and filters, the frame being packed
// The decoder's state is as large as the encoder's: they share memory (m17_voice_mem_t), so it costs nothing, and it
// holds what must outlive a call or sit beside the FFT buffer: the previous and next parameters, the 32-bit overlap-add
// memory (values before the limiter exceed 16 bits, spec 13.3), phases, the random generator, and the harmonic
// amplitudes and phases of the 10 ms frame being synthesized (A_m and phi_m, int32 x 80 each, alive while the FFT
// buffer is in use: the phase post filter needs the sum of every A_m first, spec 12.4)
#define C2_DEC_STATE_BYTES	1216
#define C2_SCRATCH_BYTES	2560	// one 512-point FFT buffer (2 KB) and the per-harmonic arrays of one 10 ms frame

typedef struct { uint32_t w[C2_ENC_STATE_BYTES / 4]; } c2_enc_state_t;
typedef struct { uint32_t w[C2_DEC_STATE_BYTES / 4]; } c2_dec_state_t;
typedef struct { int32_t w[C2_SCRATCH_BYTES / 4]; } c2_scratch_t;

// --- Encoder ---

// Start (or start over) in mode: the state as the spec's encoder has it at creation (spec 5.1, 6)
void c2_enc_init(c2_enc_state_t *e, c2_mode_t mode);
// One block of 160 samples (16-bit, 8 kHz). True when a codec frame is complete: bits then holds its 8 bytes (3200:
// every block; 1600: every second block, the first after init being the first half). bits is untouched otherwise
bool c2_enc_block(c2_enc_state_t *e, c2_scratch_t *scr, const int16_t pcm[C2_BLOCK_SAMPLES],
		  uint8_t bits[C2_FRAME_BYTES]);

// --- Decoder ---

// Start (or start over) in mode: the state of spec 10.1 (random generator seeded with 1)
void c2_dec_init(c2_dec_state_t *d, c2_mode_t mode);
// The next codec frame. bad: the channel could not trust it (FEC failure, a lost frame): the decoder keeps the last
// good parameters and fades them over a few frames, then mutes (not in the spec: a receiver's choice). Call once per
// frame, before its blocks
void c2_dec_frame(c2_dec_state_t *d, const uint8_t bits[C2_FRAME_BYTES], bool bad);
// One block of output from the frame given last (3200: its only block; 1600: its first, then its second). False when
// that frame's blocks are all out (no frame came): pcm is silence and nothing advanced
bool c2_dec_block(c2_dec_state_t *d, c2_scratch_t *scr, int16_t pcm[C2_BLOCK_SAMPLES]);

// A frame that decodes to silence (lowest energy, unvoiced), what a transmitter sends with nothing from the encoder
// in time, and in a signed stream's tail (docs/spec/m17.md 7)
extern const uint8_t c2_silence_3200[C2_FRAME_BYTES];
extern const uint8_t c2_silence_1600[C2_FRAME_BYTES];

#endif
