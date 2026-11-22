// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS's display rules (panel_cs7000p.h): the LCD board check over all 32 patterns of its five lines
// (only the reset line low passes), the panel type from the controller's ID with the unknown-ID fall-back and the
// build override, the backlight's duty window, and the display driver's init (drivers/display/st7735) on a
// recording bus with the callbacks in the order display_cs7000p.c gives them: reset ends high, nothing reaches the
// bus outside a session, and no pause is spent inside one.
//
// Where the build gives it platform/stm32 on the include path (and input_cs7000p.c with its decoders to link), the
// board's own display_cs7000p.c and input_cs7000p.c then run on a fake bus, key scan, timer and pins: nothing is
// driven before the check's two readings, and nothing at all on a board that fails them.
#define CONFIG_BACKLIGHT_TEST	1	// the display file's raw backlight hooks too (before config.h is read)
#include <stdio.h>
#include <string.h>
#include "panel_cs7000p.h"
#include "display/st7735/st7735.h"

#if defined(__has_include)
#if __has_include("lcd8080_gpio/lcd8080_gpio.h") && __has_include("gpio_keypad/gpio_keypad.h")
#define WITH_BOARD_FILES	1
#endif
#endif

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static bool is(const char *a, const char *b)
{
	return a != NULL && strcmp(a, b) == 0;
}

// ---- The LCD board check ----

static void test_board_check(void)
{
	// The byte's layout, written out: port E's two lines, then port D's three
	CHECK(cs7000p_lcd_levels(true, false, false, false, false) == 0x01);	// PE0, reset
	CHECK(cs7000p_lcd_levels(false, true, false, false, false) == 0x02);	// PE1, chip select
	CHECK(cs7000p_lcd_levels(false, false, true, false, false) == 0x04);	// PD12, data / command
	CHECK(cs7000p_lcd_levels(false, false, false, true, false) == 0x08);	// PD13, write strobe
	CHECK(cs7000p_lcd_levels(false, false, false, false, true) == 0x10);	// PD14, read strobe
	CHECK(cs7000p_lcd_levels(true, true, true, true, true) == 0x1F);

	// The assumed board: 10k to ground on the reset line alone, so with the pins' pull-ups only PE0 reads low
	CHECK(CS7000P_LCD_LEVELS_ASSUMED == 0x1E);
	CHECK(cs7000p_lcd_levels(false, true, true, true, true) == CS7000P_LCD_LEVELS_ASSUMED);
	CHECK(cs7000p_lcd_board_ok(0x1E));

	// Each of the other 31 patterns is another board
	unsigned passed = 0;
	for (unsigned levels = 0; levels < 32; levels++) {
		bool ok = cs7000p_lcd_board_ok((uint8_t)levels);
		CHECK(ok == (levels == 0x1E));
		passed += ok ? 1u : 0u;
		// The verdict: assumed only for the one pattern, read twice
		uint8_t v = cs7000p_lcd_board((uint8_t)levels, (uint8_t)levels, false);
		CHECK(v == (levels == 0x1E ? CS7000P_LCD_ASSUMED : CS7000P_LCD_UNEXPECTED));
		CHECK(cs7000p_lcd_may_drive(v) == (levels == 0x1E));
		// A line that reads differently the second time proves nothing, whichever reading had the pattern
		CHECK(cs7000p_lcd_board(0x1E, (uint8_t)levels, false) == v);
		CHECK(cs7000p_lcd_board((uint8_t)levels, 0x1E, false) == v);
		// The bench knob overrules every pattern, and says so
		CHECK(cs7000p_lcd_board((uint8_t)levels, (uint8_t)levels, true) == CS7000P_LCD_FORCED);
	}
	CHECK(passed == 1);
	// No connector at all (every line on its pull-up), every line grounded, a reading with bits no line has
	CHECK(!cs7000p_lcd_board_ok(0x1F) && !cs7000p_lcd_board_ok(0x00));
	CHECK(!cs7000p_lcd_board_ok(0x3E) && !cs7000p_lcd_board_ok(0xFE) && !cs7000p_lcd_board_ok(0x9E));

	// The states as cs7000p.h numbers them for the views, and what each allows. Unexpected is also the state before
	// the check has run (the display file's initial value): nothing may be driven in it
	CHECK(CS7000P_LCD_ASSUMED == 0 && CS7000P_LCD_UNEXPECTED == 1 && CS7000P_LCD_FORCED == 2);
	CHECK(cs7000p_lcd_may_drive(CS7000P_LCD_ASSUMED) && cs7000p_lcd_may_drive(CS7000P_LCD_FORCED));
	CHECK(!cs7000p_lcd_may_drive(CS7000P_LCD_UNEXPECTED));
	CHECK(!cs7000p_lcd_may_drive(3) && !cs7000p_lcd_may_drive(0xFF));
	// As shipped the check is not skipped
	CHECK(CS7000P_LCD_FORCE == 0);
}

// ---- The panel type ----

static const uint8_t ID_GC9XXX[3] = { 0x00, 0x91, 0x02 };
static const uint8_t ID_ST7735[3] = { 0x7C, 0x89, 0xF0 };
static const uint8_t ID_ZERO[3] = { 0x00, 0x00, 0x00 };		// not read, or a bus that answers nothing
static const uint8_t ID_ONES[3] = { 0xFF, 0xFF, 0xFF };		// floating lines on their pull-ups
static const uint8_t ID_OTHER[3] = { 0x5C, 0x89, 0xF0 };	// some controller the driver has no entry for

static void test_panel_choice(void)
{
	cs7000p_panel_t p;

	// The two IDs the display driver knows
	p = cs7000p_panel_choose(ID_GC9XXX, -1);
	CHECK(p.type == 3 && is(p.controller, "gc9xxx") && is(p.source, "id"));
	p = cs7000p_panel_choose(ID_ST7735, -1);
	CHECK(p.type == 0 && is(p.controller, "st7735") && is(p.source, "id"));
	// Only the bytes that name the controller count
	p = cs7000p_panel_choose((const uint8_t[3]){ 0x00, 0x91, 0x06 }, -1);
	CHECK(p.type == 3 && is(p.controller, "gc9xxx") && is(p.source, "id"));
	p = cs7000p_panel_choose((const uint8_t[3]){ 0x7C, 0x00, 0x00 }, -1);
	CHECK(p.type == 0 && is(p.controller, "st7735") && is(p.source, "id"));

	// The fall-back: any other ID, all zeros and all ones included, is the driver's type 0, and the report says
	// that nothing was recognized
	const uint8_t *unknown[] = { ID_ZERO, ID_ONES, ID_OTHER };
	for (unsigned i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
		p = cs7000p_panel_choose(unknown[i], -1);
		CHECK(p.type == 0 && is(p.controller, "unknown") && is(p.source, "default"));
	}
	// No type is stored on this radio: "factory" is never the source
	for (unsigned b = 0; b < 256; b++) {
		const uint8_t id[3] = { (uint8_t)b, 0x91, 0x00 };
		p = cs7000p_panel_choose(id, -1);
		CHECK(is(p.source, "id") || is(p.source, "default"));
		CHECK(p.type <= 3);
	}

	// The build override: its type whatever the ID says, reported as "build"; the controller is still the ID's
	for (int16_t t = 0; t <= 3; t++) {
		p = cs7000p_panel_choose(ID_GC9XXX, t);
		CHECK(p.type == t && is(p.source, "build") && is(p.controller, "gc9xxx"));
		p = cs7000p_panel_choose(ID_ST7735, t);
		CHECK(p.type == t && is(p.source, "build") && is(p.controller, "st7735"));
		p = cs7000p_panel_choose(ID_ONES, t);
		CHECK(p.type == t && is(p.source, "build") && is(p.controller, "unknown"));
	}
	// As shipped there is none, and the orientation is the type's own
	CHECK(CS7000P_PANEL_BUILD_TYPE == -1);
	CHECK(CS7000P_PANEL_MADCTL_XOR == 0);
}

