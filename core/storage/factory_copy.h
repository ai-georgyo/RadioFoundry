// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The factory copy (docs/spec/settings-store.md 4.2): a verified copy of the maker's data in one factory range of the
// flash map (hal_flash_map_t.factory), which the core keeps in a record sector the board names (board_t.factory_copy,
// factory_copy_cfg_t in board.h). Read once at start (factory_copy_load, from app.c after the codeplug and before the
// radio): the range and the record, each twice; the source of the bytes in use is the range when it passes the board's
// check, else the record when it does, and neither when both pass and differ. The record is written once, by the
// firmware itself: when the storage region is ours, the range passes and the record sector is blank. The one way the
// range itself is written is the restore (STORAGE ops 4 and 5): bytes the host loaded, checked, then a job on the
// housekeeping task that checks again under the storage lock and writes the range, verifies it and writes the record.
// CONFIG_FACTORY_COPY builds only; no RTOS, so the host tests run it
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
// The board took the state (factory_copy_get) and c->table as the table in use: at its radio init, and after every
// changed(). Until then STORAGE ops 4 and 5 answer BUSY, so no restore job writes c->table while the board reads it
void factory_copy_taken(void);

// The restore's result (STORAGE op 3's byte 21, the board's view): op 5's refusals in the order it checks them, then
// the job's own failures. Numbers append only
enum {
	FACTORY_COPY_RS_OK,
	FACTORY_COPY_RS_FOREIGN,	// 1 the storage region isn't ours (foreign, none, no map)
	FACTORY_COPY_RS_BUSY,		// 2 the board's busy(), a storage job pending, the shared storage buffer taken
	FACTORY_COPY_RS_STAGE,		// 3 the bytes loaded aren't the request's n, or n isn't the range's size
	FACTORY_COPY_RS_CRC,		// 4 the loaded bytes' CRC-32 isn't the request's
	FACTORY_COPY_RS_INVALID,	// 5 the loaded bytes fail the board's check (why, off)
	FACTORY_COPY_RS_UNSTABLE,	// 6 two reads of the window differ, or one failed
	FACTORY_COPY_RS_WINDOW_CRC,	// 7 the window's CRC-32 now isn't the one the host read
	FACTORY_COPY_RS_WINDOW_DIFFERS,	// 8 the window passes check and holds other bytes
	FACTORY_COPY_RS_RECORD_DIFFERS,	// 9 the record is ok and holds other bytes
	FACTORY_COPY_RS_RECORD_OTHER,	// 10 the record's sector is other, newer or unreadable: unknown bytes
	FACTORY_COPY_RS_VERIFY,		// 11 the job: the window read back isn't the loaded bytes; the record not written
	FACTORY_COPY_RS_RECORD,		// 12 the job: the record's write or its read back failed; the window restored
	FACTORY_COPY_RS_FLASH,		// 13 the job: the window's write failed
	FACTORY_COPY_RS_PENDING = 0xFE,	// a job posted, not ended
	FACTORY_COPY_RS_NONE = 0xFF,	// no restore at this start
};
// The last restore's result now (FACTORY_COPY_RS_*), and its name ("ok", "window_differs", "pending", "none", ...)
uint8_t factory_copy_result(void);
const char *factory_copy_result_name(uint8_t result);

// STORAGE op 4 FACTORY_LOAD and op 5 FACTORY_RESTORE (debuglink.md 3.13), from the debug link's task, its one caller.
// req and len: the request after the op byte. The reply after the status byte goes to out (at least
// FACTORY_COPY_OP_OUT bytes), its length to *olen; the status (DL_ST_*) is returned. Op 4: offset u16, then 1 to
// FACTORY_COPY_LOAD_CHUNK bytes of the range (offset 0 starts over, any other offset is the bytes loaded so far) ->
// loaded u16. Op 5: n u16, the loaded bytes' CRC-32 u32, the CRC-32 of the window as the host read it u32 -> reason
// u8 (FACTORY_COPY_RS_*), why u8, off u16; OK: the job is posted (op 3 shows its result), REFUSED: the reason. Both
// answer UNAVAILABLE without a usable configuration and BUSY before the load, while a job is pending or running and
// until the board took its outcome (factory_copy_taken)
#define FACTORY_COPY_LOAD_CHUNK	56u
#define FACTORY_COPY_OP_OUT	4u
uint8_t factory_copy_load_op(const factory_copy_cfg_t *c, const uint8_t *req, uint16_t len, uint8_t *out, size_t *olen);
uint8_t factory_copy_restore_op(const factory_copy_cfg_t *c, const uint8_t *req, uint16_t len, uint8_t *out,
				size_t *olen);

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
