// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Receive / transmit ordering of a discrete RF chain: one synthesizer with a receive and a transmit VCO, an IF
// receiver chip, switched synthesizer, receive and transmit rails, and a PA power (APC) loop whose reference DAC is
// also the receive front end's tuning voltage. Pure C over an ops table: the board gives the lines (pins or expander
// bits), the synthesizer, the IF chip, the DAC, the delays, and its frequency and calibration plan. Nothing in here
// knows a radio, a pin or a frequency, so tests/host/chainseqTest.c runs every sequence on a logging fake.
//
// The order is fixed. Three rules hold on every path, the error paths included, and the test watches them at every
// write:
//
//	1. the DAC is 0 (or at the floor) and the loop is open whenever the transmit rail changes;
//	2. the transmit rail is off whenever the receive VCO is selected;
//	3. the DAC never carries a receive tuning value while the loop is closed.
//
// The floor is the largest DAC value that gives no RF output (0 when the board has none): the loop is closed and
// opened with the reference there, and while it is closed no sequence takes the reference under it (chainseq_safe
// alone goes straight to 0). The sequences also keep to what a board's own interlock on the shared DAC may demand: a
// transmit line (rail or loop) only comes on with the DAC at or under the floor, and a value over the floor is only
// written with both on or both off.
//
// A line or DAC write the board refuses (its interlock), a synthesizer or IF write that fails: CHAINSEQ_BUS, counted,
// and the chain ends in chainseq_safe. Nothing is transmitted without the board's calibration (tx_plan) and, where
// the board has a lock indication, without a locked synthesizer.
//
// Contexts: chainseq_rx, _tx, _tx_off and _sleep wait and belong to one task. The burst pieces never wait and run at
// interrupt level. chainseq_safe and chainseq_safed run anywhere. They aren't reentrant against each other: the caller
// keeps its burst runner disarmed (or its interrupt masked) around a task level call, and chainseq_drive_now away from
// the task's key-up and key-down. Neither of the two stops a sequence it interrupts: one that runs on afterwards goes
// through its remaining steps and may key. From another context they are for a path that does not return into the
// sequence (a fault handler, a reset).
//
// The two sequencers this was written from are still their boards' own (boards/md380/radio_md380.c,
// boards/md9600/rfseq_md9600.c); each moves here with its own check on the radio.
#ifndef CHAINSEQ_H
#define CHAINSEQ_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "radio/pa_keyup.h"

// The lines, logical levels (true = on)
typedef enum {
	CHAINSEQ_LINE_SYNTH_RAIL,	// the synthesizer's and the VCOs' supply
	CHAINSEQ_LINE_RX_RAIL,		// the receiver's supply
	CHAINSEQ_LINE_TX_RAIL,		// the transmitter's supply (drivers, antenna switch). Alone it keys no PA
	CHAINSEQ_LINE_VCO_RX,		// on: the receive VCO, off: the transmit VCO
	CHAINSEQ_LINE_APC_LOOP,		// the PA's power loop closed: a reference over the floor keys the PA
	CHAINSEQ_LINE_IF_RUN,		// the IF chip out of power-down. Only ever turned on here: the chip keeps its
					// set-up through sleep and transmit
	CHAINSEQ_LINE_COUNT
} chainseq_line_t;

typedef enum {
	CHAINSEQ_OK,
	CHAINSEQ_REFUSED_BAND,	// rx_plan: out of band. Nothing was written
	CHAINSEQ_REFUSED_CAL,	// tx_plan: no calibration (also a drive of 0 or under its floor). Nothing was written
	CHAINSEQ_UNLOCKED,	// the synthesizer did not lock
	CHAINSEQ_BUS		// a write was refused or failed, or a drive came with the loop open
} chainseq_err_t;

