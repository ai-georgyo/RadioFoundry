// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for FreeRTOS's task.h, for this board's host tests: what the board's files ask of it. The functions
// are the test's own
#ifndef CS7000P_STUB_TASK_H
#define CS7000P_STUB_TASK_H

#include "FreeRTOS.h"

#define taskSCHEDULER_NOT_STARTED	((BaseType_t)1)
#define taskSCHEDULER_RUNNING		((BaseType_t)2)
BaseType_t xTaskGetSchedulerState(void);

#endif
