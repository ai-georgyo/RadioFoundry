// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The 8080 display bus on GPIO (platform/stm32/lcd8080_gpio) against a panel that watches every pin: nothing is
// driven before the init, a byte is on the lines through its whole write strobe, D/C and the chip select frame the
// transfers, RES drives before any other line, a read turns the lines round and back with no moment where both ends
// drive (the lines stay inputs through the last strobe's high time) and reads 00 off a panel that does not answer, and
// outside a session the lines are inputs again. The real display driver (drivers/display/st7735) then runs its init
// on this bus
#define STM32GPIO_FAKE_MODEL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "platform.h"
#include "FreeRTOS.h"
#include "lcd8080_gpio/lcd8080_gpio.h"
#include "display/st7735/st7735.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// ---- The board's side: the lock, and assertions ----

int fake_irq_depth;
static int lock_depth, locks;

static void bus_lock(void)
{
	lock_depth++;
	locks++;
}

// A look before and after: the watcher sees the lines as the session leaves them, first inside it and then outside
static void bus_unlock(void)
{
	fake_sync();
	lock_depth--;
	fake_sync();
}

static jmp_buf assert_env;
static bool assert_armed;
static int asserts;

void fake_assert(unsigned line)
{
	asserts++;
	if (!assert_armed) {
		printf("FAIL: assertion at lcd8080_gpio.c:%u\n", line);
		exit(1);	// nowhere to go on from: the radio would have restarted
	}
	longjmp(assert_env, 1);
}

// Run a call that must end in a failed assertion
#define ASSERTS(call) do { \
		volatile int before_ = asserts; \
		assert_armed = true; \
		if (setjmp(assert_env) == 0) { \
			call; \
		} \
		assert_armed = false; \
		CHECK(asserts == before_ + 1); \
	} while (0)

// ---- The panel: every pin watched at every look ----

#define LOG_MAX		50000u

static const stm32_lcd8080_cfg_t *cfg;	// NULL: the bus has not been set up, no pin may drive
static struct {
	bool cs, wr, rd, res, dc;		// the levels at the last look
	uint16_t byte_at_fall;			// the data lines when WR fell
	uint64_t t_wr_fall, t_wr_rise, t_rd_fall, t_rd_rise;
	bool wr_seen, rd_seen;			// a strobe since the session began
	bool rd_hold;				// RD rose less than rd_high_ns ago: the panel may still drive
	bool silent;				// a panel that does not answer a read (in reset, no RD)
	uint8_t log[LOG_MAX];			// the bytes the panel took, in order
	bool log_cmd[LOG_MAX];			// D/C was low
	uint32_t n, total;			// logged (up to LOG_MAX), and taken in all
	uint32_t sessions;
	uint8_t res_edges[8];			// RES levels in the order it took them
	uint32_t n_res;
	const uint8_t *answer;			// what a read strobe gets, byte by byte
	uint32_t n_answer, reads;
	uint32_t other_odr[FAKE_PORTS];		// output register bits that are not the bus's: never touched
} pan;

static uint16_t data_pins(void)
{
	return (uint16_t)(0xFFu << cfg->data_shift);
}

static uint32_t port_index(const GPIO_TypeDef *port)
{
	return (uint32_t)(port - fake_ports);
}

// The bus's own pins of port i
static uint16_t bus_pins(uint32_t i)
{
	uint16_t m = 0;
	m |= port_index(cfg->data_port) == i ? data_pins() : 0u;
	m |= port_index(cfg->cs_port) == i ? cfg->cs_pin : 0u;
	m |= port_index(cfg->dc_port) == i ? cfg->dc_pin : 0u;
	m |= port_index(cfg->wr_port) == i ? cfg->wr_pin : 0u;
	m |= port_index(cfg->rd_port) == i ? cfg->rd_pin : 0u;
	m |= port_index(cfg->res_port) == i ? cfg->res_pin : 0u;
	return m;
}

static void other_snapshot(void)
{
	for (uint32_t i = 0; i < FAKE_PORTS; i++) {
		pan.other_odr[i] = fake_ports[i].ODR & ~(uint32_t)bus_pins(i);
	}
}

