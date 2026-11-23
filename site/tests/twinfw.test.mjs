// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stock container that holds its image twice (static/js/flasher/twinfw.js) against rfoundry's twinfw.py, on the
// synthetic files site/tests/fixtures.py makes with it: the same key from the two copies, the same answer of the
// header test, the same words for every refusal, and the kind of problem the page takes each one for. Then the
// placement that joins a codec's windows (stockfw.js) and the two files of this container that are named and refused.
// The profile is found by its container, never by a radio's name. Every file is synthetic: no stock byte, no key byte.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

import { fromHex, toHex, sha256, u32le } from "../static/js/flasher/bytes.js";
import * as twinfw from "../static/js/flasher/twinfw.js";
import * as stockfw from "../static/js/flasher/stockfw.js";
import * as cm from "../static/js/flasher/codecmap.js";
import { identifyFile, openVendorFile } from "../static/js/flasher/vendorfile.js";
import { pickProblem } from "../static/js/flasher/explain.js";

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const fx = json("fixtures.json");
const fb = json("flash-boards.json");
const T = fx.twin;
// without a profile in this container the fixtures have nothing for these tests
const opts = { skip: T ? false : "no stock profile names the twin container" };
const doc = T && fx.layouts.files.find((f) => f.file === T.file);
const prof = doc?.profile, stock = T && bin(T.file), key = T && fromHex(T.key);
const profiles = T && { [T.stock_sha256]: prof };

// A file with a change fixtures.py made the same way: cut to a length, zero bytes appended, bytes XORed, words set
function changed(data, { cut = null, append = 0, flips = [], words = [] } = {}) {
	const head = cut === null ? data : data.subarray(0, cut);
	const b = new Uint8Array(head.length + append);
	b.set(head);
	for (const [at, mask] of flips) b[at] ^= mask;
	for (const [at, value] of words) new DataView(b.buffer).setUint32(at, value, true);
	return b;
}
const thrown = async (f) => {
	try {
		await f();
	} catch (e) {
		return e;
	}
	assert.fail("no error");
};

test("the container: parsed as rfoundry parses it", opts, () => {
	assert.deepEqual(twinfw.parse(stock), T.parse);
	const [off, length, n] = T.parse;
	assert.deepEqual([off, length, stock.length], [twinfw.HEADER_LEN, 2 * n + 16, prof.size]);
	assert.deepEqual([prof.payload, prof.image[1]], [[off, length], n]);
	assert.deepEqual(twinfw.parse(changed(stock, { append: 14 })), T.parse, "16 bytes after the payload still parse");
});

test("the key comes from the two copies, as rfoundry derives it, whatever the distance between them", opts, async () => {
	const got = await twinfw.deriveKey(stock, prof.key_sha256);
	assert.equal(toHex(got), T.derived_key);
	assert.equal(toHex(got), T.key, "and it is the key the file was made with");
	assert.equal(got.length, twinfw.KEY_LEN);
	assert.ok(T.keys.some((k) => k.step % 2) && T.keys.some((k) => k.step === 0), "an odd step and step 0 among the fixtures");
	for (const k of T.keys) assert.equal(toHex(await twinfw.deriveKey(bin(k.file), k.key_sha256)), k.derived_key, k.what);
	// a vote that ties: the same byte wins as in rfoundry, so the same key comes out, and the other one never does
	const tie = T.keys.find((k) => k.file === "twin-key-tie.bin");
	assert.ok(tie, "a tied vote among the fixtures");
	const other = fromHex(tie.derived_key).map((b, c) => (c % 4 === 0 ? b ^ 1 : b));
	const e = await thrown(async () => twinfw.deriveKey(bin(tie.file), await sha256(other)));
	assert.equal(e.message, "could not derive the file's key");
});