// ---- The backlight ----

static void test_backlight(void)
{
	const uint16_t period = 2400;	// 96 MHz / 40 kHz

	CHECK(CS7000P_BL_MIN == 20 && CS7000P_BL_MAX == 70);
	// Off is the pin low; 1 % and 100 % are the window's two ends
	CHECK(cs7000p_backlight_duty(0, period) == 0);
	CHECK(cs7000p_backlight_duty(1, period) == 480);	// 20 %
	CHECK(cs7000p_backlight_duty(100, period) == 1680);	// 70 %
	CHECK(cs7000p_backlight_duty(255, period) == 1680);
	// The core's steps of ten land evenly in between, and no step is dark
	uint16_t last = 0;
	for (unsigned pct = 1; pct <= 100; pct++) {
		uint16_t d = cs7000p_backlight_duty((uint8_t)pct, period);
		CHECK(d >= last && d >= 480 && d <= 1680);
		last = d;
	}
	CHECK(cs7000p_backlight_duty(10, period) > cs7000p_backlight_duty(1, period));
	CHECK(cs7000p_backlight_duty(50, period) == (uint16_t)(period * (20u * 99u + 49u * 50u) / 9900u));
	// Another timer clock (the fallback clock trees): the same duty of its own period, and no overflow at the
	// longest period a 16-bit timer has
	CHECK(cs7000p_backlight_duty(1, 800) == 160 && cs7000p_backlight_duty(100, 800) == 560);
	CHECK(cs7000p_backlight_duty(100, 65535) == (uint16_t)(65535u * 70u / 100u));
	CHECK(cs7000p_backlight_duty(100, 65535) < 65535);

	// The backlight tests' raw value: 0 is off, the maximum is on through the whole period, one step is one timer
	// clock at the usual clock
	CHECK(CS7000P_BL_RAW_MAX == 2400);
	CHECK(cs7000p_backlight_raw_duty(0, period) == 0);
	CHECK(cs7000p_backlight_raw_duty(1, period) == 1 && cs7000p_backlight_raw_duty(2399, period) == 2399);
	CHECK(cs7000p_backlight_raw_duty(2400, period) == period);
	CHECK(cs7000p_backlight_raw_duty(0xFFFF, period) == period);
	CHECK(cs7000p_backlight_raw_duty(1200, 800) == 400 && cs7000p_backlight_raw_duty(2400, 800) == 800);
	CHECK(cs7000p_backlight_raw_duty(2399, 65535) < 65535 && cs7000p_backlight_raw_duty(2400, 65535) == 65535);
}

// ---- The display driver's init on a recording bus ----

enum { EV_BEGIN, EV_END, EV_CMD, EV_DATA, EV_PIXELS, EV_RESET, EV_DELAY };

#define REC_MAX		4096u

static struct {
	uint8_t kind[REC_MAX];
	uint32_t arg[REC_MAX];		// the command, first data byte, pixel count, level or milliseconds
	uint32_t n, overflow;
	bool open, res, res_set;
	uint32_t outside;		// transfers with no session open
	uint32_t nested;		// a session opened inside one, or closed twice
	uint32_t delay_inside;		// pauses spent holding the bus
	uint32_t reset_inside;		// reset moved with the panel selected
	uint32_t res_falls, res_rises;
	uint32_t pixels, sessions;
	uint8_t last_cmd;
	uint8_t madctl, colmod;
	bool madctl_seen, colmod_seen;
} rec;

static void record(uint8_t kind, uint32_t arg)
{
	if (rec.n < REC_MAX) {
		rec.kind[rec.n] = kind;
		rec.arg[rec.n] = arg;
		rec.n++;
	} else {
		rec.overflow++;
	}
}

static void r_begin(void)
{
	if (rec.open) {
		rec.nested++;
	}
	rec.open = true;
	rec.sessions++;
	record(EV_BEGIN, 0);
}

static void r_end(void)
{
	if (!rec.open) {
		rec.nested++;
	}
	rec.open = false;
	record(EV_END, 0);
}

static void r_cmd(uint8_t c)
{
	if (!rec.open) {
		rec.outside++;
	}
	rec.last_cmd = c;
	record(EV_CMD, c);
}

static void r_data(const uint8_t *d, uint32_t n)
{
	if (!rec.open) {
		rec.outside++;
	}
	if (n > 0 && rec.last_cmd == 0x36) {
		rec.madctl = d[0];
		rec.madctl_seen = true;
	}
	if (n > 0 && rec.last_cmd == 0x3A) {
		rec.colmod = d[0];
		rec.colmod_seen = true;
	}
	record(EV_DATA, n > 0 ? d[0] : 0);
}

static void r_pixels(const uint16_t *px, uint32_t n, bool swap_rb)
{
	(void)px;
	(void)swap_rb;
	if (!rec.open) {
		rec.outside++;
	}
	rec.pixels += n;
	record(EV_PIXELS, n);
}

static void r_reset(bool level)
{
	if (rec.open) {
		rec.reset_inside++;
	}
	if (rec.res_set && rec.res && !level) {
		rec.res_falls++;
	}
	if (rec.res_set && !rec.res && level) {
		rec.res_rises++;
	}
	rec.res = level;
	rec.res_set = true;
	record(EV_RESET, level);
}

static void r_delay_ms(uint32_t ms)
{
	if (rec.open) {
		rec.delay_inside++;
	}
	record(EV_DELAY, ms);
}

// The callbacks in the order display_cs7000p.c's table has them: begin, end, cmd, data, pixels, reset, delay_ms
static const st7735_bus_t rec_bus = {
	.begin = r_begin,
	.end = r_end,
	.cmd = r_cmd,
	.data = r_data,
	.pixels = r_pixels,
	.reset = r_reset,
	.delay_ms = r_delay_ms,
};

