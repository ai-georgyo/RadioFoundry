// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS radio: the hal_radio_t over the RF chain (rfops_cs7000p.c on drivers/rf/chainseq), the modem
// (c6000_cs7000p.c) and the sub-audio code (subtone_cs7000p.c); the two bit-banged buses; the DMR burst runner's
// wiring (platform/stm32/dmr_keyup); the speaker routing; the bench's chip_reg (docs/spec/hardware-cs7000p.md 7, 8,
// 13, 14). Written from the maker's schematic and the chips' data sheets; what has run on a radio is the bring-up
// page's First radio (docs/testing/cs7000p-bringup.md), and nothing of transmit has.
//
// The radio has no calibration of the tree's own, so without the maker's table it refuses every transmission
// (rfcal_cs7000p.h, rfops_cs7000p.h): the one way to key it is the bench's drive override under a test TX limit. The
// maker's factory table is taken at init (the bytes in use the core's factory copy chose, core/storage/factory_copy.h)
// and parsed (makercal_cs7000p.h), and shown. Receive uses it (the tune row, trim A in the held level, the RSSI
// points), each value with today's as its fallback (hardware-cs7000p.md 13.1). The owner's transmit arm
// (hal_radio_t.tx_arm, 13.2) is checked against it and refuses a key-up it would serve until armed: armed, and with a
// test TX limit list naming the frequency, FM keys on its drive, pair and trim A (a tone channel at the depth of the
// table's level for its tone), and DMR (the top stage) on its drive, DMR pair at f_tx and trim B.
//
// The synthesizer's bus is PD8-PD10; the IF receiver's is PE3 (clock) and PC13 (data, both ways) with its select an
// expander bit. Each frame runs with the RTOS interrupt mask held: frames from the radio task, the burst runner's
// interrupt and the debug link's task never mix, and PC13's change of direction never interleaves with another
// task's set-up of a port C pin. PC13 is an output only inside a frame.
//
// Everything here is the radio task's, but: status readers (cs7000p_radio_state, the table's view lines), the bench
// drive (tx_drive, tx_drive_now), chip_reg (the debug link's task), cs7000p_speaker_pcm (the audio task),
// cs7000p_radio_rf_safe (any context), the factory copy's busy() and changed() (the restore's task), the transmit arm
// (the debug link's task) and the burst runner's ops (the modem's and the timer's interrupts).
#include <stddef.h>
#include <string.h>
#include "cs7000p.h"
#include "config.h"
#include "pins.h"
#include "exp_cs7000p.h"
#include "battery_cs7000p.h"
#include "rfplan_cs7000p.h"
#include "rfcal_cs7000p.h"
#include "rfops_cs7000p.h"
#include "speaker_cs7000p.h"
#include "subpwm_cs7000p.h"
#include "makercal_cs7000p.h"
#include "store_cs7000p.h"
#include "hal_time.h"
#include "bus/wire3_gpio/wire3_gpio.h"
#include "rf/sky72310/sky72310.h"
#include "rf/ak2365a/ak2365a.h"
#include "rf/subaudio/subaudio_sel.h"
#include "radio/tx_limit.h"
#include "radio/pa_keyup.h"
#include "dmr_keyup/dmr_keyup.h"
#include "hrc/dmr_glue.h"
#include "storage/storage.h"
#include "storage/factory_copy.h"
#include "debuglink/dlog.h"
#include "debuglink/dl_fmt.h"
#if CONFIG_DEBUGLINK
#include "debuglink/dl_txarm.h"
#endif
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

// 1: the bench's drive override may also key DMR bursts. The burst runner has not run on a radio: a later bench step
#ifndef CS7000P_BENCH_DMR_TX
#define CS7000P_BENCH_DMR_TX	0
#endif

#define PLL_HALF_NS		1000u	// 500 kHz: well inside the synthesizer's timing, 100 ohm in series
#define AK_HALF_NS		2000u	// 250 kHz: the chip needs 500 ns high and low, and the lines have 1k in series
#define AK_RUN_US		1000u	// out of power-down before its reset is released (700 us at least)
#define AK_CAL_MS		3u	// after its set-up: the discriminator's calibration (1.3 ms) and 1.5 ms to settle
#define AK_READ_BITS		10u	// a read frame's samples: one unstable bit, D7..D0, one past the end
// As another radio in this tree with an RSSI line and no squelch of the chip's own (boards/md380/radio_md380.c). A
// placeholder: on this board's unmeasured RSSI scale every reading is over it
#define CARRIER_DBM		(-118)

cs7000p_rf_t cs7000p_rf;
// From here on cs7000p_radio_rf_safe() has a chain to make safe. Before, there is nothing it could turn off
static volatile bool radio_up;
static bool cal_tx;		// the calibration could transmit somewhere (the view's cal=)
static bool cal_maker;		// ... the maker's table in use could, once armed (cal=maker): set at its take
static bool dmr_mode;
static bool tx_on;		// an FM transmission is up
static volatile bool tx_steady;	// ... and neither keying up nor down: the bench may move its drive
static bool dmr_armed;		// the burst runner holds a transmission
static bool asleep;
static bool rx_valid;		// a receive set-up exists to go back to
static radio_bw_t rx_bw;
static uint8_t rssi_src;	// where the last status poll's dBm came from (CS7000P_SRC_*), for the view
// A receive set-up asked for from outside the radio task (the bench's LO side or reference offset): status() runs it
static volatile bool retune_req;
// The bench wrote the modem or the synthesizer (chip_reg chips 0 and 1) since the start: a register no set-up writes
// again (the synthesizer's modulation and gain words, most of the modem's) keeps the bench's value until a restart.
// The owner's arm is refused while it is set (an armed key-up runs on the table's values and the driver's)
static volatile bool bench_chips;

// --- The maker's factory table (calibration-cs7000p.md; hardware-cs7000p.md 13.1) ---

_Static_assert(CS7000P_STORE_TABLE_SIZE == CS7000P_MAKER_SIZE, "the map's first factory range is the maker's table");
_Static_assert((unsigned)CS7000P_MK_WHY_COUNT < (unsigned)FACTORY_COPY_WHY_READ,
	       "the reader's reasons are the board's numbers, 1-0xEF");

// The bytes in use: the factory copy's load fills them before init (the window's, or the record's when the window
// fails the reader), all 0xFF when there are none. Without the copy, init reads the window into them itself
static uint8_t cal_raw[CS7000P_MAKER_SIZE];
// The table in use as parsed, and the copy's state it came with: the radio task's, written at init and when it takes
// a request (cal_req), read by the view's lines
static cs7000p_maker_t maker;
static factory_copy_state_t cal_st;
// The copy's changed() (a restore wrote): the radio task takes it at its status poll, parses the table now in use
// there and sets receive up again. The table in use never changes on another task
static volatile bool cal_req;
#if CONFIG_DEBUGLINK
// The view's generation of the table in use (board2's lines): odd while the radio task changes the table and sets
// receive up on it
static volatile uint32_t cal_gen;
#endif

