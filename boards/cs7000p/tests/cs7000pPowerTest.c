// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CS7000 M17 PLUS power file (power_cs7000p.c, included here) on a fake ADC scan, expander, timer and clock: the
// code that applies battery_cs7000p.h's rules. What must hold on the radio: the supply hold is set only after every
// first reading says the knob is on, never without the ADC; a power-off makes the transmitter safe before it writes
// a line and clears the hold last, in a write of its own; latch(true) reads the knob again; a scan that stops drops
// the hold and the radio never switches itself off on it; a pack that sags is not the knob; in the off state no LED
// comes on, not even from a led() call the power-off cut into, and the radio is told so before the safe request.
#include <stdio.h>
#include <string.h>
#include "../power_cs7000p.c"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The model's POW_DET for a battery (battery_cs7000p.h: 0.2496 x Vbat + 0.061 V)
#define POW(vbat_mv)	((uint16_t)(((vbat_mv) * 2496u + 610000u) / 10000u))
#define KNOB_OFF	0u

// --- One call log for everything the file does to the outside ---

enum { C_EXP, C_RF_SAFE, C_QUIESCE, C_BACKLIGHT, C_TIMER_START, C_TIMER_STOP, C_REBOOT };
static struct { uint8_t what; uint32_t set, clear; bool off_state; } calls[64];
static int n_calls;

static void log_call(uint8_t what, uint32_t set, uint32_t clear)
{
	if (n_calls < (int)(sizeof(calls) / sizeof(calls[0]))) {
		calls[n_calls++] = (typeof(calls[0])){ what, set, clear, cs7000p_power_off_state() };
	}
}

static int find_call(int from, uint8_t what)
{
	for (int i = from; i < n_calls; i++) {
		if (calls[i].what == what) {
			return i;
		}
	}
	return -1;
}

static bool hold_ever_set(void)
{
	for (int i = 0; i < n_calls; i++) {
		if (calls[i].what == C_EXP && (calls[i].set & EXP_POWER_HOLD) != 0u) {
			return true;
		}
	}
	return false;
}

// --- The fakes ---

int fake_irq_depth;
static uint32_t now_ms = 1000;
static uint16_t pow_seq[8];		// POW_DET in mV, one per reading: the next after each delay
static unsigned pow_n, pow_i;
static bool scan_starts = true, scan_runs = true;
static uint32_t exp_word;

static uint16_t pow_now(void)
{
	return pow_n == 0 ? 0u : pow_seq[pow_i < pow_n ? pow_i : pow_n - 1u];
}

static void readings(const uint16_t *mv, unsigned n)
{
	memcpy(pow_seq, mv, n * sizeof(mv[0]));
	pow_n = n;
	pow_i = 0;
}

static void reading(uint16_t mv)
{
	readings(&mv, 1);
}

// The scan's average as mV at the pin: with VDDA at 4095 mV a raw count is a mV
uint16_t stm32_adc_scan_avg(uint8_t rank) { return rank == CS7000P_ADC_POW_DET ? pow_now() : 0u; }
uint16_t stm32_adc_vdda_mv(void) { return 4095u; }
bool stm32_adc_scan_running(void) { return scan_runs; }
void stm32_adc_scan_rank_cb(uint8_t rank, stm32_adc_rank_cb_t cb) { (void)rank; (void)cb; }
void stm32_adc_scan_tick_cb(void (*fn)(void)) { (void)fn; }

bool stm32_adc_scan_start(const uint8_t *ch, uint8_t n, volatile uint16_t *buf, uint16_t n_scans, uint32_t hz)
{
	(void)ch; (void)n; (void)buf; (void)n_scans; (void)hz;
	scan_runs = scan_starts;
	return scan_starts;
}

uint32_t hal_millis(void) { return now_ms; }

void hal_delay_ms(uint32_t ms)
{
	now_ms += ms;
	pow_i++;
}

// The interrupt mask (stub/FreeRTOS.h, CS7000P_STUB_MASK_HOOK). Taking it is the last moment another task can run:
// with cut_in set, the start task's latch(false) runs there, once, as it would preempt the radio task on the radio
static bool cut_in;

