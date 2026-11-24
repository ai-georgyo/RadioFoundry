// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The sector journal (storage.h, docs/spec/settings-store.md 4). Two erase units at map->journal_addr:
//   J0  header: magic, target address, CRC-32 of the copy, header CRC, then a "done" byte (0xFF = pending)
//   J1  the new content of the target sector
// A rewrite: J1 erased and programmed, J0 erased and the header programmed, the target erased and programmed, then
// the done byte programmed to 0. A cut before the header is complete leaves the target as it was; a cut after it is
// finished at the next boot from J1.
// A write of up to P_MAX bytes (storage_write: a codeplug record, which may cross a sector end) is one transaction
// over the one or two sectors it touches, so a cut never leaves a record half old, half new:
//   J0  patch header: magic "OJN2", address, length, CRC-32 of bytes 0-11 and the data; marks (staged and done per
//       sector, then all done; 0xFF = not yet); the data at P_DATA
//   J1  the new content of the sector being rewritten
// J0 programmed is the commit point. Then for each sector: J1 = the sector merged with the data, "staged", the
// sector erased and programmed from J1, "done". Replay: a sector not staged is still intact, so it is merged again.
// With STORAGE_FACTORY_KEEP the map's factory ranges (hal_flash_map_t.factory) are the maker's bytes (settings-store.md
// 4.1): the list is checked at storage_init, and a sector holding some (a kept sector) is rewritten only by the kept
// rewrite, which carries those bytes over from the flash and takes the rest from the writer. Its sector entry gains a
// kept block in J0's bytes the format above leaves blank:
//   J0  the sector header and done byte as above; at K_HDR "OJNK", target, n, CRC-32 of bytes 32-43 and the n bytes;
//       at K_DATA the sector's factory bytes in address order (the kept stream)
//   J1  the merged sector (the writer's bytes, the kept ones in place)
// so the kept bytes are in two places before the target is erased, and an earlier firmware's replay of J1 keeps them
// too. storage_factory_write (the factory copy's restore, CONFIG_FACTORY_COPY) is the same kept rewrite with the
// caller's bytes for one range in place of the sector's. Without the define none of that is compiled, and every
// board's code is what it was before the ranges existed.
#include <string.h>
#include "config.h"
#include "storage/storage.h"
#if STORAGE_FACTORY_KEEP
#include "debuglink/dlog.h"
#endif

#define J_MAGIC		0x4C4E4A4Fu	// "OJNL"
#define J_HDR_LEN	16u		// magic, target, data CRC, header CRC
#define J_DONE		J_HDR_LEN	// the done byte's offset
#define P_MAGIC		0x324E4A4Fu	// "OJN2"
#define P_MARK		16u		// staged, done for the first sector, the same for the second, then all done
#define P_ALL		(P_MARK + 4u)
#define P_DATA		24u
#define P_MAX		(STORAGE_SECTOR - P_DATA)
#define CHUNK		256u
#define K_MAGIC		0x4B4E4A4Fu	// "OJNK", a kept block
#define K_HDR		32u		// the kept block in J0: magic, target, n, CRC (settings-store.md 4.1)
#define K_DATA		48u		// the kept stream in J0

static bool ready;			// usable flash (the region may still be foreign)
static storage_region_t region;
static bool claim_pending;
static uint8_t buffer[STORAGE_SECTOR] RFOUNDRY_COLD_RAM;	// task-side only (storage_buffer_acquire)
static bool buffer_taken;
static storage_stats_t stats;
#if STORAGE_FACTORY_KEEP
static bool factory_valid;		// the map's factory list passed storage_init's checks
static uint32_t granted = UINT32_MAX;	// the one kept sector wr()/er() may reach now (grant()), or none
static bool held;			// a committed entry for a kept sector is not finished: the journal holds (journal_free)
static bool drop_logged;		// the first drop of this start was logged
// The kept rewrite's chunk: every step of it, its replay and its compare run under the storage lock, one at a time,
// so they share one chunk off the stacks of the tasks that write (a CPS session, the debug link's, the start-up's)
static uint8_t kchunk[CHUNK] RFOUNDRY_COLD_RAM;
#endif

uint32_t storage_crc32(uint32_t crc, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;

	crc = ~crc;
	while (len-- > 0) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const hal_flash_map_t *storage_map(void)
{
	return board.flash != NULL ? board.flash->map : NULL;
}

static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return blen != 0 && a < b + blen && b < a + alen;
}

// Whether any byte of [addr, addr + len) is in a sector holding factory bytes (a kept sector). Constant false without
// STORAGE_FACTORY_KEEP
static bool kept(const hal_flash_map_t *m, uint32_t addr, uint32_t len)
{
	return hal_flash_factory_unit(m, STORAGE_SECTOR, addr, len);
}

#if STORAGE_FACTORY_KEEP
// The factory bytes in [addr, addr + len)
static uint32_t kept_in(const hal_flash_map_t *m, uint32_t addr, uint32_t len)
{
	uint32_t n = 0;
	for (unsigned i = 0; i < HAL_FLASH_FACTORY_MAX; i++) {
		uint64_t a = m->factory[i].addr, e = a + m->factory[i].size;
		uint64_t lo = a > addr ? a : addr, hi = e < (uint64_t)addr + len ? e : (uint64_t)addr + len;
		n += hi > lo ? (uint32_t)(hi - lo) : 0u;
	}
	return n;
}

// The factory bytes in the sector at base
static uint32_t kept_count(const hal_flash_map_t *m, uint32_t base)
{
	return kept_in(m, base, STORAGE_SECTOR);
}

// The lowest span of factory bytes in [addr, end): true and its [*lo, *hi). The list is ascending (storage_init)
static bool kept_span(const hal_flash_map_t *m, uint32_t addr, uint32_t end, uint32_t *lo, uint32_t *hi)
{
	for (unsigned i = 0; i < HAL_FLASH_FACTORY_MAX; i++) {
		uint64_t a = m->factory[i].addr, e = a + m->factory[i].size;
		if (m->factory[i].size != 0 && addr < end && e > addr && a < end) {
			*lo = a > addr ? (uint32_t)a : addr;
			*hi = e < end ? (uint32_t)e : end;
			return true;
		}
	}
	return false;
}

// The most factory bytes a kept sector of the map holds beside others: how long a kept block in J0 can be
static uint32_t kept_max(const hal_flash_map_t *m)
{
	uint32_t most = 0;
	for (unsigned i = 0; i < HAL_FLASH_FACTORY_MAX && m->factory[i].size != 0; i++) {
		uint32_t a = m->factory[i].addr, last = a + m->factory[i].size - 1u;
		uint32_t ends[2] = { a & ~(STORAGE_SECTOR - 1u), last & ~(STORAGE_SECTOR - 1u) };
		for (unsigned k = 0; k < 2; k++) {
			uint32_t c = kept_count(m, ends[k]);
			most = c != STORAGE_SECTOR && c > most ? c : most;
		}
	}
	return most;
}