// A change of the table in use starts, or ends: the view copies nothing in between
static void cal_gen_step(void)
{
#if CONFIG_DEBUGLINK
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	cal_gen = cal_gen + 1u;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

// The copy's checker: the bytes are the maker's when the reader's state is ok and every integrity rule passes (spec
// 7). The port's use rules never fail it: they only drop a field group (makercal_cs7000p.h)
static bool cal_check(const uint8_t *b, uint32_t n, uint8_t *why, uint16_t *off)
{
	return cs7000p_maker_check(b, n, CS7000P_APC_FLOOR, why, off);
}

#if !CONFIG_FACTORY_COPY
static bool all_bytes(const uint8_t *b, uint32_t n, uint8_t v)
{
	for (uint32_t i = 0; i < n; i++) {
		if (b[i] != v) {
			return false;
		}
	}
	return true;
}

// A build without the copy reads the window alone, as the copy's load does: twice, compared, then checked, under the
// storage lock (a CPS write from the debug link's task may be rewriting the window's sector). No record: the window is
// the only source
static void cal_read_window(factory_copy_state_t *s)
{
	const uint32_t a = CS7000P_STORE_TABLE_ADDR, n = CS7000P_MAKER_SIZE;
	uint8_t again[64];
	storage_lock();
	bool read = board.flash != NULL && board.flash->read != NULL && board.flash->size >= a + n &&
		    board.flash->read(a, cal_raw, n);
	bool same = read;
	for (uint32_t off = 0; read && same && off < n; off += sizeof(again)) {
		read = board.flash->read(a + off, again, sizeof(again));
		same = read && memcmp(again, cal_raw + off, sizeof(again)) == 0;
	}
	storage_unlock();
	memset(s, 0, sizeof(*s));
	s->loaded = true;
	if (!read || !same) {
		s->why = read ? FACTORY_COPY_WHY_UNSTABLE : FACTORY_COPY_WHY_READ;
	} else {
		s->win_crc = storage_crc32(0, cal_raw, n);
		if (cal_check(cal_raw, n, &s->why, &s->off)) {
			s->win = FACTORY_COPY_WIN_OK;
			s->source = FACTORY_COPY_SRC_WINDOW;
			return;
		}
		s->win = all_bytes(cal_raw, n, 0xFF) ? FACTORY_COPY_WIN_BLANK :
			 (all_bytes(cal_raw, n, 0) ? FACTORY_COPY_WIN_ZERO : FACTORY_COPY_WIN_REFUSED);
	}
	memset(cal_raw, 0xFF, n);	// no bytes in use
}
#endif

#if CONFIG_DEBUGLINK	// the view and the log
// The names the view and the log show: the copy's, and without it the same words
static const char *cal_src_name(uint8_t src)
{
#if CONFIG_FACTORY_COPY
	return factory_copy_src_name(src);
#else
	return src == FACTORY_COPY_SRC_WINDOW ? "window" : "none";
#endif
}

static const char *cal_win_name(uint8_t win)
{
#if CONFIG_FACTORY_COPY
	return factory_copy_win_name(win);
#else
	static const char *const names[] = { "unread", "blank", "zero", "refused", "ok" };
	return win < sizeof(names) / sizeof(names[0]) ? names[win] : "?";
#endif
}

// The window's reason: one of the copy's own, else the reader's
static const char *cal_why_name(uint8_t why)
{
#if CONFIG_FACTORY_COPY
	const char *n = factory_copy_why_name(why);
	if (n != NULL) {
		return n;
	}
#else
	if (why == FACTORY_COPY_WHY_READ || why == FACTORY_COPY_WHY_UNSTABLE) {
		return why == FACTORY_COPY_WHY_READ ? "read" : "unstable";
	}
#endif
	return cs7000p_maker_why_name(why);
}

static const char *cal_rec_name(uint8_t rec)
{
#if CONFIG_FACTORY_COPY
	return factory_copy_rec_name(rec);
#else
	(void)rec;
	return "off";	// a build without the copy keeps no record
#endif
}

// One line each time the table in use is taken
static void cal_log(const factory_copy_state_t *s)
{
	if (s->source != FACTORY_COPY_SRC_NONE && maker.valid) {
		if (s->source == FACTORY_COPY_SRC_RECORD) {
			dlog("cal: maker table ok src=record crc=0x%08x rec=%s fields=0x%04x win=%s", (unsigned)maker.crc,
			     cal_rec_name(s->rec), maker.fields, cal_win_name(s->win));
		} else {
			dlog("cal: maker table ok src=window crc=0x%08x rec=%s fields=0x%04x", (unsigned)maker.crc,
			     cal_rec_name(s->rec), maker.fields);
		}
	} else if (s->why == FACTORY_COPY_WHY_CONFLICT) {
		dlog("cal: window and record differ: none used");
	} else {
		dlog("cal: maker table %s (%s) at 0x%03x: none used rec=%s", cal_win_name(s->win), cal_why_name(s->why),
		     (unsigned)s->off, cal_rec_name(s->rec));
	}
}
#endif

// The table in use taken: the copy's state now (or the window read here, without the copy), then the parse. Radio
// task only, at init and at the status poll that takes a request; logged once each time
static void cal_take(void)
{
	factory_copy_state_t s;
#if CONFIG_FACTORY_COPY
	factory_copy_get(&s);	// not loaded (no configuration): all zero, the source none
#else
	cal_read_window(&s);
#endif
	const bool any = s.source != FACTORY_COPY_SRC_NONE;
	const uint32_t crc = s.source == FACTORY_COPY_SRC_WINDOW ? s.win_crc :
			     (s.source == FACTORY_COPY_SRC_RECORD ? s.rec_crc : 0u);
	(void)cs7000p_maker_parse(any ? cal_raw : NULL, crc, CS7000P_APC_FLOOR, &maker);
	cal_maker = cs7000p_cal_maker_any_fm(&maker) || (CONFIG_DMR && cs7000p_cal_maker_any_dmr(&maker));
	cal_st = s;
#if CONFIG_FACTORY_COPY
	factory_copy_taken();	// cal_raw is read: a restore may write it again from here on
#endif
#if CONFIG_DEBUGLINK
	cal_log(&s);
#endif
}

#if CONFIG_FACTORY_COPY
// The restore (settings-store.md 4.2) is refused while a transmission is up, a table it wrote is not taken yet, or the
// owner's transmit arm is not off: the arm was made against the table in use, which a restore would change
static bool cal_busy(void)
{
	return tx_on || dmr_armed || cal_req || cs7000p_rf.arm != CS7000P_ARM_OFF;
}

// Only the request: the table in use changes at the radio task's next status poll
static void cal_changed(void)
{
	cal_req = true;
}

// The maker's table is the map's first factory range (store_cs7000p.c); the copy's record is the board sector
const factory_copy_cfg_t cs7000p_factory_copy = {
	.range = 0,
	.record_addr = CS7000P_STORE_CAL_ADDR,
	.check = cal_check,
	.busy = cal_busy,
	.changed = cal_changed,
	.table = cal_raw,
};
#endif

// --- The synthesizer's bus ---

static void pll_cs(void *ctx, uint8_t cs, bool level)
{
	(void)ctx;
	(void)cs;
	PIN_WRITE(PIN_PLL_CS, level);
	if (level) {
		PIN_WRITE(PIN_PLL_DAT, false);	// parked low between frames
	}
}

static void pll_clk(void *ctx, bool level)
{
	(void)ctx;
	PIN_WRITE(PIN_PLL_CLK, level);
}

static void pll_data(void *ctx, bool level)
{
	(void)ctx;
	PIN_WRITE(PIN_PLL_DAT, level);
}

static void pll_delay(void *ctx)
{
	(void)ctx;
	stm32_wait_ns(PLL_HALF_NS);
}

static const wire3_gpio_t pll_wire = { .cs = pll_cs, .clk = pll_clk, .data = pll_data, .delay = pll_delay };

// One 16-bit word. Also from the burst runner's interrupt (the carrier's words at a burst's tune)
static bool pll_write(void *ctx, uint16_t word)
{
	(void)ctx;
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	bool ok = wire3_gpio_write((void *)&pll_wire, 0, word, 16);
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	return ok;
}

static bool pll_locked(void *ctx)
{
	(void)ctx;
	return PIN_READ(PIN_PLL_LD);
}

static const sky72310_bus_t pll_bus = { .write = pll_write, .locked = pll_locked, .delay_us = hal_delay_us };
static sky72310_t pll;

// --- The IF receiver's bus ---

static bool ak_data_out;	// PC13 is an output: only inside a frame

static void ak_cs(void *ctx, uint8_t cs, bool level)
{
	(void)ctx;
	(void)cs;
	(void)cs7000p_exp_set(level ? EXP_AK_CSN : 0u, level ? 0u : EXP_AK_CSN);
	if (level) {
		// The frame is over. PC13 is a backup-domain pad, and PC14 next to it is one too and stays an output:
		// back to an input with its pull-down (pins.h)
		stm32_pin_in(PIN_AK_SDATA, GPIO_PULLDOWN);
		ak_data_out = false;
	}
}

static void ak_clk(void *ctx, bool level)
{
	(void)ctx;
	PIN_WRITE(PIN_AK_SCLK, level);
}

static void ak_data(void *ctx, bool level)
{
	(void)ctx;
	if (ak_data_out) {
		PIN_WRITE(PIN_AK_SDATA, level);
	} else {
		stm32_pin_out(PIN_AK_SDATA, level, false);	// the level first, then the direction
		ak_data_out = true;
	}
}

static void ak_data_release(void *ctx, bool pull_up)
{
	(void)ctx;
	stm32_pin_in(PIN_AK_SDATA, pull_up ? GPIO_PULLUP : GPIO_PULLDOWN);
	ak_data_out = false;
}

static bool ak_data_in(void *ctx)
{
	(void)ctx;
	return PIN_READ(PIN_AK_SDATA);
}

static void ak_delay(void *ctx)
{
	(void)ctx;
	stm32_wait_ns(AK_HALF_NS);
}

static const wire3_gpio_t ak_wire = {
	.cs = ak_cs, .clk = ak_clk, .data = ak_data, .data_release = ak_data_release, .data_in = ak_data_in,
	.delay = ak_delay,
};

// One frame, about 0.12 ms with the two expander updates of its select, the interrupt mask held throughout
static bool ak_write(uint16_t word)
{
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	bool ok = wire3_gpio_write((void *)&ak_wire, 0, word, AK2365A_WORD_BITS);
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	return ok;
}

#if CONFIG_DEBUGLINK
// A register read for the bench: the read bit and the 6-bit address out, then the samples after the falling edges
// of clocks 7 to 16. By the data sheet's drawing the value is bits 8..1 of them; no radio has confirmed where the
// bits fall (docs/testing/cs7000p-bringup.md: write 0x5A and 0xA5 to 0x0B, read with both pulls)
static bool ak_read(uint8_t reg, bool pull_up, uint8_t *val)
{
	uint32_t v = 0;
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	bool ok = wire3_gpio_read(&ak_wire, 0, 0x40u | (reg & 0x3Fu), 7, pull_up, AK_READ_BITS, &v);
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	*val = (uint8_t)(v >> 1);
	return ok;
}
#endif

// --- The chain's callbacks (rfops_cs7000p.h) ---

static bool io_exp(void *ctx, uint32_t set, uint32_t clear)
{
	(void)ctx;
	return cs7000p_exp_set(set, clear);
}

// The shared DAC: the one way this file and the chain reach it (the interlock is behind it, exp_cs7000p.c)
static bool io_dac(void *ctx, uint16_t v)
{
	(void)ctx;
	return cs7000p_apc_dac(v);
}

static bool io_synth(void *ctx, uint8_t ref_div, uint32_t vco_hz)
{
	(void)ctx;
	return sky72310_set_vco(&pll, ref_div, vco_hz);
}

// The driver's words in its order, written here and not by ak2365a_setup(): the chip wants a wait between two of
// them (rfplan_cs7000p.h), and a frame of this bus is far shorter than that wait. The port's words, or for a receive
// test the maker's for the mode (chip_reg 3:7): the chip is the receiver's alone, so nothing keyed sees them
static bool io_if_setup(void *ctx, bool wide)
{
	const ak2365a_cfg_t *c = !cs7000p_rf.if_maker ? &cs7000p_if_cfg
			       : (dmr_mode ? &cs7000p_if_cfg_maker_dmr : &cs7000p_if_cfg_maker_fm);
	uint16_t w[AK2365A_SETUP_WORDS];
	uint8_t n = ak2365a_setup_words(c, wide, w);
	bool ok = true;
	(void)ctx;
	for (uint8_t i = 0; i < n; i++) {
		uint32_t us = cs7000p_if_word_wait_us(w[i]);
		if (us != 0u) {
			hal_delay_us(us);
		}
		ok = ak_write(w[i]) && ok;
	}
	if (!ok) {
		return false;
	}
	hal_delay_ms(AK_CAL_MS + 1u);	// a tick more: the first one of a sleep may be nearly over
	return true;
}

static bool io_locked(void *ctx)
{
	(void)ctx;
	return PIN_READ(PIN_PLL_LD);
}

// Task level only (the chain's burst pieces never wait). Two milliseconds and more sleep the task, with a tick
// added so the wait is never short; anything less is a busy wait, exact: the ramp's steps and the rails' leads
static void io_delay(void *ctx, uint32_t us)
{
	(void)ctx;
	if (us >= 2000u) {
		hal_delay_ms(us / 1000u + 1u);
	} else if (us != 0u) {
		hal_delay_us(us);
	}
}

static const cs7000p_rf_io_t rf_io = {
	.exp_set = io_exp, .dac = io_dac, .synth = io_synth, .if_setup = io_if_setup, .locked = io_locked,
	.delay_us = io_delay,
};

// --- Speaker ---

// Two callers: the core's squelch (audio, the radio task) and the PCM route (cs7000p_speaker_pcm, the audio task).
// The mutex keeps their expander and modem writes from interleaving
static bool fm_wanted, pcm_wanted;
static SemaphoreHandle_t speaker_lock;

void cs7000p_radio_speaker(void)
{
	bool locked = speaker_lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
	if (locked) {
		xSemaphoreTake(speaker_lock, portMAX_DELAY);
	}
	// In the power's off state (the knob overrode a power-off) nothing plays: the amplifier stays off
	const bool off = cs7000p_power_off_state();
	cs7000p_speaker_t s = cs7000p_speaker_lines(fm_wanted && !off, pcm_wanted && !off, cs7000p_c6000_fm_rx());
	if (!s.feed) {
		cs7000p_c6000_fm_audio(false);	// the receiver's audio shut before the amplifier comes on for a beep
	}
	// The off state asked again with the write, in one masked section. latch(false) runs in a task that preempts
	// this one (or shares its priority): it sets the off state, then clears the amplifier, so whichever comes
	// first here, the amplifier ends off
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	if (!off && cs7000p_power_off_state()) {
		s = cs7000p_speaker_lines(false, false, false);
	}
	(void)cs7000p_exp_set(s.set, s.clear);
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	if (s.feed) {
		cs7000p_c6000_fm_audio(true);
	}
	if (locked) {
		xSemaphoreGive(speaker_lock);
	}
}

void cs7000p_speaker_pcm(bool on)
{
	if (on != pcm_wanted) {
		pcm_wanted = on;
		cs7000p_radio_speaker();
	}
}

static void audio(bool on)
{
	fm_wanted = on;
	cs7000p_radio_speaker();
}

// --- Receive ---

#if CONFIG_SUBTONE
static subtone_t rx_tone_now;	// the selection the detector is set for
#endif

// A receive set-up (hardware-cs7000p.md 7.8): the chain (rfops_cs7000p.c: safe, the first LO, the receive rail, the
// IF chip, the tuning value, the audio rail), then the modem for the mode and the frequency, then the sub-audio
// detector
static bool rx_set(uint32_t hz, radio_bw_t bw, const subtone_t *tone)
{
	// DMR is a 12.5 kHz signal whatever the channel says
	bool wide = bw == RADIO_BW_25K && !dmr_mode;
#if CONFIG_SUBTONE
	bool same = rx_valid && hz == cs7000p_rf.seq.rx_hz && wide == cs7000p_rf.seq.wide;
#endif
	retune_req = false;
	if (!cs7000p_rf_rx(&cs7000p_rf, hz, wide)) {
		return false;
	}
	rx_valid = true;
	rx_bw = bw;
	asleep = false;
	cs7000p_c6000_mode(dmr_mode, hz);
	cs7000p_c6000_retune(hz);
#if CONFIG_SUBTONE
	// NULL (back from transmit, a wake-up) keeps the last selection. The same selection on the same channel keeps
	// its detector: a restart closes the squelch until the tone has been heard anew
	if (tone != NULL && (!same || !subaudio_same(tone, &rx_tone_now))) {
		rx_tone_now = *tone;
		cs7000p_subtone_rx_set(&rx_tone_now);
	} else if (!same) {
		cs7000p_subtone_rx_set(&rx_tone_now);
	}
	cs7000p_subtone_rx_run(true);
#else
	(void)tone;
#endif
	return true;
}

// The last receive set-up once more, in full (after a key-up that did not happen, a safe request, a bench change)
static void rx_again(void)
{
	if (rx_valid) {
		(void)rx_set(cs7000p_rf.seq.rx_hz, rx_bw, NULL);
	}
}

// --- Transmit ---

// The microphone for a transmission, FM or DMR alike: its amplifier's supply (MICPOW_C) and the internal microphone's
// switch (MIC-C), the modem's codec input behind them. The external microphone's switch is never set, as FM never
// set it: nothing here tells when one is plugged in. Neither bit is a transmit bit, so the interlock never refuses them
static void mic(bool on)
{
	(void)cs7000p_exp_set(on ? EXP_MIC_POWER | EXP_MIC_INT : 0u, on ? 0u : EXP_MIC_POWER | EXP_MIC_INT);
}

// A transmission's parts around the chain, before it comes down: the tone, the bench's access to the drive, the
// burst runner. The runner's disarm ends a burst without a ramp, so it is only called where bursts were armed
static void tx_parts_stop(void)
{
#if CONFIG_SUBTONE
	cs7000p_subtone_tx_stop();
#endif
	tx_steady = false;
	if (dmr_armed) {
		dmr_armed = false;
		dmr_keyup_disarm();
	}
}

// ... and after it is down: the microphone, whichever mode had it, and the modem back to FM receive
static void tx_parts_end(void)
{
	mic(false);
	if (tx_on) {
		tx_on = false;
		cs7000p_c6000_fm_tx_end();
	}
}

// keep: the end of a DMR transmission whose last switch already put the receiver back (the glue's tx_end): the RF
// stays as it is when it is receiving, and only the modem's reference hold is written again
static void tx_end(bool keep)
{
	tx_parts_stop();
	cs7000p_rf_tx_off(&cs7000p_rf, keep);
	tx_parts_end();
	if (!rx_valid) {
		return;
	}
	if (cs7000p_rf.seq.state != CHAINSEQ_ST_RX) {
		rx_again();	// the way back failed: once more from the start
	} else if (keep) {
		cs7000p_c6000_rx_hold();
	} else {
		// The chain did its whole receive set-up; the modem's part of one and the detector follow
		cs7000p_c6000_retune(cs7000p_rf.seq.rx_hz);
#if CONFIG_SUBTONE
		cs7000p_subtone_rx_run(true);
#endif
	}
}

static void tx_off(void)
{
	tx_end(false);
}

void cs7000p_radio_dmr_tx_end(void)
{
	tx_end(true);
}

// The DMR key-up's constants are the debug link's (RADIO op 37, or a key run's), not the board's: on the table's
// values the bursts' key-up would be the bench's (a boost over the drive), and the owner's arm is refused meanwhile
static bool keyup_bench(void)
{
#if CONFIG_DMR
	return pa_keyup_custom();
#else
	return false;
#endif
}

// What the transmit check needs to know now
static cs7000p_tx_req_t tx_request(uint32_t hz, uint8_t power, bool dmr)
{
	uint32_t bat = cs7000p_battery_mv(cs7000p_adc_mv(CS7000P_ADC_POW_DET));	// 0 without a reading: refused
	cs7000p_tx_req_t q = {
		.hz = hz,
		.power = power,
		.dmr = dmr,
		.now_ms = hal_millis(),
		.battery_mv = (uint16_t)(bat > 0xFFFFu ? 0xFFFFu : bat),
		.battery_critical_mv = board.battery_critical_mv,
		.debuglink = CONFIG_DEBUGLINK != 0,
		.bench_dmr = CS7000P_BENCH_DMR_TX != 0,
		.off = cs7000p_power_off_state(),	// asked here, so FM's key-up and the burst plan both refuse
		.ref_other = cs7000p_c6000_ref_coding() != CS7000P_REF_HOLD,
		.pc8_bench = cs7000p_subtone_pc8_idle() != CS7000P_SUBPWM_IDLE,
		.bench_chips = bench_chips,
		.bench_keyup = dmr && keyup_bench(),
	};
#if CONFIG_DEBUGLINK
	// A test TX limit as a list that names this frequency: the bench harness set it (tools/power_cal.py). The core
	// has matched it already; a block, or no limit at all, is not the bench
	tx_limit_info_t limit;
	tx_limit_get(&limit, NULL, 0);
	q.limit_list = limit.on && !limit.block && limit.n != 0u && tx_limit_allows(hz);
	// A raw SoC register written through the debug link since the start (dl_txarm.h): as a chip's
	q.bench_chips = q.bench_chips || dl_soc_written();
#endif
	return q;
}

#if CONFIG_DEBUGLINK
// The transmission the core last asked for, admitted or refused, for board2.tx_maker: the core gives the board a
// channel's transmit frequency, power level and tone only in tx(). It stands while the receive set-up it was asked
// from does (the same receive frequency, width and mode): a channel change, a scan step or a beacon's retune drops it.
// The radio task writes it; the view reads it under the interrupt mask, below that task's priority
static struct {
	bool set, dmr, wide;
	uint8_t power;
	radio_bw_t rx_bw;
	uint32_t hz, rx_hz;
	subtone_t tone;			// TONE_NONE: none (and always in DMR)
} tx_asked;

static void tx_note(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)
{
	tx_asked.set = rx_valid;
	tx_asked.dmr = dmr_mode;
	tx_asked.wide = bw == RADIO_BW_25K && !dmr_mode;	// DMR is a 12.5 kHz signal whatever the channel says
	tx_asked.power = power;
	tx_asked.rx_bw = rx_bw;
	tx_asked.hz = hz;
	tx_asked.rx_hz = cs7000p_rf.seq.rx_hz;
	tx_asked.tone = tone != NULL && !dmr_mode ? *tone : (subtone_t){ TONE_NONE, 0 };
}
#endif

static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
#if CONFIG_DEBUGLINK
	tx_note(hz, bw, power, tx_tone);	// for the view only: nothing below reads it
#endif
	if (tx_on) {
		tx_end(false);	// keyed already (a retune): that transmission ends in order first
	}
	if (dmr_mode) {
#if CONFIG_DMR
		// Arm only: the receiver stays until the glue has the transmit side made ready. The check is the runner's
		// plan (keyup_plan below); a refusal behind an earlier arm leaves the RF wherever a burst put it
		bool was_armed = dmr_armed;
		dmr_armed = dmr_keyup_arm(hz, power);
		// The microphone from the arm on, as FM's from its key-up: the vocoder takes it from the modem's codec,
		// and its bias settles before the first burst. A refusal takes it away again, an earlier arm's too
		mic(dmr_armed);
		if (!dmr_armed && cs7000p_rf.admit) {
			// The plan said yes and the runner no (the modem's physical layer mode holds the chip, no ops)
			cs7000p_rf_refuse(&cs7000p_rf, CS7000P_TX_MODEM);
		}
		if (!dmr_armed && was_armed) {
			// The runner dropped the earlier arm too, and the core does not call tx_off() after a refusal: the
			// whole receive set-up here (it brings a transmit side that is still tuned down first)
			(void)cs7000p_rf_drive(&cs7000p_rf, 0, 0);
			rx_again();
		}
		return dmr_armed;
#else
		return false;
#endif
	}
	cs7000p_tx_req_t q = tx_request(hz, power, false);
	// Whatever the build sends: a tone channel is one on the maker's values too, which need its level
	q.tone = tx_tone != NULL && tx_tone->type != TONE_NONE;
	if (q.tone) {
		q.tone_sel = *tx_tone;
	}
	if (cs7000p_rf_tx_admit(&cs7000p_rf, &q) != CS7000P_TX_OK) {
		return false;	// nothing was written
	}
#if CONFIG_SUBTONE
	// The tone's depth before anything is written: on the maker's values the table's level at f_tx, which the arm
	// still covers, or no key-up
	uint16_t depth_pm = 0;
	if (q.tone && !cs7000p_rf_tone_depth(&cs7000p_rf, hz, bw == RADIO_BW_25K, tx_tone, &depth_pm)) {
		cs7000p_rf_refuse(&cs7000p_rf, CS7000P_TX_UNCAL);
		return false;
	}
#endif
	// The DAC to 0 and the receiver off, then the modulation, then the RF: the PA keys last
	if (!cs7000p_rf_tx_begin(&cs7000p_rf)) {
		rx_again();
		return false;
	}
	// A key-up out of the receiver's sleep ends the sleep: the way back from here is a whole receive set-up, and
	// a flag left standing would turn down every later rx_sleep(true) and hold back status()'s restore
	asleep = false;
#if CONFIG_SUBTONE
	cs7000p_subtone_rx_run(false);
#endif
	if (!cs7000p_c6000_fm_tx(hz, bw == RADIO_BW_25K)) {
		cs7000p_rf_refuse(&cs7000p_rf, CS7000P_TX_MODEM);
		rx_again();
		return false;
	}
	tx_on = true;
	mic(true);
	if (cs7000p_rf_tx_key(&cs7000p_rf) != CS7000P_TX_OK) {
		// Not locked, or a write refused: the chain is safe and receiving again. The rest of a receive set-up
		tx_parts_end();
		rx_again();
		return false;
	}
	tx_steady = true;
#if CONFIG_SUBTONE
	if (tx_tone != NULL) {
		cs7000p_subtone_tx_start(tx_tone, depth_pm);
	}
#else
	(void)tx_tone;
#endif
	return true;
}

