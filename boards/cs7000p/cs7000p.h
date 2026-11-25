// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Internal interfaces between the CS7000 M17 PLUS board files (docs/spec/hardware-cs7000p.md). What has run on a
// radio is the bring-up page's First radio (docs/testing/cs7000p-bringup.md).
//
// Bench knobs (CS7000P_DEFS in board.mk): each is an #ifndef default in the file that uses it, never here, and a row
// of docs/testing/cs7000p-bringup.md.
#ifndef CS7000P_H
#define CS7000P_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "config.h"
#include "periph/adc_scan.h"

// Physical key bits returned by cs7000p_input.scan(). The matrix positions and the order of the six unlabeled
// function switches are in keys_cs7000p.h; SK1-SK3, the top key and PTT have a pin each (pins.h)
enum { KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_STAR, KEY_HASH, KEY_OK, KEY_BACK,
       KEY_UP, KEY_DOWN, KEY_F1, KEY_F2, KEY_SK1, KEY_SK2, KEY_SK3, KEY_TOP, KEY_PTT, KEY_COUNT };
_Static_assert(KEY_COUNT <= 32, "key bits are a 32-bit mask");

// ADC1 scan ranks (power_cs7000p.c), in scan order: the supply behind the knob, the volume pot, the microphone
// level, the IF chip's RSSI and noise level, the sub-audio input
enum { CS7000P_ADC_POW_DET, CS7000P_ADC_VOL, CS7000P_ADC_VOX, CS7000P_ADC_RSSI, CS7000P_ADC_NOISE, CS7000P_ADC_SUBAUDIO,
       CS7000P_ADC_RANKS };
// Scans a second: the sub-audio detector's sample rate
#define CS7000P_SCAN_HZ	2400u
// The power knob as the supply reading shows it. NOADC: the scan did not start, so the knob can't be read; the
// supply is then never held and the radio runs for as long as the knob powers it
typedef enum { CS7000P_KNOB_UNKNOWN, CS7000P_KNOB_ON, CS7000P_KNOB_OFF, CS7000P_KNOB_NOADC } cs7000p_knob_t;

// exp_cs7000p.c: the one writer of the expander chain and of the shared DAC (exp_cs7000p.h has the bits and the
// interlock's rule). Check and write happen in one interrupt-masked section, so the rule holds whoever calls, from
// any context. Both writers return false, with nothing written, when the interlock says no: cs7000p_exp_set for a
// transmit bit with the DAC above the floor (exp_set_ok), cs7000p_apc_dac for a value above the floor with one
// transmit bit set (exp_dac_ok). cs7000p_apc_dac(0) is off: a true 0 V
bool cs7000p_exp_set(uint32_t set, uint32_t clear);
uint32_t cs7000p_exp_get(void);
bool cs7000p_apc_dac(uint16_t value);
uint16_t cs7000p_apc_dac_get(void);

// pinsetup_cs7000p.c, board.c, power_cs7000p.c
void cs7000p_reset_levels(void);			// every pin's idle state; the LCD connector's lines stay inputs
void cs7000p_lcdbus_lock(void);				// recursive mutex shared by the display bus and the key scan
void cs7000p_lcdbus_unlock(void);
void cs7000p_power_start(void);				// first call of board_init(): the scan, then the hold if the knob is on
cs7000p_knob_t cs7000p_knob(void);
// Between latch(false) and latch(true): a radio still running is in its off state, the knob powering it. The radio
// takes no receiver restore and keeps its speaker amplifier off meanwhile; the LEDs stay off. Any context
bool cs7000p_power_off_state(void);
uint16_t cs7000p_adc_raw(uint8_t rank);
uint16_t cs7000p_adc_mv(uint8_t rank);
// subtone_cs7000p.c registers its detector's batch callback and its generator's tick; either may be NULL; any time
void cs7000p_subaudio_feed(stm32_adc_rank_cb_t batch, void (*tick)(void));
extern const hal_power_t cs7000p_power;
// flash_cs7000p.c, gps_cs7000p.c, diag_cs7000p.c, view_cs7000p.c
void cs7000p_flash_init(void);
void cs7000p_gps_init(void);
bool cs7000p_diag_line(uint8_t i, char *buf, uint8_t len);
void cs7000p_diag_reset(void);
bool cs7000p_view_line(uint8_t i, char *buf, uint8_t len);
// The first of view_cs7000p.c's lines in the second view section (board_t.view_split): the lines before it fill the
// `board` section, whose reply is near its limit (view_cs7000p.c checks the count)
#define CS7000P_VIEW_SPLIT	(36u + (CONFIG_DMR ? 1u : 0u))

