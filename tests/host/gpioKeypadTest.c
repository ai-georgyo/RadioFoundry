// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The key matrix on GPIO lines (platform/stm32/gpio_keypad) against a matrix of keys that join the lines they sit
// on: one line driven at a time and only ever low, the others released on their pull-ups, the read after the settle
// time, the lock held from the first line driven to the last one released, and the lines left as inputs whatever
// their other user had made of them
#define STM32GPIO_FAKE_MODEL
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "hal_time.h"
#include "gpio_keypad/gpio_keypad.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void hal_delay_us(uint32_t us)
{
	stm32_wait_ns(us * 1000u);
}

// ---- The matrix: a key down joins its drive line and its sense line ----

#define MAX_LINES	10u
#define LAG_NS		15000u	// a line follows what its net does this much later: its capacitance on a pull-up

static const stm32_gpio_keypad_cfg_t *cfg;
static bool down[MAX_LINES][MAX_LINES];		// [drive][sense]
static int lock_depth, locks;
static bool scanning;				// between the test's call and its return
static struct {
	bool target[2u * MAX_LINES], low[2u * MAX_LINES];	// pulled low by the net: as it will be, and as it is
	uint64_t t_change[2u * MAX_LINES];
	uint32_t driven_high, two_outputs, unlocked_outputs, not_released;
	uint32_t max_outputs;
} mx;

// The lines as one list: the drive lines, then the sense lines
static const stm32_kp_line_t *line(uint32_t i)
{
	return i < cfg->n_drive ? &cfg->drive[i] : &cfg->sense[i - cfg->n_drive];
}

static uint32_t n_lines(void)
{
	return (uint32_t)cfg->n_drive + cfg->n_sense;
}

static bool is_output(const stm32_kp_line_t *ln)
{
	return (fake_outputs(ln->port) & ln->pin) != 0;
}

// The lines joined to line i through the keys that are down, i included
static void net_of(uint32_t i, bool *in)
{
	memset(in, 0, 2u * MAX_LINES);
	in[i] = true;
	for (bool grew = true; grew;) {
		grew = false;
		for (uint32_t d = 0; d < cfg->n_drive; d++) {
			for (uint32_t s = 0; s < cfg->n_sense; s++) {
				uint32_t si = cfg->n_drive + s;
				if (down[d][s] && in[d] != in[si]) {
					in[d] = in[si] = true;
					grew = true;
				}
			}
		}
	}
}

static void watch(void)
{
	uint32_t outputs = 0;

	if (cfg == NULL) {
		return;
	}
	for (uint32_t i = 0; i < n_lines(); i++) {
		const stm32_kp_line_t *ln = line(i);
		if (is_output(ln)) {
			outputs++;
			if (ln->port->ODR & ln->pin) {
				mx.driven_high++;	// two keys could join it to another line's low
			}
			if (cfg->lock != NULL && lock_depth == 0) {
				mx.unlocked_outputs++;
			}
		}
	}
	if (outputs > 1u) {
		mx.two_outputs++;
	}
	if (outputs > mx.max_outputs) {
		mx.max_outputs = outputs;
	}
	for (uint32_t i = 0; outputs != 0u && i < n_lines(); i++) {
		// While a line drives, every other one is on its pull-up: another pull, left by the lines' other user,
		// would hold a line at a level of its own, or halfway through a key
		if (!is_output(line(i)) && (fake_pulled(line(i)->port, GPIO_PULLUP) & line(i)->pin) == 0) {
			mx.not_released++;
		}
	}
	if (!scanning) {
		// Between scans every line is an input on its pull-up: the lines' other user finds them free
		CHECK(outputs == 0);
		for (uint32_t i = 0; i < n_lines(); i++) {
			CHECK(fake_pulled(line(i)->port, GPIO_PULLUP) & line(i)->pin);
		}
	}
	for (uint32_t i = 0; i < n_lines(); i++) {
		const stm32_kp_line_t *ln = line(i);
		bool in[2u * MAX_LINES];
		bool pulled = false;
		net_of(i, in);
		for (uint32_t j = 0; j < n_lines(); j++) {
			pulled = pulled || (j != i && in[j] && is_output(line(j)) && (line(j)->port->ODR & line(j)->pin) == 0);
		}
		if (pulled != mx.target[i]) {
			mx.target[i] = pulled;
			mx.t_change[i] = fake_ns;
		}
		if (fake_ns - mx.t_change[i] >= LAG_NS) {
			mx.low[i] = pulled;
		}
		if (mx.low[i] && !is_output(ln)) {
			fake_drive(ln->port, ln->pin, 0);
		} else {
			fake_release(ln->port, ln->pin);
		}
	}
}

