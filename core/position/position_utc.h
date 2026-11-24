// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Wall-clock time from what the radio hears (docs/spec/position.md, the UTC section): the UTC of the last GNSS fix
// (board GPS or host NMEA), or a time a trusted host set (the debug link's M17 op; later a host link command or a
// network time client), each aged by the milliseconds since it came, so the caller gets "now" and not "then". The tree
// keeps no clock of its own across power cycles (radio-features.md 5.6): without a fresh source there is no time, and
// the caller says so (M17 sends timestamp 0 and never calls a received stream fresh, m17.md 7.4). One task writes the
// set time: the host link task, where the debug link runs (its M17 op is utc_set's only caller); the fix's time is the
// position service's. Any task reads. Built with CONFIG_POSITION.
#ifndef POSITION_UTC_H
#define POSITION_UTC_H

#include <stdbool.h>
#include <stdint.h>

// Where the time came from (position_utc_now's src)
enum { UTC_SRC_NONE, UTC_SRC_GNSS, UTC_SRC_HOST };

// Unix seconds now, from the freshest source no older than max_age_ms (its time stamp + the milliseconds since). src
// may be NULL. False: no source with a time in that window
bool position_utc_now(uint32_t now_ms, uint32_t max_age_ms, uint32_t *unix_s, uint8_t *src);

// A trusted host gave the time (Unix seconds, at now_ms; src UTC_SRC_HOST). RAM only; replaced by the next one
void utc_set(uint32_t unix_s, uint32_t now_ms, uint8_t src);

// Seconds since 2020-01-01T00:00:00Z (M17's timestamps) from Unix seconds; 0 before then
static inline uint32_t utc_since_2020(uint32_t unix_s)
{
	return unix_s >= 1577836800u ? unix_s - 1577836800u : 0u;
}

#endif
