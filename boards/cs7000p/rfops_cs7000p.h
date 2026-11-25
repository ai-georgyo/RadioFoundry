// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS RF chain: this board's lines, plans and transmit rules under drivers/rf/chainseq
// (docs/spec/hardware-cs7000p.md 7, 13). No platform header and no RTOS call: the expander, the shared DAC, the
// synthesizer, the IF chip, the lock line and the delay come in as callbacks, so tests/cs7000pRadioSeqTest.c runs
// every sequence on fakes with the board's own interlock (exp_cs7000p.h). Nothing here has run on a radio.
//
// What is decided here, in one place:
//
// - which expander bit is which line of the sequencer, and that a refused write is counted;
// - whether a transmission is let through (cs7000p_rf_tx_admit): the power's off state, the band, the calibration
//   or the bench override with its conditions, the owner's transmit arm over the maker's table, the battery, the
//   reference's settle time. The sequencer's transmit plan answers only for the transmission that was admitted, so
//   nothing keys past this check: not FM, not a burst;
// - that after cs7000p_rf_safe() (board_rf_safe: a fault path, a power-off) no transmit line comes on and the DAC
//   takes no value but 0 until the radio task starts its next request. A key-up that was interrupted ends there.
//   The request also stands as one to set the receiver up again, until the radio task takes it
//   (cs7000p_rf_restore_take): a request of the task's that was refused does not lose it.
//
// Contexts: the functions that wait (rx, sleep, tx_begin, tx_key, tx_off) and tx_admit belong to the radio task.
// cs7000p_rf_safe, cs7000p_rf_drive and the readers run anywhere. The burst pieces are the sequencer's own
// (chainseq_burst_* on rf->seq), called by the burst runner.
#ifndef RFOPS_CS7000P_H
#define RFOPS_CS7000P_H

#include <stdint.h>
#include <stdbool.h>
#include "rf/chainseq/chainseq.h"
#include "rfcal_cs7000p.h"

// No transmission, and no frequency measurement, before this long after the synthesizer rail came on: the
// reference oscillator's control amplifier runs from that rail and settles through 1 uF into about 6 k
#ifndef CS7000P_REF_SETTLE_MS
#define CS7000P_REF_SETTLE_MS	30
#endif
// The wait after the synthesizer rail at start-up, before anything on it is programmed
#ifndef CS7000P_RAIL_SETTLE_US
#define CS7000P_RAIL_SETTLE_US	2000
#endif
// The sequencer's times: the receive rail before the IF chip's set-up (its second LO is on that rail), the longest
// wait for the lock line, the transmit rail before the loop closes
#define CS7000P_RX_RAIL_US	1000u
#define CS7000P_LOCK_US		5000u
#define CS7000P_TX_RAIL_US	500u
// A drive armed for the bench (cs7000p_rf_drive) is used by a transmission that starts within this time, or dropped
#define CS7000P_OVERRIDE_MS	60000u

// DMR bursts (platform/stm32/dmr_keyup): the modem's RF TX interrupt comes this long before the burst's first
// symbol, the loop closes this long before the first drive, and the PA keys with the drive one fixed time before
// the first symbol. The first figure is another radio's with the same modem (boards/md9600/radio_md9600.c), the
// last a placeholder: none is measured here (tools/bench/pa_cal.py)
#define CS7000P_KEYUP_INT_AHEAD_US	2550u
#define CS7000P_KEYUP_LOOP_LEAD_US	500u
#define CS7000P_KEYUP_PA_US		800u

// The board's side of the chain. Each returns false when the write was refused or failed
typedef struct {
	bool (*exp_set)(void *ctx, uint32_t set, uint32_t clear);	// the expander (cs7000p_exp_set)
	bool (*dac)(void *ctx, uint16_t v);				// the shared DAC, 0 = off (cs7000p_apc_dac)
	bool (*synth)(void *ctx, uint8_t ref_div, uint32_t vco_hz);	// the synthesizer's words, no wait
	bool (*if_setup)(void *ctx, bool wide);				// the IF chip's set-up with its waits (task level)
	bool (*locked)(void *ctx);					// the lock line now
	void (*delay_us)(void *ctx, uint32_t us);
	void *ctx;
} cs7000p_rf_io_t;