// The factory list (settings-store.md 4.1): the ranges of size other than 0 first, then entries of all zeros;
// ascending and apart (adjacent ranges are one range), inside the chip; every sector a range touches clear of the
// settings ring, the journal and the calibration, native, staging and satellite areas (the open-firmware images may
// hold one: the maker's data can sit inside them); a sector holding some kept bytes but not only kept bytes holds
// at most what J0 can carry beside a header (STORAGE_SECTOR - K_DATA); and no copy-on-write flash (replace)
// underneath, whose one-step rewrite takes the new sector from one buffer
static bool factory_list_ok(const hal_flash_map_t *m)
{
	bool end = false;
	uint64_t prev_end = 0;
	for (unsigned i = 0; i < HAL_FLASH_FACTORY_MAX; i++) {
		uint32_t a = m->factory[i].addr, n = m->factory[i].size;
		if (end || n == 0) {
			if (n != 0 || a != 0) {
				return false;
			}
			end = true;
			continue;
		}
		if ((i > 0 && a <= prev_end) || a > board.flash->size || n > board.flash->size - a ||
		    board.flash->replace != NULL) {
			return false;
		}
		prev_end = (uint64_t)a + n;
		// The sectors the range touches, first to last
		uint32_t first = a & ~(STORAGE_SECTOR - 1u);
		uint32_t span = (((a + n - 1u) & ~(STORAGE_SECTOR - 1u)) - first) + STORAGE_SECTOR;
		if (overlaps(first, span, m->settings_addr, m->settings_size) ||
		    overlaps(first, span, m->journal_addr, 2u * STORAGE_SECTOR) ||
		    overlaps(first, span, m->calibration_addr, m->calibration_size) ||
		    overlaps(first, span, m->native_addr, m->native_size) ||
		    overlaps(first, span, m->staging_addr, m->staging_size) ||
		    overlaps(first, span, m->satellite_addr, m->satellite_size)) {
			return false;
		}
		// Only its first and last sector can be partly kept; another range may share either
		uint32_t ends[2] = { first, first + span - STORAGE_SECTOR };
		for (unsigned k = 0; k < 2; k++) {
			uint32_t c = kept_count(m, ends[k]);
			if (c != STORAGE_SECTOR && c > STORAGE_SECTOR - K_DATA) {
				return false;
			}
		}
	}
	return true;
}
#endif

uint32_t storage_staging_size(void)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || m->staging_size < 2u * STORAGE_SECTOR || m->staging_addr % STORAGE_SECTOR != 0 ||
	    m->staging_addr > board.flash->size || m->staging_size > board.flash->size - m->staging_addr) {
		return 0;
	}
	uint32_t a = m->staging_addr, n = m->staging_size;
	bool clear = !overlaps(a, n, m->settings_addr, m->settings_size) && !overlaps(a, n, m->journal_addr, 2u * STORAGE_SECTOR) &&
		     !overlaps(a, n, m->openfw_eeprom, 0x10000u) && !overlaps(a, n, m->openfw_flash, m->openfw_end - m->openfw_flash) &&
		     !overlaps(a, n, m->native_addr, m->native_size) && !overlaps(a, n, m->calibration_addr, m->calibration_size) &&
		     !kept(m, a, n);
	return clear ? n - n % STORAGE_SECTOR : 0;
}

static bool fail(void)
{
	stats.errors++;
	return false;
}

static bool rd(uint32_t addr, void *buf, uint32_t len)
{
	return board.flash->read(addr, buf, len) || fail();
}

#if STORAGE_FACTORY_KEEP
static bool factory_refuse(void)
{
	stats.factory_refused++;
	return fail();
}

// Every program and erase of this file goes through wr() and er(), and each refuses a kept sector once more, but the
// one granted: a path that misses the writers' own rule (4.1), or a read that failed between a check and a write,
// still never reaches the maker's bytes
static bool ungranted(uint32_t addr, uint32_t len)
{
	return kept(storage_map(), addr, len) &&
	       !(granted != UINT32_MAX && addr >= granted && addr - granted < STORAGE_SECTOR &&
		 len <= STORAGE_SECTOR - (addr - granted));
}

// Open the kept sector at unit (aligned) to wr()/er() and to the driver's backstop, for the one rewrite that carries
// its factory bytes over: from just before its erase to its read back. UINT32_MAX closes it, on every exit
static void grant(uint32_t unit)
{
	granted = unit;
	if (board.flash->factory_grant != NULL) {
		board.flash->factory_grant(unit);
	}
}
#endif

static bool wr(uint32_t addr, const void *buf, uint32_t len)
{
#if STORAGE_FACTORY_KEEP
	if (ungranted(addr, len)) {
		return factory_refuse();
	}
#endif
	return board.flash->write(addr, buf, len) || fail();
}

static bool er(uint32_t addr)
{
#if STORAGE_FACTORY_KEEP
	if (ungranted(addr, 1)) {
		return factory_refuse();
	}
#endif
	return board.flash->erase(addr) || fail();
}

// CRC of a flash range, read in chunks, continuing from crc (0 to start)
static bool flash_crc(uint32_t addr, uint32_t len, uint32_t crc, uint32_t *out)
{
	uint8_t c[CHUNK];

	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		if (!rd(addr + off, c, n)) {
			return false;
		}
		crc = storage_crc32(crc, c, n);
	}
	*out = crc;
	return true;
}

// Whether the flash holds exactly data
static bool same(uint32_t addr, const uint8_t *data, uint32_t len)
{
	uint8_t c[CHUNK];

	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		if (!rd(addr + off, c, n) || memcmp(c, data + off, n) != 0) {
			return false;
		}
	}
	return true;
}

// Erase dst and copy the sector at src into it, a chunk at a time
static bool copy_sector(uint32_t dst, uint32_t src)
{
	uint8_t c[CHUNK];

	if (!er(dst)) {
		return false;
	}
	for (uint32_t off = 0; off < STORAGE_SECTOR; off += CHUNK) {
		if (!rd(src + off, c, CHUNK) || !wr(dst + off, c, CHUNK)) {
			return false;
		}
	}
	return true;
}

static bool target_ok(const hal_flash_map_t *m, uint32_t target)
{
	uint32_t j = m->journal_addr;

	return (target % STORAGE_SECTOR) == 0 && target < board.flash->size && target + STORAGE_SECTOR > target &&
	       !(target + STORAGE_SECTOR > j && target < j + 2u * STORAGE_SECTOR);
}

// A patch header in J0 (h: its first P_DATA bytes): the bytes of J0 it uses, 0 if it isn't one or is torn
static uint32_t patch_len(const hal_flash_map_t *m, const uint8_t *h)
{
	uint32_t len = get32(&h[8]), crc;
	if (get32(&h[0]) != P_MAGIC || len == 0 || len > P_MAX ||
	    !flash_crc(m->journal_addr + P_DATA, len, storage_crc32(0, h, 12), &crc) || crc != get32(&h[12])) {
		return 0;
	}
	return P_DATA + len;
}

