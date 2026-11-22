// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C6000 on a board that feeds it a 450 kHz IF (docs/spec/modem-hrc6000.md 2, 3, 8): the order of driver calls
// such a board needs. The values belong to a board; the order does not: tones off before the chip can find DMR sync,
// the receive I/Q order for the first LO's side, the reference offset per frequency, the modulation pair and a
// receiver restart on a new DMR channel, and the two registers a mode set-up leaves in a state such a board cannot
// use (the DAC control word 4:0x25 and the codec inputs 4:0xE0).
//
// An opt-in subdirectory of the driver: only a board that lists drivers/modem/hrc6000/if_host compiles it. Pure C
// over hrc_t and callbacks: no RTOS call, no platform header. Every function but hrc6000_if_rx_hold is for one task
// (the radio task); none of them does anything before hrc6000_if_init has returned true. Beyond the lines below:
// the init and a mode change also write the reference offset for their frequency, before the hold opens the DAC;
// the feed-through of fm_audio_chip is open in FM receive only; every retune writes the hold again; a retune during
// an FM transmission is only noted, and applied when the transmission ends.
//
// boards/md9600/c6000_md9600.c still has its own copy of the init, mode and retune order, with its values. The copy
// goes when that board moves to these functions.
#ifndef HRC6000_IF_HOST_H
#define HRC6000_IF_HOST_H

#include "modem/hrc6000/hrc6000.h"

typedef struct {
	uint8_t dmr_mic_gain;		// hrc6000_power_on_init / hrc6000_dmr_enter
	int8_t rx_gain_db;		// hrc6000_set_rx_gain
	bool fm_audio_chip;		// FM receive audio is the chip's own, on its line out (hrc6000_fm_audio on). False: the
					// board has an analog FM path and wants PCM on the line out in FM (hrc6000_fm_rx_pcm)
	bool ref_dac_hold;		// the reference oscillator's control voltage comes from the chip's modulation DAC:
					// keep that DAC on while receiving (4:0x25 bit 5 set, bit 3 clear) after every set-up
	uint8_t e0_set, e0_clear;	// codec input bits of 4:0xE0 forced after hrc6000_dmr_enter and hrc6000_set_mode_fm_tx
					// (both 0: the driver's choice stands)
	bool (*lo_high)(void *ctx, uint32_t hz);	// the first LO is above the signal at hz: the IF is inverted and the I/Q order swapped (hrc6000_rx_iq_swap)
	// The modulation pair for hz (4:0x45 / 0x46). False: none known; DMR then gets safe_gain, FM transmit is refused
	bool (*mod_pair)(void *ctx, uint32_t hz, bool dmr, uint8_t *mod2, uint8_t *mod1);
	int8_t (*ref_offset)(void *ctx, uint32_t hz);	// hrc6000_set_ref_offset's value for hz
	void (*dmr_enter)(void);	// the platform glue's stm32_hrc_dmr_enter / _leave / _rx_restart, or NULL without DMR
	void (*dmr_leave)(void);
	void (*dmr_rx_restart)(void);
	uint8_t safe_gain;		// the DMR pair when mod_pair has none
	void *ctx;
} hrc6000_if_cfg_t;
// A callback left NULL: lo_high, the driver's I/Q order stands; mod_pair, no pair is known; ref_offset, 0.
// errors counts the driver calls that reported a bus failure. One during hrc6000_if_init leaves up false, and so does
// a 4:0x10 that does not read back as the init wrote it (a chip that is not there, on a bus that reports no failure).
// r10 and r25 are 4:0x10 and 4:0x25 as read back after the last set-up (init, a mode change, FM transmit and its end)
typedef struct { hrc_t *dev; const hrc6000_if_cfg_t *cfg; bool up, dmr, fm_tx; uint32_t tuned_hz; uint32_t errors;
		 uint8_t r10, r25; } hrc6000_if_t;

// After the board took PWD low, waited 10 ms and called hrc6000_attach: power-on init, FM receive set-up, receive
// gain, tones off, the I/Q order for hz, the receive hold. False: the chip did not answer (up stays false)
bool hrc6000_if_init(hrc6000_if_t *h, hrc_t *dev, const hrc6000_if_cfg_t *cfg, uint32_t hz);
void hrc6000_if_mode(hrc6000_if_t *h, bool dmr, uint32_t hz);	// DMR <-> FM receive; a no-op when already there
void hrc6000_if_retune(hrc6000_if_t *h, uint32_t hz);		// reference offset; I/Q on an LO side change; in DMR on a
								// new channel its pair and a receiver restart
bool hrc6000_if_fm_tx(hrc6000_if_t *h, uint32_t hz, bool wide, uint8_t mic_gain);	// false and nothing written: no pair
void hrc6000_if_fm_tx_end(hrc6000_if_t *h);			// back to FM receive
void hrc6000_if_rx_hold(hrc6000_if_t *h);			// the 4:0x25 rewrite alone (after a DMR burst sequence)

#endif
