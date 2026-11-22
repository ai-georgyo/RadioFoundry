// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR burst key-up runner for a board with one synthesizer and a PA power loop, on the HR_C6000 glue's hooks
// (hrc/dmr_glue.h, stm32_hrc_dmr_board_t) and a one-shot timer (oneshot.h). The board gives its RF chain as an ops
// table and points the glue's hooks at the functions here. Nothing in this file touches a pin or a peripheral, and it
// calls only the ops, the timer and the glue, so tests/host/dmrKeyupTest.c runs all of it on fakes.
//
// A transmission: the radio's tx() arms (dmr_keyup_arm: the board's plan, or its refusal). The glue then has the
// transmit side made ready ahead of the first burst (tx_prepare, tx_wait_lock). At each RF TX interrupt the timer
// runs the burst's points, the key-up rule's (core/radio/pa_keyup.h, docs/spec/pa-keyup.md) and the board's loop
// point ahead of them, each at its time before the first symbol, and after the last one it runs on to the PA drop at
// the burst's last symbol. The glue's switch back at the next timeslot interrupt drops the PA again, which is what
// ends a burst whose timer did not. At the transmission's end (tx_done) the receiver comes back.
//
// What a burst never does: start unarmed, start while one is keyed (the PA's time is bounded by the interrupt that
// keyed it), run on after a point the board refused, or key in the air interface's physical layer mode and the
// split duplex outbound, which key once and hold (not written here: the first board offers neither). Listening
// between bursts (the SFR lead and retune) is not written either; the boards that have these still have their own
// runners (boards/md380/radio_md380.c, boards/md9600/radio_md9600.c).
//
// A board lists platform/stm32/dmr_keyup in BOARD_DIRS in every configuration: the file is empty without CONFIG_DMR,
// and the functions below are then stand-ins that never arm. Its family must give the timer (oneshot.h).
#ifndef STM32_DMR_KEYUP_H
#define STM32_DMR_KEYUP_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hal_radio.h"
#include "radio/pa_keyup.h"

// The ops are called with the modem's and the timer's interrupts held off (from their handlers, or under the RTOS
// interrupt mask), settle_us, plan and cfg excepted: tx_tune, point, pa_off and rx_back never run inside one
// another. ctx is passed back as it is
//
// tx_tune:   false: nothing is transmitted (the burst is counted as skipped)
// settle_us: waits itself, for the lock or a fixed time, and returns the microseconds it took (not used here). At
//            task level, never from an interrupt. NULL: no wait
// point:     drive is the reference of that moment: the boost at PA_KEYUP_PT_BOOST, the level's drive at
//            PA_KEYUP_PT_DRIVE, at PA_KEYUP_PT_PA whichever of the two is current (the boost when the burst has
//            one), and 0 at DMR_KEYUP_PT_LOOP. The loop point is the first point of every burst, loop_lead_us
//            ahead of the first drive: the PA's point, or the boost when the level has one (the loop closes on no
//            reference, so it can't close after a drive). After a false the burst's other points do not run and
//            pa_off follows
// pa_off:    also called with no burst up, and twice for one burst (the timed drop, then the glue's switch back)
// plan:      at task level, in dmr_keyup_arm: the level's key-up, usually pa_keyup_plan() on the calibration's drives
//            (after its own pa_keyup_init(); the earliest point this runner can run is int_ahead_us - loop_lead_us -
//            DMR_KEYUP_MIN_US before the first symbol). False, or a drive of 0: refused, nothing is armed and no
//            other op is called
// cfg:       before plan, for pa_keyup_select(). NULL, or a NULL result: the constants stay as they are
typedef struct {
	bool (*tx_tune)(void *ctx, uint32_t hz);	// RF to transmit on hz, no waits (rails, VCO, synthesizer words)
	uint32_t (*settle_us)(void *ctx);		// task level wait after tx_tune (lock detect or a fixed time)
	bool (*point)(void *ctx, uint8_t kind, uint16_t drive);	// one key-up point: PA_KEYUP_PT_BOOST / _DRIVE / _PA or
							// DMR_KEYUP_PT_LOOP; interrupt level, never waits. False: PA left safe
	void (*pa_off)(void *ctx);			// the burst's end: drive to the floor, loop open, transmitter supply off
	bool (*rx_back)(void *ctx);			// receiver back on the last rx() frequency, no waits; true when it is
	bool (*plan)(void *ctx, uint32_t hz, uint8_t power, pa_keyup_t *ku);	// from the board's calibration; false: refuse
	const pa_keyup_cfg_t *(*cfg)(void *ctx, uint32_t hz);	// the band's constants (PA_KEYUP_FIXED(...) until measured)
	uint16_t loop_lead_us;				// the loop closes this long before the PA's point
	uint16_t int_ahead_us;				// the RF TX interrupt's lead on the first symbol
	void *ctx;
} dmr_keyup_ops_t;
#define DMR_KEYUP_PT_LOOP	PA_KEYUP_PT_BOARD
// The timer's shortest wait: points closer together than this run in one call, the later ones that much early
#define DMR_KEYUP_MIN_US	30u