// Sector k (0, 1) of the patch transaction in J0, at its address and length: staged into J1 unless it was (the
// sector is still intact then), then rewritten from J1. b: a sector buffer
static bool patch_sector(const hal_flash_map_t *m, uint32_t addr, uint32_t len, unsigned k, uint8_t *b)
{
	static const uint8_t zero = 0;
	uint32_t j0 = m->journal_addr, j1 = j0 + STORAGE_SECTOR;
	uint32_t base = (addr & ~(STORAGE_SECTOR - 1u)) + k * STORAGE_SECTOR;
	uint32_t lo = addr > base ? addr : base;
	uint32_t hi = addr + len < base + STORAGE_SECTOR ? addr + len : base + STORAGE_SECTOR;
	uint8_t mk[2];

	if (!target_ok(m, base) || !rd(j0 + P_MARK + 2u * k, mk, 2)) {
		return false;
	}
	if (mk[1] == 0) {
		return true;
	}
	if (mk[0] != 0) {
		if (!rd(base, b, STORAGE_SECTOR) || !rd(j0 + P_DATA + (lo - addr), b + (lo - base), hi - lo) ||
		    !er(j1) || !wr(j1, b, STORAGE_SECTOR) || !same(j1, b, STORAGE_SECTOR) ||
		    !wr(j0 + P_MARK + 2u * k, &zero, 1)) {
			return false;
		}
	} else if (!rd(j1, b, STORAGE_SECTOR)) {
		return false;
	}
#if STORAGE_FACTORY_KEEP
	// Only the replay of an earlier firmware's entry reaches a kept sector here, with data that misses its factory
	// bytes (kept_patch_replay): merged from the intact sector, b holds them as stored
	bool open = kept(m, base, STORAGE_SECTOR);
	if (open) {
		grant(base);
	}
	bool fine = same(base, b, STORAGE_SECTOR) || (er(base) && wr(base, b, STORAGE_SECTOR) && same(base, b, STORAGE_SECTOR));
	if (open) {
		grant(UINT32_MAX);
	}
	if (!fine) {
		return false;
	}
#else
	if (!same(base, b, STORAGE_SECTOR) && !(er(base) && wr(base, b, STORAGE_SECTOR) && same(base, b, STORAGE_SECTOR))) {
		return false;
	}
#endif
	stats.sector_writes++;
	return wr(j0 + P_MARK + 2u * k + 1u, &zero, 1);
}

// Every sector of the patch transaction in J0, then the all done mark
static bool patch_finish(const hal_flash_map_t *m, uint32_t addr, uint32_t len, uint8_t *b)
{
	static const uint8_t zero = 0;
	bool two = (addr & (STORAGE_SECTOR - 1u)) + len > STORAGE_SECTOR;
	return patch_sector(m, addr, len, 0, b) && (!two || patch_sector(m, addr, len, 1, b)) &&
	       wr(m->journal_addr + P_ALL, &zero, 1);
}

#if STORAGE_FACTORY_KEEP
// The kept rewrite's helpers (settings-store.md 4.1). Each takes the chunk buffer c (CHUNK bytes) it may use: kchunk,
// or the scan's slot buffer

// CRC of a flash range, continuing from crc
static bool crc_with(uint32_t addr, uint32_t len, uint32_t crc, uint32_t *out, uint8_t *c)
{
	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		if (!rd(addr + off, c, n)) {
			return false;
		}
		crc = storage_crc32(crc, c, n);
	}
	*out = crc;
	return true;
}

static bool blank_with(uint32_t addr, uint32_t len, uint8_t *c)
{
	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		if (!rd(addr + off, c, n)) {
			return false;
		}
		for (uint32_t i = 0; i < n; i++) {
			if (c[i] != 0xFF) {
				return false;
			}
		}
	}
	return true;
}

// The factory bytes of the chunk at base + off read over c: from src at the same offsets of its sector, or (src
// UINT32_MAX) from J0's kept stream
static bool overlay(const hal_flash_map_t *m, uint32_t base, uint32_t off, uint8_t *c, uint32_t src)
{
	uint32_t lo, hi;
	for (uint32_t p = base + off; kept_span(m, p, base + off + CHUNK, &lo, &hi); p = hi) {
		uint32_t from = src == UINT32_MAX ? m->journal_addr + K_DATA + kept_in(m, base, lo - base) : src + (lo - base);
		if (!rd(from, c + (lo - base - off), hi - lo)) {
			return false;
		}
	}
	return true;
}

// A valid kept block in J0: our magic, its CRC holds, its target a kept sector not kept whole and n that sector's
// count in the map. *target its target
static bool block_ok(const hal_flash_map_t *m, uint32_t *target, uint8_t *c)
{
	if (!rd(m->journal_addr + K_HDR, c, 16)) {
		return false;
	}
	uint32_t t = get32(&c[4]), n = get32(&c[8]), want = get32(&c[12]), crc;
	if (get32(&c[0]) != K_MAGIC || !target_ok(m, t) || n == 0 || n > STORAGE_SECTOR - K_DATA ||
	    n != kept_count(m, t) || !crc_with(m->journal_addr + K_DATA, n, storage_crc32(0, c, 12), &crc, c) ||
	    crc != want) {
		return false;
	}
	*target = t;
	return true;
}

// A factory range a write brings new bytes for (storage_factory_write): [addr, addr + size) from bytes. size 0: none,
// every factory byte is the flash's
typedef struct {
	uint32_t addr, size;
	const uint8_t *bytes;
} kept_new_t;

static const kept_new_t no_new = { 0, 0, NULL };

// The new range's bytes over the len bytes at base + off, in c
static void put_new(const kept_new_t *nw, uint32_t base, uint32_t off, uint8_t *c, uint32_t len)
{
	uint32_t lo = base + off > nw->addr ? base + off : nw->addr;
	uint64_t e = (uint64_t)nw->addr + nw->size, ce = (uint64_t)base + off + len;
	uint32_t hi = (uint32_t)(e < ce ? e : ce);
	if (nw->size != 0 && hi > lo) {
		memcpy(c + (lo - base - off), nw->bytes + (lo - nw->addr), hi - lo);
	}
}

// J1 against a second read of the sector at base with the new range's bytes over it, half a chunk of each at a time:
// the sector's other bytes, taken from one read of it, are what it holds (a marginal read stops before the commit
// point, factory_refused)
static bool j1_matches(uint32_t base, uint32_t j1, const kept_new_t *nw, uint8_t *c)
{
	for (uint32_t off = 0; off < STORAGE_SECTOR; off += CHUNK / 2u) {
		if (!rd(base + off, c, CHUNK / 2u) || !rd(j1 + off, c + CHUNK / 2u, CHUNK / 2u)) {
			return false;
		}
		put_new(nw, base, off, c, CHUNK / 2u);
		if (memcmp(c, c + CHUNK / 2u, CHUNK / 2u) != 0) {
			stats.factory_refused++;
			return false;
		}
	}
	return true;
}

