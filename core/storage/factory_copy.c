// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The factory copy (factory_copy.h, docs/spec/settings-store.md 4.2). The load at start, and every read of the restore,
// in the shared storage buffer:
//   b + 0        the window, first read        b + n        the window, second read
//   b + 2n       the record, first read        b + 2n + r   the record, second read (r = FACTORY_COPY_REC_LEN(n)),
//                                                           then the rest of the sector, a piece at a time
// and a record write builds the record's whole sector in the same buffer, then reads it back over it.
//
// Who runs what: the load on the start-up task, before anyone else looks; ops 4 and 5 on the debug link's task; the
// restore's job on the housekeeping task; the board takes the state on its radio task. The state the job leaves is
// published under the quick lock; the loaded bytes and the request are the debug link's until it posts the job and the
// job's until it ends (ops 4 and 5 answer BUSY in between)
#include <string.h>
#include "config.h"
#include "storage/factory_copy.h"

#if CONFIG_FACTORY_COPY
#include "storage/storage.h"
#include "support/sha256.h"
#include "debuglink/dl_frame.h"
#include "debuglink/dlog.h"

#define REC_SHA		16u	// the sha256 of the bytes
#define REC_RESERVED	48u	// 16 bytes 0xFF up to the bytes

static factory_copy_state_t st;
static bool volatile loaded;

// The restore. The host's bytes (op 4), the request (op 5) and the configuration the job runs for
static uint8_t ld_buf[FACTORY_COPY_MAX] RFOUNDRY_COLD_RAM;
static uint16_t volatile ld_n;
static uint16_t rq_n;
static uint32_t rq_crc, rq_wcrc;
static const factory_copy_cfg_t *job_cfg;
static const factory_copy_cfg_t *ok_cfg;	// the configuration the load found fitting the map: the only one restored
static bool volatile job_on;		// posted, not ended
static bool volatile untaken;		// a state the board hasn't taken yet (the load's, or a job's)
static uint8_t volatile result = FACTORY_COPY_RS_NONE;

static void put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

static uint32_t get16(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
	return get16(p) | (get16(p + 2) << 16);
}

static bool all(const uint8_t *b, uint32_t n, uint8_t v)
{
	for (uint32_t i = 0; i < n; i++) {
		if (b[i] != v) {
			return false;
		}
	}
	return true;
}

static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return blen != 0 && a < b + blen && b < a + alen;
}

// The board's range, or NULL when c names none of the map
static const hal_flash_range_t *range_of(const factory_copy_cfg_t *c)
{
	const hal_flash_map_t *m = storage_map();
	if (c == NULL || m == NULL || c->range >= HAL_FLASH_FACTORY_MAX || m->factory[c->range].size == 0) {
		return NULL;
	}
	return &m->factory[c->range];
}

// The configuration against the map and the chip: a range of at most FACTORY_COPY_MAX bytes on the chip, and a record
// sector on the chip clear of every area of the map and of every factory range (so that no other writer of the core
// reaches it, and the record is never a kept sector)
static bool cfg_ok(const factory_copy_cfg_t *c, const hal_flash_map_t *m, const hal_flash_range_t *r)
{
	uint32_t a = c->record_addr, size = board.flash->size;
	if (r == NULL || c->check == NULL || c->table == NULL || r->size > FACTORY_COPY_MAX || r->addr > size ||
	    r->size > size - r->addr || a % STORAGE_SECTOR != 0 || a > size || size - a < STORAGE_SECTOR) {
		return false;
	}
	uint32_t fw_flash = m->openfw_end > m->openfw_flash ? m->openfw_end - m->openfw_flash : 0u;
	if (overlaps(a, STORAGE_SECTOR, m->settings_addr, m->settings_size) ||
	    overlaps(a, STORAGE_SECTOR, m->journal_addr, 2u * STORAGE_SECTOR) ||
	    overlaps(a, STORAGE_SECTOR, m->openfw_eeprom, 0x10000u) ||
	    overlaps(a, STORAGE_SECTOR, m->openfw_flash, fw_flash) ||
	    overlaps(a, STORAGE_SECTOR, m->calibration_addr, m->calibration_size) ||
	    overlaps(a, STORAGE_SECTOR, m->native_addr, m->native_size) ||
	    overlaps(a, STORAGE_SECTOR, m->staging_addr, m->staging_size) ||
	    overlaps(a, STORAGE_SECTOR, m->satellite_addr, m->satellite_size)) {
		return false;
	}
	for (unsigned i = 0; i < HAL_FLASH_FACTORY_MAX; i++) {
		if (overlaps(a, STORAGE_SECTOR, m->factory[i].addr, m->factory[i].size)) {
			return false;
		}
	}
	return true;
}