#if CONFIG_SUBTONE
// The DCS turn-off tail from the PWM generator; the core holds the carrier for it
static bool tx_tail(bool on)
{
	return tx_on && cs7000p_subtone_tx_tail(on);
}
#endif

// --- The PA drive for the bench (docs/testing/power-calibration.md) ---

static uint16_t tx_drive_level(uint32_t hz, uint8_t level)
{
	return cs7000p_rf_drive_level(&cs7000p_rf, hz, level);
}

// Arms only. Whether the next transmission may use it is the transmit check's to say (rfops_cs7000p.c). Refused
// while the owner's arm holds the PA (cs7000p_rf_arm_holds): the bench drive and the arm exclude each other
static bool tx_drive(uint16_t drive)
{
	return cs7000p_rf_drive(&cs7000p_rf, drive, hal_millis());
}

static uint16_t tx_drive_top(void)
{
	return cs7000p_rf_drive_top(&cs7000p_rf);
}

// The debug link's task while the radio task keeps the transmission. Under the mask the radio task can't start
// the key-down's ramp between the test and the write, and it clears tx_steady before it does
static bool tx_drive_now(uint16_t drive)
{
	if (drive > CS7000P_DAC_MAX) {
		return false;
	}
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	bool ok = tx_steady && cs7000p_rf_drive_now_ok(&cs7000p_rf, drive) && chainseq_drive_now(&cs7000p_rf.seq, drive);
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	return ok;
}

