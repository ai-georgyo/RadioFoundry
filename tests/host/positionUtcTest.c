// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Wall-clock time from what the radio hears (core/position/position_utc.c on the position service; docs/spec/
// position.md 8): the UTC of board GPS and host NMEA sentences aged by the milliseconds since, a time a host set, the
// freshest winning, nothing too old; the date carried across midnight from another sentence; bad dates; the clock's
// wrap; a torn set seen by a reader. position_utc.c is included, so the test can stop its writer half way; the
// reference is the C library's timegm
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../../core/position/position_utc.c"
#include "board.h"
#include "position/nmea.h"
#include "settings/settings.h"

const board_t board = { .name = "test" };

// The position service saves its settings through the store: not used here
static settings_t store;
settings_t *settings_begin(void)
{
	return &store;
}
void settings_end(uint32_t now_ms)
{
	(void)now_ms;
}

static unsigned checks, failed;

#define CHECK(c, ...)                                                                                                \
	do {                                                                                                         \
		checks++;                                                                                            \
		if (!(c)) {                                                                                          \
			failed++;                                                                                    \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                  \
			printf(__VA_ARGS__);                                                                         \
			printf("\n");                                                                                \
		}                                                                                                    \
	} while (0)

// Unix seconds of a UTC date and time, from the C library
static uint32_t ref(int y, int mo, int d, int h, int mi, int s)
{
	struct tm t;
	memset(&t, 0, sizeof t);
	t.tm_year = y - 1900;
	t.tm_mon = mo - 1;
	t.tm_mday = d;
	t.tm_hour = h;
	t.tm_min = mi;
	t.tm_sec = s;
	return (uint32_t)timegm(&t);
}

// An NMEA sentence with its checksum, into the position service at now_ms (gps: the board GPS, else the host)
static void nmea(bool gps, uint32_t now_ms, const char *body)
{
	char line[NMEA_LINE_MAX + 1];
	uint8_t sum = 0;
	for (const char *p = body; *p; p++) {
		sum ^= (uint8_t)*p;
	}
	int n = snprintf(line, sizeof line, "$%s*%02X\r\n", body, sum);
	position_nmea(line, (size_t)n, gps, now_ms);
}

static void rmc(bool gps, uint32_t now_ms, const char *hms, const char *date)
{
	char b[96];
	snprintf(b, sizeof b, "GPRMC,%s,A,4807.038,N,01131.000,E,022.4,084.4,%s,003.1,W", hms, date);
	nmea(gps, now_ms, b);
}

static void gga(bool gps, uint32_t now_ms, const char *hms)
{
	char b[96];
	snprintf(b, sizeof b, "GPGGA,%s,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", hms);
	nmea(gps, now_ms, b);
}

static void fresh(void)
{
	position_init();
	position_gps_enable(true);
	memset(&set, 0, sizeof set);
}

static void test_none(void)
{
	uint32_t u = 77;
	uint8_t src = 9;

	fresh();
	CHECK(!position_utc_now(1000, UINT32_MAX, &u, &src) && src == UTC_SRC_NONE && u == 77, "nothing heard");
	CHECK(!position_utc_now(1000, UINT32_MAX, NULL, NULL), "NULL outputs");
	// A sentence with a time but no date (GGA alone): no time yet
	gga(true, 1000, "123519");
	CHECK(!position_utc_now(1000, UINT32_MAX, &u, &src), "time without date");
	CHECK(utc_since_2020(1577836800u) == 0 && utc_since_2020(1577836801u) == 1 && utc_since_2020(1000u) == 0,
	      "since 2020");
}

// The board GPS's time, aged: one second later is one second on; past max_age it is gone
static void test_gnss_age(void)
{
	uint32_t u, want = ref(2026, 11, 22, 12, 35, 19);
	uint8_t src;

	fresh();
	rmc(true, 10000, "123519", "221126");
	CHECK(position_utc_now(10000, 0, &u, &src) && u == want && src == UTC_SRC_GNSS, "at once: %u want %u", u, want);
	CHECK(position_utc_now(10999, 60000, &u, &src) && u == want, "0.999 s: the same second");
	CHECK(position_utc_now(12500, 60000, &u, &src) && u == want + 2u, "2.5 s on");
	CHECK(position_utc_now(70000, 60000, &u, &src) && u == want + 60u, "60 s on, at the limit");
	CHECK(!position_utc_now(70001, 60000, &u, &src) && src == UTC_SRC_NONE, "61 s: too old");
	CHECK(position_utc_now(10000u + 3600000u, 3600000u, &u, &src) && u == want + 3600u, "an hour, when allowed");
	// The GPS's fraction of a second is dropped
	rmc(true, 20000, "123529.75", "221126");
	CHECK(position_utc_now(20000, 0, &u, &src) && u == want + 10u, "fraction");
	// A void fix still carries the time
	nmea(true, 30000, "GPRMC,123539,V,,,,,,,221126,,");
	CHECK(position_utc_now(30000, 0, &u, &src) && u == want + 20u, "void fix's time");
	// Settings > Location > GPS off: its time goes with it
	position_gps_enable(false);
	CHECK(!position_utc_now(30000, UINT32_MAX, &u, &src), "GPS off");
	position_gps_enable(true);
	rmc(true, 31000, "123540", "221126");
	CHECK(position_utc_now(31000, 0, &u, &src) && u == want + 21u, "GPS on again");
}