static void sha(const uint8_t *b, uint32_t n, uint8_t out[SHA256_LEN])
{
	sha256_t s;
	sha256_init(&s);
	sha256_update(&s, b, n);
	sha256_final(&s, out);
}

void factory_copy_record_build(uint8_t *rec, uint32_t addr, const uint8_t *bytes, uint32_t n)
{
	uint8_t h[SHA256_LEN];
	uint32_t crc = storage_crc32(0, bytes, n);
	sha(bytes, n, h);
	if (bytes != rec + FACTORY_COPY_REC_HDR) {
		memcpy(rec + FACTORY_COPY_REC_HDR, bytes, n);
	}
	memset(rec, 0xFF, FACTORY_COPY_REC_HDR);
	put32(rec, FACTORY_COPY_MAGIC);
	put16(rec + 4, FACTORY_COPY_VERSION);
	put16(rec + 6, n);
	put32(rec + 8, addr);
	put32(rec + 12, crc);
	memcpy(rec + REC_SHA, h, SHA256_LEN);
	put32(rec + FACTORY_COPY_REC_HDR + n, storage_crc32(0, rec, FACTORY_COPY_REC_HDR + n));
}

uint8_t factory_copy_record_parse(const factory_copy_cfg_t *c, const uint8_t *rec)
{
	const hal_flash_range_t *r = range_of(c);
	if (r == NULL) {
		return FACTORY_COPY_REC_UNREAD;
	}
	uint32_t n = r->size;
	if (all(rec, FACTORY_COPY_REC_LEN(n), 0xFF)) {
		return FACTORY_COPY_REC_BLANK;
	}
	if (get32(rec) != FACTORY_COPY_MAGIC) {
		return FACTORY_COPY_REC_OTHER;
	}
	uint32_t v = get16(rec + 4);
	if (v > FACTORY_COPY_VERSION) {
		return FACTORY_COPY_REC_NEWER;
	}
	// A record is never taken for another range: its address and length are the range's
	uint8_t h[SHA256_LEN];
	const uint8_t *bytes = rec + FACTORY_COPY_REC_HDR;
	if (v != FACTORY_COPY_VERSION || get16(rec + 6) != n || get32(rec + 8) != r->addr ||
	    !all(rec + REC_RESERVED, FACTORY_COPY_REC_HDR - REC_RESERVED, 0xFF) ||
	    get32(bytes + n) != storage_crc32(0, rec, FACTORY_COPY_REC_HDR + n) ||
	    get32(rec + 12) != storage_crc32(0, bytes, n)) {
		return FACTORY_COPY_REC_BAD;
	}
	sha(bytes, n, h);
	uint8_t why = 0;
	uint16_t off = 0;
	if (memcmp(rec + REC_SHA, h, SHA256_LEN) != 0 || !c->check(bytes, n, &why, &off)) {
		return FACTORY_COPY_REC_BAD;
	}
	return FACTORY_COPY_REC_OK;
}

