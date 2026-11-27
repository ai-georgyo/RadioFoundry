// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for platform/stm32/platform.h, for the host tests of the STM32 data flash glue
// (platform/stm32/spi_flash): the types and calls spi_flash.c makes. The functions are the test's own
#ifndef STM32FLASH_STUB_PLATFORM_H
#define STM32FLASH_STUB_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

typedef struct { uint32_t dummy; } SPI_TypeDef;
typedef struct { uint32_t dummy; } GPIO_TypeDef;

#define GPIO_NOPULL		0x00000000u

void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool open_drain);
void stm32_pin_af(GPIO_TypeDef *port, uint16_t pins, uint8_t af, bool open_drain, uint32_t pull);
void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool level);
void stm32_spi_master_init(SPI_TypeDef *spi, bool cpol, bool cpha, uint8_t br);
bool stm32_spi_xfer(SPI_TypeDef *spi, const uint8_t *tx, uint8_t *rx, uint32_t len);

#define __get_PRIMASK()		0u
#define __get_BASEPRI()		0u

#endif
