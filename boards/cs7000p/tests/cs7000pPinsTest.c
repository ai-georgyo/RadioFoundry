// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS's idle pin states (pinsetup_cs7000p.c, built here with the real pins.h) on a fake GPIO: every
// pin set once, no line with a chip's output or a contact to ground on it ever an output, no line of the LCD
// board's connector an output, the selects high, and the outputs exactly the ones written out below. The pins are
// named here by port and number, from the pin table of docs/spec/hardware-cs7000p.md 3: a wrong line in pins.h fails
// as well. Also the rule for the software entry to the ROM bootloader, which drives some of these pins
// (romentry_cs7000p.h).
#include <stdio.h>
#include <string.h>
#include "cs7000p.h"
#include "pins.h"
#include "romentry_cs7000p.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

GPIO_TypeDef fake_ports[5];

enum { M_NONE, M_ANALOG, M_IN, M_OUT, M_AF };

static struct {
	uint8_t mode, pull, calls;
	bool level, open_drain, forced;
} pin[5][16];

static void record(GPIO_TypeDef *port, uint16_t pins, uint8_t mode, uint32_t pull, bool level, bool open_drain,
		   bool forced)
{
	unsigned p = (unsigned)(port - fake_ports);
	if (p >= 5 || pins == 0) {
		printf("FAIL a pin call on no port or no pin\n");
		failures++;
		return;
	}
	for (unsigned n = 0; n < 16; n++) {
		if (pins & (1u << n)) {
			pin[p][n].mode = mode;
			pin[p][n].pull = (uint8_t)pull;
			pin[p][n].level = level;
			pin[p][n].open_drain = open_drain;
			pin[p][n].forced = forced;
			pin[p][n].calls++;
		}
	}
}

void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool open_drain)
{
	record(port, pins, M_OUT, GPIO_NOPULL, level, open_drain, false);
}

void stm32_pin_force(GPIO_TypeDef *port, uint16_t pins, bool level)
{
	record(port, pins, M_OUT, GPIO_NOPULL, level, false, true);
}

void stm32_pin_in(GPIO_TypeDef *port, uint16_t pins, uint32_t pull)
{
	record(port, pins, M_IN, pull, false, false, false);
}

void stm32_pin_af(GPIO_TypeDef *port, uint16_t pins, uint8_t af, bool open_drain, uint32_t pull)
{
	(void)af;
	record(port, pins, M_AF, pull, false, open_drain, false);
}

void stm32_pin_analog(GPIO_TypeDef *port, uint16_t pins)
{
	record(port, pins, M_ANALOG, GPIO_NOPULL, false, false, false);
}

// A pin by port letter and number
typedef struct { char port; uint8_t n; } pn_t;
#define AT(x)	(&pin[(x).port - 'A'][(x).n])

// A chip's output or a contact to ground is on the net: never driven push-pull. The last two only until the modem
// is known not to be the master of its I2S port, which nothing here can know
static const pn_t never_driven[] = {
	{ 'C', 0 }, { 'C', 2 }, { 'C', 3 },	// the modem's interrupt lines
	{ 'B', 14 }, { 'B', 4 }, { 'C', 12 },	// its data outputs: configuration port, voice port, I2S
	{ 'A', 2 }, { 'C', 1 },			// op-amp outputs: sub-audio, tone
	{ 'A', 10 }, { 'C', 7 }, { 'A', 1 },	// the far ends' transmit lines: Bluetooth, GPS, accessory UART
	{ 'E', 14 }, { 'E', 15 }, { 'B', 10 }, { 'B', 11 },	// the selector's contacts
	{ 'E', 10 }, { 'E', 11 },		// top key, external PTT
	{ 'A', 8 }, { 'D', 15 }, { 'E', 12 }, { 'E', 13 },	// PTT, SK1, SK2, SK3: contacts behind 1k
	{ 'C', 15 }, { 'B', 8 }, { 'B', 9 },	// buses of parts that are not used
	{ 'E', 5 },				// the data flash's output
	{ 'B', 0 }, { 'B', 1 },			// the IF receiver's level outputs
	{ 'D', 11 },				// the synthesizer's lock-detect output
	{ 'A', 15 }, { 'C', 10 },
};

