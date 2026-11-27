// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS: whether a REBOOT to the ROM bootloader may go ahead (board_rom_entry_ok() in board.c, asked by
// platform/stm32/sysboot.c), and the wait for SK1 before every software reset (board_reset_prepare() in
// exp_cs7000p.c). Pure: tests/cs7000pPinsTest.c checks the first, tests/cs7000pExpTest.c the wait with the hook that
// runs it. Unverified on a radio.
//
// The loader sets up every interface it knows, on this board's nets (AN2606 Table 145): it drives PA2, an op-amp's
// output here, and PB10, a contact of the channel selector that may be at ground, from the start, except version
// 9.1, the one version the table says keeps them inputs until its serial port is used; every version drives PB14,
// the modem's data output. So the entry is allowed (CS7000P_SOFT_ROM_ENTRY, on by default; hardware-cs7000p.md 2.5,
// open question 18) only with that version, or with the selector's contact on PB10 open: on any other version,
// 9.2 included, a selector position that grounds PB10 is refused (eight of the sixteen detents, in two runs of
// four: keys_cs7000p.h), and the user turns the selector four detents either way or uses the key. SK1 held at power-on starts the same loader on the same pins and is not asked: that is the maker's own
// route
#ifndef ROMENTRY_CS7000P_H
#define ROMENTRY_CS7000P_H

#include <stdint.h>
#include <stdbool.h>

// One version, not "this one and later": the byte is not in order of age. This chip's loaders are 13.2 (0xD2) and
// 13.3 (0xD3), then 9.0, 9.1 and 9.2 (AN2606 Table 146), so the two oldest have the largest numbers; and nothing
// says what 9.2, or a byte that is none of these, does with the pins
#define CS7000P_ROM_LOADER_TX_INPUTS	0x91u

// allowed: the bench knob. loader_id: the ROM's version byte (stm32h7_rom_loader_id). pb10_high: the selector's
// contact on PB10 is open (the board's pull-up)
static inline bool cs7000p_rom_entry_ok(bool allowed, uint8_t loader_id, bool pb10_high)
{
	return allowed && (loader_id == CS7000P_ROM_LOADER_TX_INPUTS || pb10_high);
}

// SK1 pulls BOOT0 high, so any reset while it is down starts the same loader, on the same pins: a crash restart, the
// hang check, the knob watch's restart and every REBOOT, whenever someone holds Monitor. So before a software reset
// the board waits for the key to be released (CS7000P_SK1_UP_MS in a row, one step a millisecond) with the chain
// already in its reset state, the hold clear: with the knob off the radio goes dark meanwhile. At most
// CS7000P_SK1_WAIT_MS steps: the reset then goes ahead into the loader, where the radio stays until the knob is
// switched off (the reset stops the watchdog, platform/stm32/watchdog.c). The watchdog's own reset is the one this
// can't reach (hardware-cs7000p.md 5.3)
#define CS7000P_SK1_WAIT_MS	30000u
#define CS7000P_SK1_UP_MS	10u

typedef struct {
	uint32_t steps;		// steps waited
	uint32_t up;		// steps in a row with the key up
} cs7000p_sk1_wait_t;

// One step's reading of the key. True: wait another step. Asked only once the key has been read down
static inline bool cs7000p_sk1_wait_more(cs7000p_sk1_wait_t *w, bool sk1_down)
{
	w->up = sk1_down ? 0u : w->up + 1u;
	w->steps++;
	return w->up < CS7000P_SK1_UP_MS && w->steps < CS7000P_SK1_WAIT_MS;
}

#endif
