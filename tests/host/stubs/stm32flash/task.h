// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for FreeRTOS's task.h, for the host tests of the STM32 data flash glue: a scheduler that never starts
#ifndef STM32FLASH_STUB_TASK_H
#define STM32FLASH_STUB_TASK_H

#include "FreeRTOS.h"

#define taskSCHEDULER_NOT_STARTED	((BaseType_t)1)
#define taskSCHEDULER_RUNNING		((BaseType_t)2)
#define xTaskGetSchedulerState()	taskSCHEDULER_NOT_STARTED

#endif