// The last recorded event of a kind, -1 for none
static int last_of(uint8_t kind)
{
	for (int i = (int)rec.n - 1; i >= 0; i--) {
		if (rec.kind[i] == kind) {
			return i;
		}
	}
	return -1;
}

static void test_driver_init(void)
{
	static const uint8_t madctl_of_type[4] = { 0xA0, 0x60, 0xE0, 0xA0 };	// the driver's, landscape

	// Every type the choice can give, plain and with the orientation override a build can add
	for (unsigned t = 0; t < 4; t++) {
		for (unsigned x = 0; x < 2; x++) {
			// The board's panel: 160 x 128, no offsets, the driver's own color order for the type
			const st7735_t pnl = {
				.bus = &rec_bus, .type = (uint8_t)t, .width = 160, .height = 128,
				.madctl_xor = x ? 0xC0 : CS7000P_PANEL_MADCTL_XOR,
			};

			memset(&rec, 0, sizeof(rec));
			// As the bus's set-up leaves the line: low, the panel in reset
			rec.res = false;
			rec.res_set = true;
			st7735_init(&pnl);

			CHECK(rec.overflow == 0);
			// Reset ends high, after exactly one low pulse of the driver's own, moved only with the panel
			// deselected
			CHECK(rec.res);
			CHECK(rec.res_falls == 1 && rec.res_rises == 2);
			CHECK(rec.reset_inside == 0);
			CHECK(rec.n > 0 && rec.kind[0] == EV_RESET && rec.arg[0] == 1);
			int last_reset = last_of(EV_RESET);
			CHECK(last_reset >= 0 && rec.arg[last_reset] == 1);
			// The first session opens only after the last reset edge and a pause behind it
			unsigned first_begin = 0;
			while (first_begin < rec.n && rec.kind[first_begin] != EV_BEGIN) {
				first_begin++;
			}
			CHECK((int)first_begin > last_reset + 1 && rec.kind[last_reset + 1] == EV_DELAY);
			// Every transfer inside a session, the sessions balanced, and no pause with the bus held:
			// the key matrix shares the lines and its scan waits for the lock
			CHECK(rec.outside == 0 && rec.nested == 0 && !rec.open);
			CHECK(rec.delay_inside == 0);
			CHECK(rec.sessions >= 2);
			// The orientation is the type's, with the override on top; 16 bits a pixel
			CHECK(rec.madctl_seen && rec.madctl == (uint8_t)(madctl_of_type[t] ^ pnl.madctl_xor));
			CHECK(rec.colmod_seen && rec.colmod == 0x05);
			// The whole panel cleared before it is switched on, and display on is the last command
			CHECK(rec.pixels == 160u * 128u);
			int last_cmd = last_of(EV_CMD);
			CHECK(last_cmd >= 0 && rec.arg[last_cmd] == 0x29);
			CHECK(last_of(EV_PIXELS) < last_cmd);
			CHECK(rec.kind[rec.n - 1] == EV_END);
		}
	}

	// A frame after it: one session, the window, the pixels of the rows asked for
	static uint8_t fb[128 * 80];
	static const uint16_t palette[16] = { 0 };
	const st7735_t pnl = { .bus = &rec_bus, .type = 0, .width = 160, .height = 128 };
	memset(&rec, 0, sizeof(rec));
	st7735_write_rows_indexed(&pnl, fb, palette, 0, 128);
	CHECK(rec.sessions == 1 && !rec.open && rec.outside == 0 && rec.pixels == 160u * 128u);
	CHECK(rec.n > 0 && rec.kind[0] == EV_BEGIN && rec.kind[rec.n - 1] == EV_END);
	memset(&rec, 0, sizeof(rec));
	st7735_write_rows_indexed(&pnl, fb, palette, 10, 12);
	CHECK(rec.sessions == 1 && rec.pixels == 160u * 2u);
}

// ---- The board's display and input files on a fake world ----

#ifdef WITH_BOARD_FILES
#include "platform.h"	// the stand-in (tests/stub): ports as plain memory, the pin functions defined below

// What periph/pwm.h takes from the real platform.h: one timer with its four compare registers
typedef struct { uint32_t CCR1, CCR2, CCR3, CCR4; } TIM_TypeDef;
static TIM_TypeDef fake_tim8;
#define TIM8		(&fake_tim8)
#define GPIO_AF3_TIM8	3u

#include "../display_cs7000p.c"
#include "gpio_keypad/gpio_keypad.h"
#include "input/plugptt/plug_ptt.h"
#include "input/input_engine.h"

GPIO_TypeDef fake_ports[5];
int fake_irq_depth;		// the stub FreeRTOS.h's mask: the backlight's level and its lamp change under one

static struct {
	uint16_t level[5];		// what each port's pins read
	uint32_t seq;			// counts every call below, to tell before from after
	uint32_t ctl_reads, last_ctl_read;	// readings of the five control lines, and when the last one was
	uint32_t first_drive;		// when the first call that drives or sets up a driver came; 0: none yet
	uint32_t drive_calls;		// stm32_pin_out / _force / _af: the board files call none themselves
	uint16_t in_up[5];		// pins made inputs with pull-ups
	uint32_t in_other;		// inputs with another pull, analog
	uint32_t bus_inits, bus_calls, sessions, outside, reads, reads_key_down, pixels;
	const stm32_lcd8080_cfg_t *cfg;
	bool open, res;
	uint32_t bus_init_ms;		// when the bus was set up: reset is low from then on
	uint32_t res_high_ms;		// when reset was last released before the first read
	uint32_t first_read_ms;
	uint8_t first_cmd, last_cmd, madctl;
	uint32_t cmds;
	uint32_t kp_inits, kp_inits_unlocked, kp_scans, kp_scans_in_session, kp_scans_unlocked;
	const stm32_gpio_keypad_cfg_t *kp;
	uint8_t key_rows[5];
	uint32_t keys_up_after;		// scans after which the keys come up; 0: they stay as they are
	uint32_t pwm_inits;
	uint16_t pwm_period;
	uint32_t timer_hz;
	uint32_t exp, exp_writes;
	uint32_t exp_unmasked;		// expander writes made outside the backlight's masked section
	int lock_depth;
	bool other_session;		// the open session is another task's: taking the lock waits for its end
	uint32_t lock_waits;
	uint32_t delays, delays_in_session, delays_locked;
	uint32_t ms;
	uint8_t id[3];
	uint32_t flip_at_delay;		// at this pause of an init the control lines change to flip_levels
	uint8_t flip_levels;
	uint32_t logs;
} w;

