// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A one-shot timer counting microseconds, for timing inside a burst (the DMR key-up runner, dmr_keyup/dmr_keyup.h).
// One per image, and the board's STM32 family gives it: the interface is here so that the code on it stays the same
// on every family and runs in the host tests on a fake.
//
// Its interrupt has the modem interrupts' priority (the HR_C6000 glue's three lines, hrc/dmr_glue.c). So its callback
// and the modem's handlers never preempt each other, and a task keeps both out with the RTOS interrupt mask
// (portSET_INTERRUPT_MASK_FROM_ISR).
//
// Implementations so far: the STM32H7's, platform/stm32h7/periph/oneshot_tim7.c. The STM32F4 has none, so a board of
// that family cannot list platform/stm32/dmr_keyup yet: its boards time their bursts in their own radio files
// (boards/md380/radio_md380.c, boards/md9600/radio_md9600.c) until that family has one.
#ifndef STM32_ONESHOT_H
#define STM32_ONESHOT_H

#include <stdint.h>

// oneshot.h : a 1 MHz one-shot at the modem interrupts' priority; one per image, the family gives it

// Once, before the first start: the timer set up and stopped. fn is called once for every start that runs out
void stm32_oneshot_init(void (*fn)(void));	// fn runs in the timer's interrupt
// fn in us microseconds from now, 1 to 65535 (a 16-bit timer is enough; 0 counts as 1). A start while one is pending
// replaces it: one call, the new time after this start. fn itself may start the next, and may do so before the rest
// of its work: a wait that runs out while fn is still running calls fn again right after it has returned (the
// handler clears the timer's flag before it calls fn, never after). A task masks the timer's interrupt around the call
void stm32_oneshot_start(uint32_t us);		// from a task or an interrupt of that priority; restarts
// Nothing pending from here on: fn is not called again before the next start, also when the time ran out while the
// caller had the interrupt masked. Same callers as start; harmless when nothing is pending
void stm32_oneshot_stop(void);

#endif