// Why a transmission was refused (board.tx_refused's why=). The numbers are in the hardware page: append only
typedef enum {
	CS7000P_TX_OK = 0,
	CS7000P_TX_BAND = 1,		// outside 400.0-405.9875 and 406.1125-512.0 MHz
	CS7000P_TX_UNCAL = 2,		// no calibration for this frequency, level and mode, and no bench drive armed
	CS7000P_TX_OVR_NO_LINK = 3,	// a bench drive is armed, but the build has no debug link
	CS7000P_TX_OVR_NO_LIST = 4,	// ... but no test TX limit names this frequency
	CS7000P_TX_OVR_DMR = 5,		// ... but the mode is DMR and the build has no CS7000P_BENCH_DMR_TX
	CS7000P_TX_OVR_EXPIRED = 6,	// ... but it was armed more than CS7000P_OVERRIDE_MS ago (it is dropped)
	CS7000P_TX_BATTERY = 7,		// the battery reads under its critical level, or does not read
	CS7000P_TX_REF_SETTLE = 8,	// the reference has not settled since the synthesizer rail came on
	CS7000P_TX_MODEM = 9,		// the modem is not up, or refused its FM transmit set-up
	CS7000P_TX_UNLOCKED = 10,	// the synthesizer did not lock on the carrier
	CS7000P_TX_BUS = 11,		// a line or DAC write was refused (the interlock, a safe request) or failed
	CS7000P_TX_OFF = 12,		// the power's off state: the supply was let go and the knob still powers the radio
	CS7000P_TX_UNARMED = 13,	// the maker's table holds this transmission's values; the arm doesn't cover it
	CS7000P_TX_REF_CODING = 14,	// the reference DAC is in the maker's coding for a receive test (chip_reg 3:5),
					// asked for or still held by the modem (cs7000p_tx_req_t.ref_other)
	CS7000P_TX_ARM_NO_LIST = 15,	// ... the arm covers it, but no test TX limit list names this frequency
} cs7000p_tx_why_t;

// The owner's transmit arm (hardware-cs7000p.md 13.2, hal_radio_t.tx_arm): the maker's transmit values may serve a
// transmission only when the arm covers its mode and level and a test TX limit list names its frequency
// (cs7000p_tx_req_t.limit_list), so the frequencies the owner named are the radio's rule, not only the tool's. RAM
// only: zero at every start (cs7000p_rf_init), off again at every safe request (cs7000p_rf_safe: a fault path, the
// power's off state). The build cannot change it; lifting the arm later is this one line, 0 making every stage cover
// every mode, level and frequency
#ifdef CS7000P_TX_ARM_REQUIRED
#error "CS7000P_TX_ARM_REQUIRED is not a build option: lifting the arm is the one line below"
#endif
#define CS7000P_TX_ARM_REQUIRED	1
// The stages, hal_radio_t's (HAL_TX_ARM_*): each covers the ones below it
enum { CS7000P_ARM_OFF = 0, CS7000P_ARM_FM_LOW = 1, CS7000P_ARM_FM = 2, CS7000P_ARM_ALL = 3 };
// The stage covers a transmission in this mode at this power level (the board's index: 0 is the lowest)
static inline bool cs7000p_arm_covers(uint8_t arm, uint8_t level, bool dmr)
{
	return !CS7000P_TX_ARM_REQUIRED ||
	       arm >= (dmr ? CS7000P_ARM_ALL : (level == 0u ? CS7000P_ARM_FM_LOW : CS7000P_ARM_FM));
}
// The arm's verdict on a transmission the maker's table could serve: CS7000P_TX_OK, CS7000P_TX_UNARMED (the stage
// doesn't cover it) or CS7000P_TX_ARM_NO_LIST (it does, but listed is false)
static inline cs7000p_tx_why_t cs7000p_arm_why(uint8_t arm, uint8_t level, bool dmr, bool listed)
{
	if (!cs7000p_arm_covers(arm, level, dmr)) {
		return CS7000P_TX_UNARMED;
	}
	return listed || !CS7000P_TX_ARM_REQUIRED ? CS7000P_TX_OK : CS7000P_TX_ARM_NO_LIST;
}

// The reference DAC's coding in receive, chip_reg 3:5 (hardware-cs7000p.md 6.5): the measured hold (4:0x25 = 0x0C,
// the level 512 plus the offset), the maker's state (0x0E, the level the signed offset), the same with the maker's
// 4:0x26. RAM only. Anything but the hold refuses every transmission (CS7000P_TX_REF_CODING)
enum { CS7000P_REF_HOLD = 0, CS7000P_REF_MAKER = 1, CS7000P_REF_MAKER_ADC = 2 };
#define CS7000P_MAKER_ADC_CTL	0x7Du	// 4:0x26 as the maker's program leaves it (calibration-cs7000p.md 5.13)