typedef struct {
	// May be an expander bit; never waits. False: the board's interlock refused it
	bool (*line)(void *ctx, uint8_t line, bool on);
	// Program the synthesizer (no wait)
	bool (*synth)(void *ctx, uint32_t vco_hz);
	// Optional; interrupt safe (pin reads, no wait): also asked at a burst's PA point
	bool (*locked)(void *ctx);
	// IF chip set-up and calibration, with its own waits (task level)
	bool (*if_setup)(void *ctx, bool wide);
	// The shared DAC; 0 = off (a true 0 V). False: the interlock refused it
	bool (*dac)(void *ctx, uint16_t v);
	void (*delay_us)(void *ctx, uint32_t us);
	// False: out of band
	bool (*rx_plan)(void *ctx, uint32_t hz, uint32_t *vco_hz, uint16_t *tune_dac);
	// False: no calibration
	bool (*tx_plan)(void *ctx, uint32_t hz, uint8_t power, uint32_t *vco_hz, uint16_t *drive, uint16_t *floor);
	// Board timings. rail_settle_us: after the synthesizer rail in the first set-up since chainseq_init, and after
	// the receive rail in every receive set-up. lock_timeout_us: the longest wait for the lock indication (under
	// two poll times no wait can end locked: the reads are CHAINSEQ_LOCK_POLL_US apart); without one (locked NULL)
	// the fixed wait after the synthesizer's words, which is then trusted. tx_rail_lead_us: from the transmit rail to
	// the loop in an FM key-up
	uint32_t rail_settle_us, lock_timeout_us, tx_rail_lead_us;
	void *ctx;
} chainseq_ops_t;

// The lock indication counts when CHAINSEQ_LOCK_READS reads in a row agree: CHAINSEQ_LOCK_POLL_US apart at task
// level (a fractional-N synthesizer may pulse the line), back to back at a burst's points
#define CHAINSEQ_LOCK_READS	3u
#define CHAINSEQ_LOCK_POLL_US	100u
// The FM key-up and key-down: the reference between the floor and the drive in steps of radio/apc_ramp.h's shape
#define CHAINSEQ_RAMP_STEPS	8u
#define CHAINSEQ_RAMP_STEP_US	250u

// chainseq_t.state, as a call leaves it
typedef enum {
	CHAINSEQ_ST_OFF,	// after chainseq_init: safe, nothing set up, the synthesizer rail not known to be on
	CHAINSEQ_ST_IDLE,	// safe (after chainseq_safe or a failure): the PA off, the DAC 0, the receiver not set up
	CHAINSEQ_ST_RX,		// receiving on rx_hz
	CHAINSEQ_ST_SLEEP,	// chainseq_sleep: the receive rail off, the rest kept
	CHAINSEQ_ST_TX,		// FM transmit on tx_hz: the loop closed, the DAC at drive
	CHAINSEQ_ST_BURST,	// tuned for bursts on tx_hz: the transmit VCO, the loop open, the DAC 0
	CHAINSEQ_ST_BURST_LOOP,	// in a burst: the loop closed, the DAC at the floor
	CHAINSEQ_ST_BURST_ON	// in a burst: the DAC at drive, over the floor
} chainseq_state_t;

// state: a chainseq_state_t. rx_hz, wide: the last receive set-up (rx_hz 0: none yet). tx_hz, drive, floor: the last
// transmission's. refused: requests turned down with nothing written (out of band, no calibration, a call the state
// does not allow). unlocked: receive set-ups that went on unlocked, key-ups and bursts that were not let through.
// errors: CHAINSEQ_BUS. last_err: the last chainseq_err_t other than CHAINSEQ_OK
typedef struct { const chainseq_ops_t *ops; uint8_t state; uint32_t rx_hz, tx_hz; bool wide; uint16_t drive, floor;
		 uint32_t refused, unlocked, errors; uint8_t last_err; } chainseq_t;

void chainseq_init(chainseq_t *s, const chainseq_ops_t *ops);	// calls chainseq_safe

// Every step runs whatever the one before answered, and none waits. The receive rail and the receiver's set-up stay
// as they are and the DAC is left at 0: the chain receives again after the next chainseq_rx
void chainseq_safe(chainseq_t *s);		// DAC 0, APC loop open, TX rail off, receive VCO: any context

// For a board whose own fault path has just put all four in that state by a shorter way than the ops (the word of an
// expander shifted in one piece, the DAC switched off): the state as chainseq_safe leaves it, and no write. A fault
// path is the deepest thing on a task's stack, and a second pass over the same lines through the ops costs it more
// than the first. A caller that can't vouch for all four uses chainseq_safe
void chainseq_safed(chainseq_t *s);		// the state only, nothing written: any context

