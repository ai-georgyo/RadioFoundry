// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codec 2 decoder (docs/spec/codec2.md 10, 17): a codec frame's fields to the parameters of its 10 ms frames, the
// interpolation between codec frames, concealment, and two synthesized 10 ms frames per block (c2_synth.c). The
// parameters come out exactly as the reference decodes them (spec 15.1): ω0 and L exact, E and the LSPs within a few
// units of their float32 values.
#include <string.h>
#include "c2_dec_int.h"
#include "dsp/fxp.h"

// The LSPs at creation (10.1): f32(π_c (i - 1)/11), i = 1..10, C2_LSP_Q
static const int32_t c2_lsp_init[C2_ORDER] = {
	0, 76664984, 153329968, 229994960, 306659936, 383324928, 459989920, 536654912, 613319872, 689984896,
};

void c2_dec_init(c2_dec_state_t *st, c2_mode_t mode)
{
	c2_dec_t *d = (c2_dec_t *)st;

	memset(d, 0, sizeof(*d));
	d->mode = (uint8_t)mode;
	d->rng = 1;
	d->prev.wo = c2_wo_dequant(0);		// ω0_min
	d->prev.L = c2_L_of_wo(d->prev.wo);
	d->prev.e = 1u << C2_E_Q;
	memcpy(d->prev.lsp, c2_lsp_init, sizeof(d->prev.lsp));
}

// Frame 2 (n = 0) or frame 4 (n = 1) of the codec frame, as received: they carry ω0, energy and voicing (10.2)
static void own(const c2_dec_t *d, unsigned n, c2_params_t *p)
{
	p->wo = c2_wo_dequant(d->ix.wo[n]);
	p->e = c2_e_dequant(d->ix.e[n]);
	p->voiced = d->ix.voiced[2 * n + 1] != 0;
}

// f32((a + b) / 2) for two float32 ω0 in C2_WO_Q: the sum is exact in 32 bits; rounded to 24 significant bits as
// float32 rounds it (a, b >= ω0_min > 2^-5, so the sum is past 2^24 units and the halving exact)
static int32_t wo_mean(int32_t a, int32_t b)
{
	return (int32_t)(c2_f32_round_u32((uint32_t)a + (uint32_t)b) >> 1);
}

// The interpolated frame between a and b (10.3) with its received voicing bit
static void interp(const c2_params_t *a, const c2_params_t *b, uint8_t vrx, c2_params_t *p)
{
	p->voiced = vrx && (a->voiced || b->voiced);
	if (!p->voiced)
		p->wo = c2_wo_dequant(0);
	else if (a->voiced && b->voiced)
		p->wo = wo_mean(a->wo, b->wo);
	else
		p->wo = b->voiced ? b->wo : a->wo;
	// f32(sqrt(f32(E_a E_b))): exact product, the square root's floor (within a unit of C2_E_Q)
	p->e = dsp_isqrt64((uint64_t)fxp_mul64((int32_t)a->e, (int32_t)b->e));
}

// LSPs (1 - q/4) prev + (q/4) next, q = 1..3, per element in 64 bits (the LSPs reach ±8 rad: their sums do not fit)
static void lsp_interp(const int32_t *prev, const int32_t *next, unsigned q, int32_t *lsp)
{
	for (int i = 0; i < C2_ORDER; i++) {
		int64_t a = prev[i], b = next[i], s;

		s = q == 1 ? a + a + a + b : q == 2 ? a + a + b + b : a + b + b + b;
		lsp[i] = (int32_t)((s + 2) >> 2);
	}
}

