// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS power: the ADC scan, the knob and the supply hold, battery, volume position, LEDs, microphone level
// (docs/spec/hardware-cs7000p.md 5; the scales and rules are in battery_cs7000p.h). What has run on a radio is the
// bring-up page's First radio (docs/testing/cs7000p-bringup.md).
//
// The supply. The volume knob's switch powers the radio; POW_C, an expander bit, keeps it powered after the knob is
// off so the radio can save its state. That bit outlives an MCU reset and the ROM bootloader never writes it, so no
// reset word sets it (exp_cs7000p.h): the hold is set here, and only after the knob has been read on. A radio whose
// ADC does not start is never held and goes off with its knob.
//
// The scan (platform/stm32h7/periph/adc_scan) runs ADC1 over six inputs 2,400 times a second into a buffer 16 scans
// deep: a reading is an average over 6.7 ms with no locking, and the sub-audio rank is handed on in batches.
#include <stddef.h>
#include "cs7000p.h"
#include "battery_cs7000p.h"
#include "exp_cs7000p.h"
#include "pins.h"
#include "FreeRTOS.h"
#include "timers.h"
#include "hal_time.h"
#include "spi_flash/spi_flash.h"
#include "power/battery/battery.h"
#include "audio/miclevel/miclevel.h"

#define SCANS			16u	// even (the platform's rule); half of it is one sub-audio batch
// A 2-cell pack, resting voltage to charge between these, as the other radios with one
#define BAT_FULL_MV		8200u
#define BAT_EMPTY_MV		6400u

// Rank order is cs7000p.h's enum
static const uint8_t channels[CS7000P_ADC_RANKS] = {
	[CS7000P_ADC_POW_DET] = ADC_CH_POW_DET,		[CS7000P_ADC_VOL] = ADC_CH_VOL_DET,
	[CS7000P_ADC_VOX] = ADC_CH_VOX,			[CS7000P_ADC_RSSI] = ADC_CH_RSSI,
	[CS7000P_ADC_NOISE] = ADC_CH_NOISE,		[CS7000P_ADC_SUBAUDIO] = ADC_CH_SUBAUDIO,
};
// Plain .bss: the AXI SRAM, the only RAM the DMA reaches here (never RFOUNDRY_CPU_RAM)
static volatile uint16_t scan_buf[CS7000P_ADC_RANKS * SCANS];

static volatile uint8_t knob;		// a cs7000p_knob_t
static pwrsw_t pwrsw;
static battery_avg_t bat;
// The watch on the knob after a power-off that left the radio running (battery_cs7000p.h)
static TimerHandle_t off_timer;
static cs7000p_offwatch_t off_watch;
static uint32_t off_ms;
// The off state: latch(false) has run and latch(true) not since. If the radio still runs, the knob powers it, and
// the radio's receiver restore, its speaker amplifier and the LEDs stay off until it ends (cs7000p_power_off_state)
static volatile bool off_state;

uint16_t cs7000p_adc_raw(uint8_t rank)
{
	return rank < CS7000P_ADC_RANKS ? stm32_adc_scan_avg(rank) : 0;	// 0 whenever the scan is not running
}

uint16_t cs7000p_adc_mv(uint8_t rank)
{
	uint32_t vdda = stm32_adc_vdda_mv();
	if (vdda == 0) {
		vdda = CS7000P_VDDA_NOMINAL_MV;		// the chip's own measurement failed
	}
	return (uint16_t)cs7000p_adc_to_mv(cs7000p_adc_raw(rank), vdda);
}

void cs7000p_subaudio_feed(stm32_adc_rank_cb_t batch, void (*tick)(void))
{
	stm32_adc_scan_rank_cb(CS7000P_ADC_SUBAUDIO, batch);
	stm32_adc_scan_tick_cb(tick);
}

// The battery now, unfiltered: 0 with the knob off
static uint32_t battery_now_mv(void)
{
	return cs7000p_battery_mv(cs7000p_adc_mv(CS7000P_ADC_POW_DET));
}

