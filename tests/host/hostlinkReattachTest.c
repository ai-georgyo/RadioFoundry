// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// When the host link asks for the host port's reattach after a transmission (core/hostlink/hostlink_policy.c,
// docs/spec/cps-protocol.md 1.1): a port the transmission took and that no host found again in
// HOSTLINK_REATTACH_WAIT_MS, once, never on the air, never for a port no host had
#include <stdio.h>
#include <string.h>
#include "hostlink/hostlink.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define PASS_MS 5u	// the host link's pass while nothing arrives

static hostlink_reattach_t r;
static uint32_t now;
static unsigned asks;

// Passes for ms with these inputs; the asks they made are counted
static void run(uint32_t ms, bool tx, bool mounted)
{
	for (uint32_t t = 0; t < ms; t += PASS_MS) {
		asks += hostlink_reattach_check(&r, tx, mounted, now);
		now += PASS_MS;
	}
}

static void reset(uint32_t start)
{
	memset(&r, 0, sizeof(r));
	now = start;
	asks = 0;
}

// A port up before the key-up and lost with it: asked for once, HOSTLINK_REATTACH_WAIT_MS after the end, not before
static void test_lost_port(void)
{
	reset(1000);
	run(500, false, true);
	run(3000, true, false);			// the port went at the key-up
	run(HOSTLINK_REATTACH_WAIT_MS, false, false);	// the end's pass, then up to the wait's last ms
	CHECK(asks == 0);
	run(PASS_MS, false, false);
	CHECK(asks == 1);
	run(60000, false, false);		// still no host: one ask only
	CHECK(asks == 1);
}

// The usual case on a radio whose port leaves the bus at a key-up: the host enumerates it again by itself
static void test_host_comes_back(void)
{
	reset(1000);
	run(500, false, true);
	run(2000, true, false);
	run(600, false, false);
	run(10000, false, true);
	CHECK(asks == 0);
	run(2000, true, true);			// a port that stays up through a transmission
	run(10000, false, true);
	CHECK(asks == 0);
}

// No cable: no host ever had the port, so no transmission resets it
static void test_never_mounted(void)
{
	reset(0);
	for (int i = 0; i < 5; i++) {
		run(3000, true, false);
		run(10000, false, false);
	}
	CHECK(asks == 0);
}

// The port went before the transmission (pulled, suspended, a reattach no host took): not the transmission's doing
static void test_lost_before(void)
{
	reset(0);
	run(500, false, true);
	run(500, false, false);
	run(2000, true, false);
	run(10000, false, false);
	CHECK(asks == 0);
}

// Up at the pass before the key-up and gone at the first pass that sees the radio on the air still counts
static void test_lost_at_keyup(void)
{
	reset(0);
	run(PASS_MS, false, true);
	run(1000, true, false);
	run(HOSTLINK_REATTACH_WAIT_MS + PASS_MS, false, false);
	CHECK(asks == 1);
}

// Never while transmitting, and a transmission during the wait restarts it at its own end
static void test_new_transmission_in_wait(void)
{
	reset(0);
	run(500, false, true);
	run(1000, true, false);
	run(HOSTLINK_REATTACH_WAIT_MS - 100, false, false);
	run(30000, true, false);		// keyed again just before the wait ran out: nothing while on the air
	CHECK(asks == 0);
	run(HOSTLINK_REATTACH_WAIT_MS, false, false);	// the end's pass, then up to the wait's last ms
	CHECK(asks == 0);
	run(PASS_MS, false, false);
	CHECK(asks == 1);
}

// After an ask, the next needs a host to have had the port again: a reattach that worked arms it, one that didn't
// leaves later transmissions alone
static void test_after_an_ask(void)
{
	reset(0);
	run(500, false, true);
	run(1000, true, false);
	run(HOSTLINK_REATTACH_WAIT_MS + 100, false, false);
	CHECK(asks == 1);
	run(2000, true, false);			// no host took the reattach
	run(10000, false, false);
	CHECK(asks == 1);
	run(1000, false, true);			// a host has it again (the reattach's next try, or a replug)
	run(1000, true, false);
	run(HOSTLINK_REATTACH_WAIT_MS + 100, false, false);
	CHECK(asks == 2);
}

// The host back during the wait and gone again: it had the port, so nothing is owed
static void test_back_then_gone(void)
{
	reset(0);
	run(500, false, true);
	run(1000, true, false);
	run(700, false, false);
	run(100, false, true);
	run(10000, false, false);
	CHECK(asks == 0);
}

// The millisecond clock wraps during the wait
static void test_clock_wraps(void)
{
	reset(0xFFFFFFFFu - 1500u);
	run(100, false, true);
	run(1000, true, false);
	run(HOSTLINK_REATTACH_WAIT_MS, false, false);	// the end's pass, then up to the wait's last ms
	CHECK(asks == 0);
	run(PASS_MS, false, false);
	CHECK(asks == 1);
}

int main(void)
{
	test_lost_port();
	test_host_comes_back();
	test_never_mounted();
	test_lost_before();
	test_lost_at_keyup();
	test_new_transmission_in_wait();
	test_after_an_ask();
	test_back_then_gone();
	test_clock_wraps();
	if (failures != 0) {
		printf("hostlinkReattachTest: %d FAILED\n", failures);
		return 1;
	}
	printf("hostlinkReattachTest: all passed\n");
	return 0;
}
