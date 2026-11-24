// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Protected flash regions (site/static/js/flasher/protect.js): the same answers as tools/rfoundry/rfoundry/protect.py,
// and every board's list in each table the page reads (flash-boards.json boards, c7000, f4stub; radios.json flash)
// equal to the registry's (board.toml [[flash.protect]]).

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import * as protect from "../static/js/flasher/protect.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const registry = JSON.parse(readFileSync("tools/rfoundry/rfoundry/registry.json", "utf8")).boards;
const CAL = { mem: "progmem", start: 0x1FE000, end: 0x1FF000, what: "factory calibration (hardware-x.md 6.1)" };

test("check refuses an overlap, names the region and its reason", () => {
	protect.check([CAL], "progmem", 0xC000, 0x1FE000);
	protect.check([CAL], "data", 0x1FE000, 0x1FF000);
	protect.check(undefined, "progmem", 0, 0x200000);
	assert.throws(() => protect.check([CAL], "progmem", 0x1FD000, 0x1FE001), (e) =>
		e instanceof protect.ProtectedError && e.code === "protected" && /0x1FE000-0x1FF000/.test(e.message) &&
		e.message.includes(CAL.what) && e.regions[0] === CAL);
	assert.throws(() => protect.check([CAL], "eeprom", 0, 1), /unknown memory/);
});

test("a whole-chip erase is refused on a memory with a protected region", () => {
	protect.checkWhole([CAL], "data");
	assert.throws(() => protect.checkWhole([CAL], "progmem"), protect.ProtectedError);
});

test("carveBytes leaves the protected pieces out", () => {
	const two = [CAL, { ...CAL, start: 0x100000, end: 0x102000 }];
	assert.deepEqual(protect.carveBytes(two, "progmem", 0xC000, 0x200000), [[0xC000, 0x100000], [0x102000, 0x1FE000], [0x1FF000, 0x200000]]);
	assert.deepEqual(protect.carveBytes(two, "progmem", 0x1FE000, 0x1FF000), []);
	assert.deepEqual(protect.carveBytes(two, "data", 0, 0x1000), [[0, 0x1000]]);
});

// A range the firmware keeps inside a sector (an invented table) and one that covers two sectors whole: the vectors of
// tools/rfoundry/tests/test_protect.py
const TABLE = { mem: "data", start: 0x1000, end: 0x1200, what: "factory table (hardware-x.md 2.1)" };
const RECORDS = { mem: "data", start: 0x1E000, end: 0x20000, what: "maker's records (hardware-x.md 2.4)" };

test("carveBytes is byte exact: the pieces a writer through the firmware sends around a range inside a sector", () => {
	const both = [RECORDS, TABLE];	// out of order: carveBytes sorts
	assert.deepEqual(protect.carveBytes(both, "data", 0x1000, 0x2000), [[0x1200, 0x2000]]);
	assert.deepEqual(protect.carveBytes(both, "data", 0x0F80, 0x1220), [[0x0F80, 0x1000], [0x1200, 0x1220]]);
	assert.deepEqual(protect.carveBytes(both, "data", 0x1100, 0x1180), []);
	assert.deepEqual(protect.carveBytes(both, "data", 0x1D000, 0x21000), [[0x1D000, 0x1E000], [0x20000, 0x21000]]);
	assert.deepEqual(protect.carveBytes(both, "data", 0x11FF, 0x1201), [[0x1200, 0x1201]]);
	assert.deepEqual(protect.carveBytes(both, "progmem", 0x1000, 0x2000), [[0x1000, 0x2000]]);	// another memory
	// two ranges that overlap or touch leave no piece between them
	assert.deepEqual(protect.carveBytes([TABLE, { ...TABLE, start: 0x1100, end: 0x1300 }, { ...TABLE, start: 0x1300, end: 0x1310 }], "data", 0x1000, 0x2000),
		[[0x1310, 0x2000]]);
});

test("carvedUnits: a direct writer leaves out every unit an entry touches, an entry inside a sector widened to it", () => {
	assert.deepEqual(protect.carvedUnits([CAL], "progmem", 0x1FC000, 0x200000, 0x1000), [0x1FC000, 0x1FD000, 0x1FF000]);
	const all = [];
	for (let a = 0; a < 0x22000; a += 0x1000) if (a !== 0x1000 && !(a >= 0x1E000 && a < 0x20000)) all.push(a);
	assert.deepEqual(protect.carvedUnits([TABLE, RECORDS], "data", 0, 0x22000, 0x1000), all);
	assert.deepEqual(protect.carvedUnits([{ ...TABLE, start: 0x1FF0, end: 0x2010 }], "data", 0, 0x4000, 0x1000), [0, 0x3000]);	// across a sector end: both
	assert.deepEqual(protect.widen([TABLE, CAL], "data", 0x1000), [{ ...TABLE, start: 0x1000, end: 0x2000 }]);
	assert.deepEqual(protect.widen([RECORDS], "data", 0x1000), [RECORDS]);	// on the boundaries: as it is
	assert.equal(TABLE.end, 0x1200, "widen copies, it doesn't change the board's entry");
	assert.throws(() => protect.widen([TABLE], "eeprom", 0x1000), /unknown memory/);
	// the unit a direct writer erases: any overlap with an entry inside it refuses
	assert.throws(() => protect.check([TABLE], "data", 0x1000, 0x2000, "erase"), protect.ProtectedError);
	protect.check([TABLE], "data", 0x2000, 0x3000, "erase");
});

test("every table the page reads carries each board's protected regions", () => {
	const fb = json("flash-boards.json");
	const radios = json("radios.json").radios;
	for (const [id, b] of Object.entries(registry)) {
		assert.deepEqual(fb.boards[id].protect, b.protect, `flash-boards boards.${id}`);
		if (fb.c7000.boards[id]) assert.deepEqual(fb.c7000.boards[id].protect, b.protect, `c7000.boards.${id}`);
		if (fb.f4stub.boards[id]) assert.deepEqual(fb.f4stub.boards[id].protect, b.protect, `f4stub.boards.${id}`);
		const r = radios.find((x) => x.flash?.board === id);
		if (r) assert.deepEqual(r.flash.protect, b.protect, `radios.json ${r.id}`);
	}
	assert.ok(Object.values(registry).some((b) => b.protect.length), "some board has a protected region");
});
