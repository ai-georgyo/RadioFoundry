// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A channel's sub-audio selection (hal_radio.h's subtone_t) as the software detector and generator take it
// (subaudio_rx.h, subaudio_tx.h), for the board glue between the two. Header only.
//
// boards/md380/subtone_md380.c and boards/md9600/subtone_md9600.c still have their own copies of the first rule
// (kind_of), boards/md380/radio_md380.c and boards/md9600/radio_md9600.c of the second (in their receive set-up);
// each copy goes when its board moves to these functions.
#ifndef SUBAUDIO_SEL_H
#define SUBAUDIO_SEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hal_radio.h"
#include "subaudio_rx.h"

// The detector's and generator's kind (subaudio_kind_t) for a selection. NULL or a type they don't know: none
static inline uint8_t subaudio_kind(const subtone_t *t)
{
	if (t == NULL) {
		return SUBAUDIO_NONE;
	}
	switch (t->type) {
	case TONE_CTCSS: return SUBAUDIO_CTCSS;
	case TONE_DCS: return SUBAUDIO_DCS;
	case TONE_DCS_INV: return SUBAUDIO_DCS_INV;
	default: return SUBAUDIO_NONE;
	}
}

// Both select the same thing: the same kind and, where there is one, the same tone or code. A receiver set up again
// with the selection it already has (back from transmit, the same channel once more) keeps its detector: a restart
// closes it until the tone or code has been heard anew. The frequency and the width are the board's to compare. Two
// selections of no tone are the same whatever their values
static inline bool subaudio_same(const subtone_t *a, const subtone_t *b)
{
	uint8_t kind = subaudio_kind(a);
	if (kind != subaudio_kind(b)) {
		return false;
	}
	return kind == SUBAUDIO_NONE || a->value == b->value;
}

#endif
