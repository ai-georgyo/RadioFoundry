// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// One PWM channel of TIM1 to TIM5 or TIM8 on a pin (pwm.h), and the counting clock of any timer.
#include "periph/pwm.h"

// The timer's clock gate on. False: not a timer this file sets up
static bool clock_on(const TIM_TypeDef *tim)
{
	if (tim == TIM1) {
		__HAL_RCC_TIM1_CLK_ENABLE();
	} else if (tim == TIM8) {
		__HAL_RCC_TIM8_CLK_ENABLE();
	} else if (tim == TIM2) {
		__HAL_RCC_TIM2_CLK_ENABLE();
	} else if (tim == TIM3) {
		__HAL_RCC_TIM3_CLK_ENABLE();
	} else if (tim == TIM4) {
		__HAL_RCC_TIM4_CLK_ENABLE();
	} else if (tim == TIM5) {
		__HAL_RCC_TIM5_CLK_ENABLE();
	} else {
		return false;
	}
	return true;
}

void stm32_pwm_init(TIM_TypeDef *tim, uint8_t channel, GPIO_TypeDef *port, uint16_t pin, uint8_t af, uint16_t period,
		    uint16_t duty)
{
	if (channel < 1u || channel > 4u || period < 2u || !clock_on(tim)) {
		return;
	}
	const uint32_t cc = (channel - 1u) * 4u;		// the channel's four bits of CCER
	const uint32_t oc = ((channel - 1u) & 1u) * 8u;		// its byte of CCMR1 (channels 1, 2) or CCMR2 (3, 4)
	volatile uint32_t *const ccmr = channel <= 2u ? &tim->CCMR1 : &tim->CCMR2;
	// Off while its mode changes: the output and the complementary output, both active high
	tim->CCER &= ~((TIM_CCER_CC1E | TIM_CCER_CC1P | TIM_CCER_CC1NE | TIM_CCER_CC1NP) << cc);
	// An output (CCxS 00) in PWM mode 1 (OCxM 0110: high while the counter is below the compare value) with the
	// compare register preloaded, so a new duty starts with a period
	const uint32_t field = TIM_CCMR1_CC1S | TIM_CCMR1_OC1FE | TIM_CCMR1_OC1PE | TIM_CCMR1_OC1M | TIM_CCMR1_OC1CE;
	*ccmr = (*ccmr & ~(field << oc)) | ((TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1PE) << oc);
	tim->PSC = 0;
	tim->ARR = period - 1u;
	stm32_pwm_set(tim, channel, duty);
	tim->CR1 |= TIM_CR1_ARPE;
	// An advanced timer drives no output until its main output enable is set
	if (IS_TIM_BREAK_INSTANCE(tim)) {
		tim->BDTR |= TIM_BDTR_MOE;
	}
	tim->EGR = TIM_EGR_UG;	// the period and the duty from their preload registers into use, now
	tim->CCER |= TIM_CCER_CC1E << cc;
	tim->CR1 |= TIM_CR1_CEN;
	stm32_pin_af(port, pin, af, false, GPIO_NOPULL);
}

uint32_t stm32_pwm_timer_hz(TIM_TypeDef *tim)
{
	const bool apb2 = tim == TIM1 || tim == TIM8 || tim == TIM15 || tim == TIM16 || tim == TIM17;
	const uint32_t pclk = apb2 ? HAL_RCC_GetPCLK2Freq() : HAL_RCC_GetPCLK1Freq();
	// The bus prescaler's field: 0xx is 1, then 100 to 111 are 2, 4, 8 and 16
	const uint32_t ppre = apb2 ? (RCC->D2CFGR & RCC_D2CFGR_D2PPRE2) >> RCC_D2CFGR_D2PPRE2_Pos :
				     (RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) >> RCC_D2CFGR_D2PPRE1_Pos;
	const uint32_t div = (ppre & 4u) != 0 ? 2u << (ppre & 3u) : 1u;
	// RM0433 Table 57. TIMPRE 0: the bus clock when it is not divided, else twice the bus clock. TIMPRE 1: the
	// undivided clock down to a division by 4, else four times the bus clock
	if ((RCC->CFGR & RCC_CFGR_TIMPRE) != 0) {
		return pclk * (div <= 4u ? div : 4u);
	}
	return pclk * (div == 1u ? 1u : 2u);
}
