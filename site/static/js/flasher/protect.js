// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Protected flash regions: the one check every write in the web flasher makes first. A board's [[flash.protect]]
// (docs/dev/board-registry.md, "Protected regions"; data/flash-boards.json boards[id].protect) lists the factory data
// nothing may erase or write, the calibration first. Each entry is {mem, start, end, what}: the memory ("progmem",
// "data", "nand", "secreg") and an offset range in it, progmem from the platform's flash base. An entry is a byte range
// no tool changes; what a writer does with one depends on how it writes:
//
//   a direct writer (it erases the chip itself, with no journal: the stubs, the bootloaders' plans) works by whole
//     erase units: a write over one is refused before anything is erased (check), a restore leaves out every unit an
//     entry touches (carvedUnits, an entry off the unit boundaries widened to them: widen). A cut erase would lose it
//   a writer through the firmware's CPS (the Codeplug Editor, the codeplug restore) writes the sector but sends no
//     byte of a range (carveBytes): the CPS merges only the bytes sent into the sector it read from the flash
//
// The same rules as tools/rfoundry/rfoundry/protect.py.

export const NAMES = { progmem: "program memory", data: "data memory", nand: "NAND", secreg: "security registers" };

export class ProtectedError extends Error {
	constructor(message, regions) {
		super(message);
		this.name = "ProtectedError";
		this.code = "protected";
		this.regions = regions;
	}
}

const hex = (x) => "0x" + x.toString(16).toUpperCase();
export const describe = (p) => `${NAMES[p.mem]} ${hex(p.start)}-${hex(p.end)}, ${p.what}`;

// The entries of mem that overlap [start, end)
export function overlapping(protect, mem, start, end) {
	if (!(mem in NAMES)) throw new Error(`unknown memory ${mem}`);
	return (protect || []).filter((p) => p.mem === mem && p.start < end && start < p.end);
}

// Throws ProtectedError when [start, end) of mem overlaps a protected region
export function check(protect, mem, start, end, action = "write") {
	const hit = overlapping(protect, mem, start, end);
	if (hit.length) {
		throw new ProtectedError(`refusing to ${action} ${NAMES[mem]} ${hex(start)}-${hex(end)}: it overlaps a protected region ` +
			`(${hit.map(describe).join("; ")}). Nothing was written`, hit);
	}
}

// Refuses a whole-chip erase of a memory with any protected region
export function checkWhole(protect, mem) {
	const hit = (protect || []).filter((p) => p.mem === mem);
	if (hit.length) throw new ProtectedError(`refusing to erase the whole ${NAMES[mem]}: it holds protected regions (${hit.map(describe).join("; ")})`, hit);
}

// [start, end) without mem's protected regions, byte exact: [[lo, hi]] in order, the pieces a writer through the
// firmware sends. The radio's own bytes stay in the gaps
export function carveBytes(protect, mem, start, end) {
	const out = [];
	let at = start;
	for (const p of overlapping(protect, mem, start, end).sort((a, b) => a.start - b.start)) {
		if (p.start > at) out.push([at, p.start]);
		at = Math.max(at, p.end);
	}
	if (at < end) out.push([at, end]);
	return out;
}

// The entries of mem with each range widened to whole units of unit bytes (start down, end up): what a direct writer,
// which erases unit by unit, must leave alone. An entry on the unit boundaries stays as it is; the board's entries are
// copied, not changed
export function widen(protect, mem, unit) {
	if (!(mem in NAMES)) throw new Error(`unknown memory ${mem}`);
	return (protect || []).filter((p) => p.mem === mem)
		.map((p) => ({ ...p, start: p.start - (p.start % unit), end: Math.ceil(p.end / unit) * unit }));
}

// The unit-aligned addresses in [start, end) no protected region touches, for a writer that erases unit by unit (a
// 4 KB sector): an entry off the unit boundaries (a range the firmware keeps inside a sector) leaves its units out
// whole, never cut midway
export function carvedUnits(protect, mem, start, end, unit) {
	const out = [];
	for (const [lo, hi] of carveBytes(widen(protect, mem, unit), mem, start, end)) for (let a = lo; a < hi; a += unit) out.push(a);
	return out;
}