// Out of band: refused, nothing written, and what the chain was doing goes on. Else from any state (a transmission is
// brought down in order first): the writes of chainseq_safe, the synthesizer rail, the receive LO and its lock, the
// receive rail and the IF chip, rail_settle_us, the IF set-up, the tuning value. A receive LO that does not lock is
// counted (unlocked, last_err) and receive goes on: the result is CHAINSEQ_OK
chainseq_err_t chainseq_rx(chainseq_t *s, uint32_t hz, bool wide);	// full receive set-up (always full: gotchas 3.14)

// No calibration: refused, nothing written. Else: the DAC 0, the receive rail off, the transmit VCO on the carrier and
// locked, the transmit rail, tx_rail_lead_us, the DAC at the floor, the loop, the ramp to the drive. Not locked, or a
// write refused: chainseq_safe at once, then back to the last receive set-up (when there is one), and the error
chainseq_err_t chainseq_tx(chainseq_t *s, uint32_t hz, uint8_t power);	// FM key-up with lock check and apc_ramp.h ramp

// Also ends a burst transmission (no ramp), and brings the receiver back after a transmission that failed on its way
// (idle). Without a receive set-up to go back to it ends in chainseq_safe. A write refused on the way down:
// chainseq_safe at once (no second ramp), then the receiver as after any other. Nothing while receiving or asleep: a
// burst transmission's last switch may have left the chain receiving
void chainseq_tx_off(chainseq_t *s);		// ramp down, loop open, TX rail off, then chainseq_rx(last)

// true while receiving: the receive rail off; the synthesizer rail and the IF chip stay on, so the reference and the
// IF calibration hold. false while asleep: chainseq_rx(last). Nothing in any other state
void chainseq_sleep(chainseq_t *s, bool sleep);

// During an FM transmission only: the DAC at drive at once, never under the floor (0: the floor, the PA off with the
// exciter on). Key-down ramps from it. False otherwise, nothing written
bool chainseq_drive_now(chainseq_t *s, uint16_t drive);		// the PA sweep and tx_drive paths

#if CONFIG_DMR
// Burst pieces for dmr_keyup (interrupt level, no waits):

// Refused, nothing written: before the first set-up since chainseq_init, during an FM transmission, or when tx_plan
// has nothing for hz. A burst still up is ended first. The call has no power level, and takes only the VCO frequency
// and the floor from the plan (each point brings its drive): it asks tx_plan for level CHAINSEQ_BURST_PLAN_POWER. The
// plan for the level that is keyed is the caller's to check before it arms
bool chainseq_burst_tune(chainseq_t *s, uint32_t hz);		// receiver off, transmit VCO, words, TX rail
#define CHAINSEQ_BURST_PLAN_POWER	0u

// drive / loop / PA supply points. At the PA point a synthesizer that is not locked ends the burst: chainseq_burst_off,
// counted in unlocked, false (FM transmit has the same refusal at task level)
//
// kind is a pa_keyup_kind_t. CHAINSEQ_PT_LOOP (the board's own point): the transmit rail (the burst before turned it
// off) and the loop closed on the floor. PA_KEYUP_PT_PA: the lock, the loop if no loop point came, then the drive.
// PA_KEYUP_PT_BOOST and PA_KEYUP_PT_DRIVE: the drive; the first one of a burst that raises the reference asks for the
// lock as the PA's point does. The reference never goes under the floor. A drive while the loop is open is
// CHAINSEQ_BUS (the loop point comes first; a board's interlock refuses that order too). Outside a burst
// transmission: false, nothing written
bool chainseq_burst_point(chainseq_t *s, uint8_t kind, uint16_t drive);
#define CHAINSEQ_PT_LOOP	PA_KEYUP_PT_BOARD

// The burst's end: the DAC to the floor, the loop open, the transmit rail off, the DAC 0. The transmit VCO stays
// tuned for the next burst. Nothing while receiving, asleep or idle: the PA is off there, and the DAC may be the
// receiver's
void chainseq_burst_off(chainseq_t *s);

// The PA off, the receive VCO on the last receive LO, the receive rail, the tuning value: no lock wait and no IF
// set-up (the IF chip keeps its own). True when the chain receives, also when it already did. False, nothing written:
// asleep, under an FM transmission, or with no receive set-up to go back to
bool chainseq_burst_rx(chainseq_t *s);				// back to the receive LO, no waits
#endif

#endif