// --- DMR bursts (platform/stm32/dmr_keyup) ---

#if CONFIG_DMR
// One fixed point: the PA keys with the level's drive CS7000P_KEYUP_PA_US before the first symbol. Unmeasured
static const pa_keyup_cfg_t keyup_cfg = PA_KEYUP_FIXED(CS7000P_KEYUP_PA_US);

static bool keyup_tx_tune(void *ctx, uint32_t hz)
{
	(void)ctx;
	return chainseq_burst_tune(&cs7000p_rf.seq, hz);
}

// Task level, after a tune: the lock line as the chain reads it, three reads in a row, 5 ms at most. The PA's point
// asks again at each burst, so a synthesizer that is not locked by then keys nothing
static uint32_t keyup_settle(void *ctx)
{
	uint32_t us = 0;
	uint8_t run = 0;
	(void)ctx;
	for (;;) {
		run = PIN_READ(PIN_PLL_LD) ? (uint8_t)(run + 1u) : 0u;
		if (run >= CHAINSEQ_LOCK_READS || us >= CS7000P_LOCK_US) {
			return us;
		}
		hal_delay_us(CHAINSEQ_LOCK_POLL_US);
		us += CHAINSEQ_LOCK_POLL_US;
	}
}

static bool keyup_point(void *ctx, uint8_t kind, uint16_t drive)
{
	(void)ctx;
	return chainseq_burst_point(&cs7000p_rf.seq, kind, drive);
}

static void keyup_pa_off(void *ctx)
{
	(void)ctx;
	chainseq_burst_off(&cs7000p_rf.seq);
}

// The receiver back at a transmission's end, and the modem's reference hold with it (one bus write)
static bool keyup_rx_back(void *ctx)
{
	(void)ctx;
	bool ok = chainseq_burst_rx(&cs7000p_rf.seq);
	cs7000p_c6000_rx_hold();
	return ok;
}

// dmr_keyup_arm()'s question, task level: the same check as FM's key-up. False: nothing is armed
static bool keyup_plan(void *ctx, uint32_t hz, uint8_t power, pa_keyup_t *ku)
{
	uint16_t drive, top, floor;
	bool up;
	uint8_t r10, r25;
	(void)ctx;
	cs7000p_tx_req_t q = tx_request(hz, power, true);
	if (cs7000p_rf_tx_admit(&cs7000p_rf, &q) != CS7000P_TX_OK) {
		return false;
	}
	cs7000p_c6000_state(&up, &r10, &r25);
	if (!up) {
		cs7000p_rf_refuse(&cs7000p_rf, CS7000P_TX_MODEM);
		return false;
	}
	// The drives first: a plan the runner refuses (a drive of 0: the arm lowered since the check) writes nothing
	cs7000p_rf_burst_drives(&cs7000p_rf, &drive, &top, &floor);
	*ku = pa_keyup_plan(drive, top, floor);
	if (ku->drive == 0u) {
		return false;
	}
	if (cs7000p_rf.admit_arm && pa_keyup_custom()) {
		// The debug link's constants since the admission asked (op 37 is not refused while armed): the plan may hold
		// the bench's boost, over the table's drive. Nothing written yet
		cs7000p_rf_refuse(&cs7000p_rf, CS7000P_TX_ARM_BENCH);
		return false;
	}
	// The pair and 4:0x04 for f_tx before the first burst: the receive entries wrote f_rx's pair (a split channel's
	// differs) and 4:0x04 for receive. On the maker's values its pair or nothing: never the safe gain under its drive
	bool pair = false;
	if (!cs7000p_c6000_dmr_tx(hz, &pair) || (cs7000p_rf.admit_arm && !pair)) {
		cs7000p_rf_refuse(&cs7000p_rf, CS7000P_TX_MODEM);
		return false;
	}
	return true;
}

static const pa_keyup_cfg_t *keyup_cfg_for(void *ctx, uint32_t hz)
{
	(void)ctx;
	(void)hz;
	return &keyup_cfg;	// one band, one PA
}

static const dmr_keyup_ops_t keyup_ops = {
	.tx_tune = keyup_tx_tune,
	.settle_us = keyup_settle,
	.point = keyup_point,
	.pa_off = keyup_pa_off,
	.rx_back = keyup_rx_back,
	.plan = keyup_plan,
	.cfg = keyup_cfg_for,
	.loop_lead_us = CS7000P_KEYUP_LOOP_LEAD_US,
	.int_ahead_us = CS7000P_KEYUP_INT_AHEAD_US,
};
#endif

// --- hal_radio_t ---