// The parameters of 10 ms frame f (0..3) of the current codec frame
static void frame_params(const c2_dec_t *d, unsigned f, c2_params_t *p)
{
	c2_params_t f2, f4;

	if (d->bad) {
		*p = d->prev;
		return;
	}
	own(d, 0, &f2);
	if (d->mode != C2_MODE_1600) {
		if (f == 0) {
			interp(&d->prev, &f2, d->ix.voiced[0], p);
			lsp_interp(d->prev.lsp, d->next_lsp, 2, p->lsp);
		} else {
			*p = f2;
			memcpy(p->lsp, d->next_lsp, sizeof(p->lsp));
		}
	} else {
		own(d, 1, &f4);
		switch (f) {
		case 0:
			interp(&d->prev, &f2, d->ix.voiced[0], p);
			break;
		case 1:
			*p = f2;
			break;
		case 2:
			interp(&f2, &f4, d->ix.voiced[2], p);
			break;
		default:
			*p = f4;
			break;
		}
		if (f < 3)
			lsp_interp(d->prev.lsp, d->next_lsp, f + 1, p->lsp);
		else
			memcpy(p->lsp, d->next_lsp, sizeof(p->lsp));
	}
	p->L = c2_L_of_wo(p->wo);
}

// The codec frame is played out: its last 10 ms frame becomes the previous one (a concealed frame holds it)
static void finish(c2_dec_t *d)
{
	if (!d->bad)
		frame_params(d, d->mode == C2_MODE_1600 ? 3u : 1u, &d->prev);
	d->blocks = 0;
}

void c2_dec_frame(c2_dec_state_t *st, const uint8_t bits[C2_FRAME_BYTES], bool bad)
{
	c2_dec_t *d = (c2_dec_t *)st;

	// A frame before the last one's blocks are out replaces them: its parameters move on as if they had played
	if (d->blocks)
		finish(d);
	if (bad || bits == NULL) {
		d->bad = 1;
	} else {
		// After a concealment the first frame interpolates from where the fade had got to. Lost frames before the
		// first good one faded nothing: the stream starts from the state of 10.1, as the reference's does
		if (d->fade && d->good)
			d->prev.e >>= d->fade > C2_FADE_FRAMES ? C2_FADE_FRAMES : d->fade;
		d->bad = 0;
		d->good = 1;
		d->fade = 0;
		c2_unpack((c2_mode_t)d->mode, bits, &d->ix);
		c2_lsp_dequant((c2_mode_t)d->mode, d->ix.lsp, d->next_lsp);
	}
	d->blocks = d->mode == C2_MODE_1600 ? 2 : 1;
	d->blk = 0;
}

bool c2_dec_block(c2_dec_state_t *st, c2_scratch_t *scr, int16_t pcm[C2_BLOCK_SAMPLES])
{
	c2_dec_t *d = (c2_dec_t *)st;

	if (!d->blocks) {
		memset(pcm, 0, C2_BLOCK_SAMPLES * sizeof(pcm[0]));
		return false;
	}
	for (unsigned k = 0; k < 2; k++) {
		c2_params_t p;

		frame_params(d, 2u * d->blk + k, &p);
		d->draws[k] = 0;
		if (d->bad && (!d->good || d->fade >= C2_FADE_FRAMES)) {
			// Nothing to hold, or the fade is over: the waveform's tail, then silence
			if (d->fade <= C2_FADE_FRAMES)
				d->fade++;
			c2_synth_mute(d, scr, pcm + k * C2_N);
		} else {
			if (d->bad)
				p.e >>= ++d->fade;	// 3 dB per 10 ms
			c2_synth_frame(d, scr, &p, pcm + k * C2_N, &d->draws[k]);
		}
		d->shown[k] = p;
	}
	d->shown_ok = 1;
	d->blk++;
	if (--d->blocks == 0)
		finish(d);
	return true;
}

bool c2_dec_params(const c2_dec_state_t *st, uint8_t k, c2_params_t *out, uint16_t *draws)
{
	const c2_dec_t *d = (const c2_dec_t *)st;

	if (!d->shown_ok || k > 1)
		return false;
	*out = d->shown[k];
	if (draws != NULL)
		*draws = d->draws[k];
	return true;
}