// J0's kept block for the kept sector at base: the stream programmed at K_DATA from src (J1, or the target itself
// for a converted entry), then the target's factory bytes read once more and compared with it, then the block's
// header, read back. A difference (a read that went wrong once) stops before the header: nothing in J0 is then a
// valid block, and no commit point follows (factory_refused). The new range's bytes (nw) are compared with the
// caller's instead of the target's, which still holds the old ones
static bool write_block(const hal_flash_map_t *m, uint32_t base, uint32_t src, uint8_t *c, const kept_new_t *nw)
{
	uint32_t j0 = m->journal_addr, n = kept_count(m, base), s = 0, lo, hi, t;
	// The block's header is built in c, at the start and again at the end (c holds the bytes in between)
	put32(&c[0], K_MAGIC);
	put32(&c[4], base);
	put32(&c[8], n);
	uint32_t crc = storage_crc32(0, c, 12);
	for (uint32_t p = base; kept_span(m, p, base + STORAGE_SECTOR, &lo, &hi); p = hi) {
		for (uint32_t a = lo; a < hi; ) {
			uint32_t len = hi - a < CHUNK ? hi - a : CHUNK;
			if (!rd(src + (a - base), c, len) || !wr(j0 + K_DATA + s, c, len)) {
				return false;
			}
			crc = storage_crc32(crc, c, len);
			a += len;
			s += len;
		}
	}
	// The second read: the target's bytes against what J0 now holds, half a chunk of each at a time
	s = 0;
	for (uint32_t p = base; kept_span(m, p, base + STORAGE_SECTOR, &lo, &hi); p = hi) {
		for (uint32_t a = lo; a < hi; ) {
			uint32_t len = hi - a < CHUNK / 2u ? hi - a : CHUNK / 2u;
			// A span lies inside one range (the list's ranges are apart), so inside the new one or clear of it
			bool fresh = nw->size != 0 && a >= nw->addr && a - nw->addr < nw->size;
			if (fresh) {
				memcpy(c, nw->bytes + (a - nw->addr), len);
			}
			if ((!fresh && !rd(a, c, len)) || !rd(j0 + K_DATA + s, c + CHUNK / 2u, len)) {
				return false;
			}
			if (memcmp(c, c + CHUNK / 2u, len) != 0) {
				stats.factory_refused++;
				return false;
			}
			a += len;
			s += len;
		}
	}
	put32(&c[0], K_MAGIC);
	put32(&c[4], base);
	put32(&c[8], n);
	put32(&c[12], crc);
	return wr(j0 + K_HDR, c, 16) && block_ok(m, &t, c) && t == base;
}

// The target := J1 with J0's kept stream over it (the same bytes for this firmware's entries; the target's stored
// ones for a converted entry), unless it holds that already; under the grant, closed on every exit
static bool finish_kept(const hal_flash_map_t *m, uint32_t target, uint8_t *c)
{
	uint32_t j1 = m->journal_addr + STORAGE_SECTOR, want = 0, now;
	for (uint32_t off = 0; off < STORAGE_SECTOR; off += CHUNK) {
		if (!rd(j1 + off, c, CHUNK) || !overlay(m, target, off, c, UINT32_MAX)) {
			return false;
		}
		want = storage_crc32(want, c, CHUNK);
	}
	if (!crc_with(target, STORAGE_SECTOR, 0, &now, c)) {
		return false;
	}
	if (now == want) {
		return true;
	}
	grant(target);
	bool ok = er(target);
	for (uint32_t off = 0; ok && off < STORAGE_SECTOR; off += CHUNK) {
		ok = rd(j1 + off, c, CHUNK) && overlay(m, target, off, c, UINT32_MAX) && wr(target + off, c, CHUNK);
	}
	ok = ok && crc_with(target, STORAGE_SECTOR, 0, &now, c) && now == want;
	grant(UINT32_MAX);
	return ok;
}

// A journal entry for a kept sector that is not replayed: left pending (the next journaled write replaces it)
static void pending(uint32_t addr)
{
	stats.factory_refused++;
	dlog("storage: journal entry for 0x%x left pending: factory bytes", (unsigned)addr);
	(void)addr;
}

// A sector entry (its header valid, pending) whose target is a kept sector (settings-store.md 4.1, the replay table)
static void kept_replay(const hal_flash_map_t *m, uint32_t target, uint32_t hcrc)
{
	static const uint8_t done = 0;
	uint32_t j0 = m->journal_addr, crc, t;
	uint8_t *c = kchunk;

	if (kept_count(m, target) == STORAGE_SECTOR) {
		// A whole kept sector: nothing of J1 lands
		if (wr(j0 + J_DONE, &done, 1)) {
			stats.factory_replays++;
		}
		return;
	}
	// J1 must match the header before anything is copied or converted: a partly erased J0 can keep an old header.
	// Giving the entry up leaves a target this firmware may have erased with its factory bytes in J0 alone, so a
	// mismatch counts only when a second read of J1 agrees: one marginal read is not a stale header
	if (!crc_with(j0 + STORAGE_SECTOR, STORAGE_SECTOR, 0, &crc, c) ||
	    (crc != hcrc && !crc_with(j0 + STORAGE_SECTOR, STORAGE_SECTOR, 0, &crc, c))) {
		return;
	}
	if (crc != hcrc) {
		if (wr(j0 + J_DONE, &done, 1)) {
			held = false;	// given up: nothing left to hold
		}
		return;
	}
	if (!block_ok(m, &t, c)) {
		if (!blank_with(j0 + K_HDR, K_DATA - K_HDR + kept_count(m, target), c)) {
			pending(target);	// torn (a cut conversion), or a block the map doesn't explain
			return;
		}
		// Another firmware's entry, or one from before the rule: the kept block from the target as it is now
		// (whatever that firmware left there is what is kept), before anything is erased
		if (!write_block(m, target, target, c, &no_new)) {
			return;
		}
		t = target;
	}
	if (t != target) {
		pending(target);
		return;
	}
	held = true;
	if (finish_kept(m, target, c) && wr(j0 + J_DONE, &done, 1)) {
		stats.replays++;
		stats.factory_replays++;
		held = false;
	}
}

// A patch entry (pending, its CRC holds) that touches a kept sector: only an earlier firmware makes one. Replayed
// under the grant when its data misses every factory byte (an intact sector is merged again, a staged J1 was merged
// from the intact sector); left pending when it reaches one
static void kept_patch_replay(const hal_flash_map_t *m, uint32_t addr, uint32_t len)
{
	if (hal_flash_factory_bytes(m, addr, len)) {
		pending(addr);
		return;
	}
	held = true;
	if (patch_finish(m, addr, len, buffer)) {
		stats.replays++;
		stats.factory_replays++;
		held = false;
	}
}
#endif

// A pending journal entry: finish it
static void replay(void)
{
	const hal_flash_map_t *m = storage_map();
	uint8_t h[P_DATA];
	uint32_t crc;

	if (!rd(m->journal_addr, h, sizeof(h))) {
		return;
	}
	if (get32(&h[0]) == P_MAGIC) {
#if STORAGE_FACTORY_KEEP
		if (kept(m, get32(&h[4]), get32(&h[8]))) {
			if (h[P_ALL] == 0xFF && patch_len(m, h) != 0) {
				kept_patch_replay(m, get32(&h[4]), get32(&h[8]));
			}
			return;
		}
#endif
		if (h[P_ALL] == 0xFF && patch_len(m, h) != 0 && patch_finish(m, get32(&h[4]), get32(&h[8]), buffer)) {
			stats.replays++;
		}
		return;
	}
	if (get32(&h[0]) != J_MAGIC || h[J_DONE] != 0xFF ||
	    get32(&h[12]) != storage_crc32(0, h, 12) || !target_ok(m, get32(&h[4]))) {
		return;
	}
#if STORAGE_FACTORY_KEEP
	if (kept(m, get32(&h[4]), STORAGE_SECTOR)) {
		kept_replay(m, get32(&h[4]), get32(&h[8]));
		return;
	}
#endif
	if (flash_crc(m->journal_addr + STORAGE_SECTOR, STORAGE_SECTOR, 0, &crc) && crc == get32(&h[8])) {
		uint32_t target = get32(&h[4]);
		uint32_t now;
		if (!flash_crc(target, STORAGE_SECTOR, 0, &now) || now != crc) {
			if (!copy_sector(target, m->journal_addr + STORAGE_SECTOR)) {
				return;		// try again at the next boot
			}
		}
		stats.replays++;
	}
	uint8_t done = 0;
	wr(m->journal_addr + J_DONE, &done, 1);
}