// The window twice into w and w + n: its state, why, off and CRC into s
static void load_window(const factory_copy_cfg_t *c, const hal_flash_range_t *r, uint8_t *w, factory_copy_state_t *s)
{
	uint32_t n = r->size;
	if (!board.flash->read(r->addr, w, n) || !board.flash->read(r->addr, w + n, n)) {
		s->why = FACTORY_COPY_WHY_READ;
		return;
	}
	if (memcmp(w, w + n, n) != 0) {
		s->why = FACTORY_COPY_WHY_UNSTABLE;
		return;
	}
	s->win_crc = storage_crc32(0, w, n);
	uint8_t why = 0;
	uint16_t off = 0;
	if (c->check(w, n, &why, &off)) {
		s->win = FACTORY_COPY_WIN_OK;
		return;
	}
	s->why = why;
	s->off = off;
	s->win = all(w, n, 0xFF) ? FACTORY_COPY_WIN_BLANK : (all(w, n, 0) ? FACTORY_COPY_WIN_ZERO : FACTORY_COPY_WIN_REFUSED);
}

// The record twice into p and p + its length, parsed; a blank one only when the rest of its sector is blank too (read
// into the second copy's room, a piece at a time). Its state, and the CRC of its bytes into s when it is ours
static uint8_t load_record(const factory_copy_cfg_t *c, uint32_t n, uint8_t *p, factory_copy_state_t *st_out)
{
	uint32_t a = c->record_addr, len = FACTORY_COPY_REC_LEN(n);
	if (!board.flash->read(a, p, len) || !board.flash->read(a, p + len, len) || memcmp(p, p + len, len) != 0) {
		return FACTORY_COPY_REC_UNREAD;
	}
	uint8_t s = factory_copy_record_parse(c, p);
	if (s == FACTORY_COPY_REC_OK || s == FACTORY_COPY_REC_BAD) {
		st_out->rec_crc = storage_crc32(0, p + FACTORY_COPY_REC_HDR, n);
	}
	for (uint32_t off = len; s == FACTORY_COPY_REC_BLANK && off < STORAGE_SECTOR; off += len) {
		uint32_t piece = STORAGE_SECTOR - off < len ? STORAGE_SECTOR - off : len;
		if (!board.flash->read(a + off, p + len, piece)) {
			return FACTORY_COPY_REC_UNREAD;
		}
		s = all(p + len, piece, 0xFF) ? FACTORY_COPY_REC_BLANK : FACTORY_COPY_REC_OTHER;
	}
	return s;
}

// A record for bytes (n of them, not inside b): its whole sector built in b and written through the journal (a cut
// leaves the sector as it was, or the replay at the next start finishes it), then read back over b and parsed. True
// when it is ok and holds bytes, byte for byte; s->rec and s->rec_crc as read back
static bool write_record(const factory_copy_cfg_t *c, const hal_flash_range_t *r, uint8_t *b, const uint8_t *bytes,
			 factory_copy_state_t *s)
{
	uint32_t n = r->size, len = FACTORY_COPY_REC_LEN(n);
	factory_copy_record_build(b, r->addr, bytes, n);
	memset(b + len, 0xFF, STORAGE_SECTOR - len);
	bool ok = storage_sector_write(c->record_addr, b);
	s->rec_crc = 0;
	s->rec = load_record(c, n, b, s);
	return ok && s->rec == FACTORY_COPY_REC_OK && memcmp(b + FACTORY_COPY_REC_HDR, bytes, n) == 0;
}

// The source order (settings-store.md 4.2) over the window and the record as s says b holds them: the bytes in use,
// NULL for none
static const uint8_t *choose(const hal_flash_range_t *r, const uint8_t *b, factory_copy_state_t *s)
{
	uint32_t n = r->size;
	const uint8_t *w = b, *rb = b + 2u * n + FACTORY_COPY_REC_HDR;

	if (s->win == FACTORY_COPY_WIN_OK) {
		if (s->rec == FACTORY_COPY_REC_OK && memcmp(w, rb, n) != 0) {
			s->why = FACTORY_COPY_WHY_CONFLICT;
			dlog("factory copy: window and record differ: none used");
			return NULL;
		}
		s->source = FACTORY_COPY_SRC_WINDOW;
		return w;
	}
	if (s->rec == FACTORY_COPY_REC_OK) {
		s->source = FACTORY_COPY_SRC_RECORD;
		return rb;
	}
	return NULL;
}

