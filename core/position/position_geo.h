// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Distance and bearing between two positions (docs/spec/position.md, the geometry section): the one helper every
// mode's "12.3 km NE" comes from (APRS stations, DMR GPS and LRRP positions, M17 GNSS META, the heard list). It
// replaced the two mode-named copies APRS and the DMR call stack had: no other may come back. No libm
// and no double (a double cos and sqrt cost about 8 KB of flash on a soft-float build): integer and single-precision
// math, within 0.5 % of the great circle up to 1,000 km and within 1 % beyond, bearing within 1 degree. Coordinates in
// degrees x 1e7 (position.h). Pure, reentrant. Built with CONFIG_POSITION (APRS, DMR_GPS, DMR_LRRP and M17 imply it).
#ifndef POSITION_GEO_H
#define POSITION_GEO_H

#include <stdint.h>

// From position 1 to position 2: meters, and the initial bearing in degrees (0 north, 90 east, 0..359). Either output
// may be NULL
void position_range(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7, uint32_t *meters,
		    uint16_t *bearing_deg);

#endif