static unsigned port_of(const GPIO_TypeDef *port)
{
	return (unsigned)(port - fake_ports);
}

static void drives(void)
{
	if (w.first_drive == 0) {
		w.first_drive = w.seq;
	}
}

// The five control lines from a levels byte (panel_cs7000p.h's layout: PE0, PE1, PD12, PD13, PD14)
static void set_ctl(uint8_t levels)
{
	w.level[4] = (uint16_t)((w.level[4] & ~0x0003u) | (levels & 0x03u));
	w.level[3] = (uint16_t)((w.level[3] & ~0x7000u) | ((uint32_t)(levels & 0x1Cu) << 10));
}

void stm32_pin_in(GPIO_TypeDef *port, uint16_t pins, uint32_t pull)
{
	w.seq++;
	if (pull == GPIO_PULLUP) {
		w.in_up[port_of(port)] |= pins;
	} else {
		w.in_other++;
	}
}

void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool open_drain)
{
	w.seq++;
	w.drive_calls++;
	drives();
}

void stm32_pin_force(GPIO_TypeDef *port, uint16_t pins, bool level)
{
	w.seq++;
	w.drive_calls++;
	drives();
}

void stm32_pin_af(GPIO_TypeDef *port, uint16_t pins, uint8_t af, bool open_drain, uint32_t pull)
{
	w.seq++;
	w.drive_calls++;
	drives();
}

void stm32_pin_analog(GPIO_TypeDef *port, uint16_t pins)
{
	w.seq++;
	w.in_other++;
}

bool stm32_pin_read(GPIO_TypeDef *port, uint16_t pin)
{
	unsigned p = port_of(port);
	w.seq++;
	if ((p == 4 && (pin & 0x0003u) != 0) || (p == 3 && (pin & 0x7000u) != 0)) {
		w.ctl_reads++;
		w.last_ctl_read = w.seq;
	}
	return (w.level[p] & pin) != 0;
}

uint32_t hal_millis(void)
{
	return w.ms;
}

void hal_delay_ms(uint32_t ms)
{
	w.seq++;
	w.ms += ms;
	w.delays++;
	w.delays_in_session += w.open ? 1u : 0u;
	w.delays_locked += w.lock_depth != 0 ? 1u : 0u;
	if (w.flip_at_delay != 0 && w.delays == w.flip_at_delay) {
		set_ctl(w.flip_levels);
	}
}

#if CONFIG_DEBUGLINK
void dlog(const char *fmt, ...)
{
	(void)fmt;
	w.logs++;
}
#endif

// A lock taken while another task's session holds it returns once that session has ended
void cs7000p_lcdbus_lock(void)
{
	if (w.other_session) {
		w.other_session = false;
		w.open = false;
		w.lock_waits++;
	}
	w.lock_depth++;
}

void cs7000p_lcdbus_unlock(void)
{
	w.lock_depth--;
}

bool cs7000p_exp_set(uint32_t set, uint32_t clear)
{
	w.seq++;
	drives();
	w.exp = (w.exp | set) & ~clear;
	w.exp_writes++;
	w.exp_unmasked += fake_irq_depth == 0;
	return true;
}

void stm32_pwm_init(TIM_TypeDef *tim, uint8_t channel, GPIO_TypeDef *port, uint16_t pin, uint8_t af, uint16_t period,
		    uint16_t duty)
{
	// Not a drive: the pin was a low output since the pin set-up, and duty 0 keeps it low. It is board_early_init's,
	// before any task: never the display's init (that runs in a task, with port C's other writers about)
	w.seq++;
	w.pwm_inits++;
	w.pwm_period = period;
	CHECK(tim == TIM8 && channel == 4 && port == GPIOC && pin == GPIO_PIN_9 && af == GPIO_AF3_TIM8);
	CHECK(duty == 0);	// dark until the core asks for light
	CHECK(w.ctl_reads == 0 && w.bus_inits == 0);
	tim->CCR4 = duty;
}

uint32_t stm32_pwm_timer_hz(TIM_TypeDef *tim)
{
	return w.timer_hz;
}

// The bus (platform/stm32/lcd8080_gpio). The real one ignores every call before its init; here such a call is
// counted, because the display file must not even try
static bool bus_up(void)
{
	w.seq++;
	w.bus_calls++;
	if (w.cfg == NULL) {
		w.outside++;
		return false;
	}
	return true;
}

void stm32_lcd8080_init(const stm32_lcd8080_cfg_t *c)
{
	w.seq++;
	drives();
	w.bus_inits++;
	w.cfg = c;
	w.res = false;	// the panel in reset
	w.open = false;
	w.bus_init_ms = w.ms;
}

void stm32_lcd8080_begin(void)
{
	if (bus_up()) {
		w.cfg->lock();
		w.outside += w.open ? 1u : 0u;
		w.open = true;
		w.sessions++;
	}
}

void stm32_lcd8080_end(void)
{
	if (bus_up()) {
		w.outside += w.open ? 0u : 1u;
		w.open = false;
		w.cfg->unlock();
	}
}

void stm32_lcd8080_cmd(uint8_t c)
{
	if (bus_up()) {
		w.outside += w.open ? 0u : 1u;
		if (w.cmds++ == 0) {
			w.first_cmd = c;
		}
		w.last_cmd = c;
	}
}

void stm32_lcd8080_data(const uint8_t *d, uint32_t n)
{
	if (bus_up()) {
		w.outside += w.open ? 0u : 1u;
		if (n > 0 && w.last_cmd == 0x36) {
			w.madctl = d[0];
		}
	}
}

void stm32_lcd8080_pixels(const uint16_t *px, uint32_t n, bool swap_rb)
{
	if (bus_up()) {
		w.outside += w.open ? 0u : 1u;
		w.pixels += n;
	}
}

void stm32_lcd8080_reset(bool level)
{
	if (bus_up()) {
		if (level && !w.res && w.reads == 0) {
			w.res_high_ms = w.ms;
		}
		w.res = level;
	}
}

static bool key_down(void)
{
	return (w.key_rows[0] | w.key_rows[1] | w.key_rows[2] | w.key_rows[3] | w.key_rows[4]) != 0;
}

void stm32_lcd8080_read(uint8_t cmd, uint8_t *buf, uint8_t n)
{
	memset(buf, 0, n);
	if (!bus_up()) {
		return;
	}
	w.outside += w.open && w.res ? 0u : 1u;	// in a session, and out of reset
	if (w.reads == 0) {
		w.first_read_ms = w.ms;
	}
	w.reads++;
	w.reads_key_down += key_down() ? 1u : 0u;
	buf[0] = 0xEE;	// the controller's dummy byte: never part of an ID
	if (cmd == 0x04 && n >= 4) {
		memcpy(&buf[1], w.id, 3);
	} else if (cmd >= 0xDA && cmd <= 0xDC && n >= 2) {
		buf[1] = w.id[cmd - 0xDA];
	}
}

