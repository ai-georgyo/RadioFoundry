// SPDX-License-Identifier: MIT Copyright (c) 2026 The RadioFoundry Authors Safe writes to the board's data flash
// (docs/spec/settings-store.md 4): a sector rewrite goes through a journal (a copy of the new sector and a header
// saying where it goes), so a power cut in the middle of the erase or the program is finished at the next boot instead
// of leaving a half-erased codeplug sector. Shared by the codeplug editor and the open-firmware CPS shim. No RTOS here:
// storage_lock() / storage_unlock() come from storage_rtos.c in the firmware and from the test in host tests.
#ifndef RFOUNDRY_STORAGE_H
#define RFOUNDRY_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "board.h"

#define STORAGE_SECTOR		4096u	// the erase unit the core works in (boards with another erase size: no writes)

// One recursive lock around every flash transaction sequence of the core (settings, codeplug, CPS)
void storage_lock(void);
void storage_unlock(void);
// A second lock for short RAM-only sections (the settings copy): never held across a flash operation, so the radio
// task never waits for an erase
void storage_quick_lock(void);
void storage_quick_unlock(void);

// After the board's flash works, before the settings and the codeplug are read: check that the settings ring and the
// journal are ours (else the region is foreign, below), then finish a journaled rewrite that a power cut
// interrupted. False if there is no usable flash or the region is foreign (then every write is refused). With
// STORAGE_FACTORY_KEEP (hal_flash.h), a factory list that fails its checks (docs/spec/settings-store.md 4.1) leaves the
// region none: nothing is written at all
bool storage_init(void);
// Flash usable and the region ours: writes allowed
bool storage_ready(void);

// --- Foreign region guard (docs/spec/settings-store.md 5) ---
// A flash that held another firmware may have its own data where our settings ring and journal go. When the region
// holds bytes that are neither erased nor ours, and not one valid record of ours (a settings record, a journal
// header) is anywhere in it, it is foreign: nothing is erased or programmed there (and so no codeplug or CPS write
// goes through the journal) until the debug link claims it (after the host dumped it)
//
// Claim first. A blank region is taken at the first start (an ownership mark is programmed), which is right when the
// other firmware's layout is known: its data is where the map says. On a flash that held another firmware's data of
// unknown layout a blank region says nothing about the megabytes around it, and that mark would open every codeplug
// and CPS write before a backup exists. Such a board builds with -DSTORAGE_CLAIM_FIRST=1 (MCU_DEFS in its board.mk):
// a region without a valid record of ours is then foreign, blank or not, and the claim is the only way in. A define
// and not a field of the flash map, so that the boards that don't set it pay nothing for it
#ifndef STORAGE_CLAIM_FIRST
#define STORAGE_CLAIM_FIRST	0
#endif
typedef enum {
	STORAGE_REGION_NONE,		// no flash map / unusable flash
	STORAGE_REGION_OURS,		// holding our records, or blank (blank: not with STORAGE_CLAIM_FIRST)
	STORAGE_REGION_FOREIGN,		// someone else's data, or (STORAGE_CLAIM_FIRST) not claimed yet: hands off
} storage_region_t;
storage_region_t storage_region(void);
// Ask for a claim (the debug link, behind the UID token): the housekeeping task erases the settings ring and the
// journal (settings_poll -> storage_claim) and the region becomes ours. False if the region isn't foreign
bool storage_claim_request(void);
bool storage_claim_pending(void);
// Erase the region and take it (false if not foreign or the flash failed). Slow: some 10 sector erases
bool storage_claim(void);
// The flash ranges the debug link may read (the settings ring, the journal, the calibration copy and, with
// STORAGE_FACTORY_KEEP, each factory range of a list storage_init accepted): true when the whole of [addr, addr + len)
// is inside one of them
bool storage_debug_read_allowed(uint32_t addr, uint32_t len);

// --- Settings slots (the record format, shared with core/settings so the guard knows our records) ---
#define STORAGE_SLOT_SIZE	256u
#define STORAGE_SLOT_MAGIC	0x5445534Fu	// "OSET"
#define STORAGE_SLOT_HDR	12u		// magic, sequence, layout, payload length
#define STORAGE_SLOT_COMMIT	(STORAGE_SLOT_SIZE - 4u)
#define STORAGE_SLOT_PAYLOAD_MAX (STORAGE_SLOT_COMMIT - STORAGE_SLOT_HDR - 4u)
typedef enum {
	STORAGE_SLOT_FREE,		// all 0xFF
	STORAGE_SLOT_VALID,		// a complete record (magic, length, CRC, commit word)
	STORAGE_SLOT_TORN,		// our magic but not complete (a cut during a save)
	STORAGE_SLOT_OTHER,		// anything else (a torn erase, or someone else's data)
} storage_slot_t;
storage_slot_t storage_slot_check(const uint8_t slot[STORAGE_SLOT_SIZE]);

// --- Factory ranges (STORAGE_FACTORY_KEEP, docs/spec/settings-store.md 4.1) ---
// A kept sector holds bytes of a factory range (hal_flash_map_t.factory). Every rewrite of one carries those bytes
// over from the flash and takes the rest from the writer (the kept rewrite, as power safe as any journaled write);
// the writer's bytes for a range are dropped (the stats' factory_dropped) and the write still succeeds. A sector the
// ranges cover whole is never erased or programmed: a write to it succeeds and changes nothing. A failure past a kept
// rewrite's commit point holds the journal: every journaled write after it finishes that entry first, or is refused

