// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Restoring a radio from a backup bundle (backup.js). What can be put back depends on what the bundle holds:
//
//   target "rfoundry"    the radio as it was when the backup was taken on RadioFoundry: the internal flash image exactly (RadioFoundry and
//                   its codec, through the bootloader), then the codeplug areas of the SPI flash (through the CPS,
//                   which only allows the codeplug areas)
//   target "openfw"   the other open firmware the radio ran before the install (a backup taken over its CPS,
//                   openfw.js): its internal flash image exactly, through the bootloader. The SPI flash is not written
//                   back (no command for that yet, as for "stock"): that firmware's own CPS can write its codeplug back
//                   from the bundle's spi-flash.bin
//   target "codeplug"  only the codeplug areas, through the CPS: the firmware stays as it is (a codeplug backup).
//                   A board's factory range is never written by it: the step after it compares the radio's with the
//                   backup's and writes it back only through the radio's own restore command (factoryCheck, factoryWrite)
//   target "stock"  stock firmware back: the application from the user's stock file (in the bundle, or chosen
//                   again), through the bootloader. The SPI flash keeps what is in it; the bundle's copy shows what
//                   it was. Writing the whole SPI flash back needs a firmware command that doesn't exist yet
//                   (docs/dev/web-flasher.md "Restore"), so the flasher reports what differs instead of guessing.

import { RESTORE, makePlan } from "./plan.js";
import { carveBytes } from "./protect.js";
import { stockApplication } from "./stockfw.js";
import { CODEPLUG_SPAN } from "./backup.js";
import { TFTP_SIZE } from "../codeplug/image.js";
import { classify } from "../codeplug/detect.js";
import { crc32, sleep } from "./bytes.js";
import { LinkError } from "./link.js";

export class RestoreError extends Error {}

export function targets(bundle) {
	const out = [];
	const internal = bundle.parts.get("internal-flash");
	const spi = bundle.parts.get("spi-flash");
	if (bundle.meta.firmware_now?.kind === "rfoundry" && internal?.complete) out.push("rfoundry");
	if (bundle.meta.firmware_now?.kind === "openfw" && internal?.complete) out.push("openfw");
	// the memory chip read as far as the codeplug's regions: a smaller chip's backup ends before CODEPLUG_SPAN
	if (bundle.meta.firmware_now?.kind === "rfoundry" && spi && spi.data.length >= TFTP_SIZE && codeplugLayout(bundle) === "openfw") out.push("codeplug");
	if (bundle.parts.has("stock-firmware") || bundle.meta.firmware_before?.kind === "stock") out.push("stock");
	return out;
}

// What the backup's codeplug areas hold (codeplug/detect.js): "openfw" for a RadioFoundry codeplug. A backup taken right after
// a first install still holds the original firmware's codeplug: written back as it is, it would be garbage to RadioFoundry, so
// only the Codeplug Editor (which converts it) takes it
export function codeplugLayout(bundle) {
	const spi = bundle.parts.get("spi-flash");
	return spi ? classify(spi.data).layout : null;
}

export function planInternal(bundle, board) {
	const p = bundle.parts.get("internal-flash");
	if (!p || !p.complete) throw new RestoreError("the bundle has no complete internal flash read");
	if (p.addr !== board.app_base) throw new RestoreError("the internal flash read does not start at the application base");
	return makePlan(board, [[p.addr, p.data]], RESTORE);
}

export function planStock(stockFile, key, board, fb) {
	const [addr, app] = stockApplication(stockFile, key, board, fb);
	return makePlan(board, [[addr, app]], RESTORE);
}

// The 4 KB sectors of the codeplug areas whose bytes differ between the radio and the backup outside the board's
// protected regions: [addr, bytes, parts] (emulated EEPROM 0x00000-0x0FFFF and the open-firmware flash image
// 0x20000-0x11FFFF; never the calibration and RadioFoundry's own areas at 0x10000-0x1FFFF, which the CPS refuses
// anyway). protect: the board's protected regions; their data bytes are never sent. parts: [[addr, bytes]], every
// piece of the sector outside them, what the write sends (the CPS keeps the radio's bytes in the gaps,
// cps-protocol.md 2.3). bytes: the sector as the radio holds it after the write, the backup's with the radio's own
// bytes in a protected region. A sector that differs only inside a region is not written, nor is one a region covers
// whole
export function codeplugSectors(backupSpi, radioSpi, protect = []) {
	const out = [];
	for (let a = 0; a < CODEPLUG_SPAN; a += 4096) {
		if (a >= 0x10000 && a < 0x20000) continue;
		const want = backupSpi.subarray(a, a + 4096);
		const have = radioSpi.subarray(a, a + 4096);
		if (want.length < 4096) break;
		const pieces = carveBytes(protect, "data", a, a + 4096);
		let same = have.length === 4096;
		for (const [lo, hi] of pieces) for (let i = lo - a; same && i < hi - a; i++) same = want[i] === have[i];
		if (same || !pieces.length) continue;
		const data = want.slice();
		data.set(have);
		for (const [lo, hi] of pieces) data.set(want.subarray(lo - a, hi - a), lo - a);
		out.push([a, data, pieces.map(([lo, hi]) => [lo, data.subarray(lo - a, hi - a)])]);
	}
	return out;
}