storage_slot_t storage_slot_check(const uint8_t r[STORAGE_SLOT_SIZE])
{
	bool erased = true;
	for (unsigned i = 0; i < STORAGE_SLOT_SIZE && erased; i++) {
		erased = r[i] == 0xFF;
	}
	if (erased) {
		return STORAGE_SLOT_FREE;
	}
	if (get32(&r[0]) != STORAGE_SLOT_MAGIC) {
		// A save cut inside the magic: the bytes programmed so far are on their way to it (programming only
		// clears bits) and everything after them is still erased
		static const uint8_t magic[4] = { 'O', 'S', 'E', 'T' };
		unsigned i = 0;
		while (i < 4 && r[i] == magic[i]) {
			i++;
		}
		if ((r[i] & magic[i]) != magic[i]) {
			return STORAGE_SLOT_OTHER;
		}
		for (unsigned k = i + 1; k < STORAGE_SLOT_SIZE; k++) {
			if (r[k] != 0xFF) {
				return STORAGE_SLOT_OTHER;
			}
		}
		return STORAGE_SLOT_TORN;
	}
	uint16_t n = (uint16_t)(r[10] | (r[11] << 8));
	if (get32(&r[STORAGE_SLOT_COMMIT]) != 0 || n > STORAGE_SLOT_PAYLOAD_MAX ||
	    get32(&r[STORAGE_SLOT_HDR + n]) != storage_crc32(0, r, STORAGE_SLOT_HDR + n)) {
		return STORAGE_SLOT_TORN;
	}
	return STORAGE_SLOT_VALID;
}

// Whether len bytes at addr are all erased; false also when unreadable
static bool blank(uint32_t addr, uint32_t len)
{
	uint8_t c[CHUNK];
	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		if (!rd(addr + off, c, n)) {
			return false;
		}
		for (uint32_t i = 0; i < n; i++) {
			if (c[i] != 0xFF) {
				return false;
			}
		}
	}
	return true;
}

// An ownership mark: a valid settings slot of layout 0 (which no settings layout uses, so the settings load their
// defaults past it) in the first free slot of the ring. Written while the region holds no valid record of ours, so
// from then on one always exists (saves never erase the sector holding the newest record) and a cut in the middle
// of a journal rewrite (J0 erased, J1 full) can't make the region look foreign at the next boot
static void mark(const hal_flash_map_t *m)
{
	static uint8_t r[STORAGE_SLOT_SIZE];
	for (uint32_t a = m->settings_addr; a + STORAGE_SLOT_SIZE <= m->settings_addr + m->settings_size;
	     a += STORAGE_SLOT_SIZE) {
		if (!rd(a, r, sizeof(r)) || storage_slot_check(r) != STORAGE_SLOT_FREE) {
			continue;
		}
		memset(r, 0xFF, sizeof(r));
		put32(&r[0], STORAGE_SLOT_MAGIC);
		put32(&r[4], 0);		// sequence 0: any real record is newer
		r[8] = r[9] = 0;		// layout 0
		r[10] = r[11] = 0;		// no payload
		put32(&r[STORAGE_SLOT_HDR], storage_crc32(0, r, STORAGE_SLOT_HDR));
		static const uint8_t commit[4] = { 0, 0, 0, 0 };
		if (wr(a, r, STORAGE_SLOT_HDR + 4u)) {
			wr(a + STORAGE_SLOT_COMMIT, commit, sizeof(commit));
		}
		return;
	}
}

#if STORAGE_FACTORY_KEEP
// Whether J0's first bytes h (P_DATA of them) are a sector header on its way: a cut while it was programmed left the
// bytes so far, each of the magic's on its way to it (programming only clears bits), the rest blank
static bool header_torn(const uint8_t *h)
{
	static const uint8_t magic[4] = { 'O', 'J', 'N', 'L' };
	unsigned i = 0;
	while (i < 4 && h[i] == magic[i]) {
		i++;
	}
	if (i < 4 && (h[i] & magic[i]) != magic[i]) {
		return false;
	}
	for (unsigned k = i < 4 ? i + 1u : J_DONE; k < P_DATA; k++) {
		if (h[k] != 0xFF) {
			return false;
		}
	}
	return true;
}

// J0 of a kept rewrite, on a map that lists ranges (settings-store.md 4.1): a kept block at K_HDR, valid or torn (a
// cut while it was programmed), after a valid sector header or blank bytes 0-31; or a valid block after a header on
// its way (a cut at the commit point). *hdr then counts J0 as ours-looking and *used is where its blank rest begins.
// A patch entry never gets here: its data starts at P_DATA. Anything else stays with the rule of a plain header
static void kept_shape(const hal_flash_map_t *m, const uint8_t *h, bool valid, bool *hdr, uint32_t *used, uint8_t *c)
{
	uint32_t j0 = m->journal_addr, n = kept_max(m), t;
	if (n == 0 || blank_with(j0 + K_HDR, K_DATA - K_HDR + n, c)) {
		return;
	}
	bool block = block_ok(m, &t, c);
	bool lead = valid ? blank_with(j0 + J_DONE + 1u, K_HDR - J_DONE - 1u, c) : blank_with(j0, K_HDR, c);
	if (!lead && !(block && header_torn(h) && blank_with(j0 + P_DATA, K_HDR - P_DATA, c))) {
		return;
	}
	*hdr = true;
	*used = K_DATA + (block ? kept_count(m, t) : n);
}
#endif

