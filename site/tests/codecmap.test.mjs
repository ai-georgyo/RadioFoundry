// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The web flasher's codec map port (static/js/flasher/codecmap.js) against rfoundry's: the same synthetic vectors
// (tools/rfoundry/tests/codecmap_vectors.json, normalized by site/tests/fixtures.py) must give the same bytes.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { fromHex, toHex, u32le } from "../static/js/flasher/bytes.js";
import * as cm from "../static/js/flasher/codecmap.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const V = JSON.parse(readFileSync(join(DIR, "codecmap.json"), "utf8"));
const CODE = fromHex(V.code);

test("moving the code gives rfoundry's bytes", async () => {
	const out = await cm.moveCode(CODE, V.map, V.new_run);
	assert.equal(toHex(out), V.moved);
	assert.equal(u32le(out, 0x40), V.new_run + 0x10);
	assert.equal(toHex(await cm.moveCode(CODE, V.map, V.map.code.run)), V.code, "the stock address changes nothing");
	assert.deepEqual(cm.relocations(V.map), [0x40, 0x84, 0xFC]);
});

test("another build or a stale word is refused", async () => {
	const other = CODE.slice();
	other[5] ^= 1;
	await assert.rejects(cm.moveCode(other, V.map, V.new_run), cm.MapError);
	const noHash = { ...V.map, code: { ...V.map.code, sha256: null } };
	const stale = CODE.slice();
	stale.fill(0, 0x40, 0x44);
	await assert.rejects(cm.moveCode(stale, noHash, V.new_run), cm.MapError);
});

test("moving tables gives rfoundry's bytes, and groups move together", () => {
	const moves = Object.fromEntries(Object.entries(V.table_moves).map(([k, v]) => [Number(k), v]));
	assert.equal(toHex(cm.moveTables(CODE, V.map, moves)), V.tables_moved);
	assert.equal(toHex(cm.moveTables(CODE, V.map, {})), V.code);
	assert.throws(() => cm.moveTables(CODE, V.map, { [0x18001600]: 0x18003000 }), cm.MapError);
	assert.equal(cm.byCodeSha({ a: V.map }, V.map.code.sha256)[0].profile, "test-csky");
});

test("Thumb-2: movw/movt self references and island calls move as rfoundry moves them", async () => {
	const T = V.thumb;
	const code = fromHex(T.code), win = fromHex(T.window);
	const delta = T.new_run - T.map.code.run;
	const out = await cm.moveCode(code, T.map, T.new_run);
	assert.equal(toHex(out), T.moved);
	assert.equal(cm.pairValue(out, T.map.self_refs[1]), T.map.self_refs[1].value + delta);
	assert.equal(toHex(cm.moveBranches(T.window_addr, win, T.map, T.new_run)), T.window_moved);
	assert.equal(cm.branchesIn(T.map, T.window_addr, win.length), 2);
	await assert.rejects(cm.moveCode(code, T.map, T.new_run + 2), cm.MapError);
	assert.throws(() => cm.relocations(T.map), cm.MapError);
	const b = new Uint8Array(4);
	for (const [at, to] of [[0x080D1A00, 0x080E3AF4], [0x080D1A04, 0x08074794], [0x080D1A10, 0x07F00000]]) {
		cm.putBl(b, 0, at, to);
		assert.equal(cm.blTarget(b, 0, at), to);
	}
	const stale = win.slice();
	stale.fill(0, 0x10, 0x14);
	assert.throws(() => cm.moveBranches(T.window_addr, stale, T.map, T.new_run), cm.MapError);
});

test("the code and its table window move as two blocks, and the move is read back, as rfoundry does it", async () => {
	const W = V.window;
	const code = fromHex(W.code), tables = fromHex(W.tables);
	const [mc, mt] = await cm.moveWindow(code, tables, W.map, W.code_run, W.tables_run);
	assert.equal(toHex(mc), W.code_moved);
	assert.equal(toHex(mt), W.tables_moved);
	const [lo, n] = cm.tableWindow(W.map);
	assert.equal(n, tables.length);
	await assert.rejects(cm.moveWindow(code, tables, W.map, W.code_run, W.tables_run + 2), cm.MapError);
	for (const [cr, tr, verbatim] of [[W.code_run, W.tables_run, false], [0x0807537C, lo, false], [0x0807537C, lo, true], [0x08060004, 0x08098010, false]]) {
		const [c2, t2] = await cm.moveWindow(code, tables, W.map, cr, tr);
		const flash = new Uint8Array(0x100000).fill(0xFF);
		flash.set(c2, cr - 0x08000000);
		flash.set(verbatim ? tables : t2, tr - 0x08000000);
		const got = await cm.unmoveWindow(flash, 0x08000000, W.map, cr);
		assert.ok(got, `${cr} ${tr} ${verbatim}`);
		assert.equal(toHex(got[0]), W.code);
		assert.equal(toHex(got[1]), W.tables);
		assert.equal(got[2], tr);
		assert.equal(await cm.unmoveWindow(flash, 0x08000000, W.map, cr + 4), null);
		flash[tr - 0x08000000 + 0x100] ^= 1;
		assert.equal(await cm.unmoveWindow(flash, 0x08000000, W.map, cr), null);
	}
});

