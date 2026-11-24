// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stations heard (radio_heard.h, docs/spec/radio-features.md, the heard list): one entry per station, newest
// first. The radio task writes; any task reads a consistent copy: a sequence count around every write, odd while one
// is under way, and a reader copies again when it moved. One core: compiler barriers between the count and the data.
#include "config.h"
#if CONFIG_HEARD_LIST	// the whole file
#include <string.h>
#include "hal_time.h"
#include "radio/radio_heard.h"

#define BARRIER()	__atomic_signal_fence(__ATOMIC_SEQ_CST)

static struct {
	volatile uint8_t seq;	// odd while the radio task writes
	uint8_t n;
	radio_heard_t e[RADIO_HEARD_MAX];
} heard;

static void write_begin(void)
{
	heard.seq++;
	BARRIER();
}

static void write_end(void)
{
	BARRIER();
	heard.seq++;
}

// The same station: the mode and its address (or its ID)
static bool same(const radio_heard_t *a, const radio_heard_t *b)
{
	if (a->mode != b->mode || ((a->flags ^ b->flags) & RADIO_HEARD_ADDR) != 0)
		return false;
	return (a->flags & RADIO_HEARD_ADDR) ? memcmp(a->addr, b->addr, sizeof a->addr) == 0 : a->id == b->id;
}

void radio_heard_add(const radio_heard_t *e)
{
	uint8_t i = 0;
	uint16_t times = 0;

	while (i < heard.n && !same(&heard.e[i], e))
		i++;
	if (i < heard.n)
		times = heard.e[i].count;
	else if (heard.n < RADIO_HEARD_MAX)
		heard.n++;
	else
		i = RADIO_HEARD_MAX - 1u;	// full: the oldest goes
	write_begin();
	memmove(&heard.e[1], &heard.e[0], (size_t)i * sizeof heard.e[0]);
	heard.e[0] = *e;
	// Through a pointer: tools/stack_check.py reads an assignment to a member named count as a function pointer's
	// (the UI's list tables have one) and would bind every such table's calls here
	uint16_t *n = &heard.e[0].count;
	*n = times < UINT16_MAX ? (uint16_t)(times + 1u) : times;
	heard.e[0].last_ms = hal_millis();	// the contract's: a caller's value is not kept
	write_end();
}

uint8_t radio_heard_count(void)
{
	return __atomic_load_n(&heard.n, __ATOMIC_RELAXED);
}

bool radio_heard_get(uint8_t i, radio_heard_t *out)
{
	uint8_t s;
	bool ok;

	do {
		s = heard.seq;
		BARRIER();
		ok = i < heard.n;
		if (ok)
			*out = heard.e[i];
		BARRIER();
	} while ((s & 1u) != 0 || s != heard.seq);
	return ok;
}

bool radio_heard_find(const radio_heard_t *key, radio_heard_t *out)
{
	// Matched by who it is, never by where it was. Each entry is a consistent copy (radio_heard_get); a write during
	// the walk can move the station past it, so the walk starts over when the count moved
	uint8_t s;
	bool found;

	do {
		s = heard.seq;
		BARRIER();
		found = false;
		for (uint8_t i = 0; !found && radio_heard_get(i, out); i++)
			found = same(out, key);
		BARRIER();
	} while ((s & 1u) != 0 || s != heard.seq);
	return found;
}

uint8_t radio_heard_seq(void)
{
	return heard.seq;
}

void radio_heard_clear(void)
{
	write_begin();
	heard.n = 0;
	memset(heard.e, 0, sizeof heard.e);
	write_end();
}

#endif	// CONFIG_HEARD_LIST