// The newest of the GPS and the host NMEA, and a set time: the freshest wins, GNSS on a tie
static void test_freshest(void)
{
	uint32_t u, t0 = ref(2026, 11, 22, 8, 0, 0);
	uint8_t src;

	fresh();
	rmc(true, 1000, "080000", "221126");
	rmc(false, 5000, "080010", "221126");	// the host's is newer (and ten seconds on: it says so)
	CHECK(position_utc_now(6000, 60000, &u, &src) && u == t0 + 11u && src == UTC_SRC_GNSS, "host NMEA newer: %u",
	      u - t0);
	rmc(true, 7000, "080006", "221126");	// now the GPS's
	CHECK(position_utc_now(7000, 60000, &u, &src) && u == t0 + 6u, "GPS newer: %u", u - t0);

	utc_set(t0 + 100u, 8000, UTC_SRC_HOST);
	CHECK(position_utc_now(9000, 60000, &u, &src) && u == t0 + 101u && src == UTC_SRC_HOST, "set later wins");
	rmc(true, 8500, "080200", "221126");
	CHECK(position_utc_now(9000, 60000, &u, &src) && u == t0 + 120u && src == UTC_SRC_GNSS, "GPS later wins");
	utc_set(t0 + 500u, 8500, UTC_SRC_HOST);
	CHECK(position_utc_now(9000, 60000, &u, &src) && src == UTC_SRC_GNSS && u == t0 + 120u, "a tie: GNSS");
	// Only the set one is young enough
	CHECK(position_utc_now(8500u + 70000u, 60000, &u, &src) == false, "both too old");
	utc_set(t0 + 1000u, 60000, UTC_SRC_HOST);
	CHECK(position_utc_now(65000, 10000, &u, &src) && src == UTC_SRC_HOST && u == t0 + 1005u, "GNSS too old: set");
	// A set time on its own, and taken back
	fresh();
	utc_set(t0, 100, UTC_SRC_HOST);
	CHECK(position_utc_now(100, 0, &u, &src) && u == t0 && src == UTC_SRC_HOST, "set alone");
	utc_set(t0, 100, UTC_SRC_GNSS);	// a caller may vouch for its own time as GNSS
	CHECK(position_utc_now(100, 0, &u, &src) && src == UTC_SRC_GNSS, "set as GNSS");
	utc_set(0, 200, UTC_SRC_NONE);
	CHECK(!position_utc_now(200, UINT32_MAX, &u, &src), "set none: cleared");
}

// The date from an earlier (or later) sentence than the time: the midnight between moves it
static void test_midnight(void)
{
	uint32_t u;
	uint8_t src;

	fresh();
	rmc(true, 1000, "235959", "221126");
	gga(true, 2000, "000000");	// GGA has no date: RMC's is the day before
	CHECK(position_utc_now(2000, 0, &u, &src) && u == ref(2026, 11, 23, 0, 0, 0), "over midnight: %u", u);
	gga(true, 2950, "000000.95");
	CHECK(position_utc_now(2950, 0, &u, &src) && u == ref(2026, 11, 23, 0, 0, 0), "late in the second");
	fresh();
	rmc(true, 1000, "000000", "231126");
	gga(true, 2050, "000001");	// the next second, a little late: no midnight
	CHECK(position_utc_now(2050, 0, &u, &src) && u == ref(2026, 11, 23, 0, 0, 1), "after midnight: %u", u);
	fresh();
	rmc(true, 1000, "235930", "311226");
	gga(true, 41000, "000010");	// the year turns
	CHECK(position_utc_now(41000, 0, &u, &src) && u == ref(2027, 1, 1, 0, 0, 10), "new year: %u", u);
	fresh();
	rmc(true, 1000, "120000", "281226");
	gga(true, 1000u + 30u * 60000u, "123000");
	CHECK(position_utc_now(1000u + 30u * 60000u, 0, &u, &src) && u == ref(2026, 12, 28, 12, 30, 0), "same day");
	// A date a day or more from its time is no fix's
	fresh();
	rmc(true, 1000, "120000", "281226");
	gga(true, 1000u + 86400001u, "120001");
	CHECK(!position_utc_now(1000u + 86400001u, UINT32_MAX, &u, &src), "a day apart");
}

