// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DAC1 channels 1 and 2 on PA4 and PA5 (dac.h): set-up, and the two ways off. Writes are inline in the header.
//
// CR and MCR hold both channels, so every change to them is a masked read-modify-write: one channel may be written
// from an interrupt (a burst's power ramp) while a task sets up the other.
#include "periph/dac.h"

static uint32_t en_bit(uint8_t ch)
{
	return ch == 2u ? DAC_CR_EN2 : DAC_CR_EN1;
}

// The channel's data register to 0, then its enable off. Registers only
static void off(uint8_t ch)
{
	if (ch == 2u) {
		DAC1->DHR12R2 = 0;
	} else {
		DAC1->DHR12R1 = 0;
	}
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	DAC1->CR &= ~en_bit(ch);
	__set_PRIMASK(primask);
}

void stm32_dac_init(uint8_t ch, uint16_t value)
{
	if (ch != 1u && ch != 2u) {
		return;
	}
	__HAL_RCC_DAC12_CLK_ENABLE();
	stm32_pin_analog(GPIOA, ch == 2u ? GPIO_PIN_5 : GPIO_PIN_4);
	// Everything of the channel in CR but its enable: no trigger, no wave, no DMA, no underrun interrupt (the
	// vector is TIM6's too, periph/adc_scan.c), no calibration mode
	const uint32_t cr_ch = (DAC_CR_EN1 | DAC_CR_TEN1 | DAC_CR_TSEL1 | DAC_CR_WAVE1 | DAC_CR_MAMP1 | DAC_CR_DMAEN1 |
				DAC_CR_DMAUDRIE1 | DAC_CR_CEN1) << (ch == 2u ? 16u : 0u);
	const uint32_t mode = ch == 2u ? DAC_MCR_MODE2 : DAC_MCR_MODE1;
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	// The data register first: the pin starts at the value, and a data register must have been written before the
	// mode field is touched (ES0392 2.10.1). The mode takes a write only while the channel is off, and the write is
	// 000, the buffered pin: the field is never given another value
	if (ch == 2u) {
		DAC1->DHR12R2 = value & 0xFFFu;
	} else {
		DAC1->DHR12R1 = value & 0xFFFu;
	}
	if ((DAC1->CR & en_bit(ch)) == 0) {
		DAC1->MCR &= ~mode;
	}
	DAC1->CR = (DAC1->CR & ~cr_ch) | en_bit(ch);
	__set_PRIMASK(primask);
}

void stm32_dac_off(uint8_t ch)
{
	if (ch != 1u && ch != 2u) {
		return;
	}
	off(ch);
}

// A register of a block whose clock is off takes no write and must not be relied on: only with the clock on
void stm32_dac_force_zero(uint8_t ch)
{
	if ((ch != 1u && ch != 2u) || (RCC->APB1LENR & RCC_APB1LENR_DAC12EN) == 0) {
		return;
	}
	off(ch);
}
