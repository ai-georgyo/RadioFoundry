// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// When the host link asks for the host port's reattach after a transmission (core/hostlink/hostlink_policy.c,
// docs/spec/cps-protocol.md 1.1): a port the transmission took and that no host found again in
// HOSTLINK_REATTACH_WAIT_MS, once, never on the air, never for a port no host had; and its wiring to the port
// (hostlink_port_after_tx)
#include <stdio.h>
#include <string.h>
#include "hostlink/hostlink.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define PASS_MS 5u	// the host link's pass while nothing arrives

static hostlink_reattach_t r;
static uint32_t now;
static unsigned asks;

#define NONE	HAL_STREAM_HOST_NONE
#define IDLE	HAL_STREAM_HOST_IDLE
#define UP	HAL_STREAM_HOST_UP

// Passes for ms with these inputs; the asks they made are counted
static void run(uint32_t ms, bool tx, hal_stream_host_t host)
{
	for (uint32_t t = 0; t < ms; t += PASS_MS) {
		asks += hostlink_reattach_check(&r, tx, host, now);
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
	run(500, false, UP);
	run(3000, true, NONE);			// the port went at the key-up
	run(HOSTLINK_REATTACH_WAIT_MS, false, NONE);	// the end's pass, then up to the wait's last ms
	CHECK(asks == 0);
	run(PASS_MS, false, NONE);
	CHECK(asks == 1);
	run(60000, false, NONE);		// still no host: one ask only
	CHECK(asks == 1);
}

// The usual case on a radio whose port leaves the bus at a key-up: the host enumerates it again by itself
static void test_host_comes_back(void)
{
	reset(1000);
	run(500, false, UP);
	run(2000, true, NONE);
	run(600, false, NONE);
	run(10000, false, UP);
	CHECK(asks == 0);
	run(2000, true, UP);			// a port that stays up through a transmission
	run(10000, false, UP);
	CHECK(asks == 0);
}

// No cable: no host ever had the port, so no transmission resets it
static void test_never_mounted(void)
{
	reset(0);
	for (int i = 0; i < 5; i++) {
		run(3000, true, NONE);
		run(10000, false, NONE);
	}
	CHECK(asks == 0);
}

// The port lost its configuration before the transmission (a reattach no host took, a host that reset it): not the
// transmission's doing
static void test_lost_before(void)
{
	reset(0);
	run(500, false, UP);
	run(500, false, NONE);
	run(2000, true, NONE);
	run(10000, false, NONE);
	CHECK(asks == 0);
}

// Up at the pass before the key-up and gone at the first pass that sees the radio on the air still counts
static void test_lost_at_keyup(void)
{
	reset(0);
	run(PASS_MS, false, UP);
	run(1000, true, NONE);
	run(HOSTLINK_REATTACH_WAIT_MS + PASS_MS, false, NONE);
	CHECK(asks == 1);
}

// Up at the last pass on the air (a host that found the port during the transmission) and gone with its end still counts
static void test_up_at_last_tx_pass(void)
{
	reset(0);
	run(500, false, NONE);
	run(1000, true, NONE);
	run(PASS_MS, true, UP);
	run(HOSTLINK_REATTACH_WAIT_MS + PASS_MS, false, NONE);
	CHECK(asks == 1);
}

// Never while transmitting, and a transmission during the wait restarts it at its own end
static void test_new_transmission_in_wait(void)
{
	reset(0);
	run(500, false, UP);
	run(1000, true, NONE);
	run(HOSTLINK_REATTACH_WAIT_MS - 100, false, NONE);
	run(30000, true, NONE);		// keyed again just before the wait ran out: nothing while on the air
	CHECK(asks == 0);
	run(HOSTLINK_REATTACH_WAIT_MS, false, NONE);	// the end's pass, then up to the wait's last ms
	CHECK(asks == 0);
	run(PASS_MS, false, NONE);
	CHECK(asks == 1);
}

// After an ask, the next needs a host to have had the port again: a reattach that worked arms it, one that didn't
// leaves later transmissions alone
static void test_after_an_ask(void)
{
	reset(0);
	run(500, false, UP);
	run(1000, true, NONE);
	run(HOSTLINK_REATTACH_WAIT_MS + 100, false, NONE);
	CHECK(asks == 1);
	run(2000, true, NONE);			// no host took the reattach
	run(10000, false, NONE);
	CHECK(asks == 1);
	run(1000, false, UP);			// a host has it again (the reattach's next try, or a replug)
	run(1000, true, NONE);
	run(HOSTLINK_REATTACH_WAIT_MS + 100, false, NONE);
	CHECK(asks == 2);
}

// The host back during the wait and gone again: it had the port, so nothing is owed
static void test_back_then_gone(void)
{
	reset(0);
	run(500, false, UP);
	run(1000, true, NONE);
	run(700, false, NONE);
	run(100, false, UP);
	run(10000, false, NONE);
	CHECK(asks == 0);
}

// The millisecond clock wraps during the wait
static void test_clock_wraps(void)
{
	reset(0xFFFFFFFFu - 1500u);
	run(100, false, UP);
	run(1000, true, NONE);
	run(HOSTLINK_REATTACH_WAIT_MS, false, NONE);	// the end's pass, then up to the wait's last ms
	CHECK(asks == 0);
	run(PASS_MS, false, NONE);
	CHECK(asks == 1);
}

// The host's hub drops the port at the key-up, the controller sees the bus go idle, and the host resets and configures
// it again after the release: no ask. One that missed the host's reset and still sleeps after the wait gets one
static void test_awake_then_asleep(void)
{
	reset(0);
	run(500, false, UP);
	run(2000, true, IDLE);
	run(200, false, IDLE);
	run(400, false, NONE);			// the host's bus reset, then its enumeration
	run(10000, false, UP);
	CHECK(asks == 0);
	run(2000, true, IDLE);
	run(HOSTLINK_REATTACH_WAIT_MS, false, IDLE);
	CHECK(asks == 0);
	run(PASS_MS, false, IDLE);
	CHECK(asks == 1);
}

// A port asleep at the key-up (a host that suspended an unused port, or a pulled cable without VBUS sensing) and still
// configured after the wait is as it was: no ask
static void test_asleep_untouched(void)
{
	reset(0);
	run(500, false, UP);
	run(3000, false, IDLE);
	for (int i = 0; i < 5; i++) {
		run(2000, true, IDLE);
		run(10000, false, IDLE);
	}
	CHECK(asks == 0);
}

// Asleep at the key-up, then the host reset it after the release and got no answer: one ask, at the wait's end
static void test_asleep_then_deaf(void)
{
	reset(0);
	run(500, false, IDLE);
	run(2000, true, IDLE);
	run(200, false, IDLE);
	run(HOSTLINK_REATTACH_WAIT_MS - 200, false, NONE);
	CHECK(asks == 0);
	run(PASS_MS, false, NONE);
	CHECK(asks == 1);
	run(60000, false, NONE);
	CHECK(asks == 1);
}

// Asleep at the key-up, gone at it (the pass before counts), and found again by the host: no ask, nor when it sleeps
// again afterwards
static void test_asleep_host_back(void)
{
	reset(0);
	run(PASS_MS, false, IDLE);
	run(2000, true, NONE);
	run(600, false, NONE);
	run(2000, false, UP);
	run(10000, false, IDLE);
	CHECK(asks == 0);
}

// The wiring the host link task runs on each pass (hostlink_port_after_tx) on a fake port
static struct {
	hal_stream_host_t host;
	unsigned host_calls, told, told_on, reattaches;
	bool last_on;
} fp;

static hal_stream_host_t fake_host(void)
{
	fp.host_calls++;
	return fp.host;
}

static void fake_transmitting(bool on)
{
	fp.told++;
	fp.told_on += on;
	fp.last_on = on;
}

static void fake_reattach(void)
{
	fp.reattaches++;
}

static unsigned port_run(const hal_stream_t *port, uint32_t ms, bool tx, hal_stream_host_t host)
{
	unsigned yes = 0;

	fp.host = host;
	for (uint32_t t = 0; t < ms; t += PASS_MS) {
		yes += hostlink_port_after_tx(&r, port, tx, now);
		now += PASS_MS;
	}
	return yes;
}

// A lost port: the port told of the transmission on every pass, its reattach called once, when the call says so
static void test_port_wiring(void)
{
	const hal_stream_t port = { .reattach = fake_reattach, .host = fake_host, .transmitting = fake_transmitting };
	unsigned yes;

	reset(0);
	memset(&fp, 0, sizeof(fp));
	yes = port_run(&port, 500, false, UP);
	CHECK(fp.told == 500 / PASS_MS && fp.told_on == 0 && fp.host_calls == 500 / PASS_MS);
	yes += port_run(&port, 1000, true, NONE);
	CHECK(fp.told_on == 1000 / PASS_MS && fp.last_on);
	yes += port_run(&port, HOSTLINK_REATTACH_WAIT_MS, false, NONE);
	CHECK(!fp.last_on && fp.reattaches == 0 && yes == 0);
	yes += port_run(&port, 10000, false, NONE);
	CHECK(fp.reattaches == 1 && yes == 1);
}

// A port without reattach or without host gets nothing, not even the transmission; one without transmitting still
// gets its reattach
static void test_port_without(void)
{
	const hal_stream_t no_reattach = { .host = fake_host, .transmitting = fake_transmitting };
	const hal_stream_t no_host = { .reattach = fake_reattach, .transmitting = fake_transmitting };
	const hal_stream_t no_told = { .reattach = fake_reattach, .host = fake_host };
	const hal_stream_t *const without[] = { &no_reattach, &no_host };

	for (unsigned i = 0; i < 2; i++) {
		reset(0);
		memset(&fp, 0, sizeof(fp));
		unsigned yes = port_run(without[i], 500, false, UP);
		yes += port_run(without[i], 1000, true, NONE);
		yes += port_run(without[i], 10000, false, NONE);
		CHECK(yes == 0 && fp.reattaches == 0 && fp.told == 0 && fp.host_calls == 0);
	}
	reset(0);
	memset(&fp, 0, sizeof(fp));
	unsigned yes = port_run(&no_told, 500, false, UP);
	yes += port_run(&no_told, 1000, true, NONE);
	yes += port_run(&no_told, 10000, false, NONE);
	CHECK(yes == 1 && fp.reattaches == 1);
}

int main(void)
{
	test_lost_port();
	test_host_comes_back();
	test_never_mounted();
	test_lost_before();
	test_lost_at_keyup();
	test_up_at_last_tx_pass();
	test_new_transmission_in_wait();
	test_after_an_ask();
	test_back_then_gone();
	test_clock_wraps();
	test_awake_then_asleep();
	test_asleep_untouched();
	test_asleep_then_deaf();
	test_asleep_host_back();
	test_port_wiring();
	test_port_without();
	if (failures != 0) {
		printf("hostlinkReattachTest: %d FAILED\n", failures);
		return 1;
	}
	printf("hostlinkReattachTest: all passed\n");
	return 0;
}