// The window and the record read into b, the source chosen and the bytes in use copied into the board's table (all
// 0xFF for none). True when the record may be written now: the window in use and the record blank
static bool read_state(const factory_copy_cfg_t *c, const hal_flash_range_t *r, uint8_t *b, factory_copy_state_t *s)
{
	load_window(c, r, b, s);
	s->rec = load_record(c, r->size, b + 2u * r->size, s);
	const uint8_t *use = choose(r, b, s);
	if (use != NULL) {
		memcpy(c->table, use, r->size);
	} else {
		memset(c->table, 0xFF, r->size);
	}
	return s->source == FACTORY_COPY_SRC_WINDOW && s->rec == FACTORY_COPY_REC_BLANK;
}

void factory_copy_load(const factory_copy_cfg_t *c)
{
	loaded = false;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	memset(&st, 0, sizeof(st));
	ld_n = 0;
	job_on = false;
	ok_cfg = NULL;
	result = FACTORY_COPY_RS_NONE;
	if (c == NULL) {
		return;
	}
	const hal_flash_map_t *m = storage_map();
	const hal_flash_range_t *r = range_of(c);
	if (m == NULL || board.flash->size == 0) {
		st.why = FACTORY_COPY_WHY_READ;
	} else if (!cfg_ok(c, m, r)) {
		st.why = FACTORY_COPY_WHY_CONFIG;
		dlog("factory copy: the board's range or record doesn't fit its map: not read");
	} else {
		ok_cfg = c;	// a restore needs the checker, the table and a record sector clear of every other writer
		storage_lock();
		uint8_t *b = storage_buffer_acquire();
		if (b == NULL) {
			st.why = FACTORY_COPY_WHY_BUFFER;
		} else {
			// The first-start write: only the firmware writes the record, once. The region ours (claimed; never
			// while foreign or none), the window passing, the record's sector blank. Never over anything else,
			// never the window
			if (read_state(c, r, b, &st) && storage_ready()) {
				bool ok = write_record(c, r, b, c->table, &st);
				st.wrote = ok ? FACTORY_COPY_WROTE_YES : FACTORY_COPY_WROTE_FAILED;
				if (ok) {
					dlog("factory copy: record written at 0x%06x", (unsigned)c->record_addr);
				} else {
					dlog("factory copy: record write at 0x%06x failed", (unsigned)c->record_addr);
				}
			}
			storage_buffer_release();
		}
		storage_unlock();
	}
	// No bytes in use: the table blank, which the board's check refuses as it refuses a blank range
	if (c->table != NULL && st.source == FACTORY_COPY_SRC_NONE && r != NULL && r->size <= FACTORY_COPY_MAX) {
		memset(c->table, 0xFF, r->size);
	}
	st.loaded = true;
	untaken = true;		// the board's radio init takes it
	// Every field above is written before a reader on another task (the debug link) sees loaded
	__atomic_thread_fence(__ATOMIC_RELEASE);
	loaded = true;
}

bool factory_copy_loaded(void)
{
	bool l = loaded;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return l;
}

void factory_copy_get(factory_copy_state_t *s)
{
	if (!factory_copy_loaded()) {
		memset(s, 0, sizeof(*s));
		return;
	}
	storage_quick_lock();	// a restore's job may be publishing a new state
	*s = st;
	storage_quick_unlock();
}

void factory_copy_taken(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);	// the board's reads of the table come before
	untaken = false;
}

uint8_t factory_copy_result(void)
{
	return result;
}

