// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The factory copy (factory_copy.h, docs/spec/settings-store.md 4.2). The load at start, its buffer:
//   b + 0        the window, first read        b + n        the window, second read
//   b + 2n       the record, first read        b + 2n + r   the record, second read (r = FACTORY_COPY_REC_LEN(n)),
//                                                           then the rest of the sector, a piece at a time
// and the first-start write builds the record's whole sector in the same buffer, then reads it back over it.
#include <string.h>
#include "config.h"
#include "storage/factory_copy.h"

#if CONFIG_FACTORY_COPY
#include "storage/storage.h"
#include "support/sha256.h"
#include "debuglink/dlog.h"

#define REC_SHA		16u	// the sha256 of the bytes
#define REC_RESERVED	48u	// 16 bytes 0xFF up to the bytes

static factory_copy_state_t st;
static bool volatile loaded;

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

// The window twice into w and w + n: its state, why, off and CRC
static void load_window(const factory_copy_cfg_t *c, const hal_flash_range_t *r, uint8_t *w)
{
	uint32_t n = r->size;
	if (!board.flash->read(r->addr, w, n) || !board.flash->read(r->addr, w + n, n)) {
		st.why = FACTORY_COPY_WHY_READ;
		return;
	}
	if (memcmp(w, w + n, n) != 0) {
		st.why = FACTORY_COPY_WHY_UNSTABLE;
		return;
	}
	st.win_crc = storage_crc32(0, w, n);
	uint8_t why = 0;
	uint16_t off = 0;
	if (c->check(w, n, &why, &off)) {
		st.win = FACTORY_COPY_WIN_OK;
		return;
	}
	st.why = why;
	st.off = off;
	st.win = all(w, n, 0xFF) ? FACTORY_COPY_WIN_BLANK : (all(w, n, 0) ? FACTORY_COPY_WIN_ZERO : FACTORY_COPY_WIN_REFUSED);
}

// The record twice into p and p + its length, parsed; a blank one only when the rest of its sector is blank too (read
// into the second copy's room, a piece at a time). Its state, and the CRC of its bytes when it is ours
static uint8_t load_record(const factory_copy_cfg_t *c, uint32_t n, uint8_t *p)
{
	uint32_t a = c->record_addr, len = FACTORY_COPY_REC_LEN(n);
	if (!board.flash->read(a, p, len) || !board.flash->read(a, p + len, len) || memcmp(p, p + len, len) != 0) {
		return FACTORY_COPY_REC_UNREAD;
	}
	uint8_t s = factory_copy_record_parse(c, p);
	if (s == FACTORY_COPY_REC_OK || s == FACTORY_COPY_REC_BAD) {
		st.rec_crc = storage_crc32(0, p + FACTORY_COPY_REC_HDR, n);
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

// The first-start write: the record's whole sector built in b and written through the journal (a cut leaves the
// sector blank, or the replay at the next start finishes it), then read back over b and parsed: it must be ok and hold
// the window's bytes (in the board's table by now), byte for byte
static void write_record(const factory_copy_cfg_t *c, const hal_flash_range_t *r, uint8_t *b)
{
	uint32_t n = r->size, len = FACTORY_COPY_REC_LEN(n);
	factory_copy_record_build(b, r->addr, c->table, n);
	memset(b + len, 0xFF, STORAGE_SECTOR - len);
	bool ok = storage_sector_write(c->record_addr, b);
	st.rec_crc = 0;
	st.rec = load_record(c, n, b);
	ok = ok && st.rec == FACTORY_COPY_REC_OK && memcmp(b + FACTORY_COPY_REC_HDR, c->table, n) == 0;
	st.wrote = ok ? FACTORY_COPY_WROTE_YES : FACTORY_COPY_WROTE_FAILED;
	if (ok) {
		dlog("factory copy: record written at 0x%06x", (unsigned)c->record_addr);
	} else {
		dlog("factory copy: record write at 0x%06x failed", (unsigned)c->record_addr);
	}
}

// The source order (settings-store.md 4.2), the bytes in use into the board's table, and the first-start write
static void choose(const factory_copy_cfg_t *c, const hal_flash_range_t *r, uint8_t *b)
{
	uint32_t n = r->size;
	const uint8_t *w = b, *rb = b + 2u * n + FACTORY_COPY_REC_HDR;
	const uint8_t *use = NULL;

	if (st.win == FACTORY_COPY_WIN_OK) {
		if (st.rec == FACTORY_COPY_REC_OK && memcmp(w, rb, n) != 0) {
			st.why = FACTORY_COPY_WHY_CONFLICT;
			dlog("factory copy: window and record differ: none used");
		} else {
			st.source = FACTORY_COPY_SRC_WINDOW;
			use = w;
		}
	} else if (st.rec == FACTORY_COPY_REC_OK) {
		st.source = FACTORY_COPY_SRC_RECORD;
		use = rb;
	}
	if (use != NULL) {
		memcpy(c->table, use, n);
	}
	// Only the firmware writes the record, once: the region ours (claimed; never while foreign or none), the window
	// passing, the record's sector blank. Never over anything else, never the window
	if (st.source == FACTORY_COPY_SRC_WINDOW && st.rec == FACTORY_COPY_REC_BLANK && storage_ready()) {
		write_record(c, r, b);
	}
}

void factory_copy_load(const factory_copy_cfg_t *c)
{
	loaded = false;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	memset(&st, 0, sizeof(st));
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
		storage_lock();
		uint8_t *b = storage_buffer_acquire();
		if (b == NULL) {
			st.why = FACTORY_COPY_WHY_BUFFER;
		} else {
			load_window(c, r, b);
			st.rec = load_record(c, r->size, b + 2u * r->size);
			choose(c, r, b);
			storage_buffer_release();
		}
		storage_unlock();
	}
	// No bytes in use: the table blank, which the board's check refuses as it refuses a blank range
	if (c->table != NULL && st.source == FACTORY_COPY_SRC_NONE && r != NULL && r->size <= FACTORY_COPY_MAX) {
		memset(c->table, 0xFF, r->size);
	}
	st.loaded = true;
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
	*s = st;
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
	const hal_flash_range_t *r = range_of(c);
	out[0] = 1;
	put32(out + 1, r != NULL ? r->addr : 0u);
	put16(out + 5, r != NULL && r->size <= 0xFFFFu ? r->size : 0u);
	out[7] = st.source;
	out[8] = st.win;
	out[9] = st.why;
	put16(out + 10, st.off);
	put32(out + 12, st.win_crc);
	out[16] = st.rec;
	put32(out + 17, st.rec_crc);
	out[21] = 0xFF;			// the last restore's result: none (no restore at this start)
	put16(out + 22, 0);		// the bytes a restore has loaded
	out[24] = st.wrote;
	*len = FACTORY_COPY_INFO_LEN;
	return true;
}
#endif