// Classify the settings ring and the journal (storage.h "Foreign region guard")
static storage_region_t scan_region(const hal_flash_map_t *m)
{
	static uint8_t r[STORAGE_SLOT_SIZE];	// under the storage lock (off the start-up task's stack)
	bool ours = false;			// one valid record of ours somewhere
	uint16_t other = 0;

	for (uint32_t a = m->settings_addr; a + STORAGE_SLOT_SIZE <= m->settings_addr + m->settings_size;
	     a += STORAGE_SLOT_SIZE) {
		storage_slot_t s = rd(a, r, sizeof(r)) ? storage_slot_check(r) : STORAGE_SLOT_OTHER;
		ours = ours || s == STORAGE_SLOT_VALID;
		other = (uint16_t)(other + (s == STORAGE_SLOT_OTHER));
	}
	// J0: erased, or our header (a torn one counts as ours-looking), and nothing after the done byte
	uint8_t h[P_DATA];
	bool j0_blank = blank(m->journal_addr, STORAGE_SECTOR);
	if (!j0_blank) {
		bool hdr = rd(m->journal_addr, h, sizeof(h)) && get32(&h[0]) == J_MAGIC;
		bool valid = hdr && get32(&h[12]) == storage_crc32(0, h, 12);
		uint32_t used = J_HDR_LEN + 1u;
		if (!hdr && get32(&h[0]) == P_MAGIC) {
			// A patch header: valid when its CRC matches; torn (a cut while it was programmed): ours-looking,
			// but then where its data ends isn't known, so the rest isn't checked
			used = patch_len(m, h);
			hdr = true;
			valid = used != 0;
			used = valid ? used : STORAGE_SECTOR;
		}
#if STORAGE_FACTORY_KEEP
		else if (m->factory[0].size != 0) {
			kept_shape(m, h, valid, &hdr, &used, r);
		}
#endif
		ours = ours || valid;
		if (!hdr || !blank(m->journal_addr + used, STORAGE_SECTOR - used)) {
			other++;
		}
	}
	// J1: a sector copy, any content, but only after our J0 header has been written at least once
	if (j0_blank && !blank(m->journal_addr + STORAGE_SECTOR, STORAGE_SECTOR)) {
		other++;
	}
	stats.foreign_slots = other;
	// STORAGE_CLAIM_FIRST: no record of ours means not claimed, even when all of it is blank (storage.h). Nothing
	// below runs then, so no mark is programmed before the owner asked for one. In this operand order the define's
	// default folds to the code the compiler made before it existed; "!ours" first does not (16 bytes moved)
	if ((other != 0 || STORAGE_CLAIM_FIRST) && !ours) {
		return STORAGE_REGION_FOREIGN;
	}
	if (!ours) {
		mark(m);
	}
	return STORAGE_REGION_OURS;
}

bool storage_init(void)
{
	const hal_flash_map_t *m = storage_map();

	ready = false;
	region = STORAGE_REGION_NONE;
	claim_pending = false;
#if STORAGE_FACTORY_KEEP
	factory_valid = false;
	held = false;
	drop_logged = false;
	granted = UINT32_MAX;
#endif
	if (m == NULL || board.flash->size == 0 || board.flash->erase_size != STORAGE_SECTOR ||
	    board.flash->write == NULL || board.flash->erase == NULL) {
		return false;
	}
#if STORAGE_FACTORY_KEEP
	// A list that fails says the map is wrong: nothing of ours is written anywhere (region none), rather than a guess
	// at which bytes are the maker's
	if (!factory_list_ok(m)) {
		dlog("storage: factory list refused: nothing is written");
		return false;
	}
	factory_valid = true;
#endif
	ready = true;
	storage_lock();
	region = scan_region(m);
	if (region == STORAGE_REGION_OURS) {
		replay();
	}
	storage_unlock();
	return region == STORAGE_REGION_OURS;
}

bool storage_ready(void)
{
	return ready && region == STORAGE_REGION_OURS;
}

storage_region_t storage_region(void)
{
	return region;
}

bool storage_claim_request(void)
{
	if (region != STORAGE_REGION_FOREIGN) {
		return false;
	}
	claim_pending = true;
	return true;
}

bool storage_claim_pending(void)
{
	return claim_pending;
}

bool storage_claim(void)
{
	const hal_flash_map_t *m = storage_map();
	bool ok = true;

	storage_lock();
	claim_pending = false;
	if (region != STORAGE_REGION_FOREIGN) {
		storage_unlock();
		return false;
	}
	for (uint32_t a = m->settings_addr; ok && a < m->settings_addr + m->settings_size; a += STORAGE_SECTOR) {
		ok = er(a) && blank(a, STORAGE_SECTOR);
	}
	for (uint32_t a = m->journal_addr; ok && a < m->journal_addr + 2u * STORAGE_SECTOR; a += STORAGE_SECTOR) {
		ok = er(a) && blank(a, STORAGE_SECTOR);
	}
	if (ok) {
		region = STORAGE_REGION_OURS;
		stats.foreign_slots = 0;
		mark(m);
	}
	storage_unlock();
	return ok;
}

static bool inside(uint32_t addr, uint32_t len, uint32_t base, uint32_t size)
{
	return size != 0 && addr >= base && len <= size && addr - base <= size - len;
}

// Inside one factory range, on a list storage_init accepted (the maker's bytes, for a compare with a backup).
// Constant false without STORAGE_FACTORY_KEEP
static bool factory_readable(const hal_flash_map_t *m, uint32_t addr, uint32_t len)
{
#if STORAGE_FACTORY_KEEP
	for (unsigned i = 0; factory_valid && i < HAL_FLASH_FACTORY_MAX; i++) {
		if (inside(addr, len, m->factory[i].addr, m->factory[i].size)) {
			return true;
		}
	}
#endif
	(void)m;
	(void)addr;
	(void)len;
	return false;
}

bool storage_debug_read_allowed(uint32_t addr, uint32_t len)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || len == 0 || addr + len < addr) {
		return false;
	}
	return inside(addr, len, m->settings_addr, m->settings_size) ||
	       inside(addr, len, m->journal_addr, 2u * STORAGE_SECTOR) ||
	       inside(addr, len, m->calibration_addr, m->calibration_size) || factory_readable(m, addr, len);
}

// A write refused because the region is foreign (counted), or because there is no flash
static bool refused(void)
{
	if (ready && region == STORAGE_REGION_FOREIGN) {
		stats.foreign_refusals++;
	}
	return false;
}

#if STORAGE_FACTORY_KEEP
// The held entry's patch rows (patch_len holds a chunk: not in held_replay's frame)
static void __attribute__((noinline)) held_patch(const hal_flash_map_t *m, const uint8_t *h)
{
	uint32_t addr = get32(&h[4]), len = get32(&h[8]);
	if (h[P_ALL] == 0xFF && patch_len(m, h) != 0 && kept(m, addr, len)) {
		kept_patch_replay(m, addr, len);
	} else {
		held = false;
	}
}

// The replay of the held entry: only the rows for a kept sector (replay() would add its other rows' chunk to the
// stack of the task writing). J0 holding no pending entry for a kept sector any more holds nothing back
static void held_replay(void)
{
	const hal_flash_map_t *m = storage_map();
	uint8_t h[P_DATA];

	if (!rd(m->journal_addr, h, sizeof(h))) {
		return;
	}
	if (get32(&h[0]) == P_MAGIC) {
		if (!buffer_taken) {
			held_patch(m, h);
		}
		return;
	}
	uint32_t target = get32(&h[4]);
	if (get32(&h[0]) == J_MAGIC && h[J_DONE] == 0xFF && get32(&h[12]) == storage_crc32(0, h, 12) &&
	    target_ok(m, target) && kept(m, target, STORAGE_SECTOR)) {
		kept_replay(m, target, get32(&h[8]));
	} else {
		held = false;
	}
}

// The held journal: a committed entry for a kept sector that this session did not finish (a flash failure past its
// commit point) holds J0 and J1, the only copies of the kept bytes while its target may be erased. A journaled write
// first finishes it; while that fails the write is refused, so nothing erases those copies. A patch entry's replay
// merges in the shared buffer: not while someone holds it
static bool journal_free(void)
{
	if (held) {
		held_replay();
	}
	return !held || factory_refuse();
}