const char *factory_copy_src_name(uint8_t src)
{
	static const char *const names[] = { "none", "window", "record" };
	return src < sizeof(names) / sizeof(names[0]) ? names[src] : "?";
}

const char *factory_copy_win_name(uint8_t win)
{
	static const char *const names[] = { "unread", "blank", "zero", "refused", "ok" };
	return win < sizeof(names) / sizeof(names[0]) ? names[win] : "?";
}

const char *factory_copy_rec_name(uint8_t rec)
{
	static const char *const names[] = { "unread", "blank", "ok", "bad", "newer", "other" };
	return rec < sizeof(names) / sizeof(names[0]) ? names[rec] : "?";
}

const char *factory_copy_why_name(uint8_t why)
{
	static const char *const names[] = { "read", "unstable", "conflict", "buffer", "config" };
	if (why == FACTORY_COPY_WHY_NONE) {
		return "none";
	}
	unsigned i = (unsigned)why - FACTORY_COPY_WHY_READ;
	return why >= FACTORY_COPY_WHY_READ && i < sizeof(names) / sizeof(names[0]) ? names[i] : NULL;
}

const char *factory_copy_result_name(uint8_t res)
{
	static const char *const names[] = {
		"ok", "foreign", "busy", "stage", "crc", "invalid", "unstable", "window_crc", "window_differs",
		"record_differs", "record_other", "verify", "record", "flash",
	};
	if (res == FACTORY_COPY_RS_NONE || res == FACTORY_COPY_RS_PENDING) {
		return res == FACTORY_COPY_RS_NONE ? "none" : "pending";
	}
	return res < sizeof(names) / sizeof(names[0]) ? names[res] : "?";
}

bool factory_copy_info(const factory_copy_cfg_t *c, uint8_t *out, size_t *len)
{
	if (c == NULL) {
		out[0] = 0;		// present: the board keeps no factory copy
		*len = 1;
		return true;
	}
	if (!factory_copy_loaded()) {
		return false;
	}
	factory_copy_state_t s;
	factory_copy_get(&s);
	const hal_flash_range_t *r = range_of(c);
	out[0] = 1;
	put32(out + 1, r != NULL ? r->addr : 0u);
	put16(out + 5, r != NULL && r->size <= 0xFFFFu ? r->size : 0u);
	out[7] = s.source;
	out[8] = s.win;
	out[9] = s.why;
	put16(out + 10, s.off);
	put32(out + 12, s.win_crc);
	out[16] = s.rec;
	put32(out + 17, s.rec_crc);
	out[21] = result;		// the last restore's result (0xFF: none at this start, 0xFE: its job pending)
	put16(out + 22, ld_n);		// the bytes a restore has loaded
	out[24] = s.wrote;
	*len = FACTORY_COPY_INFO_LEN;
	return true;
}

// --- The restore (settings-store.md 4.2) ---

