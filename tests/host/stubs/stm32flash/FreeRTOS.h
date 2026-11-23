// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for FreeRTOS.h, for the host tests of the STM32 data flash glue (platform.h next to this file): the
// types and constants spi_flash.c, semphr.h and task.h here need. The scheduler never runs, so the glue takes no lock
#ifndef STM32FLASH_STUB_FREERTOS_H
#define STM32FLASH_STUB_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef long BaseType_t;
#define portMAX_DELAY		((TickType_t)0xFFFFFFFFu)
#define pdTRUE			((BaseType_t)1)
#define pdFALSE			((BaseType_t)0)
#define xPortIsInsideInterrupt()	pdFALSE

#endif