test("the header test: a firmware file, cut or padded too, a bootloader file, and nothing else", opts, () => {
	assert.ok(T.kinds.some((k) => k.kind === "firmware" && k.cut) && T.kinds.some((k) => k.kind === "boot") && T.kinds.some((k) => k.container === stockfw.OUTSECURITY)
		&& T.kinds.filter((k) => k.container === null).length >= 3);
	for (const k of T.kinds) {
		const data = changed(bin(k.file), k);
		assert.equal(twinfw.headerKind(data), k.kind, k.what);
		assert.equal(stockfw.containerKind(data, fb), k.container, k.what);
		assert.equal(stockfw.bootFile(data, fb), k.boot, k.what);
	}
	assert.equal(twinfw.headerKind(new Uint8Array(0)), null);
});

// What the page takes each refusal for (stockfw.js StockError code): a file that is cut or whose copies differ is
// "damaged", one whose header is another's "not-vendor"; every check of a file that was identified by its SHA-256
// before is the flasher's own ("internal")
const CODES = { "too short for a firmware file": "damaged", "not a firmware file in this container": "not-vendor",
	"the file's two copies differ": "damaged", "the file is too short to derive the key from": "damaged" };

test("every refusal in rfoundry's words, with the kind of problem it is", opts, async () => {
	const calls = new Set();
	for (const r of T.refusals) {
		const data = changed(r.file ? bin(r.file) : stock, r.change);
		const e = await thrown(async () => {
			if (r.call === "parse") return twinfw.parse(data);
			if (r.call === "derive_key") return twinfw.deriveKey(data, r.want ?? prof.key_sha256);
			return twinfw.carve(data, { ...prof, ...(r.profile || {}) }, r.wins || prof.windows, r.key ? fromHex(r.key) : key, fb.codec_sig, r.map || prof.map);
		});
		assert.ok(e instanceof stockfw.StockError, `${r.what}: ${e}`);
		assert.equal(e.message, r.message, r.what);
		assert.equal(e.code, CODES[r.message] || "internal", r.what);
		calls.add(r.call);
	}
	assert.deepEqual([...calls].sort(), ["carve", "derive_key", "parse"]);
	// and on the way every merge takes: the profile's map is the one the carve checks the code's ranges against
	const ranges = T.refusals.filter((r) => r.map);
	assert.ok(ranges.length >= 2);
	for (const r of ranges) {
		const e = await thrown(async () => stockfw.extract(stock, null, fb, { [T.stock_sha256]: { ...prof, map: r.map } }, prof.board));
		assert.equal(e.message, r.message, `extract: ${r.what}`);
	}
	for (const text of ["two copies differ in a codec window", "decrypted window", "length words", "zero words", "size ", "image length", "payload ", "outside the image", "the code's range at"])
		assert.ok(T.refusals.some((r) => r.message.includes(text)), text);
});

test("carve: every window from both copies, as the profile has them", opts, async () => {
	const segs = await twinfw.carve(stock, prof, prof.windows, key, fb.codec_sig, prof.map);
	assert.deepEqual(segs.map(([a, d]) => [a, d.length]), prof.windows.map(([a, n]) => [a, n]));
	for (const [i, [, d]] of segs.entries()) assert.equal(await sha256(d), prof.windows[i][2]);
	assert.equal(u32le(segs[0][1], 0), fb.codec_sig);
	// a code whose first word is not the codec's: refused after its hash matched (the profile says another)
	const e = await thrown(async () => twinfw.carve(stock, prof, [[prof.windows[1][0], prof.windows[1][1], prof.windows[1][2]]], key, fb.codec_sig));
	assert.match(e.message, /^codec first word is not 0x[0-9A-F]{8}$/);
});

