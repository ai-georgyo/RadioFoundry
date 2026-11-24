// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for task.h, for debuglinkAirTest only: the delay core/debuglink/debuglink_air.c waits with
#ifndef AIR_STUB_TASK_H
#define AIR_STUB_TASK_H

#include <stdint.h>

void vTaskDelay(uint32_t ticks);

#endif
