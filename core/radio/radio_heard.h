// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stations heard (docs/spec/radio-features.md, the heard list): the latest transmissions' callers, one entry per
// station, newest first, kept while the radio is on (RAM only). Generic, any mode: an entry names the mode and the
// caller by a mode's own address (M17's 48-bit callsign address) or a numeric ID (DMR, P25, NXDN, when those modes
// add themselves). M17 is its first source. The radio task adds; any task reads a consistent copy (a sequence count
// around each write). Built with CONFIG_HEARD_LIST.
#ifndef RADIO_HEARD_H
#define RADIO_HEARD_H

#include <stdbool.h>
#include <stdint.h>

#define RADIO_HEARD_MAX		8u

// radio_heard_t.flags
#define RADIO_HEARD_ADDR	0x01u	// addr holds the caller (else id)
#define RADIO_HEARD_SIG_OK	0x02u	// its latest transmission's signature verified
#define RADIO_HEARD_SIG_BAD	0x04u	// ... failed to verify
#define RADIO_HEARD_ENC		0x08u	// ... was encrypted
#define RADIO_HEARD_GNSS	0x10u	// ... carried a position (lat_e7, lon_e7)
#define RADIO_HEARD_DATA	0x20u	// ... was data (a packet), not voice

typedef struct {
	uint8_t mode;		// radio_mode_t
	uint8_t flags;		// RADIO_HEARD_*
	uint8_t addr[6];	// a mode's address, big endian
	uint32_t id;		// or a numeric ID
	uint8_t to[6];		// what it called (an address; all 0xFF broadcast; zero none)
	uint16_t count;		// transmissions heard from it (saturates)
	int16_t rssi_dbm;	// at its latest, 0 unknown
	uint32_t last_ms;	// hal_millis of its latest
	int32_t lat_e7, lon_e7;	// with RADIO_HEARD_GNSS
} radio_heard_t;

// Radio task: a transmission from e's caller (e->last_ms, e->count are set here: the entry moves to the front)
void radio_heard_add(const radio_heard_t *e);
// Any task
uint8_t radio_heard_count(void);
bool radio_heard_get(uint8_t i, radio_heard_t *out);	// 0 = the newest
// The entry of key's station (the same mode and address, or ID) as it is now, wherever the list moved it; false when
// it has left the list. A reader that holds on to a station keeps a copy of it, not its index: every reception moves
// the entries
bool radio_heard_find(const radio_heard_t *key, radio_heard_t *out);
uint8_t radio_heard_seq(void);				// bumps on every change
void radio_heard_clear(void);				// radio task

#endif