test("extract: the file's own key, whatever key the caller has; placed as one code window and one joined block", opts, async () => {
	const want = doc.placed;
	for (const k of [null, new Uint8Array(fb.key_len), key]) {
		const { profile, segments } = await stockfw.extract(stock, k, fb, profiles, prof.board);
		assert.equal(profile, prof);
		assert.deepEqual(await Promise.all(segments.map(async ([a, d]) => [a, await sha256(d), d.length])), want);
	}
	const { segments } = await stockfw.extract(stock, null, fb, profiles);
	const pl = prof.placement;
	assert.ok(pl.join && prof.windows.length > fb.descriptor.windows + 1, "more windows than a descriptor names: joined");
	assert.deepEqual(segments.map(([a, d]) => [a, d.length]), [[pl.code, prof.windows[0][1]], [pl.tables, pl.tables_len], [fb.descriptor.addr, fb.descriptor.len]]);
	assert.deepEqual(stockfw.placedWindows(prof).map(([a, n]) => [a, n]), segments.slice(0, 2).map(([a, d]) => [a, d.length]));
	assert.deepEqual(stockfw.carve(prof), prof.windows.map(([a, n]) => [a, n]));
	const [code, block] = [segments[0][1], segments[1][1]];
	// every site holds its placed value: a word, a movw/movt pair, a bl from an island into the moved code
	const wins = stockfw.carve(prof), src = wins.map(([a]) => a), dst = [pl.code, pl.tables, ...pl.extra];
	const sites = cm.layoutSites(prof.map, wins, src, dst);
	assert.ok(sites.some(([, , r]) => r.kind === "bl") && sites.some(([, , r]) => r.kind === "movw_movt") && sites.some(([w, , r]) => w > 0 && r.kind === "word"));
	for (const [w, off, r, , to] of sites) {
		const [buf, base] = w === 0 ? [code, 0] : [block, dst[w] - pl.tables];
		if (r.kind === "bl") assert.equal(cm.blTarget(buf, base + off, to[0]), to[1]);
		else if (r.kind === "movw_movt") assert.equal(cm.pairValue(buf, { off: base + off, off2: base + off + r.off2 - r.off }), to);
		else assert.equal(u32le(buf, base + off), to);
	}
	// a value anchored to a window it lies just below moved with that window
	const refs = prof.map.table_refs.filter((r) => r.anchor !== undefined);
	assert.ok(refs.length > 0);
	for (const r of refs) assert.equal(u32le(code, r.off), dst[src.indexOf(r.anchor)] - (r.anchor - r.target));
	// the map's holes are 0xFF in the code and in the block, and so is the block between its windows
	const holes = cm.holes(prof.map), th = cm.tableHoles(prof.map);
	assert.ok(holes.length > 0 && th.size > 0);
	for (const [o, n] of holes) assert.ok(code.subarray(o, o + n).every((b) => b === 0xFF), `code hole +${o}`);
	const used = new Uint8Array(block.length);
	wins.slice(1).forEach(([a, n], i) => {
		const at = dst[i + 1] - pl.tables;
		used.fill(1, at, at + n);
		for (const [o, len] of th.get(a) || []) assert.ok(block.subarray(at + o, at + o + len).every((b) => b === 0xFF), `table hole +${o}`);
	});
	const gaps = [...used.keys()].filter((i) => !used[i]);
	assert.ok(gaps.length > 0 && gaps.every((i) => block[i] === 0xFF), "0xFF between the windows of the block");
	// the descriptor names the block as its one table window
	const d = segments[2][1], w = (i) => u32le(d, 4 * i);
	assert.deepEqual([w(0), w(1), w(2), w(3), w(4), w(6), w(7), w(8)], [fb.descriptor.magic, fb.descriptor.version, pl.tag, pl.code, code.length, 1, pl.tables, pl.tables_len]);
	assert.deepEqual(Array.from({ length: 21 }, (_, i) => w(10 + i)), new Array(21).fill(0));
});

