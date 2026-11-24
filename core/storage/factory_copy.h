// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The factory copy (docs/spec/settings-store.md 4.2): a verified copy of the maker's data in one factory range of the
// flash map (hal_flash_map_t.factory), which the core keeps in a record sector the board names (board_t.factory_copy,
// factory_copy_cfg_t in board.h). Read once at start (factory_copy_load, from app.c after the codeplug and before the
// radio): the range and the record, each twice; the source of the bytes in use is the range when it passes the board's
// check, else the record when it does, and neither when both pass and differ. The record is written once, by the
// firmware itself: when the storage region is ours, the range passes and the record sector is blank. Nothing here
// writes the range. CONFIG_FACTORY_COPY builds only; no RTOS, so the host tests run it
#ifndef RFOUNDRY_FACTORY_COPY_H
#define RFOUNDRY_FACTORY_COPY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "board.h"

// The largest range a board may copy: the load holds two reads of it and two of its record in the shared storage
// buffer
#define FACTORY_COPY_MAX	960u

// The record, at offset 0 of its sector (the rest of the sector 0xFF): magic "RFFC", version u16, n u16, the range's
// address u32, CRC-32 of the n bytes, their sha256, 16 bytes 0xFF, the n bytes, then the CRC-32 of everything before
#define FACTORY_COPY_MAGIC	0x43464652u	// bytes 52 46 46 43
#define FACTORY_COPY_VERSION	1u
#define FACTORY_COPY_REC_HDR	64u
#define FACTORY_COPY_REC_LEN(n)	(FACTORY_COPY_REC_HDR + (n) + 4u)

// The source of the bytes in use (STORAGE op 3's numbering)
enum {
	FACTORY_COPY_SRC_NONE,
	FACTORY_COPY_SRC_WINDOW,	// the range itself
	FACTORY_COPY_SRC_RECORD,	// the record: the range failed check (or couldn't be read)
};

// The range ("the window") as the load found it
enum {
	FACTORY_COPY_WIN_UNREAD,	// a read failed, two reads differed, the shared buffer was taken, or no usable configuration
	FACTORY_COPY_WIN_BLANK,		// all 0xFF (fails check)
	FACTORY_COPY_WIN_ZERO,		// all 0x00 (fails check)
	FACTORY_COPY_WIN_REFUSED,	// fails check
	FACTORY_COPY_WIN_OK,		// passes check
};

// The record sector as the load found it
enum {
	FACTORY_COPY_REC_UNREAD,	// a read failed or two reads differed (or the window is unread for a reason of its own)
	FACTORY_COPY_REC_BLANK,		// the whole sector 0xFF
	FACTORY_COPY_REC_OK,		// our magic and version, every field, both CRCs and the sha256 hold, the bytes pass check
	FACTORY_COPY_REC_BAD,		// our magic and version, but one of those fails (a record for another range among them)
	FACTORY_COPY_REC_NEWER,		// our magic, a later version: shown, never used, never overwritten
	FACTORY_COPY_REC_OTHER,		// neither blank nor our magic
};

// The reason beside the window's state: the board's check numbers 1-0xEF; these are the copy's own
enum {
	FACTORY_COPY_WHY_NONE = 0,
	FACTORY_COPY_WHY_READ = 0xF0,	// unread: no flash, or a read failed
	FACTORY_COPY_WHY_UNSTABLE,	// unread: two reads differed
	FACTORY_COPY_WHY_CONFLICT,	// window and record both pass check and differ: neither is used
	FACTORY_COPY_WHY_BUFFER,	// unread: the shared storage buffer was taken
	FACTORY_COPY_WHY_CONFIG,	// unread: the board's configuration doesn't fit its map (one log line)
};

// Whether the record was written at this start
enum {
	FACTORY_COPY_WROTE_NO,
	FACTORY_COPY_WROTE_YES,		// written and read back
	FACTORY_COPY_WROTE_FAILED,	// tried and failed: shown, tried again at the next start, never in between
};

typedef struct {
	bool loaded;			// the load ran at this start
	uint8_t source;			// FACTORY_COPY_SRC_*
	uint8_t win;			// FACTORY_COPY_WIN_*
	uint8_t why;			// the board's check's reason, or FACTORY_COPY_WHY_*
	uint16_t off;			// the board's check's offset
	uint8_t rec;			// FACTORY_COPY_REC_*
	uint8_t wrote;			// FACTORY_COPY_WROTE_*
	uint32_t win_crc;		// CRC-32 (IEEE, storage_crc32) of the window's bytes; 0 when unread
	uint32_t rec_crc;		// of the record's bytes, when it is ok or bad; else 0
} factory_copy_state_t;

// At start, on the start-up task, once (c: board.factory_copy). Fills c->table with the bytes in use; takes the storage
// lock and the shared storage buffer for its length (the window and the record unread when the buffer is taken)
void factory_copy_load(const factory_copy_cfg_t *c);
bool factory_copy_loaded(void);
void factory_copy_get(factory_copy_state_t *s);

// The names the view shows (NULL for a reason of the board's own)
const char *factory_copy_src_name(uint8_t src);
const char *factory_copy_win_name(uint8_t win);
const char *factory_copy_rec_name(uint8_t rec);
const char *factory_copy_why_name(uint8_t why);

// The record for n bytes of the range at addr: FACTORY_COPY_REC_LEN(n) bytes into rec. bytes is either rec +
// FACTORY_COPY_REC_HDR (built in place) or apart from rec
void factory_copy_record_build(uint8_t *rec, uint32_t addr, const uint8_t *bytes, uint32_t n);
// The FACTORY_COPY_REC_LEN(n) bytes at rec, for c's range: FACTORY_COPY_REC_BLANK when they are all 0xFF (the rest
// of the sector is the caller's to check), else OK, BAD, NEWER or OTHER
uint8_t factory_copy_record_parse(const factory_copy_cfg_t *c, const uint8_t *rec);

// STORAGE op 3 FACTORY_INFO (debuglink.md 3.13): its reply after the status byte, for the board's configuration c
// (NULL: the board has none, a reply of one byte, present 0). False while the load hasn't run at this start (the debug
// link answers BUSY). out holds at least FACTORY_COPY_INFO_LEN bytes
#define FACTORY_COPY_INFO_LEN	25u
bool factory_copy_info(const factory_copy_cfg_t *c, uint8_t *out, size_t *len);

#endif