bool stm32_lcd8080_idle(void)
{
	return !w.open;
}

// The key scan (platform/stm32/gpio_keypad)
void stm32_gpio_keypad_init(const stm32_gpio_keypad_cfg_t *c)
{
	w.seq++;
	drives();
	w.kp_inits++;
	w.kp_inits_unlocked += w.lock_depth == 0 ? 1u : 0u;
	w.kp = c;
}

void stm32_gpio_keypad_scan(const stm32_gpio_keypad_cfg_t *c, uint8_t *row_low)
{
	w.seq++;
	drives();
	w.kp_scans++;
	w.kp_scans_unlocked += w.lock_depth == 0 ? 1u : 0u;
	w.kp_scans_in_session += w.open ? 1u : 0u;
	w.kp = c;
	memcpy(row_low, w.key_rows, sizeof(w.key_rows));
	if (w.keys_up_after != 0 && --w.keys_up_after == 0) {
		memset(w.key_rows, 0, sizeof(w.key_rows));
	}
}

// A radio at rest: every line high on its pull-up, nothing pressed, no ID, and the timer clock given (96 MHz the
// usual tree's). Then board_early_init's part: the backlight's PWM, dark
static void world_reset_hz(uint8_t ctl, uint32_t timer_hz)
{
	uint32_t ms = w.ms;

	memset(&w, 0, sizeof(w));
	memset(&fake_tim8, 0, sizeof(fake_tim8));
	w.ms = ms;	// the clock runs on
	for (unsigned p = 0; p < 5; p++) {
		w.level[p] = 0xFFFF;
	}
	set_ctl(ctl);
	w.timer_hz = timer_hz;
	cs7000p_backlight_init();
}

static void world_reset(uint8_t ctl)
{
	world_reset_hz(ctl, 96000000u);
	CHECK(w.pwm_inits == 1 && w.pwm_period == 2400 && fake_tim8.CCR4 == 0);
}

// Every way into the display file after its init
static void poke_display(void)
{
	static uint8_t frame[128 * 80];
	static const uint16_t pal[16] = { 0 };
	hal_display_panel_t info;

	cs7000p_display.flush_indexed(frame, pal, 0, 128);
	cs7000p_display.set_backlight(100);
	cs7000p_display.set_backlight_raw(cs7000p_display.backlight_raw_max);
	(void)cs7000p_display.backlight_raw();
	cs7000p_display.set_backlight(0);
	cs7000p_display.panel(&info);
}

static uint32_t scans(unsigned n)
{
	uint32_t keys = 0;

	for (unsigned i = 0; i < n; i++) {
		w.ms += 10;
		keys = cs7000p_input.scan();
	}
	return keys;
}

// Nothing reached the connector, the backlight or the key lights
static void check_untouched(void)
{
	CHECK(w.first_drive == 0 && w.drive_calls == 0);
	CHECK(w.bus_inits == 0 && w.bus_calls == 0 && w.sessions == 0 && w.reads == 0);
	CHECK(w.kp_inits == 0 && w.kp_scans == 0);
	CHECK(w.pwm_inits == 1 && fake_tim8.CCR4 == 0);	// the early set-up alone, and dark
	CHECK(w.exp_writes == 0 && w.exp == 0);
	CHECK(w.in_other == 0 && w.lock_depth == 0);
}

static void test_unexpected_board(void)
{
	cs7000p_display_state_t d;
	cs7000p_input_state_t k;

	// Before the check has run: unexpected, and no way in does anything
	world_reset(0x1E);
	cs7000p_display_state(&d);
	CHECK(d.lcd_board == CS7000P_LCD_UNEXPECTED);
	poke_display();
	CHECK(!cs7000p_matrix_key_down());
	check_untouched();

	for (unsigned levels = 0; levels < 32; levels++) {
		if (levels == 0x1E) {
			continue;
		}
		world_reset((uint8_t)levels);
		w.key_rows[3] = 0x01;	// a key held all along: it must never be looked for
		cs7000p_display.init();
		// The check itself: the five lines, and only they, as inputs with pull-ups, read twice
		CHECK(w.in_up[3] == 0x7000 && w.in_up[4] == 0x0003);
		CHECK(w.in_up[0] == 0 && w.in_up[1] == 0 && w.in_up[2] == 0);
		CHECK(w.ctl_reads == 10);
		cs7000p_display_state(&d);
		CHECK(d.lcd_board == CS7000P_LCD_UNEXPECTED && d.lcd_levels == levels);
		CHECK(d.panel.controller == NULL && d.panel.source == NULL && !d.panel.id_read);
		CHECK(d.panel.factory == -1);
		// Then every way into both files
		poke_display();
		CHECK(cs7000p_display.backlight_raw() == 0);
		CHECK(!cs7000p_matrix_key_down());
		cs7000p_input.init();
		CHECK(scans(10) == 0);
		cs7000p_input_state(&k);
		CHECK(!k.matrix_on && (k.rows[0] | k.rows[1] | k.rows[2] | k.rows[3] | k.rows[4]) == 0);
		check_untouched();
		// The keys with a line of their own are all the radio has now, and they work
		w.level[0] &= (uint16_t)~GPIO_PIN_8;	// PTT
		w.level[3] &= (uint16_t)~GPIO_PIN_15;	// SK1
		CHECK(scans(2) == ((1u << KEY_PTT) | (1u << KEY_SK1)));
		check_untouched();
	}

	// The pattern on the first reading only, or on the second only: a line that moves is not held by a resistor
	world_reset(0x1E);
	w.flip_at_delay = 2;	// between the two readings
	w.flip_levels = 0x1F;
	cs7000p_display.init();
	cs7000p_display_state(&d);
	CHECK(d.lcd_board == CS7000P_LCD_UNEXPECTED && d.lcd_levels == 0x1F);
	poke_display();
	check_untouched();
	world_reset(0x0E);
	w.flip_at_delay = 2;
	w.flip_levels = 0x1E;
	cs7000p_display.init();
	cs7000p_display_state(&d);
	CHECK(d.lcd_board == CS7000P_LCD_UNEXPECTED && d.lcd_levels == 0x0E);
	poke_display();
	check_untouched();
}