test("a copy is checked by its placed SHA-256s and never taken back to stock; a readout holds it either way", opts, async () => {
	const { segments } = await stockfw.extract(stock, null, fb, profiles);
	const form = stockfw.forms(prof)[0], parts = segments.slice(0, 2);
	assert.equal(await stockfw.checkForm(prof, form, parts, fb), true);
	assert.equal(await stockfw.unplace(prof, form, parts, fb), null, "a blanked copy has lost its holes");
	const pl = prof.placement, th = cm.tableHoles(prof.map), src = prof.windows.map(([a]) => a), dst = [pl.code, pl.tables, ...pl.extra];
	const [hole] = [...th].map(([a, list]) => dst[src.indexOf(a)] - pl.tables + list[0][0]);
	const gap = dst[2] - pl.tables - 1;
	assert.ok(dst[2] > dst[1] + prof.windows[1][1], "a gap before the second window of the block");
	for (const [what, i, at] of [["the code", 0, 0x40], ["a hole of the code", 0, cm.holes(prof.map)[0][0]], ["the block", 1, 8], ["a gap of the block", 1, gap],
		["a hole of a table window", 1, hole]]) {
		const bad = parts.map(([a, d]) => [a, d.slice()]);
		bad[i][1][at] ^= 0x01;
		assert.equal(await stockfw.checkForm(prof, form, bad, fb), false, what);
	}
	// a radio that runs RadioFoundry holds the placed codec with its descriptor; one on its original firmware the
	// stock windows, which are placed as a merge places them
	const base = fb.boards[prof.board].rom_base, fbX = { ...fb, profiles };
	const flash = new Uint8Array(0x100000).fill(0xFF);
	for (const [a, d] of segments) flash.set(d, a - base);
	assert.deepEqual((await stockfw.fromFlash(flash, fbX, profiles, base)).segments, segments);
	const plain = await twinfw.carve(stock, prof, prof.windows, key, fb.codec_sig, prof.map);
	const orig = new Uint8Array(0x100000).fill(0xFF);
	for (const [a, d] of plain) orig.set(d, a - base);
	assert.deepEqual((await stockfw.fromFlash(orig, fbX, profiles, base)).segments, segments);
	flash[pl.code - base + 0x80] ^= 1;
	assert.equal(await stockfw.fromFlash(flash, fbX, profiles, base), null);
	// and it is no candidate for the search of a codec another firmware moved (its tables are no single block)
	assert.equal(await stockfw.fromMoved(orig, fbX, profiles, base), null);
});