// The board's protected data regions in the codeplug areas whose bytes differ between the backup and the radio:
// codeplugSectors() doesn't write them back (the radio keeps its own), so the restore names them in its log
export function keptDiffering(backupSpi, radioSpi, protect = []) {
	const areas = [[0, 0x10000], [0x20000, Math.min(CODEPLUG_SPAN, backupSpi.length)]];
	const differs = (lo, hi) => {
		for (let i = lo; i < hi; i++) if (backupSpi[i] !== radioSpi[i]) return true;
		return false;
	};
	return (protect || []).filter((p) => p.mem === "data" && areas.some(([x, y]) => differs(Math.max(p.start, x), Math.min(p.end, y))));
}

// Each sector's parts through the CPS (all of it for a sector given without parts); the addresses the radio refused
export async function writeCodeplug(link, sectors, progress = () => {}) {
	const refused = [];
	let done = 0;
	for (const [addr, data, parts] of sectors) {
		if (!await link.cpsWriteParts(addr, parts ?? [[addr, data]])) refused.push(addr);
		progress(++done, sectors.length);
	}
	await link.cpsClose();
	return refused;
}

// ---- the factory copy's restore (settings-store.md 4.2, debuglink.md 3.13 ops 3-5), after a codeplug restore

// FACTORY_RESTORE's reasons and its job's results by number (append only), as rfoundry_debug.py names them
export const FACTORY_RESULTS = ["ok", "foreign", "busy", "stage", "crc", "invalid", "unstable", "window_crc", "window_differs",
	"record_differs", "record_other", "verify", "record", "flash"];
export const FACTORY_PENDING = 0xFE, FACTORY_NONE = 0xFF;
const ST_UNKNOWN = 1, ST_BAD_ARG = 2, ST_BUSY = 3, ST_UNAVAILABLE = 4, ST_REFUSED = 7;
export const factoryResult = (n) => FACTORY_RESULTS[n] ?? (n === FACTORY_PENDING ? "pending" : n === FACTORY_NONE ? "none" : "other");

const status = (e) => (e instanceof LinkError ? e.status : undefined);
const sameBytes = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

// size bytes at addr through STORAGE READ, read twice; null when the two reads differ
async function readTwice(link, addr, size) {
	const a = await link.storageRead(addr, size);
	return sameBytes(a, await link.storageRead(addr, size)) ? a : null;
}