// Every window moved as its own block (relocate, docs/spec/codec-placement.md "Layouts"): for the code and one table
// window it is the two-block move rfoundry's vectors give, and it goes back the same way, from any layout to any other
test("relocating window by window gives the two-block move's bytes, both ways, and checks every site", async () => {
	const W = V.window;
	const code = fromHex(W.code), tables = fromHex(W.tables);
	const [lo, n] = cm.tableWindow(W.map);
	const wins = [[W.map.code.run, code.length], [lo, n]];
	const stock = [W.map.code.run, lo];
	const [mc, mt] = cm.relocate(W.map, wins, [code, tables], stock, [W.code_run, W.tables_run]);
	assert.equal(toHex(mc), W.code_moved);
	assert.equal(toHex(mt), W.tables_moved);
	const back = cm.relocate(W.map, wins, [mc, mt], [W.code_run, W.tables_run], stock);
	assert.deepEqual(back.map(toHex), [W.code, W.tables]);
	// one placed layout to another, without stock in between
	const other = [0x08060004, 0x08098010];
	const via = cm.relocate(W.map, wins, [mc, mt], [W.code_run, W.tables_run], other);
	const [oc, ot] = await cm.moveWindow(code, tables, W.map, ...other);
	assert.deepEqual(via.map(toHex), [toHex(oc), toHex(ot)]);
	// the wrong source layout, a damaged site, a broken alignment or a wrong length: refused
	assert.throws(() => cm.relocate(W.map, wins, [mc, mt], stock, other), cm.MapError);
	const bad = mc.slice();
	bad[W.map.self_refs[0].off] ^= 4;
	assert.throws(() => cm.relocate(W.map, wins, [bad, mt], [W.code_run, W.tables_run], stock), cm.MapError);
	assert.throws(() => cm.relocate(W.map, wins, [code, tables], stock, [W.code_run, W.tables_run + 2]), cm.MapError);
	assert.throws(() => cm.relocate(W.map, wins, [code, tables.subarray(1)], stock, stock), cm.MapError);
});

// A code window that names its ranges and holes, a table ref anchored to a window it points just below, every window
// moved as its own block and the windows after the code joined into one, 0xFF between them and in the holes (the
// "joined" vector, shared with rfoundry's test_codecmap.py)
test("anchored refs, holes and a joined block: relocate, blank and join give rfoundry's bytes", () => {
	const J = V.joined;
	const wins = J.windows.map(([a, d]) => [a, d.length / 2]), parts = J.windows.map(([, d]) => fromHex(d));
	const src = wins.map(([a]) => a), dst = J.layout;
	const out = cm.relocate(J.map, wins, parts, src, dst);
	assert.deepEqual(out.map(toHex), J.relocated);
	assert.deepEqual(cm.relocate(J.map, wins, out, dst, src).map(toHex), J.windows.map(([, d]) => d), "and back to stock");
	// the anchored value lies two bytes below its window and moves with it
	const ref = J.map.table_refs.find((r) => r.anchor !== undefined);
	assert.equal(cm.held(ref), ref.anchor);
	assert.equal(cm.held(J.map.table_refs[0]), J.map.table_refs[0].target);
	const w = src.indexOf(ref.anchor);
	assert.ok(w > 0 && ref.target < src[w], "the value itself lies in no window");
	assert.equal(u32le(out[0], ref.off), dst[w] - (ref.anchor - ref.target));
	// the holes: the code's in order, a table window's by its stock address
	assert.deepEqual(cm.holes(J.map), [[0x100, 0x80], [0x1C0, 0x10]]);
	const th = cm.tableHoles(J.map);
	assert.deepEqual([...th], [[src[3], [[0x20, 4]]]]);
	assert.deepEqual(cm.holes(V.map), [], "a map without the keys has no holes");
	assert.equal(cm.tableHoles(V.window.map).size, 0);
	const code = cm.blank(out[0], cm.holes(J.map));
	assert.equal(toHex(code), J.code_blanked);
	assert.equal(toHex(out[0]), J.relocated[0], "blank leaves its input as it was");
	const block = (pieces) => {
		const b = new Uint8Array(J.block.length / 2).fill(0xFF);
		pieces.forEach((d, i) => b.set(d, dst[i + 1] - J.block_at));
		return b;
	};
	assert.equal(toHex(block(out.slice(1))), J.block);
	assert.equal(toHex(block(out.slice(1).map((d, i) => cm.blank(d, th.get(src[i + 1]) || [])))), J.block_blanked);
	// what changed in the code: the listed sites, the holes when blanked, nothing else
	const sites = new Set([0x20, 0x24, 0x40, 0x50, 0x54, 0x58, 0x1E0, 0x1E8].flatMap((o) => [o, o + 1, o + 2, o + 3]));
	const inHole = (i) => cm.holes(J.map).some(([o, n]) => o <= i && i < o + n);
	code.forEach((b, i) => {
		if (inHole(i)) assert.equal(b, 0xFF, `hole +${i}`);
		else if (!sites.has(i)) assert.equal(b, parts[0][i], `+${i}`);
	});
	// a site planted for a layout holds what relocate then finds there (the simulated stock files are made this way)
	const planted = parts.map((d) => d.slice());
	for (const [win, off, r, have] of cm.layoutSites(J.map, wins, src, src)) {
		planted[win].fill(0, off, off + 4);
		cm.plantSite(planted[win], off, r, have);
	}
	assert.deepEqual(planted.map(toHex), J.windows.map(([, d]) => d));
	// a value that lies in no window without its anchor: refused, as the loader before the key was
	const bare = { ...J.map, table_refs: J.map.table_refs.map(({ anchor, ...r }) => r) };
	assert.throws(() => cm.relocate(bare, wins, parts, src, dst), /lies in no codec window/);
});
