// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DAC1's two channels on their pins, PA4 (channel 1) and PA5 (channel 2), buffered, written by software (dac.c).
//
// Code 0 is not 0 V: with the output buffer on, the pin goes down to 0.2 V at the lowest (DS12110, DAC
// characteristics). A board that needs a true 0 V on the net switches the channel off (stm32_dac_off): the pin is
// then high impedance, as after a reset, and whatever pulls the net decides its level.
//
// The channel's mode stays 000, the buffered pin, and is never written to anything else: ES0392 2.10.1.
#ifndef STM32H7_DAC_H
#define STM32H7_DAC_H

#include <stdint.h>
#include "platform.h"

// The clock on, the pin to analog mode, then the data register before EN: the pin starts at value (12 bits)
void stm32_dac_init(uint8_t ch, uint16_t value);	// ch 1 = PA4, 2 = PA5; the data register before EN
// Data register 0 first (the buffer, while it is still on, takes the pin to its minimum), then EN clear. From a task
// or an interrupt
void stm32_dac_off(uint8_t ch);				// data register 0, EN clear: the pin is high impedance
// For board_rf_safe(): no clock or pin set-up, no wait, safe in a fault handler and before stm32_dac_init()
void stm32_dac_force_zero(uint8_t ch);			// stm32_dac_off if the DAC clock is on; registers only

// From a task or an interrupt. The enable is a read-modify-write of the register both channels share, so it is
// masked; it happens only when the channel was off
static inline void stm32_dac_write(uint8_t ch, uint16_t value)	// data register, then EN if it was off
{
	if (ch != 1u && ch != 2u) {
		return;		// as the other calls: a wrong number must not reach channel 1
	}
	const uint32_t en = ch == 2u ? DAC_CR_EN2 : DAC_CR_EN1;
	if (ch == 2u) {
		DAC1->DHR12R2 = value & 0xFFFu;
	} else {
		DAC1->DHR12R1 = value & 0xFFFu;
	}
	if ((DAC1->CR & en) == 0) {
		const uint32_t primask = __get_PRIMASK();
		__disable_irq();
		DAC1->CR |= en;
		__set_PRIMASK(primask);
	}
}

static inline uint16_t stm32_dac_read(uint8_t ch)	// the data register; 0 while the channel is off
{
	if ((ch != 1u && ch != 2u) || (DAC1->CR & (ch == 2u ? DAC_CR_EN2 : DAC_CR_EN1)) == 0) {
		return 0;
	}
	return (uint16_t)((ch == 2u ? DAC1->DHR12R2 : DAC1->DHR12R1) & 0xFFFu);
}

#endif