// Rewrite the erase unit at addr (aligned) with data (STORAGE_SECTOR bytes), through the journal. Skipped when the
// flash already holds exactly that (in a kept sector: outside its factory bytes)
bool storage_sector_write(uint32_t addr, const uint8_t *data);

#if STORAGE_FACTORY_KEEP && CONFIG_FACTORY_COPY
// The one write of a factory range's own bytes (the factory copy's restore, settings-store.md 4.2, is its one
// caller): the kept rewrite of the range's sector with len bytes for the range, the sector's other bytes (other
// ranges' included) carried over from the flash. addr and len must be one listed range exactly, inside one sector that
// holds other bytes too; else false, nothing written. Skipped when the range holds the bytes already. As power safe as
// any kept rewrite: a cut leaves the old bytes or the new ones, never a mix; a failure past the commit point holds the
// journal. Takes the storage lock, not the shared buffer
bool storage_factory_write(uint32_t addr, const uint8_t *bytes, uint32_t len);
#endif

// Write any range: each sector it touches is read into the shared buffer, merged and rewritten as above. False if
// the shared buffer is taken (a CPS write in progress) or the flash fails. buf NULL erases the range (all 0xFF). Up
// to 4072 bytes are one transaction over the one or two sectors they touch (settings-store.md 4); a range touching a
// kept sector goes sector by sector through storage_sector_write instead, so a record crossing into or out of a kept
// sector is not one transaction
bool storage_write(uint32_t addr, const void *buf, uint32_t len);

// The shared sector buffer (STORAGE_SECTOR bytes): NULL while someone else has it
uint8_t *storage_buffer_acquire(void);
void storage_buffer_release(void);

// The flash map, NULL without one
const hal_flash_map_t *storage_map(void);

// The staging area (hal_flash_map_t.staging_*) is shared by the TFTP codeplug write (docs/spec/tftp.md 4) and the fast
// codeplug protocol (docs/spec/cps-fast.md 7). The first word of its first sector says whose commit record is there;
// each one's boot resume leaves the other's record alone, and neither starts while the other holds the area
#define STORAGE_STAGE_MAGIC_TFTP	0x5452444Fu	// bytes 4F 44 52 54 in memory order
#define STORAGE_STAGE_MAGIC_CPSF	0x4652444Fu	// bytes 4F 44 52 46
// The staging area's size in whole sectors when the board has a usable one (on the flash, clear of the settings, the
// journal, both open-firmware images, the native area, the calibration window and every kept sector), else 0
uint32_t storage_staging_size(void);

// --- The native codeplug area (docs/spec/codeplug-native.md 4) ---
// Its header sector starts with this magic (bytes 4F 44 52 4E, little endian). Nothing writes the area yet; the magic is what
// tells our data from another firmware's there
#define STORAGE_NATIVE_MAGIC	0x4E52444Fu
typedef enum {
	STORAGE_NATIVE_BLANK,		// nothing there (or no native area in the map): nothing to do
	STORAGE_NATIVE_ERASED,		// our native data was there and is erased now
	STORAGE_NATIVE_FOREIGN,		// something that isn't ours (no magic in the header sector): left alone
	STORAGE_NATIVE_FAILED,		// no flash or map, the region isn't ours, or an erase failed
} storage_native_t;
// Empty the native area (factory reset): only when it holds our data (the magic) or nothing. The data sectors go
// first and the header sector last, so a cut in between leaves the magic and the reset can run again. Slow: one
// erase per sector that isn't blank (a 1 MB area: up to 256)
storage_native_t storage_native_erase(void);

// --- Deferred flash work: a task that must not wait for the flash (the UI) hands a job to the housekeeping task,
// which runs it from its loop (storage_job_poll, every 100 ms). One job at a time, posted from one task ---
typedef void (*storage_job_fn)(void *ctx);
// False while the last job is still pending or running
bool storage_job_post(storage_job_fn fn, void *ctx);
bool storage_job_busy(void);
// The housekeeping task: run the pending job, if any (and the factory copy's, after it)
void storage_job_poll(void);
#if STORAGE_FACTORY_KEEP && CONFIG_FACTORY_COPY
// A slot of its own for the factory copy's restore job (posted from the debug link's task; the slot above has the
// UI's task as its one poster), run by storage_job_poll after the other. False while its last job is pending or
// running
bool storage_factory_job_post(storage_job_fn fn, void *ctx);
bool storage_factory_job_busy(void);
#endif

typedef struct {
	uint32_t sector_writes;		// journaled rewrites done
	uint32_t sector_skips;		// rewrites skipped (same content)
	uint32_t replays;		// interrupted rewrites finished at boot
	uint32_t errors;		// flash operations that failed
	uint32_t foreign_refusals;	// writes refused because the region is foreign
	uint16_t foreign_slots;		// settings slots (and journal sectors) that were neither erased nor ours at boot
					// (0 in a foreign region: a blank one that must be claimed first)
	bool claim_pending;
#if STORAGE_FACTORY_KEEP
	uint32_t factory_rewrites;	// kept rewrites done
	uint32_t factory_dropped;	// writes whose bytes for a factory range differed from the flash: dropped
	uint32_t factory_replays;	// journal entries for a kept sector finished at a start or by a held journal
	uint32_t factory_refused;	// entries left pending, kept rewrites stopped by a read that differed or failed,
					// writes refused by a held journal, erases and programs of a kept sector outside
					// the grant (the last two also in errors)
#endif
} storage_stats_t;

void storage_get_stats(storage_stats_t *s);

// CRC-32 (IEEE 802.3, reflected, init and final XOR 0xFFFFFFFF); crc = 0 to start
uint32_t storage_crc32(uint32_t crc, const void *buf, uint32_t len);

#endif