static void watch(void)
{
	if (cfg == NULL) {
		for (uint32_t i = 0; i < FAKE_PORTS; i++) {
			CHECK(fake_outputs(&fake_ports[i]) == 0);	// nothing drives before the init
		}
		return;
	}
	bool cs = fake_level(cfg->cs_port, cfg->cs_pin), wr = fake_level(cfg->wr_port, cfg->wr_pin);
	bool rd = fake_level(cfg->rd_port, cfg->rd_pin), res = fake_level(cfg->res_port, cfg->res_pin);
	bool dc = fake_level(cfg->dc_port, cfg->dc_pin);
	uint16_t lines = data_pins();
	bool driving = (fake_outputs(cfg->data_port) & lines) == lines;
	bool listening = (fake_outputs(cfg->data_port) & lines) == 0;
	uint8_t byte = (uint8_t)(cfg->data_port->IDR >> cfg->data_shift);

	CHECK(driving || listening);	// the eight lines turn together
	CHECK((cfg->data_port->OTYPER & lines) == 0);	// push-pull
	// RES drives before any strobe or select does: a line that leaves its floating start for its idle level can
	// look like an edge, and only a panel in reset ignores it
	bool ctl_out = (fake_outputs(cfg->cs_port) & cfg->cs_pin) || (fake_outputs(cfg->wr_port) & cfg->wr_pin) ||
		       (fake_outputs(cfg->rd_port) & cfg->rd_pin) || (fake_outputs(cfg->dc_port) & cfg->dc_pin);
	if (ctl_out) {
		CHECK((fake_outputs(cfg->res_port) & cfg->res_pin) != 0);
	}
	// After a read strobe the lines stay ours to listen on for its high time: the panel's drivers turn off then
	if (pan.rd_hold) {
		if (fake_ns - pan.t_rd_rise >= cfg->rd_high_ns) {
			pan.rd_hold = false;
		} else {
			CHECK(listening);
		}
	}
	for (uint32_t i = 0; i < FAKE_PORTS; i++) {
		CHECK((fake_ports[i].ODR & ~(uint32_t)bus_pins(i)) == pan.other_odr[i]);
	}
	if (lock_depth == 0) {
		// Outside a session: the panel is not selected, the strobes rest, and the lines are the key matrix's
		CHECK(cs && wr && rd);
		CHECK(listening);
		CHECK((fake_pulled(cfg->data_port, cfg->idle_pull) & lines) == lines);
	}
	if (pan.cs && !cs) {
		pan.sessions++;
		pan.wr_seen = false;
		pan.rd_seen = false;
		CHECK(lock_depth == 1);
		CHECK(driving);
	}
	if (pan.wr && !wr) {
		CHECK(!cs);	// a strobe the panel would not take: a write outside a session
		CHECK(rd);
		CHECK(driving);
		if (pan.wr_seen) {
			CHECK(fake_ns - pan.t_wr_rise >= cfg->wr_high_ns);
		}
		pan.byte_at_fall = byte;
		pan.t_wr_fall = fake_ns;
	}
	if (!pan.wr && wr) {
		// Taken on the rising edge: the byte that was there when the strobe fell, D/C as it is now
		CHECK(!cs);
		CHECK(driving);
		CHECK(byte == pan.byte_at_fall);
		CHECK(fake_ns - pan.t_wr_fall >= cfg->wr_low_ns);
		pan.t_wr_rise = fake_ns;
		pan.wr_seen = true;
		if (pan.n < LOG_MAX) {
			pan.log[pan.n] = byte;
			pan.log_cmd[pan.n] = !dc;
			pan.n++;
		}
		pan.total++;
	}
	if (pan.rd && !rd) {
		// The panel drives from here: the lines must have been turned round already, with no pull of ours
		CHECK(!cs);
		CHECK(wr);
		CHECK(dc);
		CHECK(listening);
		CHECK((fake_pulled(cfg->data_port, GPIO_NOPULL) & lines) == lines);
		if (pan.rd_seen) {
			CHECK(fake_ns - pan.t_rd_rise >= cfg->rd_high_ns);
		}
		pan.t_rd_fall = fake_ns;
		uint8_t out = pan.reads < pan.n_answer ? pan.answer[pan.reads] : 0xEEu;
		if (!pan.silent) {
			fake_drive(cfg->data_port, lines, (uint16_t)(out << cfg->data_shift));
		}
	}
	if (!pan.rd && rd) {
		CHECK(listening);
		CHECK(fake_ns - pan.t_rd_fall >= cfg->rd_low_ns);
		pan.t_rd_rise = fake_ns;
		pan.rd_seen = true;
		pan.rd_hold = true;
		pan.reads++;
		fake_release(cfg->data_port, lines);
	}
	if (pan.res != res && pan.n_res < sizeof(pan.res_edges)) {
		pan.res_edges[pan.n_res++] = res;
	}
	pan.cs = cs;
	pan.wr = wr;
	pan.rd = rd;
	pan.res = res;
	pan.dc = dc;
}

