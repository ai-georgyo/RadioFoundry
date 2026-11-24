// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for the ST HAL header platform/stm32f4/platform.h includes, for stm32RngTest only: the RCC enable bit
// and the RNG block that platform/stm32f4/rng.c reads (RM0090 24.4), as a fake whose every register access goes
// through fake_rng(), so the test can script the status and the data words; and the two register types platform.h's
// own declarations name
#ifndef STM32_RNG_STUB_HAL_H
#define STM32_RNG_STUB_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	volatile uint32_t BSRR, IDR;
} GPIO_TypeDef;

typedef struct {
	volatile uint32_t DR;
} SPI_TypeDef;

typedef struct {
	volatile uint32_t AHB2ENR;
} fake_rcc_t;

typedef struct {
	volatile uint32_t CR, SR, DR;
} fake_rng_t;

extern fake_rcc_t fake_rcc;
fake_rng_t *fake_rng(void);

#define RCC			(&fake_rcc)
#define RNG			(fake_rng())
#define RCC_AHB2ENR_RNGEN	(1u << 6)
#define RNG_CR_RNGEN		(1u << 2)
#define RNG_SR_DRDY		(1u << 0)
#define RNG_SR_CECS		(1u << 1)
#define RNG_SR_SECS		(1u << 2)
#define RNG_SR_CEIS		(1u << 5)
#define RNG_SR_SEIS		(1u << 6)

#endif