// A coding in the IF host's form (hrc6000_if_cfg_t.ref_coding): true the signed level under 0x0E, false the measured
// hold; adc_ctl the 4:0x26 of every receive entry, 0 the driver's. An unknown coding is the hold
static inline bool cs7000p_ref_signed(uint8_t coding, uint8_t *adc_ctl)
{
	*adc_ctl = coding == CS7000P_REF_MAKER_ADC ? CS7000P_MAKER_ADC_CTL : 0u;
	return coding == CS7000P_REF_MAKER || coding == CS7000P_REF_MAKER_ADC;
}
// ... and back: the coding (CS7000P_REF_*) the IF host took at its last full receive set-up, from its form
static inline uint8_t cs7000p_ref_coding_of(bool sgn, uint8_t adc_ctl)
{
	if (!sgn) {
		return CS7000P_REF_HOLD;
	}
	return adc_ctl == CS7000P_MAKER_ADC_CTL ? CS7000P_REF_MAKER_ADC : CS7000P_REF_MAKER;
}

// A transmission the core asks for, with what the board knows at that moment
typedef struct {
	uint32_t hz;
	uint8_t power;			// the board's power level index
	bool dmr;
	uint32_t now_ms;
	uint16_t battery_mv;		// 0: no reading
	uint16_t battery_critical_mv;
	bool debuglink;			// the debug link is built: without it there is neither a limit nor an override
	bool limit_list;		// a test TX limit is set as a list of frequencies, and hz is on it
	bool bench_dmr;			// the build allows the bench drive in DMR (CS7000P_BENCH_DMR_TX)
	bool off;			// the power's off state (cs7000p_power_off_state)
	// The modem still holds another coding than the measured hold: it takes the one chip_reg 3:5 asks for only at
	// its next full receive set-up (the status poll's), and a burst keeps the one it holds
	bool ref_other;
} cs7000p_tx_req_t;

typedef struct {
	const cs7000p_rf_io_t *io;
	const cs7000p_cal_t *cal;
	// The maker's table in use, as checked (makercal_cs7000p.h; NULL or not valid: none). The radio task's: set after
	// cs7000p_rf_init, parsed anew only on that task. Receive (the cs7000p_cal_*_m getters), and whether a
	// transmission would find the maker's values (the arm's question; none is served)
	const cs7000p_maker_t *maker;
	chainseq_ops_t ops;
	chainseq_t seq;
	// Bench state: the first LO's side, the tuning value in place of the table's (0: the table's), the modem's
	// reference offset, the VCO line in receive by direction (chip_reg 3:6) instead of by frequency, the reference
	// DAC's coding in receive (CS7000P_REF_HOLD and the others, chip_reg 3:5), the maker's IF chip words in receive
	// instead of the port's (chip_reg 3:7, the radio's if_setup callback: rfplan_cs7000p.h), the data sheet's RSSI
	// line in place of the table's points (chip_reg 3:8)
	bool lo_high;
	bool vco_rx_dir;
	bool if_maker;
	bool rssi_sheet;
	uint16_t tune_override;
	int8_t ref_offset;
	bool ref_override;		// ref_offset is the bench's (chip_reg 3:4), for the receive level too
	uint8_t ref_coding;
	int16_t trim_ppb;		// the reference trim, taken at the next tune
	volatile bool safed;		// cs7000p_rf_safe() since the radio task's last request
	volatile bool restore;		// ... and since the radio task last took it (cs7000p_rf_restore_take)
	bool synth_on;			// the synthesizer rail is on since synth_on_ms (cs7000p_rf_start)
	uint32_t synth_on_ms;
	// The bench drive: armed by any task, used and dropped by the radio task
	volatile uint16_t ovr_drive;
	volatile uint32_t ovr_ms;
	// The owner's transmit arm (CS7000P_ARM_*) and the id it was made with (the CRC-32 of the table in use). The
	// debug link's task sets it (cs7000p_rf_arm_set), cs7000p_rf_safe() clears it, the radio task reads it
	volatile uint8_t arm;
	volatile uint32_t arm_id;
	// The transmission that passed cs7000p_rf_tx_admit: the only one the transmit plan answers for. admit_arm: it is
	// on the maker's values, under the arm (none is yet: hardware-cs7000p.md 13.2)
	bool admit, admit_ovr, admit_dmr, admit_arm;
	uint32_t admit_hz;
	uint8_t admit_power;
	uint16_t admit_drive;
	// For the views
	uint8_t pll_r;
	uint32_t vco_hz;
	bool ak_ok;
	uint16_t rx_tune;
	// The receive values last given and their sources (CS7000P_SRC_*): the tune, the reference level's offset and
	// why it is not trim A (CS7000P_RXREF_WHY_*)
	uint8_t tune_src, ref_src, ref_why;
	int8_t rx_ref;
	uint32_t tx_refused, dac_refused;
	uint8_t tx_refused_why;
} cs7000p_rf_t;