static void panel_clear(void)
{
	pan.n = 0;
	pan.total = 0;
	pan.sessions = 0;
	pan.n_res = 0;
	pan.reads = 0;
}

// ---- Two boards: the lines on the strobes' port from pin 0, and on a port of their own from pin 8 ----

static const stm32_lcd8080_cfg_t board_a = {
	.data_port = GPIOD, .data_shift = 0,
	.cs_port = GPIOE, .cs_pin = GPIO_PIN_1, .dc_port = GPIOD, .dc_pin = GPIO_PIN_12,
	.wr_port = GPIOD, .wr_pin = GPIO_PIN_13, .rd_port = GPIOD, .rd_pin = GPIO_PIN_14,
	.res_port = GPIOE, .res_pin = GPIO_PIN_0,
	.wr_low_ns = 100, .wr_high_ns = 100, .rd_low_ns = 1000, .rd_high_ns = 1000,
	.idle_pull = GPIO_PULLUP, .lock = bus_lock, .unlock = bus_unlock,
};

static const stm32_lcd8080_cfg_t board_b = {
	.data_port = GPIOB, .data_shift = 8,
	.cs_port = GPIOA, .cs_pin = GPIO_PIN_3, .dc_port = GPIOA, .dc_pin = GPIO_PIN_4,
	.wr_port = GPIOC, .wr_pin = GPIO_PIN_5, .rd_port = GPIOC, .rd_pin = GPIO_PIN_6,
	.res_port = GPIOA, .res_pin = GPIO_PIN_7,
	.wr_low_ns = 0, .wr_high_ns = 0, .rd_low_ns = 45, .rd_high_ns = 90,
	.idle_pull = GPIO_PULLDOWN, .lock = bus_lock, .unlock = bus_unlock,
};

// Before stm32_lcd8080_init: a board that found no panel it knows. Every call returns and no pin moves
static void test_before_init(void)
{
	uint8_t id[4] = { 1, 2, 3, 4 };
	uint16_t px[2] = { 0x1234, 0x5678 };

	fake_reset();
	CHECK(stm32_lcd8080_idle());
	stm32_lcd8080_reset(true);
	stm32_lcd8080_begin();
	stm32_lcd8080_cmd(0x2C);
	stm32_lcd8080_data(id, sizeof(id));
	stm32_lcd8080_pixels(px, 2, false);
	stm32_lcd8080_read(0x04, id, sizeof(id));
	stm32_lcd8080_end();
	stm32_lcd8080_reset(false);
	fake_sync();
	CHECK(id[0] == 0 && id[1] == 0 && id[2] == 0 && id[3] == 0);	// a read gives an ID nobody knows
	CHECK(locks == 0 && asserts == 0);
	CHECK(stm32_lcd8080_idle());
	for (uint32_t i = 0; i < FAKE_PORTS; i++) {
		CHECK(fake_ports[i].MODER == 0 && fake_ports[i].ODR == 0 && fake_ports[i].PUPDR == 0);
	}
}

