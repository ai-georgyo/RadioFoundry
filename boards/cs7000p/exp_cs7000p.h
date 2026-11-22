// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS output expander: three 74HC595 in a chain (IC619 -> IC620 -> IC621) on PE7 (clock), PE8 (latch) and
// PE9 (data), 24 outputs that switch the rails, the chip selects, the audio paths, the LEDs and the supply hold
// (docs/spec/hardware-cs7000p.md). From the maker's schematic; no bit has been checked on a radio, and four
// polarities come from documents only (EXP_DMR_SLEEP, EXP_AK_RUN, EXP_AK_CSN while RSTN is released, EXP_VCO_RX).
//
// Pure: the bit names, the word every reset writes, and the rule that ties the shared DAC to the transmit bits. The
// one writer of the chain and of that DAC is exp_cs7000p.c; tests/cs7000pExpTest.c checks this file.
//
// The chips have no reset and their outputs are always enabled: they are random from power-up, and they keep their
// state through an MCU reset. The chain cannot be read back (the last chip's serial output is not connected).
#ifndef EXP_CS7000P_H
#define EXP_CS7000P_H

#include <stdint.h>
#include <stdbool.h>

// Bit n of the 24-bit word. Bit 23 is shifted first and ends on IC621 QH; bit 0 is IC619 QA. The comment gives the
// schematic's net and what a 1 means
#define EXP_BITS		24u
#define EXP_AK_CSN		(1u << 0)	// A-CSN: AK2365A deselected
#define EXP_SPARE		(1u << 1)	// no label on the drawing: kept 0
#define EXP_MOTOR		(1u << 2)	// MOT-C: motor on (kept 0)
#define EXP_APC_LOOP		(1u << 3)	// RF_APC_SW: APC amplifier powered, the PA can be biased
#define EXP_BT_ON		(1u << 4)	// BLPOWER_ON: Bluetooth regulator on (kept 0)
#define EXP_MIC_EXT		(1u << 5)	// EXT-MIC-C: external microphone connected
#define EXP_AK_RUN		(1u << 6)	// A-PDN: AK2365A out of power-down
#define EXP_DMR_SLEEP		(1u << 7)	// DMR_SLEEP: HR_C6000 asleep (PWD)
#define EXP_VCO_RX		(1u << 8)	// VCOVCC_SW: receive VCO powered; 0: transmit VCO (drawing note "T:0V R:3V")
#define EXP_TX_RAIL		(1u << 9)	// 5TC: 5T on, pre-drivers, TX LO path, antenna switch to transmit
#define EXP_MIC_INT		(1u << 10)	// MIC-C: internal microphone connected
#define EXP_SYNTH_RAIL		(1u << 11)	// SAVE: 5C on, charge pump, VCOs, modulation amplifiers (and with it the
						// reference's control voltage)
#define EXP_LED_GREEN		(1u << 12)	// RX_LED: on
#define EXP_LED_RED		(1u << 13)	// TX_LED: on
#define EXP_AMP			(1u << 14)	// AFCO: speaker amplifier powered
#define EXP_RX_RAIL		(1u << 15)	// 5RC: 5R on, front end, mixer, IF amplifier, second LO tripler, the line-out
						// amplifier
#define EXP_LAMP		(1u << 16)	// LAMP: key lights on
#define EXP_SPK_INT_OFF		(1u << 17)	// PHONE_C: internal speaker disconnected
#define EXP_MIC_POWER		(1u << 18)	// MICPOW_C: microphone amplifier powered
#define EXP_SPK_EXT_OFF		(1u << 19)	// SPK_C: accessory speaker lines disconnected
#define EXP_POWER_HOLD		(1u << 20)	// POW_C: main supply held with the knob off
#define EXP_ACC_SCL		(1u << 21)	// MD-SCL: accelerometer clock high (idle)
#define EXP_AUDIO_RAIL		(1u << 22)	// FM_SW: 5C_A on, the receive audio filters behind the ADC taps
#define EXP_AF_MUTE		(1u << 23)	// AF_MUTE: no effect as drawn; kept 0

// The state after every reset and before every reset: everything off, the receive VCO selected, both chips parked,
// and the supply NOT held: with the knob on the hold changes nothing, with the knob off it must not keep the radio on.
// (The chain outlives an MCU reset and the ROM bootloader, which a held SK1 starts, never writes it: a reset word
// with the hold set could keep the radio on for good. The hold is set only after the knob has been read on.)
#define EXP_RESET_WORD	(EXP_ACC_SCL | EXP_SPK_EXT_OFF | EXP_SPK_INT_OFF | EXP_VCO_RX | EXP_DMR_SLEEP | EXP_AK_CSN)
// The two transmit bits: the transmitter's rail and the loop that biases the PA
#define EXP_TX_BITS	(EXP_TX_RAIL | EXP_APC_LOOP)
_Static_assert(EXP_RESET_WORD == 0x2A0181u, "the reset word is the documented one");
_Static_assert((EXP_RESET_WORD & (EXP_POWER_HOLD | EXP_TX_BITS)) == 0u,
	       "a reset never holds the supply or enables transmit");

// One half period of the chain's clock as a fixed count of empty loop passes, here and everywhere: at least 0.5 us
// at the 64 MHz the reset hook runs at and at least 0.16 us at 192 MHz, with one cycle a pass (SN74HC595: 100 ns
// pulses and 125 ns set-up at 2 V, 20 / 25 ns at 4.5 V, nothing given at 3.3 V). A count, not a time: the reset hook
// runs before .data exists (SystemCoreClock) and before the cycle counter does. The chain's delay hook is never NULL
// on this board
#define EXP_DELAY_PASSES	32u

// The largest DAC code that gives no RF output: the threshold of the interlock below, and the level the PA loop is
// closed and opened at. 0 until a PA floor has been measured (a bench knob, docs/testing/cs7000p-bringup.md)
#ifndef CS7000P_APC_FLOOR
#define CS7000P_APC_FLOOR	0
#endif

// What a fault path makes of the chain's shadow: the transmit bits off, the receive VCO selected, the rest as it was
static inline uint32_t exp_rf_safe_word(uint32_t shadow) { return (shadow & ~EXP_TX_BITS) | EXP_VCO_RX; }
// The shared DAC's interlock, pure (cs7000pExpTest): a value above the floor may be on the DAC only while both
// transmit bits are set (a drive) or both are clear (the receive tuning voltage), and a transmit bit may be set only
// while the DAC is at or below the floor. The floor is the largest DAC code that gives no RF output
static inline bool exp_dac_ok(uint32_t shadow, uint16_t dac, uint16_t floor)
{ return dac <= floor || (shadow & EXP_TX_BITS) == EXP_TX_BITS || (shadow & EXP_TX_BITS) == 0u; }
static inline bool exp_set_ok(uint32_t set, uint16_t dac, uint16_t floor)
{ return (set & EXP_TX_BITS) == 0u || dac <= floor; }

#endif
