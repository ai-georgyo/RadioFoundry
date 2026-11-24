// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS frequency plan (docs/spec/hardware-cs7000p.md 7): the bands, the first IF and the side of the
// first LO, the synthesizer's divider and VCO frequency for a channel, and the IF receiver's register values. Pure:
// no pins and no RTOS, so tests/cs7000pRfTest.c runs it unchanged. From the maker's schematic and the chips' data
// sheets; nothing here has run on a radio.
//
// One synthesizer (SKY72310) on a 16.8 MHz reference with two VCOs, one below and one above about 430 MHz. Receive:
// the first LO is an IF away from the signal, the 49.95 MHz IF goes to the AK2365A, whose second LO is three times
// the reference (50.4 MHz), and its 450 kHz output to the modem. Transmit: the VCO runs on the carrier.
#ifndef RFPLAN_CS7000P_H
#define RFPLAN_CS7000P_H

#include <stdint.h>
#include <stdbool.h>
#include "rf/ak2365a/ak2365a.h"
#include "rf/sky72310/sky72310.h"

#define CS7000P_F_REF_HZ	16800000u	// X601, a VC-TCXO (the value is printed on the drawing)
#define CS7000P_IF_HZ		49950000u	// XF201, the first IF's crystal filter
// 50.4 MHz from a tripler on the receive rail: a 450 kHz second IF
#define CS7000P_IF2_LO_HZ	(3u * CS7000P_F_REF_HZ)

// Receive: the maker's manual ("400-527 MHz"). Transmit: the FCC grant's two rows, the only certified range. The
// 100 kHz between them is the distress beacon band, receive only. The grant's rows bound the emission, and the check
// is on the carrier: the two inner edges are half the widest channel (12.5 kHz) inside them, so a 25 kHz channel's
// emission stays out of 406.0-406.1 MHz. The edges count as in the band table (board.c)
#define CS7000P_RX_MIN_HZ	400000000u
#define CS7000P_RX_MAX_HZ	527000000u
#define CS7000P_TX_MIN_HZ	400000000u
#define CS7000P_TX_GAP_LO_HZ	405987500u	// the last transmit carrier under the gap
#define CS7000P_TX_GAP_HI_HZ	406112500u	// the first one over it
#define CS7000P_TX_MAX_HZ	512000000u

// The side of the first LO, 0: below the signal (LO = f - IF, the VCO at 350.05 to 477.05 MHz), 1: above it.
// Low is what the VCO's tank values point to and nothing else says: open (docs/testing/cs7000p-bringup.md). The
// bench flips it at run time (board chip_reg 3:3); this is the value a start begins with
#ifndef CS7000P_LO_HIGH
#define CS7000P_LO_HIGH		0
#endif

// The synthesizer's phase detector gain (register 6, 0..31) and Mux_out source. The gain is the value another radio
// in this tree runs the same part with (boards/md380/radio_md380.c); the one that suits this loop filter is
// unknown. Mux_out is not wired: tri-stated (its default, the reference oscillator, would put a 16.8 MHz square wave
// and its harmonics on an open pin next to the synthesizer, in the receiver's range)
#define CS7000P_PLL_PD_GAIN	31u
#define CS7000P_PLL_MUX		SKY72310_MUX_OFF

bool cs7000p_rx_band(uint32_t hz);
bool cs7000p_tx_band(uint32_t hz);

// The VCO select line (EXP_VCO_LOW), the maker's rule (docs/spec/calibration-cs7000p.md 5.12): set from 400.0 MHz up
// to the switch point and 10 Hz past it (the channel and the point compared in 10 Hz units, hz / 10 <= switch / 10),
// clear above, by the channel's frequency: f_rx in receive, f_tx in transmit. Both points put the VCO itself at
// about 429-430 MHz: the receive LO at 479 MHz is 429.05 MHz, in transmit the VCO is on the carrier. So the line picks
// the VCO by the frequency it runs at, not by direction. The calibration's per-point values were set on this choice
#define CS7000P_VCO_LOW_FROM_HZ		400000000u
#define CS7000P_VCO_RX_SWITCH_HZ	479000000u
#define CS7000P_VCO_TX_SWITCH_HZ	430000000u
// The line's level for a channel at hz. lo_high: the first LO above the signal (the bench's chip_reg 3:3), where
// receive keeps the line set whatever the frequency, the rule this port ran before (that LO is above 430 MHz on
// every channel, so this keeps the bench's known state, not a choice that suits it; the maker's LO-above variant
// sets it in receive too, 5.12). Transmit follows f_tx whatever
// lo_high says: the LO's side is receive's alone (cs7000p_pll_plan)
bool cs7000p_vco_low(uint32_t hz, bool tx, bool lo_high);

typedef struct {
	uint32_t lo_hz;		// the synthesizer's nominal output: the first LO, or the carrier
	uint32_t vco_hz;	// what is programmed: lo_hz moved by the reference trim
	uint8_t ref_div;	// the reference divider (2 over the whole band: an 8.4 MHz phase detector)
} cs7000p_pll_plan_t;

// The synthesizer's set-up for receiving (the first LO, on the side lo_high says) or transmitting (the carrier) on
// hz. trim_ppb is the reference trim (core/radio/ref_trim.h): the reference runs off by a fraction, so the VCO is
// asked for that fraction more or less and lands on frequency. That is exact for the first LO and the carrier; the
// second LO is three times the same reference and is not corrected (its error lands in the 450 kHz IF, about 50 Hz
// per ppm). False: hz is outside the band for that direction, or no divider fits
bool cs7000p_pll_plan(uint32_t hz, bool tx, bool lo_high, int16_t trim_ppb, cs7000p_pll_plan_t *p);

// The AK2365A's registers for receive: everything on with the 50.4 MHz second LO, the +-6 kHz filter and the wide
// demodulation level on a 25 kHz channel, the +-3 kHz filter on a 12.5 kHz one; the data sheet's required AGC
// setting with the calibration started; the IF buffer at 6 dB. The IF level the modem wants is open
extern const ak2365a_cfg_t cs7000p_if_cfg;

// The wait ahead of one word of the IF chip's set-up (ak2365a_setup_words), microseconds. The data sheet's
// calibration procedure: the circuits the calibration runs on (LO buffer, reference, discriminator) are up 500 us
// after control 1 has set the operating mode, and only then may control 2 start it. Every set-up begins with a
// software reset, which puts the chip back in standby, so the wait is due each time
#define CS7000P_IF_MODE_US	500u
static inline uint32_t cs7000p_if_word_wait_us(uint16_t word)
{
	return ((word >> 9) & 0x3Fu) == AK2365A_REG_CTRL2 ? CS7000P_IF_MODE_US : 0u;
}

// A register of the IF chip the bench may reach (board chip_reg 2). The data sheet names control 1 to 3, the software
// reset, control 4 and 5, and forbids access to any other address (0x05 to 0x0A are reserved)
static inline bool cs7000p_if_reg_ok(uint8_t reg)
{
	return (reg >= AK2365A_REG_CTRL1 && reg <= AK2365A_REG_SRST) || reg == AK2365A_REG_CTRL4 ||
	       reg == AK2365A_REG_CTRL5;
}

#endif