// The lines as a board leaves them for the bus: inputs with pull-ups, RES held low by the panel's own resistor.
// Other pins of the same ports carry levels the bus must leave alone
static void board_start(const stm32_lcd8080_cfg_t *c)
{
	cfg = NULL;
	asserts = 0;
	fake_reset();
	for (uint32_t i = 0; i < FAKE_PORTS; i++) {
		fake_ports[i].PUPDR = 0x55555555u;
	}
	fake_drive(c->res_port, c->res_pin, 0);
	fake_sync();
	// The strobes read high before anything drives them: the first thing the panel sees is no edge at all
	memset(&pan, 0, sizeof(pan));
	pan.cs = pan.wr = pan.rd = pan.dc = true;
	CHECK(fake_level(c->cs_port, c->cs_pin) && fake_level(c->wr_port, c->wr_pin) && fake_level(c->rd_port, c->rd_pin));
	CHECK(!fake_level(c->res_port, c->res_pin));
	cfg = c;
	other_snapshot();
	lock_depth = 1;		// the init runs before the first session: the outside-a-session rules begin after it
	stm32_lcd8080_init(c);
	fake_release(c->res_port, c->res_pin);	// the pin holds RES itself from here on
	lock_depth = 0;
	fake_sync();
	// Outputs elsewhere on the bus's ports, one high and one low on each, for the untouched check
	for (uint32_t i = 0; i < FAKE_PORTS; i++) {
		uint16_t free_pins = (uint16_t)~bus_pins(i);
		uint16_t hi = (uint16_t)(free_pins & (uint16_t)-free_pins);
		uint16_t lo = (uint16_t)((free_pins & (free_pins - 1u)) & (uint16_t)-(free_pins & (free_pins - 1u)));
		fake_watch = NULL;	// the watcher off while the test itself sets pins up
		stm32_pin_out(&fake_ports[i], hi, true, false);
		stm32_pin_out(&fake_ports[i], lo, false, false);
		fake_watch = watch;
	}
	other_snapshot();
	fake_sync();
}

static void test_init(const stm32_lcd8080_cfg_t *c)
{
	uint16_t lines = (uint16_t)(0xFFu << c->data_shift);
	int locks0 = locks;

	board_start(c);
	// No strobe and no select edge on the way: the panel took nothing and was never selected
	CHECK(pan.total == 0 && pan.sessions == 0 && pan.reads == 0 && pan.n_res == 0);
	CHECK(fake_outputs(c->cs_port) & c->cs_pin);
	CHECK(fake_outputs(c->wr_port) & c->wr_pin);
	CHECK(fake_outputs(c->rd_port) & c->rd_pin);
	CHECK(fake_outputs(c->dc_port) & c->dc_pin);
	CHECK(fake_outputs(c->res_port) & c->res_pin);
	CHECK(fake_level(c->cs_port, c->cs_pin) && fake_level(c->wr_port, c->wr_pin) && fake_level(c->rd_port, c->rd_pin));
	CHECK(!fake_level(c->res_port, c->res_pin));	// the panel is in reset
	CHECK((fake_outputs(c->data_port) & lines) == 0);
	CHECK((fake_pulled(c->data_port, c->idle_pull) & lines) == lines);
	// The lines get the outputs' slope for the time they are outputs
	for (uint32_t n = 0; n < 8u; n++) {
		CHECK(fake_field2(c->data_port->OSPEEDR, c->data_shift + n) == STM32_GPIO_SPEED);
	}
	CHECK(stm32_lcd8080_idle());
	CHECK(locks == locks0);		// the init is not a session
	CHECK(fake_primask == 0);
}