#if CONFIG_DMR
// Once, from the radio's init: the ops (kept by pointer, so static) and the timer. Ops this runner can't work with
// (a missing tx_tune, point, pa_off, rx_back or plan; an int_ahead_us that leaves no room after the interrupt for the
// loop's lead, or that puts the PA drop past the timer's 65535 us) are not taken: nothing ever arms
void dmr_keyup_init(const dmr_keyup_ops_t *ops);
// Task level. False: refused (the plan, the physical layer mode holding the chip, no ops), and nothing is armed
// afterwards, an earlier arm included. A new arm ends a burst that is up and takes the new plan and frequency
bool dmr_keyup_arm(uint32_t hz, uint8_t power);		// hal_radio_t.tx in DMR: plan, arm, stm32_hrc_dmr_tx_prepare()
// Task level: a transmission still running is dropped, the timer stopped, the PA off. The RF stays where it is, on
// the transmit side or not: the caller sets its receiver up again
void dmr_keyup_disarm(void);				// tx_off, mode change, retune
// The stm32_hrc_dmr_board_t hooks, ready to be pointed at:
bool dmr_keyup_rf_switch(bool tx);
void dmr_keyup_tx_prepare(void);
void dmr_keyup_tx_wait_lock(void);
bool dmr_keyup_tx_done(void);
// skipped: bursts that were not keyed or not finished (tx_tune or a point refused, an RF TX interrupt while a burst
// was keyed, a keying this runner does not do). unarmed: RF TX interrupts with nothing armed. rx: returns to receive
// at a transmission's end
void dmr_keyup_rf_counters(uint32_t *skipped, uint32_t *unarmed, uint32_t *rx);
// hal_radio_t.dmr_keying. direct: bursts whose RF TX interrupt found the transmit side not ready and tuned it there
// (the synthesizer had no time to lock: the board's PA point decides). idle: bursts keyed on the prepared side
void dmr_keyup_keying(hal_dmr_keying_t *k);
#else	// built without DMR: nothing arms, nothing is keyed
static inline void dmr_keyup_init(const dmr_keyup_ops_t *ops) { (void)ops; }
static inline bool dmr_keyup_arm(uint32_t hz, uint8_t power) { (void)hz; (void)power; return false; }
static inline void dmr_keyup_disarm(void) {}
static inline bool dmr_keyup_rf_switch(bool tx) { return !tx; }
static inline void dmr_keyup_tx_prepare(void) {}
static inline void dmr_keyup_tx_wait_lock(void) {}
static inline bool dmr_keyup_tx_done(void) { return false; }
static inline void dmr_keyup_rf_counters(uint32_t *skipped, uint32_t *unarmed, uint32_t *rx)
{
	*skipped = *unarmed = *rx = 0;
}
static inline void dmr_keyup_keying(hal_dmr_keying_t *k) { *k = (hal_dmr_keying_t){ .last_direct = false }; }
#endif

#endif
