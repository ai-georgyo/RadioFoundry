// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for FreeRTOS.h, for the tests of the shared STM32 glue (platform.h next to this file): the interrupt
// mask the glue takes at task level, counted, and configASSERT as a call the test catches
#ifndef STM32GPIO_STUB_FREERTOS_H
#define STM32GPIO_STUB_FREERTOS_H

#include <stdint.h>

typedef uint32_t UBaseType_t;

// The test's: how many masks are held (0: a task with the modem's and the timer's interrupts open)
extern int fake_irq_depth;
#define portSET_INTERRUPT_MASK_FROM_ISR()	(fake_irq_depth++, (UBaseType_t)0x50)
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(m)	((void)(m), fake_irq_depth--)

// The test's: on the radio a failed assertion does not return (the crash record and a restart), so the test's leaves
// the code under test with a longjmp
void fake_assert(unsigned line);
#define configASSERT(x)				do { if (!(x)) { fake_assert(__LINE__); } } while (0)

#endif