static void kp_lock(void)
{
	lock_depth++;
	locks++;
}

static void kp_unlock(void)
{
	fake_sync();	// the lines as the scan leaves them, still under the lock
	lock_depth--;
	fake_sync();	// and with the lock gone
}

static void keys_up(void)
{
	memset(down, 0, sizeof(down));
}

// What the matrix gives for the keys that are down: a sense line reads low with a drive line low when keys join the
// two, directly or through other lines (three keys in a square show the fourth)
static uint8_t expect_row(uint32_t d)
{
	bool in[2u * MAX_LINES];
	uint8_t low = 0;
	net_of(d, in);
	for (uint32_t s = 0; s < cfg->n_sense && s < 8u; s++) {
		low |= in[cfg->n_drive + s] ? (uint8_t)(1u << s) : 0u;
	}
	return low;
}

// One scan, with everything the watcher counts checked
static void scan(uint8_t *rows)
{
	uint64_t t0 = fake_ns;
	int locks0 = locks;
	uint32_t n = cfg->n_drive < 8u ? cfg->n_drive : 8u;

	memset(&mx, 0, sizeof(mx));
	scanning = true;
	stm32_gpio_keypad_scan(cfg, rows);
	scanning = false;
	fake_sync();
	CHECK(mx.driven_high == 0);
	CHECK(mx.two_outputs == 0 && mx.max_outputs == 1);
	CHECK(mx.unlocked_outputs == 0 && mx.not_released == 0);
	CHECK(lock_depth == 0);
	CHECK(locks == locks0 + (cfg->lock != NULL ? 1 : 0));
	CHECK(fake_ns - t0 == (uint64_t)n * cfg->settle_us * 1000u);	// one settle time a line, nothing else waits
	CHECK(fake_fights == 0 && fake_float_reads == 0);
}

// ---- A board: five drive lines on two ports, four sense lines ----

static const stm32_kp_line_t drive_a[] = {
	{ GPIOD, GPIO_PIN_4 }, { GPIOD, GPIO_PIN_5 }, { GPIOD, GPIO_PIN_6 }, { GPIOD, GPIO_PIN_7 }, { GPIOB, GPIO_PIN_7 },
};
static const stm32_kp_line_t sense_a[] = {
	{ GPIOD, GPIO_PIN_0 }, { GPIOD, GPIO_PIN_1 }, { GPIOD, GPIO_PIN_2 }, { GPIOD, GPIO_PIN_3 },
};
static const stm32_gpio_keypad_cfg_t board_a = {
	.drive = drive_a, .sense = sense_a, .n_drive = 5, .n_sense = 4, .settle_us = 20,
	.lock = kp_lock, .unlock = kp_unlock,
};
// The same lines with nobody to share them with
static const stm32_gpio_keypad_cfg_t board_nolock = {
	.drive = drive_a, .sense = sense_a, .n_drive = 5, .n_sense = 4, .settle_us = 20,
};
// A settle time under what the lines need
static const stm32_gpio_keypad_cfg_t board_fast = {
	.drive = drive_a, .sense = sense_a, .n_drive = 5, .n_sense = 4, .settle_us = 5,
	.lock = kp_lock, .unlock = kp_unlock,
};

// Every register of every port as it is, for the "nothing else was touched" checks
typedef struct {
	uint32_t moder[FAKE_PORTS], pupdr[FAKE_PORTS], odr[FAKE_PORTS], otyper[FAKE_PORTS];
} regs_t;

static void regs_get(regs_t *r, bool matrix_too)
{
	for (uint32_t i = 0; i < FAKE_PORTS; i++) {
		uint32_t pins = 0;
		for (uint32_t j = 0; !matrix_too && j < n_lines(); j++) {
			pins |= line(j)->port == &fake_ports[i] ? line(j)->pin : 0u;
		}
		uint32_t fields = 0;
		for (uint32_t n = 0; n < 16u; n++) {
			fields |= (pins & (1u << n)) ? 3u << (2u * n) : 0u;
		}
		r->moder[i] = fake_ports[i].MODER & ~fields;
		r->pupdr[i] = fake_ports[i].PUPDR & ~fields;
		r->odr[i] = fake_ports[i].ODR & ~pins;
		r->otyper[i] = fake_ports[i].OTYPER;
	}
}