// Task level, after board_init(): the clock runs (the burst runner's timer reads it) and the ADC scan is up
static void init(void)
{
	// The table in use: the factory copy's load ran before (app.c); without the copy, the window read here. Before
	// the chain, and nothing of it is applied yet
	cal_gen_step();
	cal_take();
	if (speaker_lock == NULL) {
		speaker_lock = xSemaphoreCreateMutex();
	}
	cs7000p_rf_init(&cs7000p_rf, &rf_io, &cs7000p_cal_model);	// the chain safe
	// The table in use for receive, from the modem's first set-up on. Always this one struct: a later cal_take()
	// parses into it on this task, and the receive set-up that follows takes the new values
	cs7000p_rf.maker = &maker;
	cal_gen_step();
	bench_chips = false;	// both chips set up anew below (the modem's reset is the MCU's)
#if CONFIG_DEBUGLINK
	tx_asked.set = false;	// no key-up asked at this start yet
#endif
	radio_up = true;
	cal_tx = cs7000p_cal_any_tx(&cs7000p_cal_model);
	// The synthesizer rail and its settle time. The reference oscillator's control voltage is only defined with
	// this rail on, and the transmit check counts the reference's settle time from here
	(void)cs7000p_rf_start(&cs7000p_rf, hal_millis());
	sky72310_attach(&pll, &pll_bus, CS7000P_F_REF_HZ, CS7000P_PLL_PD_GAIN);
	pll.mux = CS7000P_PLL_MUX;
	(void)sky72310_init(&pll);
#if CONFIG_DMR
	// The earliest point the runner can run: the interrupt's lead less the loop's and the timer's shortest wait
	pa_keyup_init(&keyup_cfg, CS7000P_KEYUP_INT_AHEAD_US - CS7000P_KEYUP_LOOP_LEAD_US - DMR_KEYUP_MIN_US,
		      CS7000P_DAC_MAX);
	dmr_keyup_init(&keyup_ops);
#endif
	(void)cs7000p_c6000_init();
	// The IF receiver: out of power-down, then its reset released (the line was low since the pins were set up:
	// the hardware reset the chip needs after power-up). Its select stays high until the first set-up
	(void)cs7000p_exp_set(EXP_AK_RUN, 0);
	hal_delay_us(AK_RUN_US);
	PIN_WRITE(PIN_AK_RSTN, true);
#if CONFIG_SUBTONE
	cs7000p_subtone_init();
#endif
}

static void set_mode(radio_mode_t mode)
{
	bool dmr = CONFIG_DMR != 0 && mode == RADIO_MODE_DMR;
	if (tx_on || dmr_armed) {
		tx_end(false);	// a transmission does not outlive its mode
	}
	if (cs7000p_rf.if_maker && dmr != dmr_mode) {
		retune_req = true;	// the maker's IF words are the mode's: the receiver set up again (status())
	}
	dmr_mode = dmr;
	cs7000p_c6000_mode(dmr, cs7000p_rf.seq.rx_hz);
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone)
{
	if (!cs7000p_rx_band(hz)) {
		// Refused, and nothing set up on hz. The core also calls rx() to end a DMR transmission (the hotspot's
		// retune): one that is up ends in order all the same, back on the last receive set-up
		if (tx_on || dmr_armed) {
			tx_end(false);
		}
		return false;
	}
	// A transmission still up: its parts first, and the chain brings its RF down in order inside the receive set-up
	bool was_tx = tx_on || dmr_armed;
	tx_parts_stop();
	bool ok = rx_set(hz, bw, rx_tone);
	if (was_tx) {
		(void)cs7000p_rf_drive(&cs7000p_rf, 0, 0);	// the bench drive was that transmission's
		tx_parts_end();
	}
	return ok;
}

// Receiver power saving, FM only: the receive rail off. The synthesizer rail and the IF chip stay on, so the
// reference and the IF calibration hold; the line-out amplifier is on the receive rail, so nothing plays meanwhile
static void rx_sleep(bool sleep)
{
	if (dmr_mode || tx_on || !rx_valid || sleep == asleep) {
		return;
	}
	cs7000p_rf_sleep(&cs7000p_rf, sleep);
	asleep = sleep;
#if CONFIG_SUBTONE
	cs7000p_subtone_rx_run(!sleep);
#endif
	if (!sleep) {
		cs7000p_c6000_retune(cs7000p_rf.seq.rx_hz);	// a receive entry: the modem's reference hold again
	}
}

static void status(radio_status_t *st)
{
	// A table a restore wrote: taken here, never under a transmission, then a receive set-up as a bench change asks.
	// busy() holds until it is taken, so no second request lands in between. The view sees the change whole: the
	// table, the set-up on it and this poll's RSSI source
	const bool take = cal_req && !tx_on && !dmr_armed;
	if (take) {
		cal_gen_step();
		cal_take();
		// The parse's stores before the request's end: the transmit arm reads the table once cal_req is false
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		cal_req = false;
		retune_req = true;
	}
	// A safe request from outside (a power-off the knob overrode leaves the chain idle and the amplifier off), or
	// a bench change that wants a retune: the receiver set up again, here, in the radio task. The request is taken
	// only here, and before the set-up: one that lands during it asks again, and one that came during a
	// transmission or the receiver's sleep waits for their end. In the power's off state it waits too: the radio
	// stays dark and quiet until latch(true) ends that state or the knob's restart does
	if (rx_valid && !tx_on && !dmr_armed && !asleep && !cs7000p_power_off_state()) {
		bool safe = cs7000p_rf_restore_take(&cs7000p_rf);
		if (safe || retune_req) {
			rx_again();
			cs7000p_radio_speaker();
		}
	}
	bool receiving = cs7000p_rf.seq.state == CHAINSEQ_ST_RX;
	// The table's two RSSI points when they passed, else (or with chip_reg 3:8) the data sheet's line. The S-meter,
	// the DMR scan's strength test, roaming's leave threshold and the hotspot's report read it; the FM squelch reads
	// the noise
	st->rssi_dbm = cs7000p_rf_rssi(&cs7000p_rf, cs7000p_adc_raw(CS7000P_ADC_RSSI), cs7000p_adc_mv(CS7000P_ADC_RSSI),
				       &rssi_src);
	if (take) {
		cal_gen_step();
	}
	st->noise = cs7000p_noise(cs7000p_adc_raw(CS7000P_ADC_NOISE), receiving);
	st->carrier = receiving && st->rssi_dbm >= CARRIER_DBM;
#if CONFIG_SUBTONE
	st->tone_ok = cs7000p_subtone_rx_match();	// true for a channel without a selection
#else
	st->tone_ok = true;
#endif
}

#if CONFIG_REF_TRIM
// Taken by the plan at the next tune (rfplan_cs7000p.h), so the synthesizer's bus stays the radio task's: true
// asks the core for one
static bool ref_trim(int16_t ppb)
{
	cs7000p_rf.trim_ppb = ppb;
	return true;
}
#endif

#if CONFIG_DEBUGLINK
// --- The owner's transmit arm (hardware-cs7000p.md 13.2), from the debug link's task ---

_Static_assert((int)CS7000P_ARM_OFF == (int)HAL_TX_ARM_OFF && (int)CS7000P_ARM_FM_LOW == (int)HAL_TX_ARM_FM_LOW &&
	       (int)CS7000P_ARM_FM == (int)HAL_TX_ARM_FM && (int)CS7000P_ARM_ALL == (int)HAL_TX_ARM_ALL,
	       "the arm's stages are the hal's");
// DMR's stage only where DMR is built
#define ARM_MAX	(CONFIG_DMR ? CS7000P_ARM_ALL : CS7000P_ARM_FM)

static const char *arm_name(uint8_t stage)
{
	static const char *const names[] = { "off", "fm-low", "fm", "dmr" };
	return stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "?";
}

// The table in use is about to change: a restore's job pending or running, its outcome not yet taken by the copy, or
// its changed() not yet taken here. An arm made now would be checked against the table before it
static bool cal_pending(void)
{
#if CONFIG_FACTORY_COPY
	return cal_req || factory_copy_restore_busy();
#else
	return cal_req;
#endif
}

// The table in use: its range, its CRC-32 (the window's or the record's, whichever it came from), valid or not. The
// radio task changes it only in a take, which it ends before it waits, so this task never reads one half made
static void tx_arm_info(hal_tx_arm_info_t *info)
{
	*info = (hal_tx_arm_info_t){
		.stage = cs7000p_rf.arm,
		.max = ARM_MAX,
		.addr = CS7000P_STORE_TABLE_ADDR,
		.len = CS7000P_MAKER_SIZE,
		.crc = maker.valid ? maker.crc : 0u,
		.cal_ok = maker.valid,
	};
}

// Raised (or the same stage again): the handshake, id the CRC-32 of the table in use. The states that refuse it: the
// chain not up, the maker's coding asked for (3:5) or still held, a bench PC8 idle (3:2), a stored reference trim or
// the bench's reference offset (3:4) set or still held (trim A is the table's whole correction of the reference), a
// bench write to the modem, the synthesizer or (the debug link's note) a SoC register since the start, a bench drive
// armed or in use or the debug link's DMR key-up constants (the bench's PA and the arm exclude each other), the
// power's off state, a restore pending. Each is asked under the interrupt mask with the change itself, so a power-off
// (its off state, then a safe request that clears the arm) can't land between them. Lowered or off: always taken,
// keeping the id it was armed with; a transmission the new stage no longer covers ends at once, by the safe request
// (which clears the arm; the new stage is set after it). A change of stage asks for a receive set-up at the next
// status poll (the values written at receive entries follow the arm); the same stage again asks for none
static uint8_t tx_arm(uint8_t stage, uint32_t id)
{
	if (stage > ARM_MAX) {
		return HAL_TX_ARM_STAGE;
	}
	uint8_t why = HAL_TX_ARM_OK;
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	const uint8_t was = cs7000p_rf.arm;
	if (stage != CS7000P_ARM_OFF && stage >= was) {
		if (!radio_up || cs7000p_rf.ref_coding != CS7000P_REF_HOLD ||
		    cs7000p_c6000_ref_coding() != CS7000P_REF_HOLD || cs7000p_power_off_state() || cal_pending() ||
		    cs7000p_subtone_pc8_idle() != CS7000P_SUBPWM_IDLE || cs7000p_rf.trim_ppb != 0 ||
		    cs7000p_rf_ref_bench(&cs7000p_rf) || bench_chips || dl_soc_written() ||
		    cs7000p_rf_bench_drive(&cs7000p_rf) || keyup_bench()) {
			why = HAL_TX_ARM_STATE;
		} else if (!maker.valid) {
			why = HAL_TX_ARM_NO_CAL;
		} else if (id != maker.crc) {
			why = HAL_TX_ARM_ID;
		}
	} else {
		id = stage != CS7000P_ARM_OFF ? cs7000p_rf.arm_id : 0u;
	}
	if (why == HAL_TX_ARM_OK) {
		if (cs7000p_rf_arm_ends(&cs7000p_rf, stage)) {
			board_rf_safe();
		}
		cs7000p_rf_arm_set(&cs7000p_rf, stage, id);
		if (stage != was) {
			retune_req = true;
		}
	}
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	if (why == HAL_TX_ARM_OK) {
		dlog("radio: transmit arm %s id=0x%08x", arm_name(stage), (unsigned)id);
	} else {
		dlog("radio: transmit arm %s refused (%u)", arm_name(stage), why);
	}
	return why;
}
#endif

