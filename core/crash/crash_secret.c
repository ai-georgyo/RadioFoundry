// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Secret work and the crash record (crash_secret.h; docs/spec/crash-log.md, secret work): the count, and the scrub the
// capture applies while it is not 0. Built with the crash log or the fault trace.
#include "config.h"
#if CONFIG_CRASH_LOG || CONFIG_FAULT_TRACE	// the whole file
#include "crash/crash_secret.h"
#include "crash/crash.h"

volatile uint8_t crash_secret;

#if CONFIG_CRASH_LOG
// Every saved general register and stack word goes; where it happened (pc, lr, sp) and why (the status words) stay,
// and the record says it was scrubbed
void crash_secret_scrub(crash_record_t *r)
{
	for (unsigned i = 0; i < sizeof r->r / sizeof r->r[0]; i++)
		r->r[i] = 0;
	for (unsigned i = 0; i < CRASH_STACK_WORDS; i++)
		r->stack[i] = 0;
	r->flags |= CRASH_F_SECRET;
}
#endif

#endif
