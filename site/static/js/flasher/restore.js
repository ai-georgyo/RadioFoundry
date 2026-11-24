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
//   target "codeplug"  only the codeplug areas, through the CPS: the firmware stays as it is (a codeplug backup)
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