static void test_assumed_board(void)
{
	static uint8_t frame[128 * 80];
	static const uint16_t pal[16] = { 0 };
	cs7000p_display_state_t d;
	cs7000p_input_state_t k;
	hal_display_panel_t info;

	// The assumed board, a controller the driver knows, and a key that is held for the first three scans
	world_reset(0x1E);
	memcpy(w.id, (const uint8_t[3]){ 0x00, 0x91, 0x02 }, 3);
	w.key_rows[3] = 0x01;
	w.keys_up_after = 3;
	uint32_t t0 = w.ms;
	cs7000p_display.init();

	// Nothing was driven or set up before the second reading of the five lines
	CHECK(w.ctl_reads == 10 && w.first_drive > w.last_ctl_read);
	CHECK(w.in_up[3] == 0x7000 && w.in_up[4] == 0x0003 && w.in_other == 0);
	// The board files drive no pin themselves: the bus, the scan and the timer do
	CHECK(w.drive_calls == 0);
	cs7000p_display_state(&d);
	CHECK(d.lcd_board == CS7000P_LCD_ASSUMED && d.lcd_levels == 0x1E);

	// The bus as the board wires it
	CHECK(w.bus_inits == 1 && w.cfg != NULL);
	if (w.cfg == NULL) {
		return;		// nothing below has anything to look at
	}
	CHECK(w.cfg->data_port == GPIOD && w.cfg->data_shift == 0);
	CHECK(w.cfg->cs_port == GPIOE && w.cfg->cs_pin == GPIO_PIN_1);
	CHECK(w.cfg->res_port == GPIOE && w.cfg->res_pin == GPIO_PIN_0);
	CHECK(w.cfg->dc_port == GPIOD && w.cfg->dc_pin == GPIO_PIN_12);
	CHECK(w.cfg->wr_port == GPIOD && w.cfg->wr_pin == GPIO_PIN_13);
	CHECK(w.cfg->rd_port == GPIOD && w.cfg->rd_pin == GPIO_PIN_14);
	CHECK(w.cfg->wr_low_ns == 100 && w.cfg->wr_high_ns == 100);
	CHECK(w.cfg->rd_low_ns == 1000 && w.cfg->rd_high_ns == 1000);
	CHECK(w.cfg->idle_pull == GPIO_PULLUP);
	CHECK(w.cfg->lock == cs7000p_lcdbus_lock && w.cfg->unlock == cs7000p_lcdbus_unlock);
	CHECK(w.outside == 0 && !w.open && w.lock_depth == 0 && w.res);
	CHECK(w.delays_in_session == 0 && w.delays_locked == 0);

	// The ID: read only once the key was up (three scans saw it down, the fourth up), both ways, the dummy byte
	// dropped
	CHECK(w.kp_scans == 4 && w.kp_scans_in_session == 0 && w.kp_scans_unlocked == 0);
	CHECK(w.reads == 4 && w.reads_key_down == 0);
	CHECK(w.ms - t0 < 1000);
	// Its timing: reset stays low for a while once the bus drives it, and the first command comes no sooner after
	// the release than the 120 ms the display driver's own init gives a controller to wake (which one is not known)
	CHECK(w.res_high_ms - w.bus_init_ms >= 10);
	CHECK(w.first_read_ms - w.res_high_ms >= 120);
	CHECK(d.panel.id_read && memcmp(d.panel.id, w.id, 3) == 0 && memcmp(d.panel.id_reg, w.id, 3) == 0);
	CHECK(is(d.panel.controller, "gc9xxx") && is(d.panel.source, "id") && d.panel.type == 3);
	CHECK(d.panel.factory == -1);
	cs7000p_display.panel(&info);
	CHECK(is(info.controller, "gc9xxx") && is(info.source, "id") && info.type == 3 && info.id_read);
	// The driver's init for that type ran on this bus: its unlock sequence first, the whole panel cleared, on
	CHECK(w.first_cmd == 0xFE && w.madctl == 0xA0 && w.last_cmd == 0x29 && w.pixels == 160u * 128u);

	// The backlight: 40 kHz from the timer's clock (set up once, before the init: none of its own), dark until asked,
	// the window's ends, the key lights with it
	CHECK(w.pwm_inits == 1 && w.pwm_period == 2400 && w.exp_writes == 0);
	CHECK(cs7000p_display.backlight_raw_max == 2400 && cs7000p_display.backlight_raw() == 0);
	cs7000p_display.set_backlight(100);
	CHECK(fake_tim8.CCR4 == 1680 && w.exp == EXP_LAMP && w.exp_writes == 1);
	CHECK(cs7000p_display.backlight_raw() == 1680);
	cs7000p_display.set_backlight(1);
	CHECK(fake_tim8.CCR4 == 480 && w.exp == EXP_LAMP && w.exp_writes == 1);	// the chain: on a change only
	cs7000p_display.set_backlight(0);
	CHECK(fake_tim8.CCR4 == 0 && w.exp == 0 && w.exp_writes == 2);
	cs7000p_display.set_backlight_raw(2400);
	CHECK(fake_tim8.CCR4 == 2400 && w.exp == EXP_LAMP && cs7000p_display.backlight_raw() == 2400);
	cs7000p_display.set_backlight_raw(1);
	CHECK(fake_tim8.CCR4 == 1 && cs7000p_display.backlight_raw() == 1);
	cs7000p_display.set_backlight_raw(0);
	CHECK(fake_tim8.CCR4 == 0 && w.exp == 0);
	// Only this timer channel and this expander bit were ever touched, the bit always with the level in one masked
	// section (the UI and a power-off both set it)
	CHECK(fake_tim8.CCR1 == 0 && fake_tim8.CCR2 == 0 && fake_tim8.CCR3 == 0);
	CHECK(w.exp_unmasked == 0 && fake_irq_depth == 0);

	// A frame: one session, and no flush without a buffer
	uint32_t sessions = w.sessions;
	w.pixels = 0;
	cs7000p_display.flush_indexed(frame, pal, 0, 128);
	CHECK(w.sessions == sessions + 1 && w.pixels == 160u * 128u && !w.open && w.outside == 0);
	cs7000p_display.flush_indexed(NULL, pal, 0, 128);
	CHECK(w.sessions == sessions + 1);
	CHECK(cs7000p_display.width == 160 && cs7000p_display.height == 128 && cs7000p_display.bpp == 4);
	CHECK(cs7000p_display.flush == NULL);

	// ---- The keys on that board ----
	cs7000p_input.init();
	cs7000p_input_state(&k);
	CHECK(k.matrix_on && w.kp_inits == 1 && w.kp_inits_unlocked == 0 && w.lock_depth == 0);
	// The matrix's lines: K5 to K8 are PD4 to PD7 and K9 is PB7, driven; K1 to K4 are PD0 to PD3, sensed; 20 us
	CHECK(w.kp != NULL);
	if (w.kp == NULL) {
		return;
	}
	CHECK(w.kp->n_drive == 5 && w.kp->n_sense == 4 && w.kp->settle_us == 20);
	for (unsigned i = 0; i < 4; i++) {
		CHECK(w.kp->drive[i].port == GPIOD && w.kp->drive[i].pin == (uint16_t)(GPIO_PIN_4 << i));
		CHECK(w.kp->sense[i].port == GPIOD && w.kp->sense[i].pin == (uint16_t)(GPIO_PIN_0 << i));
	}
	CHECK(w.kp->drive[4].port == GPIOB && w.kp->drive[4].pin == GPIO_PIN_7);
	CHECK(w.kp->lock == cs7000p_lcdbus_lock && w.kp->unlock == cs7000p_lcdbus_unlock);

	// A key: K8 x K1 is 1. One scan is not a press, two are; the raw rows go to the views in scan order
	CHECK(scans(1) == 0);
	w.key_rows[3] = 0x01;
	CHECK(scans(1) == 0);
	CHECK(scans(1) == (1u << KEY_1));
	cs7000p_input_state(&k);
	CHECK(k.rows[3] == 0x01 && k.rows[0] == 0 && k.rows[1] == 0 && k.rows[2] == 0 && k.rows[4] == 0);
	// A function switch (K5 x K1, switch 1) and a side key with it
	w.key_rows[0] = 0x01;
	w.level[4] &= (uint16_t)~GPIO_PIN_12;	// SK2
	CHECK(scans(2) == ((1u << KEY_1) | (1u << CS7000P_FN_KEYS[0]) | (1u << KEY_SK2)));
	CHECK(w.kp_scans_unlocked == 0 && w.kp_scans_in_session == 0 && w.lock_depth == 0);

	// While the display is being written the matrix is not scanned and keeps its state, and the scan does not wait
	// for the bus: the side keys and PTT are read on time
	const uint32_t held = (1u << KEY_1) | (1u << CS7000P_FN_KEYS[0]) | (1u << KEY_SK2);
	uint32_t before = w.kp_scans;
	w.open = true;
	w.other_session = true;
	memset(w.key_rows, 0, sizeof(w.key_rows));
	w.level[0] &= (uint16_t)~GPIO_PIN_8;	// PTT
	CHECK(scans(2) == (held | (1u << KEY_PTT)));
	CHECK(scans(1) == (held | (1u << KEY_PTT)));
	CHECK(w.kp_scans == before && w.lock_waits == 0 && w.open && w.lock_depth == 0);
	// Not for longer than three scans: the fourth waits its turn and reads the matrix, with the session over
	CHECK(scans(1) == (held | (1u << KEY_PTT)));	// the first reading with the keys up: not settled yet
	CHECK(w.lock_waits == 1 && w.kp_scans == before + 1 && w.kp_scans_in_session == 0 && w.lock_depth == 0);
	CHECK(scans(1) == ((1u << KEY_SK2) | (1u << KEY_PTT)));
	CHECK(w.kp_scans == before + 2 && w.lock_waits == 1);
	w.level[0] |= GPIO_PIN_8;
	w.level[4] |= GPIO_PIN_12;
	CHECK(scans(2) == 0);
	// The next display session gets its three scans' grace again
	w.open = true;
	w.other_session = true;
	scans(3);
	CHECK(w.lock_waits == 1 && w.kp_scans == before + 4 && w.open);
	scans(1);
	CHECK(w.lock_waits == 2 && w.kp_scans == before + 5 && !w.open);
	// A session of this very task: the lock is recursive, so taking it does not wait and the bus is still not idle
	// under it. The lines are never scanned then, however many scans come, and a caller that asks whether a key is
	// down is told yes: it must not take the lines for free
	before = w.kp_scans;
	w.open = true;
	scans(6);
	CHECK(w.kp_scans == before && w.kp_scans_in_session == 0 && w.lock_waits == 2 && w.open && w.lock_depth == 0);
	CHECK(cs7000p_matrix_key_down());
	CHECK(w.kp_scans == before && w.kp_scans_in_session == 0 && w.lock_depth == 0);
	w.open = false;
	CHECK(!cs7000p_matrix_key_down() && w.kp_scans == before + 1 && w.kp_scans_in_session == 0);

	// The other keys with a line each: SK1 PD15, SK3 PE13, the top key PE10
	w.level[3] &= (uint16_t)~GPIO_PIN_15;
	CHECK(scans(2) == (1u << KEY_SK1));
	w.level[3] |= GPIO_PIN_15;
	w.level[4] &= (uint16_t)~GPIO_PIN_13;
	CHECK(scans(2) == (1u << KEY_SK3));
	w.level[4] |= GPIO_PIN_13;
	w.level[4] &= (uint16_t)~GPIO_PIN_10;
	CHECK(scans(2) == (1u << KEY_TOP));
	w.level[4] |= GPIO_PIN_10;
	CHECK(scans(2) == 0);

	// The accessory's PTT (PE11): after the line has rested high it is a PTT like the other
	scans(PLUG_PTT_ARM_MS / 10 + 2);
	cs7000p_input_state(&k);
	CHECK(k.ext_ptt_armed && !cs7000p_input.ptt_line_stuck());
	w.level[4] &= (uint16_t)~GPIO_PIN_11;
	CHECK(scans(2) == (1u << KEY_PTT));
	CHECK(scans(400) == (1u << KEY_PTT) && !cs7000p_input.ptt_line_stuck());	// a long press is a press
	w.level[4] |= GPIO_PIN_11;
	CHECK(scans(2) == 0);

	// The channel selector: its four lines shown as read (PE14, PE15, PB10, PB11), no position, no steps
	w.level[4] &= (uint16_t)~GPIO_PIN_14;	// ECN0 closed
	w.level[1] &= (uint16_t)~GPIO_PIN_11;	// ECN3 closed
	scans(10);
	cs7000p_input_state(&k);
	CHECK(k.chsw_raw == 0x06 && k.chsw_pos == -1);
	w.level[4] |= GPIO_PIN_14;
	w.level[4] &= (uint16_t)~GPIO_PIN_15;	// ECN1 closed
	scans(10);
	cs7000p_input_state(&k);
	CHECK(k.chsw_raw == 0x05 && k.chsw_pos == -1);
	CHECK(cs7000p_input.encoder == NULL && cs7000p_input.position == NULL && !cs7000p_input.encoder_limited);
	CHECK(w.drive_calls == 0 && w.in_other == 0);
}

