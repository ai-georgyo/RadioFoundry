// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for FreeRTOS.h, for debuglinkAirTest only: the heap calls core/debuglink/debuglink_air.c makes
#ifndef AIR_STUB_FREERTOS_H
#define AIR_STUB_FREERTOS_H

#include <stddef.h>

void *pvPortMalloc(size_t n);
void vPortFree(void *p);

#endif
