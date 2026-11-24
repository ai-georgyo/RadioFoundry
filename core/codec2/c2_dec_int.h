// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Codec 2 decoder's own declarations (docs/spec/codec2.md 10-13, 17): its state laid over c2_dec_state_t, the
// synthesis of one 10 ms frame (c2_synth.c) that c2_dec.c drives, and the exact helpers the host tests check one by
// one. Only c2_dec.c, c2_synth.c and their host tests include it.
#ifndef RFOUNDRY_C2_DEC_INT_H
#define RFOUNDRY_C2_DEC_INT_H

#include <stdbool.h>
#include <stdint.h>
#include "c2_int.h"
#include "dsp/dsp.h"

// Fixed-point formats inside the synthesis (spec 17.2)
#define C2_LOG_Q	24	// log2 values (power spectra, amplitudes, thresholds): Q24, ±128
#define C2_DB_Q		20	// the background estimate b and the frame energy e, dB: Q20, ±2048 dB
#define C2_OUT_Q	4	// output samples before the limiter and the overlap-add memory: Q4 (spec 13.3: past 16 bits)
#define C2_LOG_ZERO	(-(1 << 30))	// log2 of 0 (an empty harmonic band): below every value the synthesis forms

// Concealment (not in the spec, spec 17.4): a bad frame repeats the last good parameters with the energy halved every
// 10 ms (3 dB), C2_FADE_FRAMES times, then the output is muted until a good frame comes
#define C2_FADE_FRAMES	8	// 80 ms of fade, the last at -24 dB

// The state: what outlives a call. The 10 ms frames' parameters are c2_params_t (c2_int.h)
typedef struct __attribute__((may_alias)) {
	// --- synthesis state (c2_synth.c) ---
	int32_t amp[C2_L_MAX + 1];	// log2 A_m, C2_LOG_Q, of the frame being synthesized; [0] unused
	uint32_t phi[C2_L_MAX + 1];	// the direction of conj H_A(b_m) (12.1), two int16 (c2_synth.c phase_bin)
	int32_t ola[C2_N - 1];		// overlap-add memory o[0..78] (13.2), C2_OUT_Q
	uint32_t phi_ex_lo;		// φ_ex (12.2), a float32 value held exactly as Q48 in 64 bits (two words: the
	int32_t phi_ex_hi;		// state is only 4-byte aligned)
	int32_t bg;			// background estimate b (12.4), C2_DB_Q
	uint32_t rng;			// the random generator (12.3)
	// --- parameters (c2_dec.c) ---
	c2_params_t prev;		// the previous codec frame's last 10 ms frame (10.1, 10.3)
	int32_t next_lsp[C2_ORDER];	// the current codec frame's LSPs (10.2: frame 2 of 3200, frame 4 of 1600)
	c2_index_t ix;			// the current codec frame's fields
	c2_params_t shown[2];		// the last block's two frames, for c2_dec_params
	uint16_t draws[2];		// their random draws
	uint8_t mode;			// c2_mode_t
	uint8_t blocks;			// blocks of the current codec frame still to play (0: none, c2_dec_block returns false)
	uint8_t blk;			// the next one's index in the codec frame
	uint8_t shown_ok;		// shown[] holds a block
	uint8_t good;			// a good frame has come since init
	uint8_t bad;			// the current codec frame is a concealment of the last good one
	uint8_t fade;			// 10 ms frames concealed since the last good one (saturates past C2_FADE_FRAMES)
	uint8_t pad;
} c2_dec_t;

_Static_assert(sizeof(c2_dec_t) <= sizeof(c2_dec_state_t), "the decoder's state fits C2_DEC_STATE_BYTES");

// The scratch while a frame is synthesized: the FFT buffer and the per-call temporaries (2,400 B)
typedef struct __attribute__((may_alias)) {
	int32_t buf[DSP_FFT_SIZE];	// 512 words: the per-bin logs (11), then the frame's spectrum and waveform (13.1)
	int32_t cosl[C2_ORDER];		// cos ω_i, Q29 (10.4)
	int32_t ak[C2_ORDER + 1];	// the coefficients of A(z/γ) (11.2), Q23
	int32_t lwg[67];		// log2 |H_W|² at every fourth bin, mirrored at both ends (11.2), Q16
} c2_synth_scratch_t;

// --- c2_synth.c ---
// Synthesize one 10 ms frame with parameters p: 80 samples to pcm. draws: the random draws it took (12.3)
void c2_synth_frame(c2_dec_t *d, c2_scratch_t *scr, const c2_params_t *p, int16_t pcm[C2_N], uint16_t *draws);
// A muted frame: the overlap-add memory played out, then silence (concealment past the fade)
void c2_synth_mute(c2_dec_t *d, c2_scratch_t *scr, int16_t pcm[C2_N]);
// Advance φ_ex by one frame of ω0 (12.2), the float32 arithmetic of the reference reproduced exactly
void c2_phi_ex_step(c2_dec_t *d, int32_t wo);

// The random generator (12.3): the next value, 0..32767
uint32_t c2_rand(c2_dec_t *d);

// The exact helpers (spec 17.3), each checked by the host tests
uint32_t c2_f32_round_u32(uint32_t x);				// x rounded to 24 significant bits (nearest, even)
uint32_t c2_bin_band(int32_t wo, unsigned j);			// trunc(j/2 · ω0 / r_d + 0.5), j odd (11.3)
uint32_t c2_bin_phase(int32_t wo, unsigned m);			// 12.1
uint32_t c2_bin_synth(int32_t wo, unsigned m);			// 13.1 (limited to 255)
int32_t c2_log2(uint32_t x);					// log2 x, C2_LOG_Q, x > 0
int32_t c2_exp2(int32_t x, int q);				// 2^(x / 2^C2_LOG_Q) · 2^q, rounded down, saturating
int32_t c2_cos(int32_t x);					// cos x, x in C2_LSP_Q rad, Q30

#endif