// What the page can do about the board's factory range once the codeplug is back: the radio's factory copy (op 3), its
// window read twice, and the backup's bytes, compared. {kind, info, window, backup, crc: {radio, record, backup}, range,
// others}; kind, in the order decided:
//   unavailable  the firmware is older than the factory copy ops (op 3 unknown or a bad argument), or refuses the read
//                of the range: the tool command, after an update
//   busy         the radio has not loaded its copy at this start yet (op 3 busy), or two reads of the window differed
//   none         the board keeps no factory copy (op 3 unavailable, a build without it, or present 0): the kept
//                notice alone, as before the step; an update would change nothing
//   disagree     the radio's range is not a data entry of the board's protected regions: the page and the firmware
//                disagree, so nothing is sent
//   short        the backup's memory chip read ends before the range
//   same         the window holds the backup's bytes
//   empty        the backup's range is all 0xFF or all 0x00: no table to write back
//   other_radio  the identity compare (as rfoundry_debug.py's): another protected data range of the board differs between
//                the radio and the backup (nothing of RadioFoundry's writes them), so the backup looks like another radio's
//   unchecked    no other range to compare, or one the radio doesn't open to reads (or reads differently twice) or
//                the backup doesn't hold
//   offer        the window differs from the backup and the backup is this radio's: factoryWrite() may run
// The page never takes a shortcut past the identity compare: the tool's --no-identity is the way past it, on purpose
// not a click
// busyWaitMs: between the tries of an op 3 that answered busy (five in all)
export async function factoryCheck(link, backupSpi, protect = [], { busyWaitMs = 500 } = {}) {
	let info;
	for (let i = 0; ; i++) {
		try {
			info = await link.factoryInfo();
			break;
		} catch (e) {
			if ([ST_UNKNOWN, ST_BAD_ARG].includes(status(e))) return { kind: "unavailable" };
			if (status(e) === ST_UNAVAILABLE) return { kind: "none" };
			if (status(e) !== ST_BUSY) throw e;
			if (i >= 4) return { kind: "busy" };
			await sleep(busyWaitMs);
		}
	}
	if (!info) return { kind: "none" };
	const data = (protect || []).filter((p) => p.mem === "data");
	const range = data.find((p) => p.start === info.addr && p.end === info.addr + info.size);
	if (!range) return { kind: "disagree", info };
	if (backupSpi.length < range.end) return { kind: "short", info };
	const backup = backupSpi.slice(range.start, range.end);
	let window;
	try {
		window = await readTwice(link, range.start, info.size);
	} catch (e) {
		if (status(e) === ST_REFUSED || status(e) === ST_UNAVAILABLE) return { kind: "unavailable", info };
		throw e;
	}
	if (!window) return { kind: "busy", info };
	const crc = { radio: crc32(window), record: ["ok", "bad"].includes(info.record) ? info.recordCrc : null, backup: crc32(backup) };
	const out = { info, window, backup, crc, range, others: [] };
	if (sameBytes(window, backup)) return { ...out, kind: "same" };
	if (backup.every((x) => x === 0xFF) || backup.every((x) => x === 0)) return { ...out, kind: "empty" };
	for (const p of data.filter((x) => x !== range).sort((a, b) => a.start - b.start)) {
		let radio = null;
		try {
			radio = await readTwice(link, p.start, p.end - p.start);
		} catch (e) {
			if (status(e) !== ST_REFUSED && status(e) !== ST_UNAVAILABLE) throw e;
		}
		const file = backupSpi.length >= p.end ? backupSpi.subarray(p.start, p.end) : null;
		out.others.push({ entry: p, radio: radio && crc32(radio), file: file && crc32(file), same: !!(radio && file && sameBytes(radio, file)) });
	}
	if (out.others.some((o) => o.radio !== null && o.file !== null && !o.same)) return { ...out, kind: "other_radio" };
	if (!out.others.length || out.others.some((o) => !o.same)) return { ...out, kind: "unchecked" };
	return { ...out, kind: "offer" };
}

// The restore of an "offer" (factoryCheck): ops 4 and 5 with the CRC-32 of the window the check read, the job's result
// polled (op 3, up to waitMs), the window read back. {result, refused, why, off, info, same}: result a FACTORY_RESULTS
// name, "busy" when the radio answered busy, "timeout" when the job was still pending; refused true when nothing was
// written (op 5 refused it, or busy); same: the window read back holds the backup's bytes. Success: result "ok" and same
export async function factoryWrite(link, chk, { pollMs = 200, waitMs = 10000 } = {}) {
	try {
		await link.factoryRestore(chk.backup, chk.crc.radio);
	} catch (e) {
		if (status(e) === ST_REFUSED && e.data?.length) {
			const d = e.data;
			return { result: factoryResult(d[0]), refused: true, why: d[1] ?? 0, off: d.length >= 4 ? d[2] | (d[3] << 8) : 0 };
		}
		if (status(e) === ST_BUSY) return { result: "busy", refused: true };
		throw e;
	}
	const until = Date.now() + waitMs;
	let info;
	for (;;) {
		info = await link.factoryInfo();
		if (!info || info.result !== FACTORY_PENDING) break;
		if (Date.now() > until) return { result: "timeout", refused: false, info };
		await sleep(pollMs);
	}
	const after = await readTwice(link, chk.range.start, chk.backup.length);
	// a reason of op 5's own (1-10) here is the job's check again, under the storage lock: it changed nothing
	const n = info ? info.result : -1;
	return { result: info ? factoryResult(n) : "other", refused: n >= 1 && n <= 10, info, same: !!after && sameBytes(after, chk.backup) };
}

// The page's words for a factoryWrite() outcome that isn't success: [key, fmt] for t(). The reasons that mean "the
// bytes moved or were read differently meanwhile" share one text, as do the two failed writes; "ok" without the
// backup's bytes read back is the read back's
const FACTORY_WHY = { foreign: "foreign", busy: "busy", stage: "changed", crc: "changed", unstable: "changed", window_crc: "changed",
	invalid: "invalid", window_differs: "window_differs", record_differs: "record_differs", record_other: "record_other",
	verify: "write", flash: "write", record: "record", ok: "readback", timeout: "timeout" };
export const FACTORY_WHY_KEYS = [...new Set(Object.values(FACTORY_WHY))].map((k) => `flash.restore.factory_why_${k}`).concat("flash.restore.factory_why_other");
export function factoryWhyKey(r) {
	const k = FACTORY_WHY[r.result];
	return k ? [`flash.restore.factory_why_${k}`] : ["flash.restore.factory_why_other", { name: r.result }];
}