void fake_mask_enter(void)
{
	if (cut_in && fake_irq_depth == 0) {
		cut_in = false;
		cs7000p_power.latch(false);
	}
	fake_irq_depth++;
}

// As the real writer: the mask first, then the word
bool cs7000p_exp_set(uint32_t set, uint32_t clear)
{
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	log_call(C_EXP, set, clear);
	exp_word = (exp_word & ~clear) | set;
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	return true;
}

void board_rf_safe(void) { log_call(C_RF_SAFE, 0, 0); }
void stm32_spi_flash_quiesce(void) { log_call(C_QUIESCE, 0, 0); }

static void set_backlight(uint8_t percent) { log_call(C_BACKLIGHT, percent, 0); }
static const hal_display_t display = { .set_backlight = set_backlight };
const board_t board = { .display = &display };

static int timer_obj;
static TimerCallbackFunction_t timer_cb;
TimerHandle_t xTimerCreate(const char *name, TickType_t period, BaseType_t reload, void *id,
			   TimerCallbackFunction_t cb)
{
	(void)name; (void)period; (void)reload; (void)id;
	timer_cb = cb;
	return &timer_obj;
}

BaseType_t xTimerStart(TimerHandle_t t, TickType_t wait)
{
	(void)t;
	(void)wait;
	log_call(C_TIMER_START, 0, 0);
	return pdTRUE;
}

BaseType_t xTimerStop(TimerHandle_t t, TickType_t wait)
{
	(void)t;
	(void)wait;
	log_call(C_TIMER_STOP, 0, 0);
	return pdTRUE;
}

static bool reboot(uint8_t mode)
{
	log_call(C_REBOOT, mode, 0);
	return true;
}

const hal_system_t stm32_system = { .reboot = reboot };

// --- The runs ---

// A fresh start of the image: the file's state as after a reset, then board_init's first call
static void boot(const uint16_t *mv, unsigned n)
{
	knob = CS7000P_KNOB_UNKNOWN;
	pwrsw = (pwrsw_t){ 0 };
	bat = (battery_avg_t){ 0 };
	off_timer = NULL;
	off_state = false;
	scan_runs = scan_starts;
	exp_word = EXP_RESET_WORD;
	n_calls = 0;
	readings(mv, n);
	cs7000p_power_start();
}

static void test_hold(void)
{
	static const uint16_t vals[3] = { KNOB_OFF, POW(4000), POW(7400) };	// open, a sagged pack, a good one
	const uint32_t open_pow = CS7000P_KNOB_OPEN_POW_DET_MV;

	// Every sequence of the three first readings: the hold exactly when none of them is the switch open
	for (unsigned a = 0; a < 3; a++) {
		for (unsigned b = 0; b < 3; b++) {
			for (unsigned c = 0; c < 3; c++) {
				const uint16_t mv[3] = { vals[a], vals[b], vals[c] };
				boot(mv, 3);
				const bool on = a != 0 && b != 0 && c != 0;
				CHECK(hold_ever_set() == on && ((exp_word & EXP_POWER_HOLD) != 0u) == on);
				CHECK(cs7000p_knob() == (on ? CS7000P_KNOB_ON : CS7000P_KNOB_OFF));
				CHECK(pow_i == 2u);	// three readings, a sample time apart
			}
		}
	}
	// At the open threshold and just under it
	const uint16_t at[3] = { (uint16_t)open_pow, (uint16_t)open_pow, (uint16_t)open_pow };
	boot(at, 3);
	CHECK(cs7000p_knob() == CS7000P_KNOB_ON && hold_ever_set());
	const uint16_t under[3] = { (uint16_t)open_pow, (uint16_t)(open_pow - 1u), (uint16_t)open_pow };
	boot(under, 3);
	CHECK(cs7000p_knob() == CS7000P_KNOB_OFF && !hold_ever_set());
	// No ADC: never a hold, and the radio never switches itself off on a reading it has not got
	const uint16_t good[3] = { POW(7400), POW(7400), POW(7400) };
	scan_starts = false;
	boot(good, 3);
	CHECK(cs7000p_knob() == CS7000P_KNOB_NOADC && !hold_ever_set() && n_calls == 0);
	CHECK(cs7000p_power.switch_on() && cs7000p_power.switch_on());
	cs7000p_power.latch(false);
	cs7000p_power.latch(true);
	CHECK(!hold_ever_set() && find_call(0, C_TIMER_START) < 0);
	scan_starts = true;
	// A second call is no second start
	boot(good, 3);
	n_calls = 0;
	cs7000p_power_start();
	CHECK(n_calls == 0);
}