static void test_accessory_line_low(void)
{
	cs7000p_input_state_t k;

	// A plug that holds the accessory's PTT line low from power-on: never a press, and after a while reported
	world_reset(0x1E);
	w.level[4] &= (uint16_t)~GPIO_PIN_11;
	cs7000p_input.init();
	CHECK(scans(2) == 0 && !cs7000p_input.ptt_line_stuck());
	CHECK(scans(CS7000P_EXT_PTT_STUCK_MS / 10 - 3) == 0 && !cs7000p_input.ptt_line_stuck());
	CHECK(scans(2) == 0 && cs7000p_input.ptt_line_stuck());
	cs7000p_input_state(&k);
	CHECK(!k.ext_ptt_armed);
	// The radio's own PTT still keys
	w.level[0] &= (uint16_t)~GPIO_PIN_8;
	CHECK(scans(2) == (1u << KEY_PTT));
	w.level[0] |= GPIO_PIN_8;
	// Released once, it counts from then on
	w.level[4] |= GPIO_PIN_11;
	CHECK(scans(PLUG_PTT_ARM_MS / 10 + 2) == 0 && !cs7000p_input.ptt_line_stuck());
	w.level[4] &= (uint16_t)~GPIO_PIN_11;
	CHECK(scans(2) == (1u << KEY_PTT) && !cs7000p_input.ptt_line_stuck());
}

