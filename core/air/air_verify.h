// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What a board's provider glue adds to a fit (hal_air.h: air_profile_t.verify, AIR_FIT_UNTESTED). A board names the
// profiles checked on air on it as a set of AIR_VERIFY_* bits (its glue's air_verified); a fit for a profile whose bit
// is not in the set says AIR_FIT_UNTESTED. A profile with verify 0 is never verified; the debug link's raw bench sends
// AIR_VERIFY_ALL in its short header, so any board with a set takes it as before the set had bits.
#ifndef RFOUNDRY_AIR_VERIFY_H
#define RFOUNDRY_AIR_VERIFY_H

#include <stdint.h>

#include "../../hal/hal_air.h"

static inline uint32_t air_fit_verify(uint32_t fit, uint8_t verified, const air_profile_t *p)
{
	return fit != 0 && (verified & p->verify) == 0 ? fit | AIR_FIT_UNTESTED : fit;
}

#endif
