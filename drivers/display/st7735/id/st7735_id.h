// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The st7735 driver's panel type (drivers/display/st7735/st7735.h) for a panel whose controller differs between
// units: the controller from its own ID, the orientation from the type the unit has stored where it has one. No
// hardware access: the board reads the ID and the stored type and passes them in. Its own directory, so only a board
// that lists it builds it.
//
// boards/md380/panel_md380.c still has its own copy of this rule; it goes when that board moves to this function.
#ifndef ST7735_ID_H
#define ST7735_ID_H

#include <stdint.h>

// id: the three bytes the read display ID command (0x04) returns after its dummy byte; all zero when it wasn't read.
// stored_type: the panel type a board has stored for this unit (the factory's byte); only its low bits count. -1:
// none, and 0xFF (an erased byte) says nothing either.
//
//   ID 00 91 xx  "gc9xxx" (the unlock variant's init): type 3, or 2 (flipped) where the stored type has bit 0 clear
//   ID 7C xx xx  "st7735": type 0, or 1 (rotated 180 degrees) where the stored type's low two bits are 1
//   another ID   "unknown": the stored type's low two bits, 0 with none
//
// Returns the controller's name. *type: the driver's type. *source: what chose it, "id" (the controller was
// recognized), "factory" (the stored type alone) or "default". Either pointer may be NULL
const char *st7735_panel_from_id(const uint8_t id[3], int16_t stored_type, uint8_t *type, const char **source);

#endif