test("a placement or a map that does not hold together is refused in rfoundry's words", opts, async () => {
	const plain = await twinfw.carve(stock, prof, prof.windows, key, fb.codec_sig, prof.map);
	assert.ok(T.joins.length >= 7);
	for (const j of T.joins) {
		const p = { ...prof, placement: { ...prof.placement, ...j.placement }, ...(j.map ? { map: j.map } : {}) };
		const e = await thrown(async () => stockfw.placedParts(p, plain, fb));
		assert.ok(e instanceof stockfw.StockError && e.code === "internal", `${j.what}: ${e}`);
		assert.equal(e.message, j.message, j.what);
	}
	// a window not moved by a multiple of 4, a site that does not hold its stock value: the map's own refusals
	const extra = prof.placement.extra;
	const odd = { ...prof, placement: { ...prof.placement, extra: [...extra.slice(0, -1), extra.at(-1) + 2], tables_len: prof.placement.tables_len + 2 } };
	assert.match((await thrown(async () => stockfw.placedParts(odd, plain, fb))).message, /^can't move the codec: /);
	const ref = prof.map.table_refs.find((r) => (r.kind || "word") === "word");
	const stale = plain.map(([a, d]) => [a, d.slice()]);
	stale[0][1][ref.off] ^= 4;
	assert.match((await thrown(async () => stockfw.placedParts(prof, stale, fb))).message, /^can't move the codec: .*doesn't hold/);
	// the placed bytes are held to the profile's pins
	const pinned = { ...prof, placed: { ...prof.placed, tables: [0, "00".repeat(32)] } };
	assert.match((await thrown(async () => stockfw.place(pinned, plain, fb))).message, /doesn't match the profile/);
	// without the map, nothing is moved
	const noMap = { ...prof, map: null, windows: [[prof.windows[0][0], prof.windows[0][1], "00".repeat(32)], ...prof.windows.slice(1)] };
	const e = await thrown(async () => stockfw.placedParts(noMap, plain, fb));
	assert.match(e.message, /^no codec map for /);
	assert.equal(e.code, "unmapped");
});

test("the named files that give no codec: a version nobody has mapped, and a bootloader file", opts, async () => {
	const u = T.unmapped;
	assert.ok(u, "a file of this container that is known and not mapped");
	assert.ok(Object.keys(fb.unmapped_files).length > 0, "data/flash-boards.json carries the unmapped files");
	for (const f of Object.values(fb.unmapped_files)) assert.ok(fb.boards[f.board] && f.name && f.version, JSON.stringify(f));
	const data = changed(stock, u.change);
	assert.equal(await sha256(data), u.sha256);
	const fbU = { ...fb, unmapped_files: { [u.sha256]: u.entry } };
	const e = await thrown(async () => stockfw.extract(data, null, fbU, fb.profiles, u.entry.board));
	assert.equal(e.message, u.message);
	assert.deepEqual([e.code, e.params.known], ["unmapped", u.entry.name]);
	// it names the version and the files to use instead: the board's own
	assert.ok(e.message.includes(u.entry.version));
	for (const p of Object.values(fb.profiles).filter((x) => x.board === u.entry.board)) assert.ok(e.message.includes(p.name), p.name);
	assert.equal(stockfw.unmapped(u.entry, fb), u.message);
	// without its entry the same bytes are an unknown stock file, in the words that always were
	const plainErr = await thrown(async () => stockfw.extract(data, null, fb, fb.profiles, u.entry.board));
	assert.match(plainErr.message, /^unknown stock file, not a supported codec source\. Supported: /);
	// the bootloader file: its own sentence, by its header (no hash is known for it), with or without a board
	const boot = bin(T.boot.file);
	for (const id of [null, prof.board]) {
		const b = await thrown(async () => stockfw.extract(boot, null, fb, fb.profiles, id));
		assert.equal(b.message, T.boot.message);
		assert.equal(b.code, "not-vendor");
	}
	// a file with a profile is never judged by its header
	assert.equal((await stockfw.extract(stock, null, fb, profiles)).profile, prof);
});

// A firmware file of this container that no profile names is still the maker's (twinfw.headerKind): one cut short or
// padded on its way is "damaged" (download it again), a whole one a version without a map for the container's radios.
// Never "not the manufacturer's file", which tells the user to look for another project's build
test("a cut, padded or unknown firmware file of this container: damaged or unmapped, never not-vendor", opts, async () => {
	const twins = [...new Set(Object.values(fb.profiles).filter((p) => p.container === stockfw.TWIN).map((p) => p.board))];
	assert.ok(twins.includes(prof.board));
	const cases = [
		["cut to 1 MB (an interrupted download)", changed(stock, { cut: 1 << 20 }), "damaged"],
		["4 KB appended", changed(stock, { append: 4096 }), "damaged"],
		["its last payload byte changed (another version)", changed(stock, { flips: [[stock.length - 3, 0x5A]] }), "unmapped"],
	];
	for (const [what, data, problem] of cases) {
		assert.equal(twinfw.headerKind(data), "firmware", what);
		const m = await identifyFile(data, fb, prof.board);
		assert.deepEqual([m.board, m.problem], [null, problem], `${what}: ${JSON.stringify(m.why)}`);
		assert.deepEqual(m.boards, twins, what);
		assert.ok(!/RadioFoundry knows/.test(m.why), what);
		const opened = await openVendorFile("M.bin", data, fb, prof.board);
		assert.equal(opened.best, null, what);
		assert.equal(pickProblem(opened, prof.board).problem, problem, what);
		assert.equal(pickProblem(opened, prof.board, { need: "key" }).problem, problem, what);
	}
	// the file as it is, with its profile: the codec, as always
	assert.equal((await identifyFile(stock, { ...fb, profiles: { ...fb.profiles, ...profiles } }, prof.board)).kind, "codec");
	// a bootloader file of the container keeps its own sentence
	assert.equal((await identifyFile(bin(T.boot.file), fb, prof.board)).problem, "not-vendor");
});