// A write's bytes for a range differ from the flash: dropped, counted, the first of a start logged
static void dropped(const hal_flash_map_t *m, uint32_t addr)
{
	stats.factory_dropped++;
	for (unsigned i = 0; !drop_logged && i < HAL_FLASH_FACTORY_MAX; i++) {
		uint32_t a = m->factory[i].addr, n = m->factory[i].size;
		if (n != 0 && addr >= a && addr - a < n) {
			drop_logged = true;
			dlog("storage: kept 0x%06x-0x%06x, a write's bytes there dropped", (unsigned)a, (unsigned)(a + n - 1u));
		}
	}
}

// Step 0 of the kept rewrite: the sector at addr against data. 0: nothing to write (the flash's bytes outside the
// factory bytes equal data's, or the sector is kept whole), 1: a rewrite, -1: unreadable. A difference in the
// factory bytes is dropped either way
static int kept_compare(const hal_flash_map_t *m, uint32_t addr, const uint8_t *data)
{
	uint8_t *c = kchunk;
	bool rest = false;
	uint32_t diff = UINT32_MAX, lo, hi;

	for (uint32_t off = 0; off < STORAGE_SECTOR; off += CHUNK) {
		if (!rd(addr + off, c, CHUNK)) {
			stats.factory_refused++;
			return -1;
		}
		uint32_t p = addr + off, end = addr + off + CHUNK;
		while (p < end) {
			if (!kept_span(m, p, end, &lo, &hi)) {
				lo = hi = end;
			}
			rest = rest || memcmp(c + (p - addr - off), data + (p - addr), lo - p) != 0;
			for (uint32_t a = lo; diff == UINT32_MAX && a < hi; a++) {
				diff = c[a - addr - off] != data[a - addr] ? a : UINT32_MAX;
			}
			p = hi;
		}
	}
	if (diff != UINT32_MAX) {
		dropped(m, diff);
	}
	return rest && kept_count(m, addr) != STORAGE_SECTOR ? 1 : 0;
}

// The kept rewrite (settings-store.md 4.1, its step table): J1 the merged sector, J0 the kept block and the header
// (the commit point), then the target under the grant, then the done byte. A failure past the commit point holds the
// journal (journal_free). data NULL (storage_factory_write): the sector's own bytes, nw's range from nw's bytes
static bool kept_rewrite(const hal_flash_map_t *m, uint32_t addr, const uint8_t *data, const kept_new_t *nw)
{
	static const uint8_t done = 0;
	uint32_t j0 = m->journal_addr, j1 = j0 + STORAGE_SECTOR, crc = 0, check;
	uint8_t *c = kchunk;

	// 1: J1 := data with the target's factory bytes over it, a chunk at a time (data is often the shared buffer); or
	// the target with the new range's bytes over it
	bool ok = er(j1);
	for (uint32_t off = 0; ok && off < STORAGE_SECTOR; off += CHUNK) {
		if (data != NULL) {
			memcpy(c, data + off, CHUNK);
			ok = overlay(m, addr, off, c, addr);
		} else {
			ok = rd(addr + off, c, CHUNK);
			put_new(nw, addr, off, c, CHUNK);
		}
		ok = ok && wr(j1 + off, c, CHUNK);
		crc = storage_crc32(crc, c, CHUNK);
	}
	// 2, 3: J0 erased, the kept block from J1, checked against the target (or the new bytes) once more. Without the
	// writer's data, the sector's other bytes are checked against a second read of it too
	if (!ok || !crc_with(j1, STORAGE_SECTOR, 0, &check, c) || check != crc ||
	    (data == NULL && !j1_matches(addr, j1, nw, c)) || !er(j0) || !write_block(m, addr, j1, c, nw)) {
		return false;
	}
	// 4: the header, the commit point
	uint8_t h[J_HDR_LEN];
	put32(&h[0], J_MAGIC);
	put32(&h[4], addr);
	put32(&h[8], crc);
	put32(&h[12], storage_crc32(0, h, 12));
	if (!wr(j0, h, sizeof(h))) {
		return false;
	}
	// 5, 6: the target from J1 under the grant, read back; the done byte
	held = true;
	grant(addr);
	ok = er(addr);
	for (uint32_t off = 0; ok && off < STORAGE_SECTOR; off += CHUNK) {
		ok = rd(j1 + off, c, CHUNK) && wr(addr + off, c, CHUNK);
	}
	ok = ok && crc_with(addr, STORAGE_SECTOR, 0, &check, c) && check == crc;
	grant(UINT32_MAX);
	if (!ok || !wr(j0 + J_DONE, &done, 1)) {
		return false;
	}
	held = false;
	stats.sector_writes++;
	stats.factory_rewrites++;
	return true;
}

// storage_sector_write on a kept sector: the kept rewrite when a byte outside the factory bytes changes. A held entry
// is finished first, even for a skip: it may be this very sector's, erased, whose factory bytes the compare would
// read as 0xFF and whose replay would later undo a write skipped against it
static bool kept_sector_write(const hal_flash_map_t *m, uint32_t addr, const uint8_t *data)
{
	if (!journal_free()) {
		return false;
	}
	int r = kept_compare(m, addr, data);
	if (r == 0) {
		stats.sector_skips++;
	}
	return r == 0 || (r > 0 && kept_rewrite(m, addr, data, &no_new));
}
#endif

#if STORAGE_FACTORY_KEEP && CONFIG_FACTORY_COPY
bool storage_factory_write(uint32_t addr, const uint8_t *bytes, uint32_t len)
{
	const hal_flash_map_t *m = storage_map();
	if (!storage_ready()) {
		return refused();
	}
	// Exactly one listed range: its address and its size, nothing else
	bool listed = false;
	for (unsigned i = 0; factory_valid && i < HAL_FLASH_FACTORY_MAX; i++) {
		listed = listed || (m->factory[i].size != 0 && m->factory[i].addr == addr && m->factory[i].size == len);
	}
	uint32_t base = addr & ~(STORAGE_SECTOR - 1u);
	// Inside one sector that holds other bytes too: a range across a sector end would take two rewrites, not one
	// all-or-nothing step, and a whole kept sector is never written
	if (!listed || bytes == NULL || len > STORAGE_SECTOR - (addr - base) || kept_count(m, base) == STORAGE_SECTOR ||
	    !target_ok(m, base)) {
		return false;
	}
	const kept_new_t nw = { addr, len, bytes };
	storage_lock();
	bool ok = journal_free();
	if (ok && same(addr, bytes, len)) {
		stats.sector_skips++;
	} else if (ok) {
		ok = kept_rewrite(m, base, NULL, &nw);
	}
	storage_unlock();
	return ok;
}
#endif

