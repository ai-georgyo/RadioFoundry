// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The st7735 driver's panel type from the controller's ID (drivers/display/st7735/id): the controller picks the
// init, a stored type only the orientation, and a panel that answers nothing gets the stored type or type 0
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "display/st7735/id/st7735_id.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const uint8_t GC9102[3] = { 0x00, 0x91, 0x02 };
static const uint8_t GC9106[3] = { 0x00, 0x91, 0x06 };
static const uint8_t ST7735S[3] = { 0x7C, 0x89, 0xF0 };
static const uint8_t NONE[3] = { 0x00, 0x00, 0x00 };	// not read, or a bus nothing drives pulled low
static const uint8_t BLANK[3] = { 0xFF, 0xFF, 0xFF };	// ... pulled high
static const uint8_t OTHER[3] = { 0x5C, 0x89, 0xF0 };

static uint8_t type_of(int16_t stored, const uint8_t id[3])
{
	uint8_t type = 0xEE;
	(void)st7735_panel_from_id(id, stored, &type, NULL);
	return type;
}

static bool is(const char *a, const char *b)
{
	return a != NULL && strcmp(a, b) == 0;
}

int main(void)
{
	// The unlock variant: normal (3) unless the stored type says flipped (bit 0 clear)
	CHECK(type_of(-1, GC9102) == 3);
	CHECK(type_of(0xFF, GC9102) == 3);	// an erased byte says nothing
	CHECK(type_of(3, GC9102) == 3);
	CHECK(type_of(2, GC9102) == 2);
	CHECK(type_of(1, GC9102) == 3);		// the other controller's types: only the orientation is taken
	CHECK(type_of(0, GC9102) == 2);
	CHECK(type_of(0xFE, GC9102) == 2);	// only the low bits count
	CHECK(type_of(-1, GC9106) == 3);	// the third ID byte is not looked at

	// ST7735 class: normal (0) unless the stored type says rotated (1)
	CHECK(type_of(-1, ST7735S) == 0);
	CHECK(type_of(0xFF, ST7735S) == 0);
	CHECK(type_of(0, ST7735S) == 0);
	CHECK(type_of(1, ST7735S) == 1);
	CHECK(type_of(5, ST7735S) == 1);
	CHECK(type_of(2, ST7735S) == 0);	// the unlock variant's types never give this controller that init
	CHECK(type_of(3, ST7735S) == 0);

	// An ID nobody knows, or none: the stored type, type 0 where there is none
	CHECK(type_of(-1, NONE) == 0);
	CHECK(type_of(-1, BLANK) == 0);
	CHECK(type_of(0xFF, NONE) == 0);
	CHECK(type_of(-1, OTHER) == 0);
	CHECK(type_of(2, NONE) == 2);
	CHECK(type_of(0x07, BLANK) == 3);
	CHECK(type_of(1, OTHER) == 1);
	CHECK(type_of(-200, GC9102) == 3);	// any negative value: none
	CHECK(type_of(-200, NONE) == 0);
	// 0x00 0x91 needs both bytes, 0x7C only the first
	CHECK(type_of(-1, (const uint8_t[3]){ 0x00, 0x90, 0x02 }) == 0);
	CHECK(type_of(-1, (const uint8_t[3]){ 0x01, 0x91, 0x02 }) == 0);
	CHECK(type_of(-1, (const uint8_t[3]){ 0x7C, 0x00, 0x00 }) == 0);

	// The controller's name and what chose the type, as the debug link shows them (docs/spec/debuglink.md,
	// display.panel_controller and display.panel_source)
	uint8_t type = 0xEE;
	const char *source = NULL;
	CHECK(is(st7735_panel_from_id(GC9102, -1, &type, &source), "gc9xxx") && is(source, "id") && type == 3);
	CHECK(is(st7735_panel_from_id(GC9102, 2, &type, &source), "gc9xxx") && is(source, "id") && type == 2);
	CHECK(is(st7735_panel_from_id(ST7735S, 1, &type, &source), "st7735") && is(source, "id") && type == 1);
	CHECK(is(st7735_panel_from_id(NONE, 2, &type, &source), "unknown") && is(source, "factory") && type == 2);
	CHECK(is(st7735_panel_from_id(BLANK, 0xFF, &type, &source), "unknown") && is(source, "default") && type == 0);
	CHECK(is(st7735_panel_from_id(OTHER, -1, &type, &source), "unknown") && is(source, "default") && type == 0);
	// The first ID byte alone names the ST7735 class, whatever follows it: with a stored type of the other
	// controller an unknown panel would get that type, this one never does
	const uint8_t st_bare[3] = { 0x7C, 0x00, 0x00 };
	CHECK(is(st7735_panel_from_id(st_bare, -1, &type, &source), "st7735") && is(source, "id") && type == 0);
	CHECK(is(st7735_panel_from_id(st_bare, 3, &type, &source), "st7735") && is(source, "id") && type == 0);
	CHECK(is(st7735_panel_from_id(OTHER, 3, &type, &source), "unknown") && is(source, "factory") && type == 3);

	// Either result may be left out
	source = NULL;
	CHECK(is(st7735_panel_from_id(ST7735S, -1, NULL, &source), "st7735") && is(source, "id"));
	CHECK(is(st7735_panel_from_id(GC9102, -1, NULL, NULL), "gc9xxx"));

	// Every type the function can return is one the driver has
	for (int stored = -1; stored < 256; stored++) {
		CHECK(type_of((int16_t)stored, GC9102) >= 2 && type_of((int16_t)stored, GC9102) <= 3);
		CHECK(type_of((int16_t)stored, ST7735S) <= 1);
		CHECK(type_of((int16_t)stored, NONE) <= 3);
	}

	printf("st7735IdTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