// The LCD board's connector, J801: display bus, key matrix, control lines
static const pn_t lcd_connector[] = {
	{ 'D', 0 }, { 'D', 1 }, { 'D', 2 }, { 'D', 3 }, { 'D', 4 }, { 'D', 5 }, { 'D', 6 }, { 'D', 7 },
	{ 'B', 7 }, { 'D', 12 }, { 'D', 13 }, { 'D', 14 }, { 'E', 0 }, { 'E', 1 },
};

// Every output there may be after the set-up, with its level
static const struct { pn_t p; bool level; const char *what; } outputs[] = {
	{ { 'B', 12 }, true, "modem configuration select" },	{ { 'B', 13 }, false, "modem configuration clock" },
	{ { 'B', 15 }, false, "modem configuration data in" },	{ { 'B', 6 }, true, "modem voice select" },
	{ { 'B', 3 }, false, "modem voice clock" },		{ { 'B', 5 }, false, "modem voice data in" },
	{ { 'D', 9 }, true, "synthesizer select" },		{ { 'D', 8 }, false, "synthesizer data" },
	{ { 'D', 10 }, false, "synthesizer clock" },		{ { 'C', 14 }, false, "IF receiver reset" },
	{ { 'E', 3 }, false, "IF receiver clock" },		{ { 'E', 4 }, true, "data flash select" },
	{ { 'E', 2 }, false, "data flash clock" },		{ { 'E', 6 }, false, "data flash data in" },
	{ { 'C', 8 }, false, "sub-audio out" },			{ { 'C', 9 }, false, "backlight" },
};

static bool listed_output(unsigned p, unsigned n, bool *level)
{
	for (unsigned i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
		if ((unsigned)(outputs[i].p.port - 'A') == p && outputs[i].p.n == n) {
			*level = outputs[i].level;
			return true;
		}
	}
	return false;
}