static void test_latch(void)
{
	const uint16_t good[3] = { POW(7400), POW(7400), POW(7400) };
	boot(good, 3);
	CHECK((exp_word & EXP_POWER_HOLD) != 0u && !cs7000p_power_off_state());

	// latch(false): the off state first, the flash quiet, the transmitter safe before any line is written, the
	// amplifier, lamp and LEDs, and the hold last in a write of its own
	n_calls = 0;
	cs7000p_power.latch(false);
	const int q = find_call(0, C_QUIESCE), s = find_call(0, C_RF_SAFE), e = find_call(0, C_EXP);
	CHECK(q >= 0 && s > q && e > s && calls[s].off_state);
	int last = -1;
	for (int i = 0; i < n_calls; i++) {
		if (calls[i].what == C_EXP) {
			last = i;
			CHECK((calls[i].set & EXP_POWER_HOLD) == 0u);
		}
	}
	CHECK(last >= 0 && calls[last].set == 0u && calls[last].clear == EXP_POWER_HOLD);
	CHECK((exp_word & (EXP_POWER_HOLD | EXP_AMP | EXP_LAMP | EXP_LED_GREEN | EXP_LED_RED)) == 0u);
	CHECK(find_call(0, C_BACKLIGHT) >= 0 && calls[find_call(0, C_BACKLIGHT)].set == 0u);
	CHECK(find_call(last, C_TIMER_START) > last && cs7000p_power_off_state());

	// The off state: no LED comes on, one going off still goes
	exp_word |= EXP_LED_RED;
	cs7000p_power.led(LED_GREEN, true);
	cs7000p_power.led(LED_RED, false);
	CHECK((exp_word & (EXP_LED_GREEN | EXP_LED_RED)) == 0u);

	// latch(true) on readings that say on: the off state over, the knob read again, the hold set again
	readings(good, 3);
	n_calls = 0;
	cs7000p_power.latch(true);
	CHECK(!cs7000p_power_off_state() && find_call(0, C_TIMER_STOP) == 0);
	CHECK((exp_word & EXP_POWER_HOLD) != 0u && cs7000p_knob() == CS7000P_KNOB_ON);
	cs7000p_power.led(LED_GREEN, true);
	CHECK((exp_word & EXP_LED_GREEN) != 0u);
	cs7000p_power.led(LED_GREEN, false);
	// ... and on a reading of the switch open: no hold
	cs7000p_power.latch(false);
	const uint16_t off_mid[3] = { POW(7400), KNOB_OFF, POW(7400) };
	readings(off_mid, 3);
	n_calls = 0;
	cs7000p_power.latch(true);
	CHECK(!hold_ever_set() && cs7000p_knob() == CS7000P_KNOB_OFF && !cs7000p_power.switch_on());

	// latch(false) from the task above, cutting into a led() call at the last moment it can: no LED is left on,
	// whichever LED and whether the call was past its look at the off state or not
	for (uint8_t which = 0; which < 2; which++) {
		boot(good, 3);
		cut_in = true;
		cs7000p_power.led(which == 0 ? LED_GREEN : LED_RED, true);
		CHECK(!cut_in && cs7000p_power_off_state() && (exp_word & (EXP_LED_GREEN | EXP_LED_RED)) == 0u);
		CHECK(fake_irq_depth == 0);
	}
	// ... and one that goes off still goes
	boot(good, 3);
	cs7000p_power.led(LED_RED, true);
	CHECK((exp_word & EXP_LED_RED) != 0u);
	cut_in = true;
	cs7000p_power.led(LED_RED, false);
	CHECK(!cut_in && (exp_word & EXP_LED_RED) == 0u && fake_irq_depth == 0);
}