// The ports in a state with something in every register: outputs and pulls on the pins that are not the matrix's
static void board_start(const stm32_gpio_keypad_cfg_t *c)
{
	cfg = NULL;
	fake_reset();
	keys_up();
	fake_ports[1].MODER = 0x00041000u;	// PB6 and PB9 outputs
	fake_ports[1].ODR = 0x0200u;
	fake_ports[1].PUPDR = 0x00000002u;
	fake_ports[3].MODER = 0x55000000u;	// PD12..PD15 outputs
	fake_ports[3].ODR = 0xA000u;
	fake_ports[3].PUPDR = 0x00AA0000u;
	fake_sync();
	cfg = c;
	scanning = true;	// not ours yet: the init makes the lines inputs
	stm32_gpio_keypad_init(c);
	scanning = false;
	fake_sync();
}

static void test_init(void)
{
	regs_t before, after;

	cfg = &board_a;
	fake_watch = NULL;
	fake_reset();
	// The lines as something else left them: pulled down, ones in the output register
	fake_ports[3].PUPDR = 0x0000AAAAu;
	fake_ports[3].ODR = 0x00FFu;
	fake_ports[1].PUPDR = 0x00008000u;
	fake_ports[1].ODR = 0x0080u;
	regs_get(&before, false);
	stm32_gpio_keypad_init(&board_a);
	regs_get(&after, false);
	CHECK(memcmp(&before, &after, sizeof(before)) == 0);
	for (uint32_t i = 0; i < n_lines(); i++) {
		CHECK(!is_output(line(i)));
		CHECK(fake_pulled(line(i)->port, GPIO_PULLUP) & line(i)->pin);
		CHECK(fake_level(line(i)->port, line(i)->pin));
	}
	CHECK(locks == 0);	// the init is the board's to place: not under the lock
	fake_watch = watch;
}

static void test_scan(const stm32_gpio_keypad_cfg_t *c)
{
	uint8_t rows[MAX_LINES];
	regs_t before, after;

	board_start(c);
	regs_get(&before, true);
	memset(rows, 0xEE, sizeof(rows));
	scan(rows);
	for (uint32_t d = 0; d < c->n_drive; d++) {
		CHECK(rows[d] == 0);
	}
	CHECK(rows[c->n_drive] == 0xEE);
	// Each key alone: its own bit in its own row, and nothing else
	for (uint32_t d = 0; d < c->n_drive; d++) {
		for (uint32_t s = 0; s < c->n_sense; s++) {
			keys_up();
			down[d][s] = true;
			scan(rows);
			for (uint32_t r = 0; r < c->n_drive; r++) {
				CHECK(rows[r] == (r == d ? 1u << s : 0u));
			}
		}
	}
	// Keys together, also the ones that join lines into one net (two in a row, two in a column, three in a square)
	static const uint8_t sets[][4][2] = {
		{ { 0, 0 }, { 0, 3 }, { 0, 0 }, { 0, 0 } },
		{ { 1, 2 }, { 4, 2 }, { 1, 2 }, { 1, 2 } },
		{ { 0, 0 }, { 0, 1 }, { 1, 0 }, { 0, 0 } },
		{ { 0, 0 }, { 1, 1 }, { 2, 2 }, { 4, 3 } },
		{ { 3, 0 }, { 3, 1 }, { 3, 2 }, { 3, 3 } },
	};
	for (uint32_t k = 0; k < sizeof(sets) / sizeof(sets[0]); k++) {
		keys_up();
		for (uint32_t j = 0; j < 4; j++) {
			down[sets[k][j][0]][sets[k][j][1]] = true;
		}
		scan(rows);
		for (uint32_t d = 0; d < c->n_drive; d++) {
			CHECK(rows[d] == expect_row(d));
		}
		if (k == 2) {
			CHECK(rows[0] == 0x03 && rows[1] == 0x03);	// the square's fourth corner shows, as on any matrix
		}
	}
	// Every key down: every line in one net, and still one output at a time and never a high one
	for (uint32_t d = 0; d < c->n_drive; d++) {
		for (uint32_t s = 0; s < c->n_sense; s++) {
			down[d][s] = true;
		}
	}
	scan(rows);
	for (uint32_t d = 0; d < c->n_drive; d++) {
		CHECK(rows[d] == 0x0F);
	}
	// Nothing but the lines themselves was written, and they are as the init left them
	regs_get(&after, true);
	CHECK(memcmp(&before, &after, sizeof(before)) == 0);
	CHECK(fake_primask == 0);
}