// The knob from readings PWRSW_SAMPLE_MS apart (each an average over its own scans), and the hold when every one of
// them says on. Task level
static void hold_by_knob(void)
{
	uint32_t mv[CS7000P_HOLD_READINGS] = { 0 };
	const bool adc_ok = stm32_adc_scan_running();

	for (unsigned i = 0; adc_ok && i < CS7000P_HOLD_READINGS; i++) {
		if (i != 0) {
			hal_delay_ms(PWRSW_SAMPLE_MS);
		}
		mv[i] = battery_now_mv();
	}
	const cs7000p_knob_t k = cs7000p_knob_from_readings(adc_ok, mv, CS7000P_HOLD_READINGS);
	if (cs7000p_knob_holds(k)) {
		(void)cs7000p_exp_set(EXP_POWER_HOLD, 0);	// never refused: not a transmit bit
	}
	pwrsw = (pwrsw_t){ 0 };
	knob = (uint8_t)k;
}

// The knob while the radio sits in its off state, from the timer task: a restart when it went off and came back
static void off_poll(TimerHandle_t timer)
{
	(void)timer;
	if (!off_state || !stm32_adc_scan_running()) {
		return;		// no reading: the radio stays as it is until the knob takes its supply
	}
	if (cs7000p_offwatch_restart(&off_watch, battery_now_mv(), hal_millis() - off_ms)) {
		// A plain reset: the reset word goes out first (board_reset_prepare), the next start reads the knob again
		(void)stm32_system.reboot(HAL_REBOOT_NORMAL);
	}
}

// The first call of board_init(), task level: the scan, then the hold if the knob is on. The six inputs are in
// analog mode already (cs7000p_reset_levels). The scan's start returns when its buffer has filled once, some tens of
// milliseconds; from reset to the hold is that plus two sample times. A knob contact that bounces inside it ends as
// "off": the radio goes off, or restarts through the watch below if the knob is on after all
void cs7000p_power_start(void)
{
	if (knob != CS7000P_KNOB_UNKNOWN) {
		return;
	}
	if (!stm32_adc_scan_start(channels, CS7000P_ADC_RANKS, scan_buf, SCANS, CS7000P_SCAN_HZ)) {
		knob = CS7000P_KNOB_NOADC;	// every reading is absent: no hold, ever
		return;
	}
	hold_by_knob();
	// Without the timer (no heap left) a radio in its off state just stays there until the knob is cycled
	off_timer = xTimerCreate("off", pdMS_TO_TICKS(PWRSW_SAMPLE_MS), pdTRUE, NULL, off_poll);
}

cs7000p_knob_t cs7000p_knob(void)
{
	return (cs7000p_knob_t)knob;
}

bool cs7000p_power_off_state(void)
{
	return off_state;
}

// false: let go of the supply. With the knob off the radio is gone 5 to 15 ms later. If it is still running, the
// knob powers it (a power-off the radio asked for itself: a critical battery, the auto power off), and the radio
// stays dark and quiet in its off state: no receiver restore, no speaker amplifier, no LED. The core either waits for
// good, and the watch restarts the radio when the knob has been off and on again, or comes back with latch(true)
// after its own wait.
// true: that second case. The off state ends (the radio's next status poll sets the receiver and the speaker up
// again), and the supply is held again if the knob reads on
static void latch(bool on)
{
	if (on) {
		off_state = false;
		if (off_timer != NULL) {
			(void)xTimerStop(off_timer, portMAX_DELAY);
		}
		if (knob != CS7000P_KNOB_NOADC) {
			hold_by_knob();
		}
		return;
	}
	// First: from here on the radio task leaves the receiver, the amplifier and the LEDs as this leaves them, so a
	// status poll between the safe request below and the end of this can't take the restore and bring them back.
	// led() and the speaker ask for this state and write their bit in one masked section: a call this task cut
	// into either wrote before the clear below, or sees the state
	off_state = true;
	stm32_spi_flash_quiesce();	// the data flash as other firmware expects to find it; waits for a write in flight
	board_rf_safe();
	if (board.display != NULL && board.display->set_backlight != NULL) {
		board.display->set_backlight(0);
	}
	(void)cs7000p_exp_set(0, EXP_AMP | EXP_LAMP | EXP_LED_GREEN | EXP_LED_RED);
	(void)cs7000p_exp_set(0, EXP_POWER_HOLD);	// last, in a write of its own
	cs7000p_offwatch_start(&off_watch, knob == CS7000P_KNOB_OFF);
	off_ms = hal_millis();
	if (off_timer != NULL && knob != CS7000P_KNOB_NOADC) {
		(void)xTimerStart(off_timer, portMAX_DELAY);
	}
}

