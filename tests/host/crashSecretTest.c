// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Secret work and the crash record (core/crash/crash_secret.c, crash_secret.h; docs/spec/crash-log.md, secret work): the
// count nests; the scrub zeroes every saved general register and stack word of a filled record and keeps pc, lr, sp,
// the status words and the rest, with CRASH_F_SECRET set; crash_commit applies it only while the count is not 0; the
// fault trace line prints zeros for r0-r13 then, and the registers otherwise. On the stand-in RTOS of crashCaptureTest
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "crash/crash.h"
#include "crash/crash_secret.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static jmp_buf restarted;
static crash_record_t store;
bool fake_irq_off;

void board_rf_safe(void) {}
crash_record_t *hal_crash_store(void) { return &store; }
uint8_t hal_crash_arch(void) { return CRASH_ARCH_CM4; }
static crash_boot_t boot_words;
crash_boot_t *hal_crash_boot(void) { return &boot_words; }
uint8_t hal_crash_reset_flags(uint32_t *raw) { *raw = 0; return CRASH_RESET_UNKNOWN; }
bool hal_crash_ram_addr(uint32_t a) { (void)a; return false; }	// host pointers don't fit: no stack copy here
bool hal_crash_code_addr(uint32_t a) { (void)a; return false; }
uint32_t hal_millis(void) { return 77; }
void hal_crash_restart(void) { longjmp(restarted, 1); }
static char task_name[] = "radio";
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return task_name; }
char *pcTaskGetName(TaskHandle_t t) { return t; }
void vApplicationStackOverflowHook(TaskHandle_t task, char *name);
void vApplicationMallocFailedHook(void);

// A record as a platform fills it, every field not 0
static void fill(crash_record_t *r)
{
	memset(r, 0, sizeof *r);
	r->arch = CRASH_ARCH_CM4;
	r->kind = CRASH_KIND_FAULT;
	r->flags = CRASH_F_ISR;
	r->cause = 3;
	r->pc = 0x0800C1A4u;
	r->lr = 0x0800C001u;
	r->sp = 0x2001FF00u;
	r->psr = 0x61000003u;
	for (unsigned i = 0; i < 14; i++)
		r->r[i] = 0xD0000000u + i;	// as if a key were in them
	for (unsigned i = 0; i < 4; i++)
		r->status[i] = 0x100u + i;
	for (unsigned i = 0; i < CRASH_STACK_WORDS; i++)
		r->stack[i] = 0x5EC0000u + i;
	r->uptime_ms = 999;
	r->build = 0x12345678u;
	memcpy(r->task, "ui", 2);
}

static void test_scrub(void)
{
	crash_record_t r, was;

	fill(&r);
	was = r;
	crash_secret_scrub(&r);
	for (unsigned i = 0; i < 14; i++)
		CHECK(r.r[i] == 0);
	for (unsigned i = 0; i < CRASH_STACK_WORDS; i++)
		CHECK(r.stack[i] == 0);
	CHECK(r.flags == (CRASH_F_ISR | CRASH_F_SECRET));
	CHECK(r.pc == was.pc && r.lr == was.lr && r.sp == was.sp && r.psr == was.psr && r.cause == was.cause);
	CHECK(memcmp(r.status, was.status, sizeof r.status) == 0 && r.kind == was.kind && r.arch == was.arch);
	CHECK(r.uptime_ms == was.uptime_ms && r.build == was.build && memcmp(r.task, was.task, sizeof r.task) == 0);
	// Everything but r[], stack[] and the flag is as it was
	was.flags |= CRASH_F_SECRET;
	memset(was.r, 0, sizeof was.r);
	memset(was.stack, 0, sizeof was.stack);
	CHECK(memcmp(&r, &was, sizeof r) == 0);
	CHECK(CRASH_F_SECRET == 0x20u && (CRASH_F_SECRET & (CRASH_F_ISR | CRASH_F_DIRTY | CRASH_F_STACK | CRASH_F_SEEN |
							      CRASH_F_SENT)) == 0);
}

static void test_count(void)
{
	CHECK(crash_secret == 0);
	crash_secret_enter();
	CHECK(crash_secret == 1);
	crash_secret_enter();		// nests: signing inside a key read
	CHECK(crash_secret == 2);
	crash_secret_leave();
	CHECK(crash_secret == 1);
	crash_secret_leave();
	CHECK(crash_secret == 0);
}

// crash_commit scrubs only during secret work
static void test_commit(void)
{
	crash_record_t r;

	fill(&r);
	memset(&store, 0, sizeof store);
	if (setjmp(restarted) == 0)
		crash_commit(&r);
	CHECK(crash_valid(&store) && store.r[0] == 0xD0000000u && store.r[13] == 0xD000000Du);
	CHECK((store.flags & CRASH_F_SECRET) == 0);

	fill(&r);
	memset(&store, 0, sizeof store);
	crash_secret_enter();
	if (setjmp(restarted) == 0)
		crash_commit(&r);
	crash_secret_leave();
	CHECK(crash_valid(&store) && (store.flags & CRASH_F_SECRET) != 0);
	for (unsigned i = 0; i < 14; i++)
		CHECK(store.r[i] == 0);
	for (unsigned i = 0; i < CRASH_STACK_WORDS; i++)
		CHECK(store.stack[i] == 0);
	CHECK(store.pc == 0x0800C1A4u && store.lr == 0x0800C001u && store.sp == 0x2001FF00u && store.psr == 0x61000003u);
	CHECK(store.status[0] == 0x100u && store.status[3] == 0x103u);
}

// The fault trace line
static char line[512];
static void out(const char *s)
{
	if (strlen(line) + strlen(s) < sizeof line)
		strcat(line, s);
}

static void test_trace(void)
{
	uint32_t regs[14];
	crash_trace_t t = { .vector = 3, .pc = 0x0300ABCDu, .psr = 0x80000140u, .lr = 0x03001111u, .sp = 0x00810000u,
			    .r = regs, .nr = 14 };

	for (unsigned i = 0; i < 14; i++)
		regs[i] = 0xC0DE0000u + i;
	line[0] = 0;
	crash_trace_line(&t, 1, out);
	CHECK(strstr(line, "r0=c0de0000 ") != NULL && strstr(line, "r13=c0de000d ") != NULL);
	CHECK(strstr(line, "pc=0300abcd ") != NULL && strstr(line, "sp=00810000 ") != NULL);

	line[0] = 0;
	crash_secret_enter();
	crash_trace_line(&t, 2, out);
	crash_secret_leave();
	CHECK(strstr(line, "c0de") == NULL);
	CHECK(strstr(line, "r0=00000000 ") != NULL && strstr(line, "r13=00000000 ") != NULL);
	CHECK(strstr(line, "pc=0300abcd ") != NULL && strstr(line, "lr=03001111 ") != NULL &&
	      strstr(line, "psr=80000140 ") != NULL && strstr(line, "sp=00810000 ") != NULL);
	// The CRC still covers what was printed: from '#' to the space before '*'
	const char *a = strchr(line, '#'), *star = strchr(line, '*');
	CHECK(a != NULL && star != NULL);
	if (a != NULL && star != NULL) {
		char body[400];
		size_t n = (size_t)(star - a);
		memcpy(body, a, n);
		body[n] = 0;
		char crc[5];
		snprintf(crc, sizeof crc, "%04x", crash_trace_crc(body));
		CHECK(strncmp(star + 1, crc, 4) == 0);
	}
}

int main(void)
{
	test_scrub();
	test_count();
	test_commit();
	test_trace();
	printf("# crashSecretTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