// No volume: the pot is in the analog path. No mic_gain: the modem's microphone gain steps are in no document, so
// the transmit set-up keeps one value. No tx_tone, tx_dtmf, rx_am, tx_mark: not written
const hal_radio_t cs7000p_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
#if CONFIG_DMR
	.dmr_tx_end = stm32_hrc_dmr_tx_end,
	.dmr_keying = dmr_keyup_keying,
#endif
#if CONFIG_SUBTONE
	.tx_tail = tx_tail,
#endif
	.status = status,
	.audio = audio,
	.rx_sleep = rx_sleep,
#if CONFIG_REF_TRIM
	.ref_trim = ref_trim,
#endif
	.tx_drive_level = tx_drive_level,
	.tx_drive = tx_drive,
	.tx_drive_top = tx_drive_top,
	.tx_drive_now = tx_drive_now,
#if CONFIG_DEBUGLINK
	.tx_arm_info = tx_arm_info,
	.tx_arm = tx_arm,
#endif
};

// --- For the base's files ---

void cs7000p_radio_rf_safe(void)
{
	if (radio_up) {
		cs7000p_rf_safe(&cs7000p_rf);
	}
}

void cs7000p_radio_state(cs7000p_radio_state_t *s)
{
	bool up;
	uint8_t r10, r25;
	cs7000p_c6000_state(&up, &r10, &r25);
	*s = (cs7000p_radio_state_t){
		.lo_high = radio_up ? cs7000p_rf.lo_high : CS7000P_LO_HIGH != 0,
		.pll_locked = PIN_READ(PIN_PLL_LD),	// the line now
		.ak_ok = cs7000p_rf.ak_ok,
		.hrc_ok = up,
		.cal_tx = cal_tx,
		.cal_maker = cal_maker,
		.pll_r = cs7000p_rf.pll_r,
		.hrc_r10 = r10,
		.hrc_r25 = r25,
		.vco_hz = cs7000p_rf.vco_hz,
		.unlocks = cs7000p_rf.seq.unlocked,
		.tx_refused = cs7000p_rf.tx_refused,
		.dac_refused = cs7000p_rf.dac_refused,
		.tx_refused_why = cs7000p_rf.tx_refused_why,
		.rx_tune = cs7000p_rf.rx_tune,
		.pc8_duty = cs7000p_subtone_pc8(),
		.ref_offset = cs7000p_rf.ref_offset,
	};
}

#if CONFIG_DEBUGLINK
// --- board2's lines (hardware-cs7000p.md 15), from the debug link's task ---
//
// They show one state of the table in use and of the receive set-up on it, never a mix: the radio task changes the
// table (a restore's take) and sets receive up on it with cal_gen odd, and the view copies what its lines show only
// while cal_gen is even, under the interrupt mask, so the radio task can't start a change during the copy. A reply's
// board2 lines all come from the copy its first line takes. Should a change outlast VIEW_WAIT_MS, the lines say busy

#define VIEW_WAIT_MS	250u

typedef struct {
	factory_copy_state_t st;	// the copy's state the table in use came with
	uint16_t fields;
	uint8_t battery, restore;	// the table's battery byte (0: none valid); the last restore's result
	// The receive reference: the modem's coding and the one 3:5 asks for, the level's offset and source, why it is
	// not trim A, 4:0x04; the trims
	uint8_t coding, ask, ref_src, ref_why;
	int8_t rx_ref, r04;
	bool a_ok, b_ok;
	int16_t d_a, d_b;
	// The other sources, the VCO line now and its rule, the IF words
	uint8_t tune_src, rssi_src;
	bool vco_low, vco_dir, if_maker;
	// The maker's squelch thresholds at the receive frequency, or the lookup's reason
	bool sq_ok;
	uint8_t o1, c1, o9, c9, sq_why;
	// The owner's transmit arm, its id, and whether a test TX limit list is set (an armed key-up needs one)
	uint8_t arm;
	uint32_t arm_id;
	bool list;
	bool chips;			// bench_chips: the arm refused until a restart
	bool soc;			// a raw SoC register write since the start (dl_soc_written): the same
	uint16_t bench_drive;		// a bench drive armed, or the drive of a transmission on one (0: none)
	bool bench_keyup;		// the DMR key-up's constants the debug link's
	// What a key-up on the table would take: the transmission the core last asked for while its receive set-up
	// stands (tx_at), else the receive set-up as a simplex channel's at the lowest level, no tone
	bool tx_at, tx_dmr, tx_tone;	// ... tx_tone: that request named a tone (FM)
	uint8_t tx_level;
	uint32_t tx_hz;
	cs7000p_tx_view_t tx;
	bool busy;			// no state could be copied (a change outlasted VIEW_WAIT_MS)
} cal_view_t;

static cal_view_t view;

static void view_fill(cal_view_t *v)
{
	v->st = cal_st;
	v->fields = maker.fields;
	v->battery = maker.valid ? maker.battery : 0u;
#if CONFIG_FACTORY_COPY
	v->restore = factory_copy_result();
#else
	v->restore = 0xFFu;
#endif
	v->coding = cs7000p_c6000_ref_coding();
	v->ask = cs7000p_rf.ref_coding;
	v->ref_src = cs7000p_rf.ref_src;
	v->ref_why = cs7000p_rf.ref_why;
	v->rx_ref = cs7000p_rf.rx_ref;
	v->r04 = cs7000p_rf.r04;
	v->a_ok = cs7000p_maker_trim(&maker, false, &v->d_a);
	v->b_ok = cs7000p_maker_trim(&maker, true, &v->d_b);
	v->tune_src = cs7000p_rf.tune_src;
	v->rssi_src = rssi_src;
	v->vco_low = (cs7000p_exp_get() & EXP_VCO_LOW) != 0u;
	v->vco_dir = cs7000p_rf.vco_rx_dir;
	v->if_maker = cs7000p_rf.if_maker;
	const uint32_t hz = cs7000p_rf.seq.rx_hz;
	v->sq_why = CS7000P_MK_LK_OK;
	v->sq_ok = cs7000p_maker_squelch(&maker, hz, 1, &v->o1, &v->c1, &v->sq_why) &&
		   cs7000p_maker_squelch(&maker, hz, 9, &v->o9, &v->c9, &v->sq_why);
	v->arm = cs7000p_rf.arm;
	v->arm_id = cs7000p_rf.arm_id;
	tx_limit_info_t limit;
	tx_limit_get(&limit, NULL, 0);
	v->list = limit.on && !limit.block && limit.n != 0u;
	v->chips = bench_chips;
	v->soc = dl_soc_written();
	v->bench_drive = cs7000p_rf.ovr_drive != 0u ? cs7000p_rf.ovr_drive
			 : cs7000p_rf_bench_drive(&cs7000p_rf) ? cs7000p_rf.admit_drive : 0u;
	v->bench_keyup = keyup_bench();
	v->tx_at = tx_asked.set && rx_valid && tx_asked.rx_hz == hz && tx_asked.rx_bw == rx_bw && tx_asked.dmr == dmr_mode;
	v->tx_dmr = dmr_mode;
	v->tx_tone = v->tx_at && tx_asked.tone.type != TONE_NONE;
	if (v->tx_at) {
		v->tx_hz = tx_asked.hz;
		v->tx_level = tx_asked.power;
		cs7000p_rf_tx_view(&cs7000p_rf, tx_asked.hz, tx_asked.power, dmr_mode, tx_asked.wide, &tx_asked.tone, &v->tx);
	} else {
		v->tx_hz = rx_valid ? hz : 0u;
		v->tx_level = 0;
		cs7000p_rf_tx_view(&cs7000p_rf, v->tx_hz, 0, dmr_mode, rx_bw == RADIO_BW_25K && !dmr_mode, NULL, &v->tx);
	}
	v->busy = false;
}

// The copy the lines format: taken while no change is under way, else after it (the debug link's task waits: it is
// below the radio task, which ends a change within one receive set-up)
static void view_take(void)
{
	for (uint32_t waited = 0;; waited++) {
		const uint32_t g = cal_gen;
		if ((g & 1u) == 0u) {
			const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
			const bool same = cal_gen == g;
			if (same) {
				view_fill(&view);
			}
			portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
			if (same) {
				return;
			}
		}
		if (waited >= VIEW_WAIT_MS) {
			view.busy = true;
			return;
		}
		hal_delay_ms(1);
	}
}

static const char *src_name(uint8_t src)
{
	return src == CS7000P_SRC_MAKER ? "maker" : (src == CS7000P_SRC_BENCH ? "bench" : "default");
}

// A trim as d, or "-" when its group did not pass
static void trim_text(char *t, uint8_t len, bool ok, int16_t d)
{
	if (ok) {
		dl_fmt(t, len, "%d", d);
	} else {
		t[0] = '-';
		t[1] = '\0';
	}
}

// The window's state, the source of the table in use, the window's CRC-32 (the one a host compares with its backup),
// the window's reason and offset, the field groups and the battery byte of the table in use
static void fmt_cal_table(const cal_view_t *v, char *buf, uint8_t len)
{
	dl_fmt(buf, len, "cal_table=%s src=%s crc=0x%08x why=%s off=0x%03x fields=0x%04x bat=0x%02x",
	       cal_win_name(v->st.win), cal_src_name(v->st.source), (unsigned)v->st.win_crc, cal_why_name(v->st.why),
	       (unsigned)v->st.off, v->fields, v->battery);
}

