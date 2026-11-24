// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The air-framed modes as the radio state machine sees them (radio_sm.c): one row per mode the build has (P25, NXDN,
// M17), so radio_sm.c loops over the table instead of testing each mode with #if, and a new mode adds a row instead of
// a case at every site. A row is the mode's module's (radio_p25.c, radio_nxdn.c, radio_m17.c); radio_air_modes.c lists
// the built ones. Transmit goes through the row's radio_air_ops_t (radio_air.h), as before. Radio task, except where a
// member says any task.
#ifndef RADIO_AIR_MODES_H
#define RADIO_AIR_MODES_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "radio/radio.h"
#include "radio/radio_air.h"

// What a scan stop or dual watch hears of a mode's transmission (the row's heard()); moved from radio_sm.c
enum { RADIO_AIR_HEARD_NONE, RADIO_AIR_HEARD_PENDING, RADIO_AIR_HEARD_PLAYED, RADIO_AIR_HEARD_OTHER };

// The row's auto_tx() answer. Not keyed: NO, or key now and let go at once (ONCE: a packet, whose frames end the
// transmission), or key now and hold (HOLD: a test run that lasts until the mode says stop or the TX time-out).
// While a HOLD transmission is keyed the radio core asks again every tick: HOLD keeps it, NO lets go (tx_stop)
enum { RADIO_AIR_AUTO_NO, RADIO_AIR_AUTO_ONCE, RADIO_AIR_AUTO_HOLD };

// A mode's names, FM and DMR included, for every table that names modes (the debug link's mode key, radio_udp's JSON,
// the debug link's mode word): radio_mode_key "fm", "dmr", "p25", "nxdn", "m17"; radio_mode_name the word a display
// shows, never translated ("FM", "DMR", "P25", "NXDN", "M17"). "?" for a mode the build doesn't know. Any task
const char *radio_mode_key(uint8_t mode);
const char *radio_mode_name(uint8_t mode);

#if CONFIG_AIR
typedef struct {
	uint8_t mode;				// the radio_mode_t this row runs (RADIO_MODE_P25, ...)
	const char *key;			// radio_mode_key's word for it: "p25"
	const char *name;			// radio_mode_name's: "P25"
	// What the mode adds to the radio's feature string while it fits (build_modes_now): "P25,P25_TRUNK,",
	// "NXDN,", "M17,", each name followed by a comma, as the string has them today
	const char *features;
	bool (*fits)(void);			// the board's provider carries it (any task): offered by the UI, the VFO
	bool (*take)(channel_t *ch);		// a channel read from the codeplug becomes this mode (radio_*_take)
	// The provider receives channel ch at number (0: the VFO): what scan and dual watch visit. NULL: fits()
	bool (*receives)(const channel_t *ch, uint16_t number);
	// On the mode's channel, tuned, the DMR stack stopped. own_id: the radio's DMR ID (P25 takes it; others ignore it)
	bool (*begin)(const channel_t *ch, uint16_t number, uint32_t own_id);
	void (*end)(void);
	bool (*running)(void);
	void (*retuned)(void);
	// The settings changed while it transmitted: taken now
	void (*apply)(const channel_t *ch, uint16_t number, uint32_t own_id);
	// Every tick: the engine's view into v's part for this mode. True when it changed
	bool (*poll)(radio_view_t *v);
	// From v: a transmission on the channel shows as a reception; what a scan stop hears of it
	// (RADIO_AIR_HEARD_*); the green LED
	bool (*rx)(const radio_view_t *v);
	uint8_t (*heard)(const radio_view_t *v);
	bool (*played)(const radio_view_t *v);
	// The engine stopped (air_stop): v's part says off and idle
	void (*view_off)(radio_view_t *v);
	// Refusal before anything is queued (TXDENY_NONE: none), beyond the shared checks: M17 without a callsign.
	// NULL: none
	uint8_t (*tx_deny)(void);
	// The mode wants to transmit on its own (a packet, a test run): RADIO_AIR_AUTO_*. Not keyed: the radio core keys
	// as for PTT, with view.tx_auto, on ONCE or HOLD, and on ONCE calls ops->tx_stop at once (the engine ends the
	// transmission when its frames are out). keyed: a HOLD transmission is on air; NO now lets go. busy: the channel
	// is busy (carrier, or a reception). The TX time-out applies to both. NULL: never
	uint8_t (*auto_tx)(bool busy, bool keyed, uint32_t now_ms);
	const radio_air_ops_t *ops;
} radio_air_mode_t;

// The built modes' rows, and how many (radio_air_modes.c)
extern const radio_air_mode_t *const radio_air_modes[];
extern const uint8_t radio_air_mode_count;
// The row for a radio_mode_t, NULL when the build has none for it (FM, DMR, a mode switched off)
const radio_air_mode_t *radio_air_mode_for(uint8_t mode);

// Each mode's row, defined in its module
#if CONFIG_P25
extern const radio_air_mode_t radio_p25_mode;
#endif
#if CONFIG_NXDN
extern const radio_air_mode_t radio_nxdn_mode;
#endif
#if CONFIG_M17
extern const radio_air_mode_t radio_m17_mode;
#endif
#endif

#endif
