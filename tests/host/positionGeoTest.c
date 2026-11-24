// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The one distance and bearing helper (core/position/position_geo.c, position_geo.h; docs/spec/position.md 9)
// against the haversine and the initial bearing in double precision with the C library: known pairs (a few meters,
// 100 km, 1,000 km, nearly antipodal, across the date line and over the poles) and seeded random pairs at every
// range, held to the header's contract (0.5 % up to 1,000 km, 1 % beyond, the bearing within 1 degree), and the
// edges: either output NULL, latitudes past a pole, longitudes a turn apart
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "position/position_geo.h"
#include "fuzz.h"

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

#define DEG		10000000	// 1e-7 degree
#define LAT_MAX		(90 * DEG)
#define LON_MAX		(180 * DEG)
#define HALF_EARTH_M	20015086.8	// pi * 6371 km

// The reference: the haversine on the same 6371 km sphere and the initial bearing, in double
static void ref(int32_t la1, int32_t lo1, int32_t la2, int32_t lo2, double *m, double *deg)
{
	const double k = M_PI / 180.0 / 1e7;
	double p1 = la1 * k, p2 = la2 * k, dl = ((double)lo2 - lo1) * k;
	double a = sin((p2 - p1) / 2) * sin((p2 - p1) / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
	*m = 6371000.0 * 2 * atan2(sqrt(a), sqrt(1 - (a > 1 ? 1 : a)));
	double b = atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) * 180 / M_PI;
	*deg = b < 0 ? b + 360 : b;
}

static double bearing_err(double got, double want)
{
	double e = fabs(got - want);
	return e > 180 ? 360 - e : e;
}

// The contract: 0.5 % of the great circle up to 1,000 km, 1 % beyond (and a meter for rounding)
static bool distance_ok(uint32_t got, double want)
{
	double tol = (want <= 1000000.0 ? 0.005 : 0.01) * want + 1.0;
	return fabs((double)got - want) <= tol;
}

static double worst_rel_short, worst_rel_long, worst_deg;

static void pair(const char *name, double lat1, double lon1, double lat2, double lon2, bool check_bearing)
{
	int32_t la1 = (int32_t)lround(lat1 * DEG), lo1 = (int32_t)lround(lon1 * DEG);
	int32_t la2 = (int32_t)lround(lat2 * DEG), lo2 = (int32_t)lround(lon2 * DEG);
	uint32_t m;
	uint16_t b;
	double rm, rb;

	position_range(la1, lo1, la2, lo2, &m, &b);
	ref(la1, lo1, la2, lo2, &rm, &rb);
	CHECK(distance_ok(m, rm), "%s: %u m, reference %.1f", name, m, rm);
	CHECK(b < 360 && (!check_bearing || bearing_err(b, rb) <= 1.0), "%s: %u deg, reference %.2f", name, b, rb);
}

static void test_known(void)
{
	// Short: meters to a few km
	pair("50 m east at 33.7 N", 33.7, -84.1, 33.7, -84.0994601, true);
	pair("10 m north", 51.5, -0.12, 51.50009, -0.12, true);
	pair("2 km south-west", 45.0, 7.0, 44.9873, 6.982, true);
	// 100 km
	pair("100 km north", 45.0, 7.0, 45.8993, 7.0, true);
	pair("100 km east at 60 N", 60.0, 10.0, 60.0, 11.7986, true);
	pair("100 km diagonal", -33.9, 18.4, -33.3, 19.2, true);
	// 1,000 km
	pair("1,000 km on the equator", 0.0, 0.0, 0.0, 8.9932, true);
	pair("1,000 km north-east", 40.0, -100.0, 46.3, -91.6, true);
	pair("London to Rome", 51.5074, -0.1278, 41.9028, 12.4964, true);
	// Long
	pair("Anchorage to Tokyo", 61.2181, -149.9003, 35.6762, 139.6503, true);
	pair("Sydney to Santiago", -33.8688, 151.2093, -33.4489, -70.6693, true);
	pair("Quito to Singapore", -0.18, -78.47, 1.35, 103.82, true);
	// Nearly antipodal: the far side of the globe, the bearing any (all roads lead there)
	pair("0.1 deg short of the antipode", 33.7, -84.1, -33.7, 95.8, false);
	pair("10 km short of the antipode, equator", 0.0, 0.0, 0.0, 179.91, false);
	pair("antipodes", 33.7, -84.1, -33.7, 95.9, false);
	// The date line, either way
	pair("over the date line westward", 10.0, 179.9, 10.5, -179.8, true);
	pair("over the date line eastward", -20.0, -179.95, -20.1, 179.95, true);
	pair("180 E to 179 W", 0.0, 180.0, 0.0, -179.0, true);
	// The poles: over one, to one, near one
	pair("over the north pole", 89.9, 0.0, 89.9, 180.0, true);
	pair("over the south pole", -89.5, 30.0, -89.5, -150.0, true);
	pair("to the north pole", 45.0, 12.0, 90.0, 0.0, true);
	pair("from near the south pole", -89.99, 0.0, -80.0, 90.0, true);
	pair("pole to pole", 90.0, 0.0, -90.0, 0.0, false);

	// Anchors: a degree of latitude is 111.195 km on this sphere (the float gives it within 2e-4); the four quarters
	uint32_t m;
	uint16_t b;
	position_range(0, 0, DEG, 0, &m, &b);
	CHECK(m >= 111173 && m <= 111217 && b == 0, "1 deg north: %u %u", m, b);
	position_range(0, 0, 0, DEG, &m, &b);
	CHECK(b == 90, "east %u", b);
	position_range(0, 0, -DEG, 0, &m, &b);
	CHECK(b == 180, "south %u", b);
	position_range(0, 0, 0, -DEG, &m, &b);
	CHECK(b == 270, "west %u", b);
	position_range(337000000, -841000000, 337000000, -841000000, &m, &b);
	CHECK(m == 0 && b == 0, "same point");
}

