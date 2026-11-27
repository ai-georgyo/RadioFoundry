// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Which of its two data pads an STM32H7 I2S block drives as a full-duplex slave: the pure part of the probe in
// hrc/i2s_pcm_soc.h, and what a board's view shows of it.
//
// The reference manual gives both answers. RM0433 Table 426 ("Serial data line swapping"): full duplex, IOSWP 0: SDI
// in, SDO out, for master and slave. Section 50.9.2 ("Pin sharing with SPI function"): SDO is "to receive the audio
// sample in slave" and SDI "to transmit the audio sample in slave". A board wires one pad to the codec's data input
// and the other to its data output, and the wrong sense makes the pad on the codec's output a push-pull output
// against it. So nothing is assumed: the half file tries each sense with only the transmit pad given to the block (the
// pad on the codec's input, safe to drive) and counts that pad's level changes. The receive pad stays a GPIO input
// until one sense, and only one, has been seen to drive the transmit pad.
//
// The board says which pad is which with STM32_I2S_TX_ON_SDI in MCU_DEFS: 1 when its transmit line (to the codec's
// data input) is on the pad that is SDI by default, 0 (the default) when it is on the SDO pad.
#ifndef STM32H7_I2S_DIR_H
#define STM32H7_I2S_DIR_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { STM32H7_I2S_DIR_UNKNOWN, STM32H7_I2S_DIR_OK, STM32H7_I2S_DIR_SWAPPED, STM32H7_I2S_DIR_BUSY,
	       STM32H7_I2S_DIR_NONE } stm32h7_i2s_dir_t;	// not probed yet; as Table 426 says; the other sense;
								// the transmit pad is driven by something else; neither sense drives it
typedef struct { uint8_t dir; bool ioswp, running; uint16_t edges[2]; uint32_t underruns, overruns, frame_errors; }
	stm32h7_i2s_state_t;
#define STM32H7_I2S_PROBE_MS	4u
#define STM32H7_I2S_PROBE_EDGES	8u	// level changes of the transmit pad in the window that count as "driven"
// The IOSWP value to try in pass 0 and pass 1 for a board whose transmit line is on the SDI pad (tx_on_sdi) or not:
// Table 426's sense first
static inline bool stm32h7_i2s_try_ioswp(bool tx_on_sdi, uint8_t pass) { return tx_on_sdi != (pass != 0u); }

// The verdict of a probe. line_free: the transmit pad followed a pull-up and a pull-down before anything was tried
// (false: something drives it, and neither pass was run). edges_pass0 / _pass1: the pad's level changes while the
// block ran in pass 0's and pass 1's sense. In the sense that is not the block's, the pad is its input and nothing
// drives it: a pad that moved in both passes is moved by something else, the direction is not proven, and the answer
// is BUSY like a line that was not free
static inline stm32h7_i2s_dir_t stm32h7_i2s_verdict(bool line_free, uint16_t edges_pass0, uint16_t edges_pass1)
{
	const bool drove0 = edges_pass0 >= STM32H7_I2S_PROBE_EDGES;
	const bool drove1 = edges_pass1 >= STM32H7_I2S_PROBE_EDGES;
	if (!line_free || (drove0 && drove1)) {
		return STM32H7_I2S_DIR_BUSY;
	}
	if (drove0) {
		return STM32H7_I2S_DIR_OK;
	}
	return drove1 ? STM32H7_I2S_DIR_SWAPPED : STM32H7_I2S_DIR_NONE;
}

// The verdict, the sense in use (ioswp: the IOSWP bit; meaningful with OK or SWAPPED), whether the stream runs, the
// last probe's edge counts for both passes, and the stream errors cleared since the reset. Any context
void stm32h7_i2s_state(stm32h7_i2s_state_t *out);	// defined in the half file; for the board's view

#endif
