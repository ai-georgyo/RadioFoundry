// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for FreeRTOS.h, for this board's host tests: the interrupt mask macros exp_cs7000p.c takes around a
// check and its write (the test counts the depth, and fails a chain or DAC write made outside a masked section), and
// the few types and constants task.h, semphr.h and timers.h here need. A test that compiles a file taking the mask
// defines fake_irq_depth; built with CS7000P_STUB_MASK_HOOK it also defines fake_mask_enter(), which takes the mask
// itself (and may run an "interrupt" just before it does)
#ifndef CS7000P_STUB_FREERTOS_H
#define CS7000P_STUB_FREERTOS_H

#include <stdint.h>

extern int fake_irq_depth;
#ifdef CS7000P_STUB_MASK_HOOK
void fake_mask_enter(void);
#define portSET_INTERRUPT_MASK_FROM_ISR()	(fake_mask_enter(), (uint32_t)0x50)
#else
#define portSET_INTERRUPT_MASK_FROM_ISR()	(fake_irq_depth++, (uint32_t)0x50)
#endif
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(m)	((void)(m), fake_irq_depth--)

typedef uint32_t TickType_t;
typedef long BaseType_t;
#define portMAX_DELAY		((TickType_t)0xFFFFFFFFu)
#define pdTRUE			((BaseType_t)1)
#define pdFALSE			((BaseType_t)0)
#define pdMS_TO_TICKS(ms)	((TickType_t)(ms))

#endif