bool storage_sector_write(uint32_t addr, const uint8_t *data)
{
	const hal_flash_map_t *m = storage_map();
	bool ok = false;

	if (!storage_ready() || !target_ok(m, addr)) {
		return refused();
	}
	storage_lock();
#if STORAGE_FACTORY_KEEP
	if (kept(m, addr, STORAGE_SECTOR)) {
		ok = kept_sector_write(m, addr, data);
		storage_unlock();
		return ok;
	}
	// A held entry first, as above: an earlier firmware's patch entry can reach this sector too
	if (!journal_free()) {
		storage_unlock();
		return false;
	}
#endif
	if (same(addr, data, STORAGE_SECTOR)) {
		stats.sector_skips++;
		storage_unlock();
		return true;
	}
	if (board.flash->replace != NULL) {
		// Copy on write underneath: the new sector replaces the old in one step, so the journal's copy, header and
		// the erase would only wear the flash (on NAND, 11 page programs against 2)
		ok = board.flash->replace(addr, data) && same(addr, data, STORAGE_SECTOR);
		stats.sector_writes += ok;
		if (!ok) {
			stats.errors++;
		}
		storage_unlock();
		return ok;
	}
	uint32_t crc = storage_crc32(0, data, STORAGE_SECTOR);
	uint32_t j1 = m->journal_addr + STORAGE_SECTOR;
	uint8_t h[J_HDR_LEN];
	put32(&h[0], J_MAGIC);
	put32(&h[4], addr);
	put32(&h[8], crc);
	put32(&h[12], storage_crc32(0, h, 12));
	uint32_t check;
	uint8_t done = 0;
	if (er(j1) && wr(j1, data, STORAGE_SECTOR) && flash_crc(j1, STORAGE_SECTOR, 0, &check) && check == crc &&
	    er(m->journal_addr) && wr(m->journal_addr, h, sizeof(h)) &&
	    er(addr) && wr(addr, data, STORAGE_SECTOR) && same(addr, data, STORAGE_SECTOR) &&
	    wr(m->journal_addr + J_DONE, &done, 1)) {
		stats.sector_writes++;
		ok = true;
	}
	storage_unlock();
	return ok;
}

uint8_t *storage_buffer_acquire(void)
{
	uint8_t *b = NULL;

	storage_lock();
	if (!buffer_taken) {
		buffer_taken = true;
		b = buffer;
	}
	storage_unlock();
	return b;
}

void storage_buffer_release(void)
{
	storage_lock();
	buffer_taken = false;
	storage_unlock();
}

bool storage_write(uint32_t addr, const void *buf, uint32_t len)
{
	const hal_flash_map_t *m = storage_map();
	const uint8_t *p = buf;

	if (!storage_ready()) {
		return refused();
	}
	if (len == 0 || addr + len < addr || addr + len > board.flash->size) {
		return len == 0;
	}
	storage_lock();
#if STORAGE_FACTORY_KEEP
	// Before the shared buffer is ours: a held patch entry's replay merges in it
	if (held && !journal_free()) {
		storage_unlock();
		return false;
	}
#endif
	uint8_t *b = storage_buffer_acquire();
	bool ok = b != NULL;
	// A range touching a kept sector takes the sector path, so the kept rewrite: the patch transaction would erase
	// that sector from one read of it, the factory bytes in J1 only
	if (ok && p != NULL && len <= P_MAX && !kept(m, addr, len)) {
		// One transaction for the one or two sectors it touches (above)
		if (same(addr, p, len)) {
			stats.sector_skips++;
		} else {
			memset(b, 0xFF, P_DATA);
			put32(&b[0], P_MAGIC);
			put32(&b[4], addr);
			put32(&b[8], len);
			put32(&b[12], storage_crc32(storage_crc32(0, b, 12), p, len));
			memcpy(b + P_DATA, p, len);
			ok = er(m->journal_addr) && wr(m->journal_addr, b, P_DATA + len) &&
			     same(m->journal_addr, b, P_DATA + len) && patch_finish(m, addr, len, b);
		}
		len = 0;
	}
	while (ok && len > 0) {
		uint32_t base = addr & ~(STORAGE_SECTOR - 1u);
		uint32_t off = addr - base;
		uint32_t n = STORAGE_SECTOR - off < len ? STORAGE_SECTOR - off : len;
		ok = rd(base, b, STORAGE_SECTOR);
		if (ok) {
			if (p != NULL) {
				memcpy(b + off, p, n);
				p += n;
			} else {
				memset(b + off, 0xFF, n);
			}
			ok = storage_sector_write(base, b);
		}
		addr += n;
		len -= n;
	}
	if (b != NULL) {
		storage_buffer_release();
	}
	storage_unlock();
	return ok;
}

storage_native_t storage_native_erase(void)
{
	const hal_flash_map_t *m = storage_map();
	// a map without a native area (a small chip's): nothing there to empty, so a factory reset goes on
	if (storage_ready() && m != NULL && m->native_size == 0) {
		return STORAGE_NATIVE_BLANK;
	}
	if (!storage_ready() || m == NULL || (m->native_addr % STORAGE_SECTOR) != 0 ||
	    (m->native_size % STORAGE_SECTOR) != 0 || m->native_addr + m->native_size > board.flash->size) {
		return STORAGE_NATIVE_FAILED;
	}
	storage_native_t r = STORAGE_NATIVE_FAILED;
	uint8_t h[4];
	storage_lock();
	if (!rd(m->native_addr, h, sizeof(h))) {
		goto out;
	}
	if (get32(h) != STORAGE_NATIVE_MAGIC) {
		r = blank(m->native_addr, m->native_size) ? STORAGE_NATIVE_BLANK : STORAGE_NATIVE_FOREIGN;
		goto out;
	}
	for (uint32_t a = m->native_addr + m->native_size; a > m->native_addr; ) {
		a -= STORAGE_SECTOR;		// from the end: the header sector last
		if (!blank(a, STORAGE_SECTOR) && !er(a)) {
			goto out;
		}
	}
	r = blank(m->native_addr, STORAGE_SECTOR) ? STORAGE_NATIVE_ERASED : STORAGE_NATIVE_FAILED;
out:
	storage_unlock();
	return r;
}

static storage_job_fn volatile job_fn;	// set last by the poster, cleared by the housekeeping task when done
static void *volatile job_ctx;

bool storage_job_post(storage_job_fn fn, void *ctx)
{
	if (fn == NULL || job_fn != NULL) {
		return false;
	}
	job_ctx = ctx;
	job_fn = fn;
	return true;
}

bool storage_job_busy(void)
{
	return job_fn != NULL;
}

#if STORAGE_FACTORY_KEEP && CONFIG_FACTORY_COPY
static storage_job_fn volatile factory_job_fn;	// as job_fn, for the factory copy's restore alone
static void *volatile factory_job_ctx;

bool storage_factory_job_post(storage_job_fn fn, void *ctx)
{
	if (fn == NULL || factory_job_fn != NULL) {
		return false;
	}
	factory_job_ctx = ctx;
	factory_job_fn = fn;
	return true;
}

bool storage_factory_job_busy(void)
{
	return factory_job_fn != NULL;
}
#endif

void storage_job_poll(void)
{
	storage_job_fn fn = job_fn;
	if (fn != NULL) {
		fn(job_ctx);
		job_fn = NULL;
	}
#if STORAGE_FACTORY_KEEP && CONFIG_FACTORY_COPY
	fn = factory_job_fn;
	if (fn != NULL) {
		fn(factory_job_ctx);
		factory_job_fn = NULL;
	}
#endif
}

void storage_get_stats(storage_stats_t *s)
{
	*s = stats;
	s->claim_pending = claim_pending;
}
