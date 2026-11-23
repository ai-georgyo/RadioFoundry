// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for FreeRTOS's semphr.h, for the host tests of the STM32 data flash glue: a mutex that is never taken
// (the scheduler never runs there)
#ifndef STM32FLASH_STUB_SEMPHR_H
#define STM32FLASH_STUB_SEMPHR_H

#include "FreeRTOS.h"

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
	static int mutex;
	return &mutex;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait)
{
	(void)m;
	(void)wait;
	return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t m)
{
	(void)m;
	return pdTRUE;
}

#endif