// Seeded random pairs at spans from centimeters to the globe: the contract everywhere, the bearing where it means
// something (not within 10 km of a pole, not near the antipode, not a point onto itself)
static void test_random(void)
{
	static const int32_t span[] = { 100, 10000, 1000000, 10000000, 100000000, 900000000 };
	fuzz_t f;
	fuzz_init(&f, 0x6E0C);
	long n = fuzz_iters(200000), bad = 0;

	for (long i = 0; i < n && bad < 5; i++) {
		int32_t la1 = (int32_t)fuzz_below(&f, 2u * LAT_MAX + 1u) - LAT_MAX;
		int32_t lo1 = (int32_t)fuzz_below(&f, 2u * LON_MAX + 1u) - LON_MAX;
		int32_t sp = span[fuzz_below(&f, sizeof span / sizeof span[0])];
		int64_t a = (int64_t)la1 + (int64_t)fuzz_below(&f, 2u * (uint32_t)sp + 1u) - sp;
		int64_t o = (int64_t)lo1 + (int64_t)fuzz_below(&f, 2u * (uint32_t)sp + 1u) - sp;
		int32_t la2 = (int32_t)(a > LAT_MAX ? LAT_MAX : a < -LAT_MAX ? -LAT_MAX : a);
		int32_t lo2 = (int32_t)(o > LON_MAX ? o - 2 * (int64_t)LON_MAX : o < -LON_MAX ? o + 2 * (int64_t)LON_MAX : o);
		uint32_t m, m21;
		uint16_t b;
		double rm, rb;

		position_range(la1, lo1, la2, lo2, &m, &b);
		position_range(la2, lo2, la1, lo1, &m21, NULL);
		ref(la1, lo1, la2, lo2, &rm, &rb);
		double rel = rm > 1000.0 ? fabs(m - rm) / rm : 0.0;	// below a km the meter's rounding is most of it
		if (rm <= 1000000.0 && rel > worst_rel_short) {
			worst_rel_short = rel;
		} else if (rm > 1000000.0 && rel > worst_rel_long) {
			worst_rel_long = rel;
		}
		bool ok = distance_ok(m, rm) && (m > m21 ? m - m21 : m21 - m) <= 2u && b < 360;
		bool polar = la1 > LAT_MAX - 1000000 || la1 < -LAT_MAX + 1000000 || la2 > LAT_MAX - 1000000 ||
			     la2 < -LAT_MAX + 1000000;
		if (!polar && rm > 20.0 && rm < HALF_EARTH_M - 20000.0) {
			double e = bearing_err(b, rb);
			if (e > worst_deg) {
				worst_deg = e;
			}
			ok = ok && e <= 1.0;
		}
		if (!ok) {
			bad++;
			CHECK(false, "(%d,%d)-(%d,%d): %u m (back %u), ref %.1f; %u deg, ref %.2f", la1, lo1, la2, lo2, m, m21,
			      rm, b, rb);
		}
	}
	checks++;
	printf("positionGeoTest: worst %.2e of the range from 1 to 1,000 km, %.2e beyond, %.2f deg\n", worst_rel_short,
	       worst_rel_long, worst_deg);
}

// Either output may be NULL; latitudes past a pole are the pole; a longitude a turn on is the same place
static void test_edges(void)
{
	uint32_t m = 1, m2;
	uint16_t b = 1, b2;

	position_range(0, 0, DEG, DEG, NULL, NULL);
	position_range(0, 0, DEG, DEG, &m, NULL);
	position_range(0, 0, DEG, DEG, NULL, &b);
	position_range(0, 0, DEG, DEG, &m2, &b2);
	CHECK(m == m2 && b == b2 && b == 45, "NULL outputs: %u %u", m, b);
	position_range(INT32_MIN, 0, INT32_MAX, 0, &m, &b);
	CHECK(fabs(m - HALF_EARTH_M) <= 50.0 && b < 360, "past the poles: %u", m);
	position_range(LAT_MAX + 1, 5, 45 * DEG, 5, &m, &b);
	position_range(LAT_MAX, 5, 45 * DEG, 5, &m2, &b2);
	CHECK(m == m2 && b == b2 && b == 180, "past the north pole is the pole: %u %u", m, b);
	// 170 E and 190 W are one place; the longest int32 longitudes too (214.7 E is 145.3 W)
	position_range(45 * DEG, 170 * DEG, 46 * DEG, 171 * DEG, &m, &b);
	position_range(45 * DEG, -190 * DEG, 46 * DEG, 171 * DEG, &m2, &b2);
	CHECK(m == m2 && b == b2, "a turn apart: %u %u / %u %u", m, b, m2, b2);
	position_range(0, INT32_MAX, 0, INT32_MIN, &m, &b);
	double rm, rb;
	ref(0, INT32_MAX, 0, INT32_MIN, &rm, &rb);
	CHECK(distance_ok(m, rm) && b == 270, "int32 longitudes: %u ref %.0f, %u", m, rm, b);
}

int main(void)
{
	test_known();
	test_random();
	test_edges();
	printf("positionGeoTest: %u checks, %u failed\n", checks, failed);
	return failed ? 1 : 0;
}
