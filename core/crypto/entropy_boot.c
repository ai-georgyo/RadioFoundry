// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The boot count under the nonce counter (entropy_boot.h). Whether the store has a count is read from its stats: with
// nothing left unsaved (dirty clear) on a store that writes, the record in force holds what the live copy does, the
// count included.
#include "config.h"

#if CONFIG_ENTROPY

#include <string.h>
#include "crypto/entropy_boot.h"
#include "crypto/entropy.h"
#include "settings/settings.h"
#include "../../hal/hal_time.h"

static struct {
	uint32_t boot;		// the count the counter runs on
	uint32_t pending;	// the next one, in the settings but maybe not stored yet; 0 none
	bool saved;		// the store has boot
	bool started;
} b;

// noinline: inlined into the start task, its frame (the chip ID, the pool's digest and generator words) would sit
// under the radio core's set-up, that task's deepest path (make stack-check)
__attribute__((noinline)) void entropy_boot_start(const hal_system_t *sys, uint32_t now_ms)
{
	settings_t *s = settings_begin();
	uint32_t boot = settings_boot_count(s);
	boot = boot < SETTINGS_BOOT_COUNT_MAX ? boot + 1u : boot;	// at the end it stays: the counter is spent then
	settings_set_boot_count(s, boot);
	settings_end(now_ms);
	// The start task is the one that saves the settings: at once, so the count is stored before any nonce uses it.
	// False on a store that can't write (no flash, another firmware's settings area) or a failed write
	uint32_t t[2];
	t[0] = hal_micros();
	bool saved = settings_flush();
	t[1] = hal_micros();
	hal_chip_t c;
	memset(&c, 0, sizeof c);
	bool uid = sys != NULL && sys->chip != NULL && sys->chip(&c);
	entropy_init(sys, uid ? c.uid : NULL, uid ? sizeof c.uid : 0u, boot, saved);
	// The time since reset and the settings write's duration (a flash program, its busy polls): they vary between
	// start-ups by some microseconds, but nobody can vouch for how many, so uncredited (crypto.md 8.1)
	entropy_mix(t, sizeof t, 0u);
	b.boot = boot;
	b.pending = 0;
	b.saved = saved;
	b.started = true;
}

void entropy_boot_poll(uint32_t now_ms)
{
	if (!b.started) {
		return;
	}
	// Each tick's microsecond, which interrupts and the other tasks move: uncredited, but it takes the pool apart
	// from an earlier start-up's long before a key-up (crypto.md 8.1)
	entropy_mix_u32(hal_micros(), 0u);
	if (!b.saved || b.pending != 0) {
		settings_stats_t st;
		settings_get_stats(&st);
		if (st.dirty || st.load == SETTINGS_RAM || st.load == SETTINGS_FOREIGN) {
			return;		// not stored yet
		}
		if (b.pending != 0) {
			b.boot = b.pending;
			b.pending = 0;
		}
		b.saved = true;
		entropy_rebase(b.boot, true);
		return;
	}
	if (entropy_nonce_want_bump() && b.boot < SETTINGS_BOOT_COUNT_MAX) {
		b.pending = b.boot + 1u;
		settings_t *s = settings_begin();
		if (settings_boot_count(s) < b.pending) {
			settings_set_boot_count(s, b.pending);	// never lowered
		}
		settings_end(now_ms);
	}
}

uint32_t entropy_boot_count(void)
{
	return b.boot;
}

bool entropy_boot_saved(void)
{
	return b.saved;
}

#endif