// PTT and the top key (emergency on a long press) held while the radio is switched on: the board's two-scan debounce
// first reports them in its second scan, and the core's start-up guard (core/input/input_engine.c) still takes them
// as held at power-on. Nothing until they are released, then both work
static const keymap_entry_t start_keymap[] = {
	{ KEY_TOP, ACT_NONE, ACT_EMERGENCY, false, false, 0 },	// as board.c maps them
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
};
static unsigned start_events, start_ptt_down, start_emergency;

static void start_emit(const input_event_t *ev, void *ctx)
{
	start_events++;
	start_ptt_down += ev->action == ACT_PTT && ev->gesture == GESTURE_DOWN ? 1u : 0u;
	start_emergency += ev->action == ACT_EMERGENCY && ev->gesture == GESTURE_LONG ? 1u : 0u;
}

static void start_scans(input_engine_t *e, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		w.ms += INPUT_SCAN_MS;
		input_engine_step(e, cs7000p_input.scan(), 0, w.ms, start_emit, NULL);
	}
}

static void test_held_at_power_on(void)
{
	input_engine_t e;

	world_reset(0x1E);
	w.level[0] &= (uint16_t)~GPIO_PIN_8;	// PTT
	w.level[4] &= (uint16_t)~GPIO_PIN_10;	// the top key
	cs7000p_input.init();
	input_engine_init(&e, start_keymap, sizeof(start_keymap) / sizeof(start_keymap[0]), w.ms);
	start_scans(&e, (INPUT_HOLD_MS + 500) / INPUT_SCAN_MS);
	CHECK(start_events == 0);
	w.level[0] |= GPIO_PIN_8;
	w.level[4] |= GPIO_PIN_10;
	start_scans(&e, 5);
	CHECK(start_events == 0);
	// Released once: a press counts
	w.level[0] &= (uint16_t)~GPIO_PIN_8;
	start_scans(&e, 5);
	CHECK(start_ptt_down == 1);
	w.level[0] |= GPIO_PIN_8;
	w.level[4] &= (uint16_t)~GPIO_PIN_10;
	start_scans(&e, (INPUT_HOLD_MS + 100) / INPUT_SCAN_MS);
	CHECK(start_emergency == 1);
	w.level[4] |= GPIO_PIN_10;
	start_scans(&e, 5);
}

static void test_id_cases(void)
{
	cs7000p_display_state_t d;

	// A key that stays down: the read waits for it, then goes ahead and says so
	world_reset(0x1E);
	memcpy(w.id, (const uint8_t[3]){ 0x7C, 0x89, 0xF0 }, 3);
	w.key_rows[1] = 0x04;
	uint32_t t0 = w.ms;
	cs7000p_display.init();
	CHECK(w.ms - t0 >= 500 && w.ms - t0 < 1500);
	CHECK(w.reads == 4 && w.reads_key_down == 4 && w.kp_scans > 10);
#if CONFIG_DEBUGLINK
	CHECK(w.logs == 1);
#endif
	cs7000p_display_state(&d);
	CHECK(is(d.panel.controller, "st7735") && is(d.panel.source, "id") && d.panel.type == 0);
	CHECK(w.first_cmd == 0x11 && w.madctl == 0xA0);

	// An ID the driver has no entry for, here a bus that answers nothing but its pull-ups: the standard init is
	// sent all the same, and the report says what it is
	world_reset(0x1E);
	memset(w.id, 0xFF, 3);
	cs7000p_display.init();
	cs7000p_display_state(&d);
	CHECK(d.panel.id_read && d.panel.id[0] == 0xFF && d.panel.id_reg[2] == 0xFF);
	CHECK(is(d.panel.controller, "unknown") && is(d.panel.source, "default") && d.panel.type == 0);
	CHECK(w.first_cmd == 0x11 && w.last_cmd == 0x29 && w.pixels == 160u * 128u && w.logs == 0);
	// With no key to wait for, the read still leaves the controller its time after the reset's release
	CHECK(w.kp_scans == 1 && w.first_read_ms - w.res_high_ms >= 120);

	// The fallback clock tree's timer clock: still 40 kHz, the same duty of the shorter period
	world_reset_hz(0x1E, 32000000u);
	CHECK(w.pwm_inits == 1 && w.pwm_period == 800);
	cs7000p_display.init();
	CHECK(w.pwm_inits == 1);
	cs7000p_display.set_backlight(100);
	CHECK(fake_tim8.CCR4 == 560);
	cs7000p_display.set_backlight_raw(1200);
	CHECK(fake_tim8.CCR4 == 400 && cs7000p_display.backlight_raw() == 1200);

	// A timer clock no 40 kHz period fits: no PWM at all, the pin stays the low output it was. The panel works, the
	// backlight and the key lights stay off
	world_reset_hz(0x1E, 70000u);
	CHECK(w.pwm_inits == 0);
	cs7000p_display.init();
	cs7000p_display_state(&d);
	CHECK(d.lcd_board == CS7000P_LCD_ASSUMED && w.pwm_inits == 0);
	cs7000p_display.set_backlight(100);
	cs7000p_display.set_backlight_raw(2400);
	CHECK(fake_tim8.CCR4 == 0 && w.exp_writes == 0 && cs7000p_display.backlight_raw() == 0);

	// After a good start, a start that fails the check leaves every way in closed again
	world_reset(0x1F);
	cs7000p_display.init();
	cs7000p_display_state(&d);
	CHECK(d.lcd_board == CS7000P_LCD_UNEXPECTED && d.panel.controller == NULL && !d.panel.id_read);
	poke_display();
	cs7000p_input.init();
	CHECK(scans(5) == 0);
	check_untouched();
}

static void test_board_files(void)
{
	test_unexpected_board();
	test_assumed_board();
	test_accessory_line_low();
	test_held_at_power_on();
	test_id_cases();
}
#else
static void test_board_files(void)
{
	printf("# cs7000pPanelTest: display_cs7000p.c and input_cs7000p.c not run "
	       "(no platform/stm32 on the include path)\n");
}
#endif

int main(void)
{
	test_board_check();
	test_panel_choice();
	test_backlight();
	test_driver_init();
	test_board_files();
	printf("# cs7000pPanelTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