static void test_session(const stm32_lcd8080_cfg_t *c)
{
	static const uint8_t params[] = { 0x00, 0xFF, 0xA5, 0x5A, 0x01, 0x80 };
	static const uint16_t px[] = { 0xF800, 0x07E0, 0x001F, 0x1234 };
	uint16_t lines = (uint16_t)(0xFFu << c->data_shift);
	int locks0 = locks;

	board_start(c);
	stm32_lcd8080_reset(true);
	CHECK(fake_level(c->res_port, c->res_pin));
	stm32_lcd8080_reset(false);
	CHECK(!fake_level(c->res_port, c->res_pin));
	stm32_lcd8080_reset(true);
	CHECK(fake_level(c->res_port, c->res_pin));
	CHECK(pan.n_res == 3 && pan.res_edges[0] == 1 && pan.res_edges[1] == 0 && pan.res_edges[2] == 1);
	CHECK(pan.total == 0 && pan.sessions == 0);	// the reset line alone: no session, no lock
	CHECK(locks == locks0);

	stm32_lcd8080_begin();
	CHECK(!stm32_lcd8080_idle());
	CHECK(lock_depth == 1 && locks == locks0 + 1);
	CHECK(pan.sessions == 1);
	CHECK(!fake_level(c->cs_port, c->cs_pin));
	CHECK((fake_outputs(c->data_port) & lines) == lines);
	stm32_lcd8080_cmd(0x2A);
	stm32_lcd8080_data(params, sizeof(params));
	stm32_lcd8080_cmd(0x2C);
	stm32_lcd8080_pixels(px, 4, false);
	stm32_lcd8080_pixels(px, 4, true);
	stm32_lcd8080_data(params, 0);
	stm32_lcd8080_end();
	CHECK(stm32_lcd8080_idle());
	CHECK(lock_depth == 0);
	CHECK(fake_level(c->cs_port, c->cs_pin) && fake_level(c->wr_port, c->wr_pin) && fake_level(c->rd_port, c->rd_pin));
	CHECK((fake_outputs(c->data_port) & lines) == 0);
	CHECK((fake_pulled(c->data_port, c->idle_pull) & lines) == lines);

	// What the panel took: each command with D/C low, everything else with it high, pixels high byte first
	static const uint8_t want[] = {
		0x2A, 0x00, 0xFF, 0xA5, 0x5A, 0x01, 0x80,
		0x2C, 0xF8, 0x00, 0x07, 0xE0, 0x00, 0x1F, 0x12, 0x34,
		0x00, 0x1F, 0x07, 0xE0, 0xF8, 0x00, 0xA2, 0x22,		// red and blue exchanged: 0x1234 -> 0xA222
	};
	CHECK(pan.total == sizeof(want));
	for (uint32_t i = 0; i < sizeof(want) && i < pan.n; i++) {
		CHECK(pan.log[i] == want[i]);
		CHECK(pan.log_cmd[i] == (i == 0 || i == 7));
	}
	CHECK(pan.sessions == 1 && pan.reads == 0);
	// The time the strobes took: each byte its low and its high time
	CHECK(fake_ns == (uint64_t)sizeof(want) * (c->wr_low_ns + c->wr_high_ns));
	CHECK(fake_fights == 0 && fake_float_reads == 0 && fake_primask == 0 && asserts == 0);
}

static void test_read(const stm32_lcd8080_cfg_t *c)
{
	static const uint8_t id_answer[] = { 0x00, 0x7C, 0x89, 0xF0 };	// the dummy byte, then three ID bytes
	uint8_t id[4] = { 0 };
	uint8_t one = 0x55;
	uint16_t lines = (uint16_t)(0xFFu << c->data_shift);

	board_start(c);
	pan.answer = id_answer;
	pan.n_answer = sizeof(id_answer);
	stm32_lcd8080_begin();
	stm32_lcd8080_read(0x04, id, sizeof(id));
	CHECK(memcmp(id, id_answer, sizeof(id)) == 0);
	CHECK(pan.reads == 4);
	CHECK(pan.total == 1 && pan.log[0] == 0x04 && pan.log_cmd[0]);
	// Round again: the lines drive, with the pull they have between sessions, and the next write gets through
	CHECK((fake_outputs(c->data_port) & lines) == lines);
	CHECK((fake_pulled(c->data_port, c->idle_pull) & lines) == lines);
	stm32_lcd8080_cmd(0x3A);
	stm32_lcd8080_data(&one, 1);
	CHECK(pan.total == 3 && pan.log[1] == 0x3A && pan.log_cmd[1] && pan.log[2] == 0x55 && !pan.log_cmd[2]);
	// A read of nothing: the command alone
	stm32_lcd8080_read(0xDA, id, 0);
	CHECK(pan.reads == 4 && pan.total == 4 && pan.log[3] == 0xDA);
	stm32_lcd8080_end();
	CHECK(fake_ns >= 4u * (uint64_t)(c->rd_low_ns + c->rd_high_ns));
	// Both ends never drove at once, and nothing was read off a line nobody drove
	CHECK(fake_fights == 0 && fake_float_reads == 0 && fake_primask == 0 && asserts == 0);
}