int main(void)
{
	unsigned set = 0;

	cs7000p_reset_levels();

	// Every pin once: all of ports A to E but the expander's three lines and the debug port
	for (unsigned p = 0; p < 5; p++) {
		for (unsigned n = 0; n < 16; n++) {
			const bool left_alone = (p == 4 && n >= 7 && n <= 9) || (p == 0 && (n == 13 || n == 14));
			if (pin[p][n].calls != (left_alone ? 0u : 1u)) {
				printf("FAIL P%c%u set %u times\n", 'A' + p, n, pin[p][n].calls);
				failures++;
			}
			set += pin[p][n].calls;
			// Nothing is forced here (the reset hook's way), nothing is an alternate function yet, and no
			// output is open drain
			CHECK(!pin[p][n].forced && pin[p][n].mode != M_AF && !pin[p][n].open_drain);
			bool level = false;
			if (listed_output(p, n, &level)) {
				if (pin[p][n].mode != M_OUT || pin[p][n].level != level) {
					printf("FAIL P%c%u is not an output at %d\n", 'A' + p, n, level);
					failures++;
				}
			} else if (pin[p][n].mode == M_OUT) {
				printf("FAIL P%c%u is an output\n", 'A' + p, n);
				failures++;
			}
		}
	}
	CHECK(set == 75);

	for (unsigned i = 0; i < sizeof(never_driven) / sizeof(never_driven[0]); i++) {
		if (AT(never_driven[i])->mode == M_OUT || AT(never_driven[i])->mode == M_AF) {
			printf("FAIL P%c%u is driven\n", never_driven[i].port, never_driven[i].n);
			failures++;
		}
	}
	// The LCD connector: inputs with pull-ups, every line, until the display code has checked the board
	for (unsigned i = 0; i < sizeof(lcd_connector) / sizeof(lcd_connector[0]); i++) {
		if (AT(lcd_connector[i])->mode != M_IN || AT(lcd_connector[i])->pull != GPIO_PULLUP) {
			printf("FAIL P%c%u is not an input with a pull-up\n", lcd_connector[i].port, lcd_connector[i].n);
			failures++;
		}
	}
	// The four selects high (the IF receiver's is an expander bit)
	CHECK(AT(((pn_t){ 'B', 12 }))->level && AT(((pn_t){ 'B', 6 }))->level && AT(((pn_t){ 'D', 9 }))->level &&
	      AT(((pn_t){ 'E', 4 }))->level);
	// Lines nothing defines: analog, no input stage left on them
	CHECK(pin[0][0].mode == M_ANALOG && pin[0][1].mode == M_ANALOG && pin[0][9].mode == M_ANALOG &&
	      pin[0][10].mode == M_ANALOG);
	// The DAC's pin is not driven: the pull-down on the board keeps the PA's reference at 0 V
	CHECK(pin[0][4].mode == M_ANALOG && pin[0][5].mode == M_ANALOG);
	// The scan's six inputs, and USB until its driver starts
	CHECK(pin[0][6].mode == M_ANALOG && pin[2][5].mode == M_ANALOG && pin[2][4].mode == M_ANALOG &&
	      pin[1][0].mode == M_ANALOG && pin[1][1].mode == M_ANALOG && pin[0][2].mode == M_ANALOG);
	CHECK(pin[0][11].mode == M_ANALOG && pin[0][12].mode == M_ANALOG);
	// PC13 and PC14 are backup-domain pads: one output at a time. PC14 is the static one; PC13 is an input with a
	// pull-down outside a frame
	CHECK(pin[2][13].mode == M_IN && pin[2][13].pull == GPIO_PULLDOWN && pin[2][14].mode == M_OUT && !pin[2][14].level);
	CHECK(pin[2][15].mode == M_IN);
	// The I2S pads are inputs: the platform's probe decides which one the MCU may drive
	CHECK(pin[0][15].mode == M_IN && pin[2][10].mode == M_IN && pin[2][11].mode == M_IN && pin[2][12].mode == M_IN);
	// Keys and the selector: inputs without a pull of ours (the board has the pull-ups)
	CHECK(pin[0][8].mode == M_IN && pin[0][8].pull == GPIO_NOPULL && pin[3][15].mode == M_IN &&
	      pin[4][12].mode == M_IN && pin[4][13].mode == M_IN && pin[4][10].mode == M_IN && pin[4][11].mode == M_IN);
	CHECK(pin[4][14].pull == GPIO_NOPULL && pin[4][15].pull == GPIO_NOPULL && pin[1][10].pull == GPIO_NOPULL &&
	      pin[1][11].pull == GPIO_NOPULL);

	// The ROM entry: never without the bench knob, whatever the loader and the selector say
	static const uint8_t loaders[] = { 0x90u, 0x91u, 0x92u, 0xD2u, 0xD3u, 0x00u, 0xFFu };
	for (unsigned i = 0; i < sizeof(loaders); i++) {
		CHECK(!cs7000p_rom_entry_ok(false, loaders[i], false) && !cs7000p_rom_entry_ok(false, loaders[i], true));
		// With the knob: PB10's contact open is enough for any loader
		CHECK(cs7000p_rom_entry_ok(true, loaders[i], true));
	}
	// With the knob and the contact closed: version 9.1 only. Not "9.1 and later": nothing says what 9.2 does with
	// the pins, and the two oldest loaders have the largest bytes
	CHECK(cs7000p_rom_entry_ok(true, 0x91u, false));
	CHECK(!cs7000p_rom_entry_ok(true, 0x90u, false) && !cs7000p_rom_entry_ok(true, 0x92u, false));
	CHECK(!cs7000p_rom_entry_ok(true, 0xD2u, false) && !cs7000p_rom_entry_ok(true, 0xD3u, false));
	CHECK(!cs7000p_rom_entry_ok(true, 0x00u, false) && !cs7000p_rom_entry_ok(true, 0xFFu, false));
	// The contact board.c reads is PB10
	CHECK(PIN_PORT(PIN_CHSW_2) == GPIOB && PIN_MASK(PIN_CHSW_2) == GPIO_PIN_10);

	printf("# cs7000pPinsTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
