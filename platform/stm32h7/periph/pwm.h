// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// One PWM channel of a general purpose or advanced timer on a pin (pwm.c): TIM1 to TIM5 and TIM8, channels 1 to 4.
// The counter runs at the timer's clock (no prescaler), so the PWM frequency is stm32_pwm_timer_hz() / period.
#ifndef STM32H7_PWM_H
#define STM32H7_PWM_H

#include <stdint.h>
#include "platform.h"

// period: timer clocks per PWM period, 2 to 65535 (less: nothing happens). duty: timer clocks the pin
// is high in each period, 0 = low all the time, period or more = high all the time. The channel runs before the pin
// is given to the timer, so the pin goes from what it was straight to the PWM. Two channels of one timer share the
// period: the last call's. Nothing happens for a timer or channel this file does not know
void stm32_pwm_init(TIM_TypeDef *tim, uint8_t channel, GPIO_TypeDef *port, uint16_t pin, uint8_t af, uint16_t period, uint16_t duty);
// The timer's counting clock in Hz: the bus clock, doubled or quadrupled as the bus prescaler and RCC_CFGR TIMPRE say
// (RM0433 Table 57). Any timer, not only the ones stm32_pwm_init() takes. Valid after stm32_clock_init()
uint32_t stm32_pwm_timer_hz(TIM_TypeDef *tim);

// The duty from the next period on (the compare register is preloaded: no short or long pulse in between). From a
// task or an interrupt
static inline void stm32_pwm_set(TIM_TypeDef *tim, uint8_t channel, uint16_t duty)
{
	volatile uint32_t *const ccr = channel == 1u ? &tim->CCR1 : channel == 2u ? &tim->CCR2 :
				       channel == 3u ? &tim->CCR3 : &tim->CCR4;
	*ccr = duty;
}

#endif