// A panel that does not drive the lines on a read (still in reset, no RD, the wrong select): each line keeps the
// charge the MCU left on it. That must read 00, the failure a board recognizes, not the command byte again. The same
// read of a panel that answers still gets its bytes
static void test_no_answer(const stm32_lcd8080_cfg_t *c)
{
	static const uint8_t id_answer[] = { 0xFF, 0x7C, 0x89, 0xF0 };
	uint8_t id[4] = { 1, 2, 3, 4 };

	board_start(c);
	fake_hold = true;
	pan.silent = true;
	stm32_lcd8080_begin();
	stm32_lcd8080_read(0x04, id, sizeof(id));
	CHECK(id[0] == 0 && id[1] == 0 && id[2] == 0 && id[3] == 0);
	CHECK(pan.reads == 4 && pan.total == 1 && pan.log[0] == 0x04);
	stm32_lcd8080_read(0xDA, id, 1);
	CHECK(id[0] == 0);
	pan.silent = false;
	pan.answer = id_answer;
	pan.n_answer = sizeof(id_answer);
	pan.reads = 0;
	stm32_lcd8080_read(0xDB, id, sizeof(id));
	CHECK(memcmp(id, id_answer, sizeof(id)) == 0);
	stm32_lcd8080_end();
	CHECK(fake_fights == 0 && fake_float_reads == 0 && fake_primask == 0 && asserts == 0);
	fake_hold = false;
}

// A transfer with no session open: an assertion, and the panel sees no strobe
static void test_outside(const stm32_lcd8080_cfg_t *c)
{
	uint8_t b[2] = { 1, 2 };
	uint16_t px = 0xFFFF;

	board_start(c);
	ASSERTS(stm32_lcd8080_cmd(0x29));
	ASSERTS(stm32_lcd8080_data(b, 2));
	ASSERTS(stm32_lcd8080_pixels(&px, 1, false));
	ASSERTS(stm32_lcd8080_read(0x04, b, 2));
	ASSERTS(stm32_lcd8080_end());
	fake_sync();
	CHECK(pan.total == 0 && pan.reads == 0 && pan.sessions == 0);
	CHECK(lock_depth == 0);
	CHECK(stm32_lcd8080_idle());
	// Nor a session inside a session (the lock is recursive, the lines are not)
	stm32_lcd8080_begin();
	CHECK(lock_depth == 1);
	ASSERTS(stm32_lcd8080_begin());
	lock_depth--;		// the second begin's lock: the radio restarts there, the test goes on
	CHECK(!stm32_lcd8080_idle());
	stm32_lcd8080_cmd(0x29);
	stm32_lcd8080_end();
	CHECK(pan.total == 1 && pan.sessions == 1 && lock_depth == 0);
	CHECK(stm32_lcd8080_idle());
}

// ---- The real driver on the bus ----

static uint32_t delay_total_ms;

static void bus_delay_ms(uint32_t ms)
{
	fake_sync();
	CHECK(lock_depth == 0);		// the driver's pauses leave the bus to the key scan
	delay_total_ms += ms;
}

static const st7735_bus_t bus = {
	.begin = stm32_lcd8080_begin, .end = stm32_lcd8080_end, .cmd = stm32_lcd8080_cmd, .data = stm32_lcd8080_data,
	.pixels = stm32_lcd8080_pixels, .reset = stm32_lcd8080_reset, .delay_ms = bus_delay_ms,
};