// The checks of op 5, in order, under the storage lock, for c's range r. The shared buffer is taken at check 2 into
// *bp (NULL when it isn't; the caller releases it) and holds the window's two reads and the record's after them. why
// and off: the board's check's, for INVALID
static uint8_t restore_checks(const factory_copy_cfg_t *c, const hal_flash_range_t *r, uint8_t **bp, uint8_t *why,
			      uint16_t *off)
{
	uint32_t n = r->size;
	factory_copy_state_t s;

	*bp = NULL;
	if (!storage_ready()) {
		return FACTORY_COPY_RS_FOREIGN;
	}
	if ((c->busy != NULL && c->busy()) || storage_job_busy() || (*bp = storage_buffer_acquire()) == NULL) {
		return FACTORY_COPY_RS_BUSY;
	}
	if (ld_n != rq_n || rq_n != n) {
		return FACTORY_COPY_RS_STAGE;
	}
	if (storage_crc32(0, ld_buf, n) != rq_crc) {
		return FACTORY_COPY_RS_CRC;
	}
	if (!c->check(ld_buf, n, why, off)) {
		return FACTORY_COPY_RS_INVALID;
	}
	uint8_t *b = *bp;
	memset(&s, 0, sizeof(s));
	load_window(c, r, b, &s);
	if (s.why == FACTORY_COPY_WHY_READ || s.why == FACTORY_COPY_WHY_UNSTABLE) {
		return FACTORY_COPY_RS_UNSTABLE;
	}
	if (s.win_crc != rq_wcrc) {
		return FACTORY_COPY_RS_WINDOW_CRC;
	}
	// An owner must not swap a good table for another radio's by accident
	if (s.win == FACTORY_COPY_WIN_OK && memcmp(b, ld_buf, n) != 0) {
		return FACTORY_COPY_RS_WINDOW_DIFFERS;
	}
	uint8_t rec = load_record(c, n, b + 2u * n, &s);
	if (rec == FACTORY_COPY_REC_OK && memcmp(b + 2u * n + FACTORY_COPY_REC_HDR, ld_buf, n) != 0) {
		return FACTORY_COPY_RS_RECORD_DIFFERS;
	}
	if (rec == FACTORY_COPY_REC_OTHER || rec == FACTORY_COPY_REC_NEWER || rec == FACTORY_COPY_REC_UNREAD) {
		return FACTORY_COPY_RS_RECORD_OTHER;
	}
	return FACTORY_COPY_RS_OK;
}

// The window read twice into b, against the loaded bytes
static bool window_holds(const hal_flash_range_t *r, uint8_t *b)
{
	uint32_t n = r->size;
	return board.flash->read(r->addr, b, n) && board.flash->read(r->addr, b + n, n) && memcmp(b, ld_buf, n) == 0 &&
	       memcmp(b + n, ld_buf, n) == 0;
}

// The job, on the housekeeping task. Step 0: every check again, under the storage lock, which it holds to its end so
// no writer runs between the checks and the window's write. 1: the window (unless it holds the bytes already); 2: two
// reads of it compared with the bytes; 3: the record. Then the state as the flash now is, the board's table with it,
// and changed()
static void restore_job(void *ctx)
{
	(void)ctx;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);	// the bytes and the request, as the debug link left them
	const factory_copy_cfg_t *c = job_cfg;
	const hal_flash_range_t *r = range_of(c);
	uint8_t *b, why = 0;
	uint16_t off = 0;
	bool past0 = false;

	storage_lock();
	uint8_t res = restore_checks(c, r, &b, &why, &off);
	if (res == FACTORY_COPY_RS_OK) {
		past0 = true;
		uint32_t n = r->size;
		factory_copy_state_t s;
		memset(&s, 0, sizeof(s));
		// 1: b holds the window's first read from the checks
		if (memcmp(b, ld_buf, n) != 0 && !storage_factory_write(r->addr, ld_buf, n)) {
			res = FACTORY_COPY_RS_FLASH;
		} else if (!window_holds(r, b)) {
			res = FACTORY_COPY_RS_VERIFY;		// 2: the record not touched
		} else if (!write_record(c, r, b, ld_buf, &s)) {
			res = FACTORY_COPY_RS_RECORD;		// 3: the window restored, the record as the flash has it
		}
		// The state as the flash now is, whatever the job left, read from scratch; the first-start write's outcome
		// kept
		memset(&s, 0, sizeof(s));
		s.loaded = true;
		s.wrote = st.wrote;
		(void)read_state(c, r, b, &s);
		storage_quick_lock();
		st = s;
		storage_quick_unlock();
	}
	if (b != NULL) {
		storage_buffer_release();
	}
	storage_unlock();
	if (res == FACTORY_COPY_RS_OK) {
		dlog("factory copy: restored 0x%06x, record written", (unsigned)r->addr);
	} else {
		dlog("factory copy: restore %s (%u)", factory_copy_result_name(res), (unsigned)res);
	}
	result = res;
	// After every job that reached the window's step, whatever its result: the board takes the window as it is now,
	// and ops 4 and 5 wait until it did. A board without changed() never takes a later state: nothing waits for it.
	// Both before the job ends, so no op runs between the new state and the board's request
	if (past0) {
		untaken = c->changed != NULL;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (c->changed != NULL) {
			c->changed();
		}
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	job_on = false;
}