// display_cs7000p.c, input_cs7000p.c: the ops, and their state for the views and the diagnostics page
extern const hal_display_t cs7000p_display;
extern const hal_input_t cs7000p_input;
// lcd_board: 0 assumed, 1 unexpected, 2 forced
typedef struct { uint8_t lcd_levels; uint8_t lcd_board; hal_display_panel_t panel; } cs7000p_display_state_t;
void cs7000p_display_state(cs7000p_display_state_t *s);
// board_early_init(): the backlight's PWM on PC9 at duty 0 (the low the pin already is), before any task that changes
// a port C pin's mode exists. The display's init only sets the level afterwards
void cs7000p_backlight_init(void);
typedef struct { uint8_t rows[5]; uint8_t chsw_raw; int8_t chsw_pos; bool ext_ptt_armed, matrix_on; } cs7000p_input_state_t;
void cs7000p_input_state(cs7000p_input_state_t *s);

// radio_cs7000p.c, audio_cs7000p.c (the PCM stream is DMR-only code, so the audio ops are too)
extern const hal_radio_t cs7000p_radio;
#if CONFIG_DMR
extern const hal_audio_t cs7000p_audio;
#endif
bool cs7000p_chip_reg(uint8_t chip, uint8_t page, uint8_t reg, uint32_t *val, bool write);
typedef struct { bool lo_high, pll_locked, ak_ok, hrc_ok, cal_tx, cal_maker; uint8_t pll_r, hrc_r10, hrc_r25; uint32_t vco_hz,
		 unlocks, tx_refused, dac_refused; uint8_t tx_refused_why; uint16_t rx_tune, pc8_duty; int8_t ref_offset; }
	cs7000p_radio_state_t;
void cs7000p_radio_state(cs7000p_radio_state_t *s);	// plain data, no bus traffic
// board_rf_safe() wrote the lines: the sequencer's state follows, no write, inside board_rf_safe()'s masked section
void cs7000p_radio_rf_safe(void);
// The maker's factory table: the factory copy's configuration (board.factory_copy; core/storage/factory_copy.h,
// CONFIG_FACTORY_COPY builds), and the view's two lines of the table in use and the copy's record
#if CONFIG_FACTORY_COPY
extern const factory_copy_cfg_t cs7000p_factory_copy;
#endif
void cs7000p_cal_table_line(char *buf, uint8_t len);
void cs7000p_cal_rec_line(char *buf, uint8_t len);
// ... and the receive side's: the reference level and its source, the tune's and the RSSI scale's sources
void cs7000p_ref_line(char *buf, uint8_t len);
void cs7000p_rx_cal_line(char *buf, uint8_t len);
// ... and the maker's squelch thresholds at the receive frequency, shown only; the owner's transmit arm. Each from a
// copy of its own
void cs7000p_sq_maker_line(char *buf, uint8_t len);
void cs7000p_tx_arm_line(char *buf, uint8_t len);
// The same six as board2's lines k = 0 to 5, in that order, all from one copy of a state that isn't changing: k = 0
// takes it (the reply's first board2 line), the others format it. "<key>=busy" when no copy could be taken
#define CS7000P_CAL_VIEW_LINES	6u
void cs7000p_cal_view_line(uint8_t k, char *buf, uint8_t len);

#endif