// The lines come back from their other user: pulled down, the output register full of ones (a display's last byte)
static void test_shared(void)
{
	uint8_t rows[MAX_LINES];

	board_start(&board_a);
	down[2][1] = true;
	down[4][3] = true;
	for (int round = 0; round < 3; round++) {
		scanning = true;	// the other user's time on the lines
		for (uint32_t i = 0; i < n_lines(); i++) {
			stm32_pin_in(line(i)->port, line(i)->pin, round == 1 ? GPIO_NOPULL : GPIO_PULLDOWN);
			line(i)->port->ODR |= line(i)->pin;
		}
		scanning = false;
		scan(rows);
		for (uint32_t d = 0; d < board_a.n_drive; d++) {
			CHECK(rows[d] == expect_row(d));
		}
		CHECK(rows[2] == 0x02 && rows[4] == 0x08);
		for (uint32_t i = 0; i < n_lines(); i++) {
			CHECK(!is_output(line(i)));
			CHECK(fake_pulled(line(i)->port, GPIO_PULLUP) & line(i)->pin);
		}
	}
	// Called with interrupts off already: they stay off
	fake_primask = 1;
	scan(rows);
	CHECK(fake_primask == 1);
	fake_primask = 0;
}

// The read waits for the settle time: with less than the lines need, a key is not seen yet, or the row before it
// still shows
static void test_settle(void)
{
	uint8_t rows[MAX_LINES];

	board_start(&board_fast);
	down[1][2] = true;
	scan(rows);
	CHECK(rows[1] == 0);
	board_start(&board_a);
	down[1][2] = true;
	scan(rows);
	CHECK(rows[1] == 0x04 && rows[2] == 0);
}

// More lines than a row's byte has bits: the first eight of each, the others left alone
static void test_wide(void)
{
	static const stm32_kp_line_t drive_w[] = {
		{ GPIOA, GPIO_PIN_0 }, { GPIOA, GPIO_PIN_1 }, { GPIOA, GPIO_PIN_2 }, { GPIOA, GPIO_PIN_3 }, { GPIOA, GPIO_PIN_4 },
		{ GPIOA, GPIO_PIN_5 }, { GPIOA, GPIO_PIN_6 }, { GPIOA, GPIO_PIN_7 }, { GPIOA, GPIO_PIN_8 }, { GPIOA, GPIO_PIN_9 },
	};
	static const stm32_kp_line_t sense_w[] = {
		{ GPIOC, GPIO_PIN_0 }, { GPIOC, GPIO_PIN_1 }, { GPIOC, GPIO_PIN_2 }, { GPIOC, GPIO_PIN_3 }, { GPIOC, GPIO_PIN_4 },
		{ GPIOC, GPIO_PIN_5 }, { GPIOC, GPIO_PIN_6 }, { GPIOC, GPIO_PIN_7 }, { GPIOC, GPIO_PIN_8 },
	};
	static const stm32_gpio_keypad_cfg_t board_w = {
		.drive = drive_w, .sense = sense_w, .n_drive = 10, .n_sense = 9, .settle_us = 20,
		.lock = kp_lock, .unlock = kp_unlock,
	};
	static const stm32_gpio_keypad_cfg_t board_8 = {
		.drive = drive_w, .sense = sense_w, .n_drive = 8, .n_sense = 8, .settle_us = 20,
		.lock = kp_lock, .unlock = kp_unlock,
	};
	uint8_t rows[MAX_LINES + 1u];

	cfg = NULL;
	fake_reset();
	keys_up();
	stm32_gpio_keypad_init(&board_w);
	CHECK(fake_ports[0].PUPDR == 0x00005555u && fake_ports[2].PUPDR == 0x00005555u);	// eight lines each
	cfg = &board_8;		// the matrix the scan may touch
	down[7][7] = true;
	down[3][0] = true;
	memset(rows, 0xEE, sizeof(rows));
	scanning = true;
	stm32_gpio_keypad_scan(&board_w, rows);
	scanning = false;
	fake_sync();
	CHECK(rows[7] == 0x80 && rows[3] == 0x01 && rows[0] == 0);
	CHECK(rows[8] == 0xEE && rows[9] == 0xEE && rows[10] == 0xEE);
	CHECK(fake_ports[0].MODER == 0 && fake_ports[0].PUPDR == 0x00005555u);
	CHECK(fake_ports[2].MODER == 0 && fake_ports[2].PUPDR == 0x00005555u);
	CHECK(mx.driven_high == 0 && mx.two_outputs == 0 && mx.not_released == 0 && fake_fights == 0);
}

int main(void)
{
	test_init();
	test_scan(&board_a);
	test_scan(&board_nolock);
	test_shared();
	test_settle();
	test_wide();
	if (failures == 0) {
		printf("gpioKeypadTest: all tests passed\n");
	}
	return failures != 0;
}
