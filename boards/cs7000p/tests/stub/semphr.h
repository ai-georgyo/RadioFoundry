// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for FreeRTOS's semphr.h, for this board's host tests: a mutex as the board's files use one. The
// functions are the test's own
#ifndef CS7000P_STUB_SEMPHR_H
#define CS7000P_STUB_SEMPHR_H

#include "FreeRTOS.h"

typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t m);

#endif
