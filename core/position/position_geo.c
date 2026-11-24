// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Distance and bearing between two positions (position_geo.h, docs/spec/position.md 9): the haversine and the
// initial bearing on a 6371 km sphere, in single precision with polynomials instead of libm's sinf, cosf, atan2f and
// sqrtf (4 KB of flash with their range reduction, which angles of at most pi never need). Each polynomial is good to
// 1e-5 rad or better; the haversine and the bearing are written without the cancellations the textbook forms have,
// so a station a few meters away still gets a true bearing.
#include <stdbool.h>
#include <stddef.h>
#include "position/position_geo.h"

#define GEO_DEG		10000000L	// position units per degree (position.h: 1e-7 degree)
#define GEO_LAT_MAX	(90L * GEO_DEG)
#define PI_F		3.14159265f
#define EARTH_M		6371000.0f

// sin x for |x| <= pi / 2: the Taylor series to x^13, error under 1e-9 (less than the float's own step)
static float sin_h(float x)
{
	float x2 = x * x;
	return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f *
		(1.0f - x2 / 156.0f))))));
}

// cos x for |x| <= pi / 2: to x^12, error under 7e-9
static float cos_h(float x)
{
	float x2 = x * x;
	return 1.0f - x2 / 2.0f * (1.0f - x2 / 12.0f * (1.0f - x2 / 30.0f * (1.0f - x2 / 56.0f * (1.0f - x2 / 90.0f *
		(1.0f - x2 / 132.0f)))));
}

// sqrt of v >= 0: a bit-pattern first guess (within 4 %) and three Newton steps, exact to the float's last bit or two
static float sqrt_f(float v)
{
	if (!(v > 0.0f)) {
		return 0.0f;
	}
	union { float f; uint32_t u; } g = { v };
	g.u = 0x1FBD1DF5u + (g.u >> 1);
	for (int i = 0; i < 3; i++) {
		g.f = 0.5f * (g.f + v / g.f);
	}
	return g.f;
}

// atan2(y, x) in radians, (-pi, pi], 0 for (0, 0): atan on the octant 0..1 by Abramowitz and Stegun 4.4.49, error
// under 1e-5 rad
static float atan2_f(float y, float x)
{
	float ax = x < 0.0f ? -x : x, ay = y < 0.0f ? -y : y;
	if (ax == 0.0f && ay == 0.0f) {
		return 0.0f;
	}
	bool swap = ay > ax;
	float t = swap ? ax / ay : ay / ax, t2 = t * t;
	float a = t * (0.9998660f - t2 * (0.3302995f - t2 * (0.1801410f - t2 * (0.0851330f - t2 * 0.0208351f))));
	if (swap) {
		a = PI_F / 2.0f - a;
	}
	if (x < 0.0f) {
		a = PI_F - a;
	}
	return y < 0.0f ? -a : a;
}

// A latitude past a pole (no real position, but any int32 may arrive from the air) is the pole: the polynomials hold
// to pi / 2 only, and the differences below then fit 32 bits
static int32_t lat_clamp(int32_t lat)
{
	return lat > GEO_LAT_MAX ? (int32_t)GEO_LAT_MAX : lat < -GEO_LAT_MAX ? (int32_t)-GEO_LAT_MAX : lat;
}

// The longitude difference to the short way round, at most 180 degrees: one turn added or taken is enough for any two
// int32 longitudes (their difference is under 430 degrees)
static int32_t dlon_e7(int32_t lon1, int32_t lon2)
{
	int64_t d = (int64_t)lon2 - lon1;
	if (d > 180LL * GEO_DEG) {
		d -= 360LL * GEO_DEG;
	} else if (d < -180LL * GEO_DEG) {
		d += 360LL * GEO_DEG;
	}
	return (int32_t)d;
}

static float rad(int32_t e7)
{
	return (float)e7 * (PI_F / 180.0f / (float)GEO_DEG);
}

// The haversine's central angle in radians
static float central(int32_t lat1, int32_t lat2, int32_t dlon)
{
	float sp = sin_h(rad(lat2 - lat1) / 2.0f), sl = sin_h(rad(dlon) / 2.0f);
	float a = sp * sp + cos_h(rad(lat1)) * cos_h(rad(lat2)) * sl * sl;
	if (a > 1.0f) {
		a = 1.0f;
	}
	return 2.0f * atan2_f(sqrt_f(a), sqrt_f(1.0f - a));
}

static uint32_t distance_m(int32_t lat1, int32_t lat2, int32_t dl)
{
	float c = central(lat1, lat2, dl);
	if (c > PI_F / 2.0f) {
		// Past a quarter of the globe the float cannot resolve 1 - a (a step of 1.2e-7 is km of arc near the
		// antipode): measure to the antipode instead, which is a short distance, and take it from pi
		c = PI_F - central(lat1, -lat2, dl > 0 ? dl - (int32_t)(180 * GEO_DEG) : dl + (int32_t)(180 * GEO_DEG));
	}
	return (uint32_t)(EARTH_M * c + 0.5f);
}

static uint16_t bearing_deg(int32_t lat1, int32_t lat2, int32_t dl)
{
	// y = sin(dl) cos(p2); x = cos(p1) sin(p2) - sin(p1) cos(p2) cos(dl) = sin(dp) + 2 sin(p1) cos(p2) sin(dl/2)^2
	float hp = rad(lat2 - lat1) / 2.0f, hl = rad(dl) / 2.0f;
	float sl = sin_h(hl), c2 = cos_h(rad(lat2));
	float y = 2.0f * sl * cos_h(hl) * c2;
	float x = 2.0f * sin_h(hp) * cos_h(hp) + 2.0f * sin_h(rad(lat1)) * c2 * sl * sl;
	float b = atan2_f(y, x) * (180.0f / PI_F);
	if (b < 0.0f) {
		b += 360.0f;
	}
	uint16_t d = (uint16_t)(b + 0.5f);
	return d >= 360u ? 0u : d;
}

void position_range(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7, uint32_t *meters,
		    uint16_t *bearing_deg_out)
{
	int32_t la1 = lat_clamp(lat1_e7), la2 = lat_clamp(lat2_e7), dl = dlon_e7(lon1_e7, lon2_e7);

	if (meters != NULL) {
		*meters = distance_m(la1, la2, dl);
	}
	if (bearing_deg_out != NULL) {
		*bearing_deg_out = bearing_deg(la1, la2, dl);
	}
}
