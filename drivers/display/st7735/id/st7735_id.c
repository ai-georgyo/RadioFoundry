// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Panel type from the controller's ID (st7735_id.h)
#include <stddef.h>
#include <stdbool.h>
#include "st7735_id.h"

const char *st7735_panel_from_id(const uint8_t id[3], int16_t stored_type, uint8_t *type, const char **source)
{
	bool stored = stored_type >= 0 && stored_type != 0xFF;
	const char *controller = "unknown";
	const char *src = "id";
	uint8_t t;

	if (id[0] == 0x00 && id[1] == 0x91) {
		// The ID picks the init; a stored type made for the other controller still tells this one's orientation by
		// its bit 0 (0 and 2 are the flipped ones)
		controller = "gc9xxx";
		t = stored && (stored_type & 1) == 0 ? 2 : 3;
	} else if (id[0] == 0x7C) {
		controller = "st7735";
		t = stored && (stored_type & 3) == 1 ? 1 : 0;
	} else if (stored) {
		t = (uint8_t)(stored_type & 3);
		src = "factory";
	} else {
		t = 0;
		src = "default";
	}
	if (type != NULL) {
		*type = t;
	}
	if (source != NULL) {
		*source = src;
	}
	return controller;
}