// The states in which ops 4 and 5 wait: before the load, a job pending or running (its slot too: it is freed only
// after the job returns), its outcome not taken
static bool restore_busy(void)
{
	return !factory_copy_loaded() || job_on || storage_factory_job_busy() || untaken;
}

// Ops 4 and 5: UNAVAILABLE without a range for c, BUSY in the states above, UNAVAILABLE again for a configuration the
// load didn't accept (a checker or table missing, a record sector some other writer reaches: settings-store.md 4.2).
// DL_ST_OK to go on
static uint8_t op_gate(const factory_copy_cfg_t *c, const hal_flash_range_t *r)
{
	if (r == NULL || r->size > FACTORY_COPY_MAX) {
		return DL_ST_UNAVAILABLE;
	}
	if (restore_busy()) {
		return DL_ST_BUSY;
	}
	return c == ok_cfg ? DL_ST_OK : DL_ST_UNAVAILABLE;
}

uint8_t factory_copy_load_op(const factory_copy_cfg_t *c, const uint8_t *req, uint16_t len, uint8_t *out, size_t *olen)
{
	const hal_flash_range_t *r = range_of(c);
	uint8_t g = op_gate(c, r);
	*olen = 0;
	if (g != DL_ST_OK) {
		return g;
	}
	if (len < 3u || len > 2u + FACTORY_COPY_LOAD_CHUNK) {
		return DL_ST_BAD_ARG;
	}
	uint32_t at = get16(req), k = len - 2u;
	if ((at != 0 && at != ld_n) || at + k > r->size) {
		return DL_ST_BAD_ARG;
	}
	memcpy(ld_buf + at, req + 2, k);
	ld_n = (uint16_t)(at + k);
	put16(out, ld_n);
	*olen = 2;
	return DL_ST_OK;
}

uint8_t factory_copy_restore_op(const factory_copy_cfg_t *c, const uint8_t *req, uint16_t len, uint8_t *out,
				size_t *olen)
{
	const hal_flash_range_t *r = range_of(c);
	uint8_t *b, why = 0;
	uint16_t off = 0;
	uint8_t g = op_gate(c, r);
	*olen = 0;
	if (g != DL_ST_OK) {
		return g;
	}
	if (len != 10u) {
		return DL_ST_BAD_ARG;
	}
	rq_n = (uint16_t)get16(req);
	rq_crc = get32(req + 2);
	rq_wcrc = get32(req + 6);
	storage_lock();
	uint8_t res = restore_checks(c, r, &b, &why, &off);
	if (b != NULL) {
		storage_buffer_release();
	}
	storage_unlock();
	if (res == FACTORY_COPY_RS_OK) {
		job_cfg = c;
		result = FACTORY_COPY_RS_PENDING;
		job_on = true;
		__atomic_thread_fence(__ATOMIC_RELEASE);	// the bytes and the request before the job can run
		if (!storage_factory_job_post(restore_job, NULL)) {
			job_on = false;
			res = FACTORY_COPY_RS_BUSY;
		}
	}
	out[0] = res;
	out[1] = res == FACTORY_COPY_RS_INVALID ? why : 0u;
	put16(out + 2, res == FACTORY_COPY_RS_INVALID ? off : 0u);
	*olen = FACTORY_COPY_OP_OUT;
	if (res != FACTORY_COPY_RS_OK) {
		result = res;
		dlog("factory copy: restore refused: %s", factory_copy_result_name(res));
		return DL_ST_REFUSED;
	}
	return DL_ST_OK;
}
#endif