static void test_switch_on(void)
{
	const uint16_t good[3] = { POW(7400), POW(7400), POW(7400) };
	boot(good, 3);
	// A pack that sags far under a transmission, for as long as it likes: still on
	reading(POW(5000));
	for (int i = 0; i < 50; i++) {
		CHECK(cs7000p_power.switch_on());
		now_ms += 100;
	}
	CHECK(cs7000p_knob() == CS7000P_KNOB_ON);
	// The switch open: off on the third reading a sample time apart, and it stays off
	reading(KNOB_OFF);
	CHECK(cs7000p_power.switch_on());
	now_ms += 100;
	CHECK(cs7000p_power.switch_on());
	now_ms += 100;
	CHECK(!cs7000p_power.switch_on() && cs7000p_knob() == CS7000P_KNOB_OFF);
	reading(POW(7400));
	now_ms += 100;
	CHECK(!cs7000p_power.switch_on());

	// The scan stops for good with the knob on: the hold goes, and the radio never switches itself off
	boot(good, 3);
	scan_runs = false;
	n_calls = 0;
	CHECK(cs7000p_power.switch_on());
	CHECK(n_calls == 1 && calls[0].what == C_EXP && calls[0].set == 0u && calls[0].clear == EXP_POWER_HOLD);
	CHECK(cs7000p_knob() == CS7000P_KNOB_NOADC);
	for (int i = 0; i < 10; i++) {
		CHECK(cs7000p_power.switch_on());
		now_ms += 100;
	}
	CHECK(!hold_ever_set() && cs7000p_power.volume() == -1);
	scan_runs = true;
}

static void test_battery(void)
{
	const uint16_t good[3] = { POW(7400), POW(7400), POW(7400) };
	boot(good, 3);
	// The switch open: no reading. A flat pack: its reading, for the core's thresholds to judge
	reading(KNOB_OFF);
	CHECK(cs7000p_power.battery_mv() == 0);
	reading((uint16_t)(CS7000P_KNOB_OPEN_POW_DET_MV - 1u));
	CHECK(cs7000p_power.battery_mv() == 0);
	bat = (battery_avg_t){ 0 };
	reading(POW(5300));
	uint16_t mv = cs7000p_power.battery_mv();
	CHECK(mv > 5200 && mv < 5400);
	bat = (battery_avg_t){ 0 };
	reading(POW(7400));
	mv = cs7000p_power.battery_mv();
	CHECK(mv > 7300 && mv < 7500);
}

// The watch after a power-off the knob overrode: the timer's poll restarts the radio only after the switch was open
static void test_off_watch(void)
{
	const uint16_t good[3] = { POW(7400), POW(7400), POW(7400) };
	boot(good, 3);
	CHECK(timer_cb != NULL);
	cs7000p_power.latch(false);
	n_calls = 0;
	// A pack that sags and recovers (a critical battery's power-off with the knob on): never a restart
	for (int i = 0; i < 300; i++) {
		reading(i % 5 < 2 ? POW(5000) : POW(6200));
		now_ms += PWRSW_SAMPLE_MS;
		timer_cb(off_timer);
	}
	CHECK(find_call(0, C_REBOOT) < 0);
	// The knob off and on again: the restart
	for (int i = 0; i < 3; i++) {
		reading(KNOB_OFF);
		now_ms += PWRSW_SAMPLE_MS;
		timer_cb(off_timer);
	}
	for (int i = 0; i < 3; i++) {
		reading(POW(7400));
		now_ms += PWRSW_SAMPLE_MS;
		timer_cb(off_timer);
	}
	CHECK(find_call(0, C_REBOOT) >= 0 && calls[find_call(0, C_REBOOT)].set == HAL_REBOOT_NORMAL);
	// Not in the off state: the poll does nothing, not even on the knob off and on again (a timer tick that was
	// already on its way when latch(true) stopped the timer)
	cs7000p_power.latch(true);
	n_calls = 0;
	for (int i = 0; i < 20; i++) {
		reading(i < 10 ? KNOB_OFF : POW(7400));
		now_ms += PWRSW_SAMPLE_MS;
		timer_cb(off_timer);
	}
	CHECK(n_calls == 0);
}

int main(void)
{
	test_hold();
	test_latch();
	test_switch_on();
	test_battery();
	test_off_watch();
	printf("# cs7000pPowerTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