// False once the knob is off: its switch takes POW_DET to 0 V within a millisecond while the hold keeps the radio
// running. A few readings of the switch open in a row (cs7000p_knob_reading): a pack that sags under a transmission
// is not a switch-off
static bool switch_on(void)
{
	if (knob == CS7000P_KNOB_ON && !stm32_adc_scan_running()) {
		// The scan stopped for good: every reading is 0 from here on, which must not read as the knob. Without
		// a reading the supply is not held, as at a start without the ADC
		(void)cs7000p_exp_set(0, EXP_POWER_HOLD);
		knob = CS7000P_KNOB_NOADC;
	}
	bool reading_on = true;
	if (knob == CS7000P_KNOB_ON) {
		reading_on = pwrsw_on(&pwrsw, cs7000p_knob_reading(battery_now_mv()), hal_millis());
		if (!reading_on) {
			knob = CS7000P_KNOB_OFF;
		}
	}
	return cs7000p_knob_switch_on((cs7000p_knob_t)knob, reading_on);
}

// For display, smoothed. 0 with the knob's switch open or without a reading: the core's battery monitor ignores 0, so
// the moments between the knob going off and the power-off are not a flat battery. Anything else is the battery,
// a flat one included: the core's thresholds judge it
static uint16_t battery_mv(void)
{
	if (battery_now_mv() < CS7000P_KNOB_OPEN_MV) {
		return 0;
	}
	return battery_avg_mv(&bat, hal_millis(), battery_now_mv);
}

static uint8_t battery_pct(void)
{
	return battery_linear_pct(battery_mv(), BAT_EMPTY_MV, BAT_FULL_MV);
}

// The knob's position for the volume bar. The pot itself is in the analog audio path: nothing here sets the volume.
// -1 without a reading
static int8_t volume(void)
{
	if (!stm32_adc_scan_running()) {
		return -1;
	}
	return (int8_t)cs7000p_volume_pct(cs7000p_adc_raw(CS7000P_ADC_VOL));
}

// On or off only: the LEDs are expander outputs (the green ones' resistor is drawn 22 ohm: no dimming). None comes
// on in the off state: the core goes on driving them for the squelch
static void led(uint8_t which, bool on)
{
	const uint32_t bit = which == LED_GREEN ? EXP_LED_GREEN : EXP_LED_RED;
	// The off state asked and the bit written in one masked section. latch(false) runs in a task that preempts
	// this one: it sets the off state, then clears the LEDs, so whichever comes first here, none is left on
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	if (!on || !off_state) {
		(void)cs7000p_exp_set(on ? bit : 0u, on ? 0u : bit);
	}
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
}

#if CONFIG_VOX
// The microphone level detector's line, for VOX: the hal's logarithmic scale of the 12-bit reading. Uncalibrated.
// Not written (config.mk): the detector has no supply while receiving, so this reads near 0
static uint8_t mic_level(void)
{
	return miclevel_log(cs7000p_adc_raw(CS7000P_ADC_VOX), 12);
}
#endif

const hal_power_t cs7000p_power = {
	.init = cs7000p_power_start,
	.latch = latch,
	.battery_mv = battery_mv,
	.battery_pct = battery_pct,
	.volume = volume,
	.switch_on = switch_on,
	.led = led,
#if CONFIG_VOX
	.mic_level = mic_level,
#endif
};