// The copy's record: its state, the CRC-32 of its bytes (- when it holds none of ours), whether this start wrote it
// (2: tried and failed, tried again at the next start), the last restore's result now (none: no restore at this
// start; pending while its job runs; the record's fields are the table in use's, taken after the job)
static void fmt_cal_rec(const cal_view_t *v, char *buf, uint8_t len)
{
	const bool ours = v->st.rec == FACTORY_COPY_REC_OK || v->st.rec == FACTORY_COPY_REC_BAD;
	char crc[12];
	if (ours) {
		dl_fmt(crc, sizeof(crc), "0x%08x", (unsigned)v->st.rec_crc);
	} else {
		crc[0] = '-';
		crc[1] = '\0';
	}
#if CONFIG_FACTORY_COPY
	const char *restore = factory_copy_result_name(v->restore);
#else
	const char *restore = "none";
#endif
	dl_fmt(buf, len, "cal_rec=%s crc=%s wrote=%u restore=%s", cal_rec_name(v->st.rec), crc, v->st.wrote, restore);
}

static const char *coding_name(uint8_t k)
{
	static const char *const codings[] = { "hold", "maker", "maker7d" };
	return k < 3u ? codings[k] : "?";
}

// The receive reference (hardware-cs7000p.md 6.5, 15): the coding the modem holds (taken at its last full receive
// set-up), the level the last receive entry wrote in it (the bias), 4:0x04 as the last receive entry or DMR key-up
// wrote it (trim B in DMR on the arm's top stage, else today's), the two trims of the table in use, the level's
// source and, when it is not trim A, why; and the coding chip_reg 3:5 asks for while the modem has not taken it yet
// (until the next status poll)
static void fmt_ref(const cal_view_t *v, char *buf, uint8_t len)
{
	static const char *const whys[] = { "none", "table", "group", "rx_bound", "build" };
	const uint8_t k = v->coding, w = v->ref_why;
	const unsigned bias = k == CS7000P_REF_HOLD ? (unsigned)(0x200 + v->rx_ref) : ((unsigned)(uint16_t)v->rx_ref & 0x3FFu);
	char a[8], b[8], ask[16] = "";
	trim_text(a, sizeof(a), v->a_ok, v->d_a);
	trim_text(b, sizeof(b), v->b_ok, v->d_b);
	if (v->ask != k) {
		dl_fmt(ask, sizeof(ask), " ask=%s", coding_name(v->ask));
	}
	dl_fmt(buf, len, "ref=%s bias=0x%03x r04=0x%02x d_a=%s d_b=%s src=%s why=%s%s", coding_name(k), bias,
	       (unsigned)(uint8_t)v->r04, a, b, src_name(v->ref_src), w < 5u ? whys[w] : "?", ask);
}

// The receive side's other sources: the tune's (the first word), the RSSI scale's (the last status poll's: maker, the
// data sheet's line, or that line by chip_reg 3:8), the VCO line now and its rule in receive (chip_reg 3:6), the IF
// chip's words (chip_reg 3:7)
static void fmt_rx_cal(const cal_view_t *v, char *buf, uint8_t len)
{
	const uint8_t r = v->rssi_src;
	const char *rssi = r == CS7000P_SRC_MAKER ? "maker" : (r == CS7000P_SRC_BENCH ? "bench" : "sheet");
	dl_fmt(buf, len, "rx_cal=%s rssi=%s vco=%s vco_rule=%s ifw=%s", src_name(v->tune_src), rssi,
	       v->vco_low ? "low" : "high", v->vco_dir ? "dir" : "freq", v->if_maker ? "maker" : "port");
}

// The maker's squelch thresholds at the receive frequency, shown and never applied (the squelch is the port's,
// cs7000p_noise): open and close at levels 1 and 9 as 8-bit noise codes, the scale of noise_raw >> 4 (calibration-
// cs7000p.md 5.7; they were set under the maker's IF words, chip_reg 3:7). none: no valid table, its SQUELCH group
// refused, or no receive frequency the rows reach, with the lookup's reason
static void fmt_sq_maker(const cal_view_t *v, char *buf, uint8_t len)
{
	if (v->sq_ok) {
		dl_fmt(buf, len, "sq_maker=ok o1=%u c1=%u o9=%u c9=%u", v->o1, v->c1, v->o9, v->c9);
	} else {
		dl_fmt(buf, len, "sq_maker=none why=%s", cs7000p_maker_lookup_name(v->sq_why));
	}
}

// The owner's transmit arm (13.2): the stage, the id it was armed with (the CRC-32 of the table in use then), and
// whether a test TX limit list is set (list=0: an armed key-up on the maker's values is refused, why=15); chips=1
// only after a bench write to the modem or the synthesizer since the start, soc=1 after one to a SoC register (either:
// the arm refused until a restart). Then, only while one holds, the bench's PA state that refuses the arm:
// bench_drive= the bench drive armed or in use, bench_keyup=1 the debug link's DMR key-up constants
static void fmt_tx_arm(const cal_view_t *v, char *buf, uint8_t len)
{
	char drive[20] = "";
	if (v->bench_drive != 0u) {
		dl_fmt(drive, sizeof(drive), " bench_drive=%u", (unsigned)v->bench_drive);
	}
	dl_fmt(buf, len, "tx_arm=%s id=0x%08x list=%u%s%s%s%s", arm_name(v->arm), (unsigned)v->arm_id, v->list ? 1u : 0u,
	       v->chips ? " chips=1" : "", v->soc ? " soc=1" : "", drive, v->bench_keyup ? " bench_keyup=1" : "");
}

// A drive, d or a depth the view shows, or "-" when its lookup gave none
static void have_text(char *t, uint8_t len, bool have, int32_t n)
{
	if (have) {
		dl_fmt(t, len, "%d", (int)n);
	} else {
		t[0] = '-';
		t[1] = '\0';
	}
}

// What a key-up on the table in use would take (13.2), shown before the owner arms and never a key-up itself: ok when
// the table serves it (an armed key-up whose stage covers it then takes these values), none with no= the first lookup
// that refuses in the admission's order and why= its reason. at=tx: the transmission the core last asked for (refused
// ones included) while the receive set-up it was asked from stands; at=rx: no such request, the receive frequency as
// a simplex channel's, the lowest level, no tone. hz= the transmit frequency, mode=, lvl= the power level; the drive
// at both levels (lo=, hi=: 12-bit DAC codes), the mode's pair= (4:0x45, 4:0x46), d_a= trim A, in DMR d_b= trim B,
// on a tone channel tone_pm= the tone's depth in per mille; "-" where that lookup gives none
static void fmt_tx_maker(const cal_view_t *v, char *buf, uint8_t len)
{
	const cs7000p_tx_view_t *x = &v->tx;
	char lo[8], hi[8], a[8], b[16] = "", tone[20] = "", pair[12], no[32] = "";
	have_text(lo, sizeof(lo), (x->have & CS7000P_TXV_LO) != 0u, x->drive[0]);
	have_text(hi, sizeof(hi), (x->have & CS7000P_TXV_HI) != 0u, x->drive[1]);
	have_text(a, sizeof(a), (x->have & CS7000P_TXV_TRIM_A) != 0u, x->d_a);
	if (v->tx_dmr) {
		char t[8];
		have_text(t, sizeof(t), (x->have & CS7000P_TXV_TRIM_B) != 0u, x->d_b);
		dl_fmt(b, sizeof(b), " d_b=%s", t);
	} else if (v->tx_tone) {
		char t[8];
		have_text(t, sizeof(t), (x->have & CS7000P_TXV_TONE) != 0u, x->tone_pm);
		dl_fmt(tone, sizeof(tone), " tone_pm=%s", t);
	}
	if ((x->have & CS7000P_TXV_PAIR) != 0u) {
		dl_fmt(pair, sizeof(pair), "0x%02x,0x%02x", x->mod2, x->mod1);
	} else {
		pair[0] = '-';
		pair[1] = '\0';
	}
	if (x->no != CS7000P_TXL_OK) {
		dl_fmt(no, sizeof(no), " no=%s why=%s", cs7000p_cal_txl_name(x->no), cs7000p_cal_txl_why_name(x->why));
	}
	dl_fmt(buf, len, "tx_maker=%s at=%s hz=%u mode=%s lvl=%u lo=%s hi=%s pair=%s d_a=%s%s%s%s",
	       x->no == CS7000P_TXL_OK ? "ok" : "none", v->tx_at ? "tx" : "rx", (unsigned)v->tx_hz,
	       v->tx_dmr ? "dmr" : "fm", v->tx_level, lo, hi, pair, a, b, tone, no);
}

static void (*const view_fmt[CS7000P_CAL_VIEW_LINES])(const cal_view_t *v, char *buf, uint8_t len) = {
	fmt_cal_table, fmt_cal_rec, fmt_ref, fmt_rx_cal, fmt_sq_maker, fmt_tx_arm, fmt_tx_maker,
};
static const char *const view_keys[CS7000P_CAL_VIEW_LINES] = {
	"cal_table", "cal_rec", "ref", "rx_cal", "sq_maker", "tx_arm", "tx_maker",
};

static void view_line(uint8_t k, char *buf, uint8_t len)
{
	if (view.busy) {
		dl_fmt(buf, len, "%s=busy", view_keys[k]);
	} else {
		view_fmt[k](&view, buf, len);
	}
}

void cs7000p_cal_view_line(uint8_t k, char *buf, uint8_t len)
{
	if (k >= CS7000P_CAL_VIEW_LINES) {
		buf[0] = '\0';
		return;
	}
	if (k == 0u) {
		view_take();	// the reply's first board2 line: its copy for the rest
	}
	view_line(k, buf, len);
}

// Each line alone, from a copy of its own (the tests, one line at a time)
void cs7000p_cal_table_line(char *buf, uint8_t len)
{
	view_take();
	view_line(0, buf, len);
}

void cs7000p_cal_rec_line(char *buf, uint8_t len)
{
	view_take();
	view_line(1, buf, len);
}