// The chain safe and nothing set up. io and cal stay valid
void cs7000p_rf_init(cs7000p_rf_t *rf, const cs7000p_rf_io_t *io, const cs7000p_cal_t *cal);
// Start-up: the synthesizer rail on, CS7000P_RAIL_SETTLE_US, and the time noted for the reference's settle time.
// False: the expander refused
bool cs7000p_rf_start(cs7000p_rf_t *rf, uint32_t now_ms);

// Receive on hz: the sequencer's full set-up, then the audio rail (the sub-audio filter's supply). False: out of
// band (nothing written), or a write failed (the chain is safe and not receiving)
bool cs7000p_rf_rx(cs7000p_rf_t *rf, uint32_t hz, bool wide);
// The receive rail off (true) and the receiver back (false). After a safe request the way back is a full set-up
void cs7000p_rf_sleep(cs7000p_rf_t *rf, bool sleep);

// May this transmission go ahead? Nothing is written. CS7000P_TX_OK: it is admitted, and the sequencer's transmit
// plan answers for it (hz, the drive of its level or the bench drive) until cs7000p_rf_tx_off. Anything else: it
// is refused and counted, and an earlier admission is gone. In the power's off state that is all: an armed bench
// drive is neither used nor dropped, and a safe request stands
cs7000p_tx_why_t cs7000p_rf_tx_admit(cs7000p_rf_t *rf, const cs7000p_tx_req_t *q);
// FM, in this order around the modem's transmit set-up. tx_begin: the DAC to 0 (it held the tuning voltage and must
// not become a power request) and the receive rail off. tx_key: the VCO line for the carrier, the synthesizer on it
// and locked, the transmit rail, the loop closed on no reference, the ramp to the drive. Not CS7000P_TX_OK: refused
// and counted, the chain is safe, and receiving again when it was before (the caller redoes its own receive steps)
bool cs7000p_rf_tx_begin(cs7000p_rf_t *rf);
cs7000p_tx_why_t cs7000p_rf_tx_key(cs7000p_rf_t *rf);
// The end of a transmission, FM (ramp down) or bursts: the PA off, the receiver back on its last set-up in full,
// the admission and the bench drive gone. keep: when the chain already receives (a burst transmission's last switch
// put it back, without the IF chip's set-up) it is left as it is; without keep it gets the whole set-up then too
void cs7000p_rf_tx_off(cs7000p_rf_t *rf, bool keep);
// A refusal the caller found itself (the modem): counted like the others, and the admission is gone
void cs7000p_rf_refuse(cs7000p_rf_t *rf, cs7000p_tx_why_t why);

// The owner's transmit arm. arm_ends: the transmission admitted now is on the maker's values and stage would not
// cover it, so the caller ends it at once (board_rf_safe) before it lowers the arm. arm_set: the stage and its id;
// nothing else is checked here (the handshake is the radio's, radio_cs7000p.c), and nothing is written. The debug
// link's task, under the interrupt mask: no admission runs between the two
bool cs7000p_rf_arm_ends(const cs7000p_rf_t *rf, uint8_t stage);
void cs7000p_rf_arm_set(cs7000p_rf_t *rf, uint8_t stage, uint32_t id);

// For the burst runner's plan, after cs7000p_rf_tx_admit: the admitted drive, the top level's drive at that
// frequency (the bench drive's own when the table has none) and the floor
void cs7000p_rf_burst_drives(const cs7000p_rf_t *rf, uint16_t *drive, uint16_t *top, uint16_t *floor);

// board_rf_safe()'s part here, any context, no wait, and nothing written: the caller has already switched the DAC
// off and shifted the safe word (exp_rf_safe_word: the loop open, the transmit rail off, the VCO line parked), and
// holds the interrupt mask across those writes and this call, so no burst point runs in between. From now until the
// radio task's next request no transmit line comes on and the DAC takes no value but 0, and the sequencer is told
// its lines moved (chainseq_safed). A sequence this cuts into fails at its next such write and ends safe. The owner's
// transmit arm goes off with it
void cs7000p_rf_safe(cs7000p_rf_t *rf);
// The radio task's side of it: true once for the safe requests since the last call. The chain is then idle with the
// DAC at 0 (unless a request of the task's has set it up since), and whatever else the board switched off with it
// is off: the caller runs its receive set-up again. Not cleared by a request, as the latch is: a key-up that was
// refused right after a safe request (every one on a radio without calibration) writes nothing and must not leave
// the receiver off. Take it before the set-up, so that a request landing during it is taken next time
bool cs7000p_rf_restore_take(cs7000p_rf_t *rf);