static void test_driver(const stm32_lcd8080_cfg_t *c, uint8_t type)
{
	const st7735_t panel = { .bus = &bus, .type = type, .width = 160, .height = 128 };
	int locks0 = locks;

	board_start(c);
	delay_total_ms = 0;
	st7735_init(&panel);
	fake_sync();
	// Out of reset at the end, after one low pulse: high, low, high from the init's own low
	CHECK(fake_level(c->res_port, c->res_pin));
	CHECK(pan.n_res == 3 && pan.res_edges[0] == 1 && pan.res_edges[1] == 0 && pan.res_edges[2] == 1);
	// Selected only inside sessions (the watcher checks the lock at every select), each session closed again
	CHECK(pan.sessions >= 3 && (int)pan.sessions == locks - locks0);
	CHECK(lock_depth == 0 && stm32_lcd8080_idle());
	CHECK(fake_level(c->cs_port, c->cs_pin));
	CHECK(delay_total_ms >= 120);
	// The init table went through: sleep out (types 0 and 1) or the unlock pair first, then 16 bits a pixel, the
	// whole panel cleared and the display on as the last command
	CHECK(pan.n > 0 && pan.log_cmd[0] && pan.log[0] == (type <= 1 ? 0x11 : 0xFE));
	bool colmod = false;
	for (uint32_t i = 0; i + 1u < pan.n; i++) {
		colmod = colmod || (pan.log_cmd[i] && pan.log[i] == 0x3A && !pan.log_cmd[i + 1u] && pan.log[i + 1u] == 0x05);
	}
	CHECK(colmod);
	CHECK(pan.n < LOG_MAX);
	CHECK(pan.log_cmd[pan.n - 1u] && pan.log[pan.n - 1u] == 0x29);
	CHECK(pan.log_cmd[pan.n - 2u - 160u * 128u * 2u] && pan.log[pan.n - 2u - 160u * 128u * 2u] == 0x2C);
	uint32_t zeros = 0;
	for (uint32_t i = pan.n - 1u - 160u * 128u * 2u; i < pan.n - 1u; i++) {
		zeros += !pan.log_cmd[i] && pan.log[i] == 0;
	}
	CHECK(zeros == 160u * 128u * 2u);

	// A frame's rows through the driver: its window, then the pixels (type 3 is a BGR panel: red and blue exchanged)
	static uint16_t fb[160 * 128];
	fb[160 * 5] = 0xF800;
	fb[160 * 5 + 1] = 0x0841;
	panel_clear();
	st7735_write_rows(&panel, fb, 5, 6);
	CHECK(pan.sessions == 1 && pan.total == 5 + 5 + 1 + 320);
	CHECK(pan.log[0] == 0x2A && pan.log_cmd[0] && pan.log[5] == 0x2B && pan.log_cmd[5]);
	CHECK(pan.log[7] == 5 && pan.log[9] == 5);	// rows 5..5
	CHECK(pan.log[10] == 0x2C && pan.log_cmd[10]);
	if (type == 3) {
		CHECK(pan.log[11] == 0x00 && pan.log[12] == 0x1F && pan.log[13] == 0x08 && pan.log[14] == 0x41);
	} else {
		CHECK(pan.log[11] == 0xF8 && pan.log[12] == 0x00 && pan.log[13] == 0x08 && pan.log[14] == 0x41);
	}
	CHECK(lock_depth == 0 && stm32_lcd8080_idle());
	CHECK(fake_fights == 0 && fake_float_reads == 0 && fake_primask == 0 && asserts == 0);
}

int main(void)
{
	static const stm32_lcd8080_cfg_t *const boards[] = { &board_a, &board_b };

	fake_watch = watch;
	test_before_init();
	for (uint32_t b = 0; b < 2; b++) {
		test_init(boards[b]);
		test_session(boards[b]);
		test_read(boards[b]);
		test_no_answer(boards[b]);
		test_outside(boards[b]);
		for (uint8_t type = 0; type < 4; type++) {
			test_driver(boards[b], type);
		}
	}
	if (failures == 0) {
		printf("lcd8080GpioTest: all tests passed\n");
	}
	return failures != 0;
}