// Dates and times that aren't: none of them is a time
static void test_bad_fields(void)
{
	static const struct {
		const char *hms, *date;
		bool ok;
		int y, mo, d, h, mi, s;
	} cases[] = {
		{ "000000", "290224", true, 2024, 2, 29, 0, 0, 0 },	// a leap day
		{ "000000", "290223", false, 0, 0, 0, 0, 0, 0 },
		{ "000000", "290200", true, 2000, 2, 29, 0, 0, 0 },	// 2000 was a leap year
		{ "000000", "290299", false, 0, 0, 0, 0, 0, 0 },				// 1999 was not
		{ "235959", "311299", true, 1999, 12, 31, 23, 59, 59 },	// 80-99: the 1900s
		{ "000000", "010179", true, 2079, 1, 1, 0, 0, 0 },
		{ "000000", "310426", false, 0, 0, 0, 0, 0, 0 },
		{ "000000", "001126", false, 0, 0, 0, 0, 0, 0 },
		{ "000000", "321226", false, 0, 0, 0, 0, 0, 0 },
		{ "000000", "011326", false, 0, 0, 0, 0, 0, 0 },
		{ "240000", "221126", false, 0, 0, 0, 0, 0, 0 },
		{ "236000", "221126", false, 0, 0, 0, 0, 0, 0 },
		{ "235960", "221126", false, 0, 0, 0, 0, 0, 0 },				// a leap second's 60: skipped
		{ "000000", "010170", true, 2070, 1, 1, 0, 0, 0 },	// 70 is 2070 here
	};
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		uint32_t u = 0;
		uint8_t src;
		fresh();
		rmc(true, 1000, cases[i].hms, cases[i].date);
		bool ok = position_utc_now(1000, 0, &u, &src);
		uint32_t want = cases[i].ok ? ref(cases[i].y, cases[i].mo, cases[i].d, cases[i].h, cases[i].mi,
						  cases[i].s) : 0;
		CHECK(ok == cases[i].ok && (!ok || u == want), "%s %s: %d %u want %u", cases[i].date, cases[i].hms, ok, u,
		      want);
	}
}

// The millisecond clock wraps every 49.7 days: the age is a difference, so a stamp before the wrap ages right. A stamp
// just past now_ms (another task's, after the caller read the clock) is age 0; far past it is too old
static void test_clock(void)
{
	uint32_t u, t0 = ref(2026, 6, 1, 0, 0, 0);
	uint8_t src;

	fresh();
	utc_set(t0, 0xFFFFF000u, UTC_SRC_HOST);
	CHECK(position_utc_now(0x00001000u, 60000, &u, &src) && u == t0 + 8u, "across the wrap: %u", u - t0);
	rmc(true, 0xFFFFFC00u, "000000", "010626");
	CHECK(position_utc_now(0x00000400u, 60000, &u, &src) && u == t0 + 2u && src == UTC_SRC_GNSS, "GNSS across it");
	fresh();
	utc_set(t0, 5000, UTC_SRC_HOST);
	CHECK(position_utc_now(4500, 0, &u, &src) && u == t0, "a little ahead: age 0");
	CHECK(position_utc_now(4000, 0, &u, &src) && u == t0, "1 s ahead: age 0");
	CHECK(!position_utc_now(3999, 60000, &u, &src), "further ahead: too old");
	// No overflow at the end of the 32-bit clock
	utc_set(UINT32_MAX - 1u, 0, UTC_SRC_HOST);
	CHECK(position_utc_now(5000, 10000, &u, &src) && u == UINT32_MAX, "saturates");
}

// A reader that comes while utc_set is writing (it preempted the writer) leaves the set time out and doesn't wait
static void test_torn(void)
{
	uint32_t u, t0 = ref(2026, 11, 22, 0, 0, 0);
	uint8_t src;

	fresh();
	utc_set(t0, 1000, UTC_SRC_HOST);
	set.seq++;		// the writer stopped half way
	set.unix_s = 12345;
	CHECK(!position_utc_now(1000, 60000, &u, &src), "torn: left out");
	rmc(true, 1000, "000000", "221126");
	CHECK(position_utc_now(1000, 60000, &u, &src) && src == UTC_SRC_GNSS && u == t0, "torn: the other source");
	set.seq++;
	set.unix_s = t0 + 5u;
	set.ms = 2000;
	CHECK(position_utc_now(2000, 60000, &u, &src) && src == UTC_SRC_HOST && u == t0 + 5u, "whole again");
	CHECK((set.seq & 1u) == 0 && set.seq == 4u, "two writes: %u", set.seq);
}

int main(void)
{
	test_none();
	test_gnss_age();
	test_freshest();
	test_midnight();
	test_bad_fields();
	test_clock();
	test_torn();
	printf("positionUtcTest: %u checks, %u failed\n", checks, failed);
	return failed ? 1 : 0;
}