void cs7000p_ref_line(char *buf, uint8_t len)
{
	view_take();
	view_line(2, buf, len);
}

void cs7000p_rx_cal_line(char *buf, uint8_t len)
{
	view_take();
	view_line(3, buf, len);
}

void cs7000p_sq_maker_line(char *buf, uint8_t len)
{
	view_take();
	view_line(4, buf, len);
}

void cs7000p_tx_arm_line(char *buf, uint8_t len)
{
	view_take();
	view_line(5, buf, len);
}

void cs7000p_tx_maker_line(char *buf, uint8_t len)
{
	view_take();
	view_line(6, buf, len);
}
#endif

// --- chip_reg (hardware-cs7000p.md 14): the bench's way to the chips, from the debug link's task ---

#if CONFIG_DEBUGLINK
enum { CHIP_MODEM, CHIP_PLL, CHIP_IF, CHIP_BOARD };
enum {
	BOARD_RX_TUNE, BOARD_EXP, BOARD_PC8, BOARD_LO_SIDE, BOARD_REF_OFFSET, BOARD_REF_CODING, BOARD_VCO_RULE,
	BOARD_IF_WORDS, BOARD_RSSI_LINE
};
#define REF_RELEASE	0x100u	// chip_reg 3:4: no bench offset (written: released; read: none set)

// The receive tuning value. Refused whenever a transmit bit is set: the same DAC is the PA's reference then, and
// the board's interlock alone would let a value through with both bits set. Test and write under one mask, so the
// radio task can't key up in between; a key-up that was already past its DAC write meets the interlock at its
// transmit rail. 0: the table's value again, at the receive set-up it asks for (the next status poll)
static bool board_rx_tune(uint32_t v)
{
	if (v > CS7000P_DAC_MAX) {
		return false;
	}
	const uint16_t code = cs7000p_cal_tune_clamp(v);
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	bool receiving = cs7000p_rf.seq.state == CHAINSEQ_ST_RX;
	bool ok = (cs7000p_exp_get() & EXP_TX_BITS) == 0u && !tx_on;
	if (ok && v != 0u && receiving) {
		ok = cs7000p_apc_dac(code);
	}
	if (ok) {
		cs7000p_rf.tune_override = (uint16_t)v;
		if (v != 0u && receiving) {
			cs7000p_rf.rx_tune = code;
			cs7000p_rf.tune_src = CS7000P_SRC_BENCH;
		}
		if (v == 0u) {
			// The A/B's other half (the maker's row, or today's value) without waiting for a channel change
			retune_req = true;
		}
	}
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	return ok;
}

static bool board_reg(uint8_t reg, uint32_t *val, bool write)
{
	switch (reg) {
	case BOARD_RX_TUNE:
		if (write && !board_rx_tune(*val)) {
			return false;
		}
		*val = cs7000p_apc_dac_get();	// the shared DAC's code now
		return true;
	case BOARD_EXP:
		if (write) {
			return false;	// the expander's one writer is exp_cs7000p.c, through the code that owns each bit
		}
		*val = cs7000p_exp_get();
		return true;
	case BOARD_PC8:
		// Not while transmission is armed: PC8's mean level is part of the reference's control voltage, which an
		// armed key-up takes from the table alone (raising the arm asks for 50 %)
		if (write && (*val > 0xFFFFu || cs7000p_rf.arm != CS7000P_ARM_OFF || cs7000p_rf.admit_arm ||
			      !cs7000p_subtone_pc8_set((uint16_t)*val))) {
			return false;
		}
		*val = cs7000p_subtone_pc8();
		return true;
	case BOARD_LO_SIDE:
		if (write) {
			if (*val > 1u) {
				return false;
			}
			cs7000p_rf.lo_high = *val != 0u;	// the next receive set-up tunes to it and sets the I/Q order
			retune_req = true;
		}
		*val = cs7000p_rf.lo_high ? 1u : 0u;
		return true;
	case BOARD_REF_OFFSET:
		if (write) {
			// A signed byte: 0..127 as it is, -128..-1 as its two's complement byte or sign extended. It is the
			// receive level's offset and 4:0x04's, over trim A. REF_RELEASE gives the level back to trim A (or
			// today's value) and 4:0x04 back to today's. An offset is refused while transmission is armed: an
			// armed key-up takes the reference from the table alone (raising the arm asks for none). The release
			// is not: while armed none can be set, so it finds the values it writes and asks only for a receive
			// set-up, which the status poll holds past any transmission; and were one set, it goes back to trim A
			if (*val == REF_RELEASE) {
				cs7000p_rf.ref_override = false;
				cs7000p_rf.ref_offset = cs7000p_cal_ref_offset(cs7000p_rf.cal);
			} else if ((*val > 0xFFu && *val < 0xFFFFFF80u) || cs7000p_rf.arm != CS7000P_ARM_OFF ||
				   cs7000p_rf.admit_arm) {
				return false;
			} else {
				cs7000p_rf.ref_offset = (int8_t)(uint8_t)*val;
				cs7000p_rf.ref_override = true;
			}
			retune_req = true;	// written at the next receive set-up
		}
		*val = cs7000p_rf.ref_override ? (uint8_t)cs7000p_rf.ref_offset : REF_RELEASE;
		return true;
	case BOARD_REF_CODING:
		// The reference DAC's coding in receive (hardware-cs7000p.md 6.5): 0 the measured hold, 1 the maker's
		// state, 2 the same with the maker's 4:0x26, for a receive test of the maker's coding. Not 0: every
		// transmission is refused. The modem takes it at the next receive set-up, in full
		if (write) {
			if (*val > CS7000P_REF_MAKER_ADC) {
				return false;
			}
			cs7000p_rf.ref_coding = (uint8_t)*val;
			retune_req = true;
		}
		*val = cs7000p_rf.ref_coding;
		return true;
	case BOARD_VCO_RULE:
		// 0: the VCO by frequency (the maker's rule), 1: by direction in receive, the rule before it (for a receive
		// test across 479 MHz). Transmit follows its frequency either way
		if (write) {
			if (*val > 1u) {
				return false;
			}
			cs7000p_rf.vco_rx_dir = *val != 0u;	// the next receive set-up selects by it
			retune_req = true;
		}
		*val = cs7000p_rf.vco_rx_dir ? 1u : 0u;
		return true;
	case BOARD_IF_WORDS:
		// 0: the port's IF chip words, 1: the maker's for the mode (rfplan_cs7000p.h), for a receive test of
		// the table's squelch and RSSI points under the words they were set under. The chip only receives, so
		// a transmission is neither refused nor changed by it
		if (write) {
			if (*val > 1u) {
				return false;
			}
			cs7000p_rf.if_maker = *val != 0u;	// the next receive set-up writes them
			retune_req = true;
		}
		*val = cs7000p_rf.if_maker ? 1u : 0u;
		return true;
	case BOARD_RSSI_LINE:
		// 0: the table's RSSI points (when they passed), 1: the data sheet's line, for an A/B of what the dBm scale
		// does to the DMR scan, roaming and the S-meter. Taken at the next status poll; receive and transmit are
		// not changed by it
		if (write) {
			if (*val > 1u) {
				return false;
			}
			cs7000p_rf.rssi_sheet = *val != 0u;
		}
		*val = cs7000p_rf.rssi_sheet ? 1u : 0u;
		return true;
	default:
		return false;
	}
}

// May the bench write the modem or the synthesizer now? Never while transmission is armed (the owner's arm not off, or
// a transmission admitted on it not yet followed by a receive set-up): an armed key-up runs on the table's values
// under the sequencer's rules, and a write would change them on air (the carrier's words, the deviation, the
// reference) or linger into the next key-up (4:0x25 until a receive entry). The synthesizer's not while any
// transmission is up either, the bench's included: the PA is off before the VCO moves (7.5). Asked under the mask
// the write runs in, as board_rx_tune, so the radio task can't key up in between
static bool bench_write_ok(uint8_t chip)
{
	if (cs7000p_rf.arm != CS7000P_ARM_OFF || cs7000p_rf.admit_arm) {
		return false;
	}
	return chip != CHIP_PLL ||
	       ((cs7000p_exp_get() & EXP_TX_BITS) == 0u && !tx_on && !dmr_armed && !cs7000p_rf.admit);
}

bool cs7000p_chip_reg(uint8_t chip, uint8_t page, uint8_t reg, uint32_t *val, bool write)
{
	if (!radio_up) {
		return false;
	}
	switch (chip) {
	case CHIP_MODEM: {
		if (!write) {
			return cs7000p_c6000_reg(page, reg, val, false);
		}
		const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
		const bool may = bench_write_ok(chip);
		bench_chips = bench_chips || may;	// tried: a failed transaction may still have landed
		const bool ok = may && cs7000p_c6000_reg(page, reg, val, true);
		portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
		return ok;
	}
	case CHIP_PLL: {
		// Write only (Mux_out is not wired): the value written is what comes back. The driver's next tune
		// overwrites the divider registers
		if (!write || page != 0 || reg > SKY72310_REG_DELTA_SIGMA || *val > 0xFFFu) {
			return false;
		}
		const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
		const bool may = bench_write_ok(chip);
		bench_chips = bench_chips || may;
		const bool ok = may && pll_write(NULL, sky72310_word(reg, (uint16_t)*val));
		portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
		return ok;
	}
	case CHIP_IF: {
		// Page 0 reads with the data line pulled down while the chip does not drive it, page 1 pulled up: a value
		// that is the same both ways came from the chip. Only the registers the data sheet names
		uint8_t v = 0;
		if (page > 1u || !cs7000p_if_reg_ok(reg) || (write && *val > 0xFFu)) {
			return false;
		}
		if (write && !ak_write(ak2365a_word(reg, (uint8_t)*val))) {
			return false;
		}
		if (!ak_read(reg, page != 0, &v)) {
			return false;
		}
		*val = v;
		return true;
	}
	case CHIP_BOARD:
		return page == 0 && board_reg(reg, val, write);
	default:
		return false;
	}
}
#endif
