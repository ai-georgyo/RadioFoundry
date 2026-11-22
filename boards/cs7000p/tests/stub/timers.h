// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for FreeRTOS's timers.h, for this board's host tests: a software timer as power_cs7000p.c uses one.
// The functions are the test's own
#ifndef CS7000P_STUB_TIMERS_H
#define CS7000P_STUB_TIMERS_H

#include "FreeRTOS.h"

typedef void *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t timer);
TimerHandle_t xTimerCreate(const char *name, TickType_t period, BaseType_t reload, void *id,
			   TimerCallbackFunction_t cb);
BaseType_t xTimerStart(TimerHandle_t timer, TickType_t wait);
BaseType_t xTimerStop(TimerHandle_t timer, TickType_t wait);

#endif
