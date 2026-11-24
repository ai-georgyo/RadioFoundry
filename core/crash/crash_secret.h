// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Secret work and the crash record (docs/spec/crash-log.md, the secret work section; docs/spec/p256.md 10). The crash
// record and the fault trace keep the general registers at the fault (all of r0-r13 on the HR_C7000, r0-r3 and r12
// on an STM32) and 16 words at the stack pointer, and they leave the radio through the debug link. A fault while a key
// or a signing nonce sits in registers or on the stack would put it there. So code that holds a secret outside its
// wiped context (P-256 signing, reading a signing key or a traffic key into the engine) brackets that work with
// crash_secret_enter / _leave; while the count is not 0 the capture (crash_commit, the fault trace line) zeroes every
// saved general register and every saved stack word, keeps pc, lr, sp and the status words, and sets CRASH_F_SECRET.
// Nests; a few instructions. The count's update is plain, not atomic: any task may bracket work as long as one that
// preempts another never waits inside its bracket (the air provider's task over the radio task), so it leaves the
// count as it found it before the preempted task's update goes on (crash-log.md 1.4). A task that could block inside
// its bracket while a lower one is in the middle of its enter or leave would lose an update; no interrupt handler
// brackets. Without the crash log and the fault trace both, nothing is recorded and these are empty.
#ifndef RFOUNDRY_CRASH_SECRET_H
#define RFOUNDRY_CRASH_SECRET_H

#include <stdint.h>
#include "config.h"

#if CONFIG_CRASH_LOG || CONFIG_FAULT_TRACE
extern volatile uint8_t crash_secret;	// core/crash/crash_secret.c

static inline void crash_secret_enter(void)
{
	crash_secret++;
	__atomic_signal_fence(__ATOMIC_SEQ_CST);	// the secret's first load stays after the flag
}

static inline void crash_secret_leave(void)
{
	__atomic_signal_fence(__ATOMIC_SEQ_CST);	// and its last use before the clear
	crash_secret--;
}
#else
static inline void crash_secret_enter(void) {}
static inline void crash_secret_leave(void) {}
#endif

#endif
