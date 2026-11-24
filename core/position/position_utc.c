// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Wall-clock time from what the radio hears (position_utc.h, docs/spec/position.md 8): the UTC of the newest GPS or
// host NMEA sentence (position_utc_last) and the time a trusted host set, each with the millisecond it came at, so the
// time now is that plus the milliseconds since. Nothing here survives a restart.
//
// The set time has one writer, the host link task (the debug link's M17 op, utc_set's only caller), and any task reads
// it: a sequence count around it (odd while a write is under way) lets a reader see a torn write and leave that source
// out of its answer, with no lock and no wait. One core: only a compiler barrier is needed between the count and the
// fields. A second writer would need a lock around utc_set.
#include "config.h"

#if CONFIG_POSITION

#include <stddef.h>
#include "position/position_utc.h"
#include "position/position.h"

#define DAY_MS		86400000u
#define NMEA_SLACK_MS	500u	// how far apart two sentences of the same second may arrive
#define AHEAD_MS	1000u	// a stamp this far past the caller's now_ms is a race between tasks: age 0

static struct {
	uint32_t seq;		// even: the fields are whole; odd: utc_set is writing them
	uint32_t unix_s;
	uint32_t ms;
	uint8_t src;		// UTC_SRC_NONE: no time set
} set;

void utc_set(uint32_t unix_s, uint32_t now_ms, uint8_t src)
{
	uint32_t s = __atomic_load_n(&set.seq, __ATOMIC_RELAXED);
	__atomic_store_n(&set.seq, s + 1u, __ATOMIC_RELAXED);
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	__atomic_store_n(&set.unix_s, unix_s, __ATOMIC_RELAXED);
	__atomic_store_n(&set.ms, now_ms, __ATOMIC_RELAXED);
	__atomic_store_n(&set.src, src, __ATOMIC_RELAXED);
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	__atomic_store_n(&set.seq, s + 2u, __ATOMIC_RELAXED);
}

// A consistent copy of the set time, false while utc_set is writing it (a reader that preempted the writer would see
// the same half-written fields on every retry: it leaves the source out instead)
static bool set_read(uint32_t *unix_s, uint32_t *ms, uint8_t *src)
{
	uint32_t s = __atomic_load_n(&set.seq, __ATOMIC_RELAXED);
	if ((s & 1u) != 0) {
		return false;
	}
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	*unix_s = __atomic_load_n(&set.unix_s, __ATOMIC_RELAXED);
	*ms = __atomic_load_n(&set.ms, __ATOMIC_RELAXED);
	*src = __atomic_load_n(&set.src, __ATOMIC_RELAXED);
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	return __atomic_load_n(&set.seq, __ATOMIC_RELAXED) == s && *src != UTC_SRC_NONE;
}

static bool leap(uint32_t y)
{
	return (y % 4u == 0u && y % 100u != 0u) || y % 400u == 0u;
}

// Days from 1970-01-01 to an NMEA date (ddmmyy: 80-99 the 1900s, as core/satellite reads it), false when it is no
// date. Counted from 1 March of year 0, the leap day last in the year, then less 1970-01-01's 719468
static bool date_days(uint32_t ddmmyy, uint32_t *days)
{
	static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	uint32_t d = ddmmyy / 10000u, m = ddmmyy / 100u % 100u, yy = ddmmyy % 100u;
	uint32_t y = yy >= 80u ? 1900u + yy : 2000u + yy;

	if (ddmmyy > 311299u || m < 1u || m > 12u || d < 1u || d > mdays[m - 1u] + (m == 2u && leap(y) ? 1u : 0u)) {
		return false;
	}
	if (m <= 2u) {
		y--;
		m += 12u;
	}
	*days = 365u * y + y / 4u - y / 100u + y / 400u + (153u * (m - 3u) + 2u) / 5u + d - 1u - 719468u;
	return true;
}

// The last NMEA UTC as Unix seconds at the millisecond its time came (time_ms). The date may be from another
// sentence, before or after: the midnights between the two stamps move it to the time's day
static bool gnss_unix(uint32_t ddmmyy, uint32_t date_ms, uint32_t hms, uint32_t time_ms, uint32_t *unix_s)
{
	uint32_t hh = hms / 10000u, mm = hms / 100u % 100u, ss = hms % 100u, days;

	if (hms > 235959u || mm > 59u || ss > 59u || !date_days(ddmmyy, &days)) {
		return false;
	}
	int32_t gap = (int32_t)(time_ms - date_ms);	// the time's stamp after the date's
	if (gap > (int32_t)DAY_MS || gap < -(int32_t)DAY_MS) {
		return false;	// the two are a day or more apart: not the same fix
	}
	uint32_t sod = hh * 3600u + mm * 60u + ss;
	// The time of day the date's stamp had, counted in the time's day: below 0 a midnight came between, from a day
	// up one came the other way
	int32_t at_date = (int32_t)(sod * 1000u + NMEA_SLACK_MS) - gap;
	int32_t shift = at_date < 0 ? 1 : at_date >= (int32_t)DAY_MS ? -1 : 0;
	if (shift < 0 && days == 0u) {
		return false;
	}
	*unix_s = (days + (uint32_t)shift) * 86400u + sod;
	return true;
}

// How old a stamp is at now_ms. One a little past now_ms was taken by another task after the caller read its clock
static uint32_t age_of(uint32_t now_ms, uint32_t stamp_ms)
{
	uint32_t d = now_ms - stamp_ms;
	return (uint32_t)(stamp_ms - now_ms) <= AHEAD_MS ? 0u : d;
}

bool position_utc_now(uint32_t now_ms, uint32_t max_age_ms, uint32_t *unix_s, uint8_t *src)
{
	uint32_t best_unix = 0, best_age = 0, u, ms, date, date_ms, hms;
	uint8_t best = UTC_SRC_NONE, s;

	// GNSS first: on a tie it wins, being the source that is trusted by itself
	if (position_utc_last(&date, &date_ms, &hms, &ms) && gnss_unix(date, date_ms, hms, ms, &u)) {
		uint32_t age = age_of(now_ms, ms);
		if (age <= max_age_ms) {
			best = UTC_SRC_GNSS;
			best_unix = u;
			best_age = age;
		}
	}
	if (set_read(&u, &ms, &s)) {
		uint32_t age = age_of(now_ms, ms);
		if (age <= max_age_ms && (best == UTC_SRC_NONE || age < best_age)) {
			best = s;
			best_unix = u;
			best_age = age;
		}
	}
	if (src != NULL) {
		*src = best;
	}
	if (best == UTC_SRC_NONE) {
		return false;
	}
	uint32_t add = best_age / 1000u;
	if (unix_s != NULL) {
		*unix_s = best_unix > UINT32_MAX - add ? UINT32_MAX : best_unix + add;
	}
	return true;
}

#endif
