// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for platform/stm32/platform.h, for this board's host tests only: just enough for the real pins.h,
// pinsetup_cs7000p.c, exp_cs7000p.c and the platform's periph/dac.h to compile. The ports, RCC and the DAC are plain
// memory the tests look at; every PIN_* definition still comes from pins.h. The pin functions are the test's own
// (each test models what it checks: the pin modes, or the expander chain behind PE7-PE9).
#ifndef CS7000P_STUB_PLATFORM_H
#define CS7000P_STUB_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include "soc.h"	// the real one (host clean): the reset hooks' prototypes
#include "hal_system.h"

// The platform's system ops (reboot): the test's own where a file under test calls them
extern const hal_system_t stm32_system;

typedef struct {
	uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR;
} GPIO_TypeDef;

extern GPIO_TypeDef fake_ports[5];
#define GPIOA			(&fake_ports[0])
#define GPIOB			(&fake_ports[1])
#define GPIOC			(&fake_ports[2])
#define GPIOD			(&fake_ports[3])
#define GPIOE			(&fake_ports[4])

#define GPIO_PIN_0		((uint16_t)(1u << 0))
#define GPIO_PIN_1		((uint16_t)(1u << 1))
#define GPIO_PIN_2		((uint16_t)(1u << 2))
#define GPIO_PIN_3		((uint16_t)(1u << 3))
#define GPIO_PIN_4		((uint16_t)(1u << 4))
#define GPIO_PIN_5		((uint16_t)(1u << 5))
#define GPIO_PIN_6		((uint16_t)(1u << 6))
#define GPIO_PIN_7		((uint16_t)(1u << 7))
#define GPIO_PIN_8		((uint16_t)(1u << 8))
#define GPIO_PIN_9		((uint16_t)(1u << 9))
#define GPIO_PIN_10		((uint16_t)(1u << 10))
#define GPIO_PIN_11		((uint16_t)(1u << 11))
#define GPIO_PIN_12		((uint16_t)(1u << 12))
#define GPIO_PIN_13		((uint16_t)(1u << 13))
#define GPIO_PIN_14		((uint16_t)(1u << 14))
#define GPIO_PIN_15		((uint16_t)(1u << 15))

// The ST HAL's values (stm32h7xx_hal_gpio.h)
#define GPIO_NOPULL		0x00000000u
#define GPIO_PULLUP		0x00000001u
#define GPIO_PULLDOWN		0x00000002u

// The clock enables the board code reads and writes itself (stm32h743xx.h: GPIOA is bit 0 of AHB4ENR, DAC12 bit 29
// of APB1LENR)
typedef struct {
	uint32_t AHB4ENR, APB1LENR;
} RCC_TypeDef;

extern RCC_TypeDef fake_rcc;
#define RCC			(&fake_rcc)
#define RCC_AHB4ENR_GPIOAEN	(1u << 0)
#define RCC_AHB4ENR_GPIOBEN	(1u << 1)
#define RCC_AHB4ENR_GPIOCEN	(1u << 2)
#define RCC_AHB4ENR_GPIODEN	(1u << 3)
#define RCC_AHB4ENR_GPIOEEN	(1u << 4)
#define RCC_APB1LENR_DAC12EN	(1u << 29)

// What periph/dac.h's inline writes touch
typedef struct {
	uint32_t CR, DHR12R1, DHR12R2;
} DAC_TypeDef;

extern DAC_TypeDef fake_dac;
#define DAC1			(&fake_dac)
#define DAC_CR_EN1		(1u << 0)
#define DAC_CR_EN2		(1u << 16)

// The cycle counter and the clock it counts (CMSIS), for a busy wait
typedef struct {
	volatile uint32_t CYCCNT;
} DWT_Type;
extern DWT_Type fake_dwt;
#define DWT			(&fake_dwt)
extern uint32_t SystemCoreClock;

extern int fake_primask;
#define __get_PRIMASK()		((uint32_t)fake_primask)
#define __disable_irq()		((void)(fake_primask = 1))
#define __set_PRIMASK(m)	((void)(fake_primask = (int)(m)))
// Counted: the expander's half period is a loop of these, and the test reads the count as its clock
extern uint32_t fake_nops;
#define __NOP()			((void)fake_nops++)

// The test's fake GPIO and time
void stm32_gpio_clock(GPIO_TypeDef *port);
void stm32_pin_force(GPIO_TypeDef *port, uint16_t pins, bool level);
void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool open_drain);
void stm32_pin_in(GPIO_TypeDef *port, uint16_t pins, uint32_t pull);
void stm32_pin_af(GPIO_TypeDef *port, uint16_t pins, uint8_t af, bool open_drain, uint32_t pull);
void stm32_pin_analog(GPIO_TypeDef *port, uint16_t pins);
void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool level);
bool stm32_pin_read(GPIO_TypeDef *port, uint16_t pin);
void stm32_wait_ns(uint32_t ns);
void board_sysboot_prepare(void);

#endif