// hal_radio_t.tx_drive: arm a drive (1..CS7000P_DAC_MAX) for the next transmission, 0: none. False: out of range
bool cs7000p_rf_drive(cs7000p_rf_t *rf, uint16_t drive, uint32_t now_ms);
// hal_radio_t.tx_drive_level: 0 without calibration or outside the transmit ranges
uint16_t cs7000p_rf_drive_level(const cs7000p_rf_t *rf, uint32_t hz, uint8_t level);
// hal_radio_t.tx_drive_top: during an FM transmission the top level's drive at its frequency, else 0
uint16_t cs7000p_rf_drive_top(const cs7000p_rf_t *rf);

// The reference offset of the held level at a receive entry (the bias, 0x200 + it under the measured hold; the
// signed level under the maker's coding): the bench's (chip_reg 3:4) while it is set, else cs7000p_cal_rx_ref_m (trim
// A, bounded, else today's). Noted for the view. 4:0x04 in receive and in the FM transmit set-up stays ref_offset,
// today's value. Radio task: the IF host asks it at its receive entries
int8_t cs7000p_rf_rx_ref(cs7000p_rf_t *rf);
// The IF host's ref_offset callback (c6000_cs7000p.c, one call): a receive entry's held level (cs7000p_rf_rx_ref),
// or, inside the FM transmit set-up (tx_setup), its 4:0x04 and transmit bias: ref_offset, today's value, never trim
// A, so that the table changes no FM transmission (hardware-cs7000p.md 13.1)
int8_t cs7000p_rf_ref_for(cs7000p_rf_t *rf, bool tx_setup);
// 4:0x04 at a receive entry (the IF host's rx_tx_offset): ref_offset, never trim A. What 4:0x04 does under the hold
// is not measured, and a DMR burst carries the value
static inline int8_t cs7000p_rf_rx_r04(const cs7000p_rf_t *rf)
{
	return rf->ref_offset;
}

// The RSSI in dBm at a status poll: the data sheet's line while the bench asks for it (chip_reg 3:8; src
// CS7000P_SRC_BENCH), else cs7000p_cal_rssi_m on the table in use. src may be NULL
int16_t cs7000p_rf_rssi(const cs7000p_rf_t *rf, uint32_t raw12, uint32_t mv, uint8_t *src);

// The modem's callbacks (hrc6000_if_cfg_t). The pair: the table's; without one, 0 / 0 (an unmodulated carrier) for
// an FM transmission admitted on the bench drive, and false otherwise
bool cs7000p_rf_mod_pair(const cs7000p_rf_t *rf, uint32_t hz, bool dmr, uint8_t *mod2, uint8_t *mod1);

// --- The radio's other files (target only; nothing below is part of the host-clean code above) ---

// radio_cs7000p.c: the one chain
extern cs7000p_rf_t cs7000p_rf;
void cs7000p_radio_dmr_tx_end(void);	// stm32_hrc_dmr_board_t.tx_end
void cs7000p_radio_speaker(void);	// apply the speaker routing again (the modem's set-ups open its feed-through)

// c6000_cs7000p.c: the modem. Radio task, but for _rx_hold (one bus write, also from the burst runner) and _reg
bool cs7000p_c6000_init(void);				// power-up and set-up; false: the chip did not answer
void cs7000p_c6000_mode(bool dmr, uint32_t hz);		// DMR <-> FM receive
void cs7000p_c6000_retune(uint32_t hz);			// every receive entry
bool cs7000p_c6000_fm_tx(uint32_t hz, bool wide);	// false, nothing written: not up, or no modulation pair
void cs7000p_c6000_fm_tx_end(void);
void cs7000p_c6000_rx_hold(void);
bool cs7000p_c6000_fm_rx(void);				// the chip is in FM receive: its line out plays its own FM audio
void cs7000p_c6000_fm_audio(bool on);			// that audio's feed-through to the line out (4:0x36)
void cs7000p_c6000_state(bool *up, uint8_t *r10, uint8_t *r25);
uint8_t cs7000p_c6000_ref_coding(void);			// the coding (CS7000P_REF_*) the modem took last; any task
bool cs7000p_c6000_reg(uint8_t page, uint8_t reg, uint32_t *val, bool write);	// chip_reg, chip 0

#endif
