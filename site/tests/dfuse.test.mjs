// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A radio installed through its processor's own ROM bootloader by plain DfuSe (dfu.js runPlain, dfuse.js), against
// the simulated loader (fake.js FakeRomRadio): the device check, the read protection probe, the confirmed Read
// Unprotect, the plain write and the leave; the backend's contract; and the write step drawn against a small stand-in
// for the DOM. The board is the registry fixture's first one of the family, found by what it is, not by name.
//
// The route itself is tested on that board without a codec route (site/tests/fixtures.py rom_board_without_codec, as
// rfoundry's own tests build it), so these tests don't depend on which radios have one. The codec step of the route
// (the plan's three modes, the keep settled by what the loader says, an install without the codec) is tested on the
// board as the registry has it, and skipped where it has no codec route.
// Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

// ---- just enough DOM for dom.js h(), the progress bar and the log box
class TextNode {
	constructor(text) { this.text = text; }
	get textContent() { return this.text; }
}
class El {
	constructor(tag) {
		this.tagName = tag.toUpperCase();
		this.children = [];
		this.attrs = {};
		this.listeners = {};
		this.className = "";
		this.dataset = {};
		this.style = {};
		this.isConnected = false;
	}
	append(...cs) { for (const c of cs) this.children.push(c instanceof El || c instanceof TextNode ? c : new TextNode(String(c))); }
	replaceChildren(...cs) { this.children = []; this.append(...cs); }
	remove() {}
	get childNodes() { return [...this.children]; }
	setAttribute(k, v) { this.attrs[k] = v; }
	getAttribute(k) { return this.attrs[k] ?? null; }
	addEventListener(ev, f) { (this.listeners[ev] ||= []).push(f); }
	// the handlers' promises, so a test can wait for a click's work
	fire(ev) { return Promise.all((this.listeners[ev] || []).map((f) => f({ preventDefault() {}, currentTarget: this }))); }
	click() { return this.disabled ? Promise.resolve([]) : this.fire("click"); }
	get textContent() { return this.children.map((c) => c.textContent).join(""); }
	set textContent(v) { this.children = [new TextNode(String(v))]; }
}
const strings = readFileSync(fileURLToPath(new URL("../i18n/en.json", import.meta.url)), "utf8");
globalThis.Node = El;
globalThis.document = { createElement: (t) => new El(t), createElementNS: (ns, t) => new El(t), body: new El("body"),
	getElementById: (id) => (id === "i18n" ? { textContent: strings } : null) };
const en = JSON.parse(strings);
const all = (el) => [el, ...el.children.filter((c) => c instanceof El).flatMap(all)];

const { Dfu, DfuError, WrongDeviceError, WebUsbTransport, romDevice, checkPlain, interfaceStringIndexes, PLAIN_FAMILY, FLASH_WORD, KEY_LEN } = await import("../static/js/flasher/dfu.js");
const { makePlan, codecSectors, KEEP, MERGE, BLANK, BLOCK, FIRST_BLOCK } = await import("../static/js/flasher/plan.js");
const { decideCodec } = await import("../static/js/flasher/decide.js");
const stockfw = await import("../static/js/flasher/stockfw.js");
const vendorfile = await import("../static/js/flasher/vendorfile.js");
const { pickProblem, supportedFiles } = await import("../static/js/flasher/explain.js");
const { forDevBuild, segments, verify } = await import("../static/js/flasher/manifest.js");
const { FakeRadio, FakeRomRadio, romSimulation, romLayout, simulation, randomBytes, rng } = await import("../static/js/flasher/fake.js");
const { Link, LinkError, probe, CMD, frame, parseFrames, restartOutcome, restartRequest, RESTART_WHY } = await import("../static/js/flasher/link.js");
const { ProtectedError } = await import("../static/js/flasher/protect.js");
const { backendOf } = await import("../static/js/flasher/backends.js");
const { hex, fromHex, toHex, sha256, u32le } = await import("../static/js/flasher/bytes.js");
const { supportReport } = await import("../static/js/support.js");
const dfuse = await import("../static/js/flasher/dfuse.js");
const { stopBars } = await import("../static/js/flasher/dom.js");

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const bin = (n) => new Uint8Array(readFileSync(join(DIR, n)));
const fbReal = json("flash-boards.json");
const fx = json("fixtures.json");
const radios = json("radios.json").radios;
// the first radio of the family as the registry has it, and a keyed one to tell it from
const realRadio = radios.find((r) => fbReal.boards[r.flash.board]?.family === PLAIN_FAMILY);
const keyed = radios.find((r) => fbReal.boards[r.flash.board]?.family === "usb-dfu");
assert.ok(realRadio, "a radio of the plain DfuSe family in the fixtures");
assert.ok(keyed, "a keyed USB DFU radio in the fixtures");
const realBoard = fbReal.boards[realRadio.flash.board];
const ROM = fx.rom[realBoard.id];
assert.ok(ROM, "the board's plans from rfoundry in the fixtures");
// the same radio without a codec route: its board as data/flash-boards.json would carry it, its radio entry with the
// reason it has none. The tests of the route itself run on these
const board = ROM.plain.board;
const radio = { ...realRadio, flash: { ...realRadio.flash, codec_none: ROM.plain.codec_none, stock_download: null } };
const fb = { ...fbReal, boards: { ...fbReal.boards, [board.id]: board } };
// the codec step's tests: on the board as the registry has it, where it has a codec route
const withRoute = { skip: ROM.codec && !realRadio.flash.codec_none ? false : "the fixture's radio of this family has no codec route" };
const noWait = { wait: async () => {} };

// An image with a vector table: a RAM stack pointer and a reset vector, then firmware-like bytes. 150 KB: two sectors
function testImage(len = 150 * 1024 + 20) {
	const img = randomBytes(rng(7), len);
	img.set([0xFC, 0xFF, 0x01, 0x20, 0x01, 0x02, 0x00, 0x08], 0);
	return img;
}
// A simulated radio of the board without a codec route (real: as the registry has it), an image and its plan
async function installable(state = "stock", opts = {}, { real = false } = {}) {
	const sim = await romSimulation(real ? fbReal : fb, board.id);
	const fake = new FakeRomRadio(sim, { state, spiSize: 0x200000, ...opts });
	const image = testImage();
	const manifest = await forDevBuild(sim.board, image);
	return { sim, fake, image, manifest, plan: dfuse.installPlan(sim.board, { manifest, image }) };
}

test("the family's board: a ROM loader at the flash base, no key, nothing protected; without a codec route no codec window", () => {
	for (const b of [realBoard, board]) {
		assert.equal(b.app_base, b.rom_base, "the application starts at the flash base");
		assert.equal(b.key_sha256, null);
		assert.deepEqual(b.protected, []);
		assert.equal(b.sectors[0].addr, b.rom_base);
		assert.equal(b.sectors.reduce((n, s) => n + s.size, 0), b.flash_end - b.rom_base);
	}
	assert.equal(realRadio.flash.backend, "dfuse");
	assert.ok(realRadio.flash.bootloader.length && realRadio.flash.notice);
	// every radio says whether it has a codec route: the key is there, null or the reason it has none
	for (const r of radios) assert.ok(r.flash.codec_none === null || (typeof r.flash.codec_none === "string" && r.flash.codec_none), r.id);
	// the board without one: no window, no source, nothing to keep, and its region reaches where a codec route ends
	assert.deepEqual([board.codec.id, board.codec.windows, board.codec.sources, board.keep_codec_supported], [null, [], [], false]);
	assert.equal(typeof radio.flash.codec_none, "string");
	assert.ok(board.app_region[1] >= realBoard.app_region[1]);
	// with one, as the registry has it: its codec sectors are one run that starts where the application region ends
	// and ends with the descriptor, below any second bank; the image never shares a sector with the codec
	if (!withRoute.skip) {
		const secs = codecSectors(realBoard), [lo, hi] = realBoard.app_region;
		assert.ok(secs.length > 0 && realBoard.keep_codec_supported);
		assert.equal(secs[0].addr, hi, "the codec's flash follows the application's");
		secs.forEach((s, i) => assert.equal(s.addr, hi + secs.slice(0, i).reduce((n, x) => n + x.size, 0), "one run"));
		assert.equal(secs.at(-1).addr + secs.at(-1).size, fbReal.descriptor.addr + fbReal.descriptor.len);
		for (const [a, n] of realBoard.codec.windows) assert.ok(a >= hi && a + n <= fbReal.descriptor.addr + fbReal.descriptor.len && a >= lo);
		assert.ok(realBoard.codec.sources.length > 0 && realBoard.codec.id);
	}
});

test("backend: install is radio, build, write, done; an update and a reinstall skip what they know", () => {
	const b = backendOf(radio);
	assert.equal(b.id, "dfuse");
	// a radio without a codec route, and a caller that names no radio
	for (const ctx of [{ radio }, {}]) {
		assert.deepEqual(b.steps("install", { ...ctx, recovering: false }), ["radio", "build", "write", "done"]);
		assert.deepEqual(b.steps("install", { ...ctx, recovering: true }), ["build", "write", "done"]);
		assert.deepEqual(b.steps("update", ctx), ["update", "write", "done"]);
	}
	// with a codec route the shared codec step comes before the write in each
	const routed = { ...radio, flash: { ...radio.flash, codec_none: null } };
	assert.deepEqual(b.steps("install", { recovering: false, radio: routed }), ["radio", "build", "codec", "write", "done"]);
	assert.deepEqual(b.steps("install", { recovering: true, radio: routed }), ["build", "codec", "write", "done"]);
	assert.deepEqual(b.steps("update", { radio: routed }), ["update", "codec", "write", "done"]);
	assert.equal(dfuse.codecRoute(routed), true);
	assert.equal(dfuse.codecRoute(radio), false);
	assert.equal(dfuse.codecRoute(null), false);
	// no "current firmware" step: a radio not known to run RadioFoundry is on its original firmware
	assert.equal(b.installNow, "stock");
	assert.ok(!b.steps("install", { recovering: false, radio: routed }).includes("now"));
	for (const task of ["backup", "restore", "restart", "help"]) assert.equal(b.steps(task, {}), null, task);
	for (const s of ["codec", "write", "done", "backup", "restore"]) assert.equal(typeof b.views[s], "function", s);
	assert.ok(b.available(radio, fb, "install"));
	assert.equal(b.available(radio, fb, "backup"), radio.backup.rfoundry === "full");
	// a board without the loader's USB IDs can't be reached; a keyed radio is another backend's
	assert.ok(!b.available(radio, { boards: { [board.id]: { ...board, dfu_ids: null } } }, "install"));
	assert.notEqual(backendOf(keyed).id, "dfuse");
	// the original firmware is the maker's to put back: words, never a file or a backup; a backup restores no firmware
	assert.equal(b.recovery.first, "updatemode");
	assert.equal(b.recovery.stock, "maker");
	assert.equal(typeof b.recovery.maker, "function");
	assert.deepEqual(b.restores.targets, ["codeplug"]);
	// and no channel step: the original firmware's codeplug is in no layout the page converts
	assert.equal(b.stockChannels, false);
	assert.notEqual(backendOf(keyed).stockChannels, false);
	for (const k of [b.recovery.stockText, b.recovery.calm, b.untested, b.restores.note]) assert.ok(en[k], k);
});

test("the device check: this board's whole flash on alternate 0, never a keyed bootloader or another chip's layout", async () => {
	const ok = { string1: "STMicroelectronics", interfaces: [romLayout(board), "@Option Bytes  (simulated)"] };
	romDevice(board, ok);
	assert.throws(() => romDevice(board, { ...ok, string1: "AnyRoad Technology" }), WrongDeviceError);
	// a keyed radio's bootloader as the page sees it (the simulated one), offered to this board
	const other = new FakeRadio(await simulation(fb, keyed.flash.board), { state: "bootloader" });
	const theirs = await other.dfuTransport().fingerprint();
	assert.throws(() => romDevice(board, theirs), WrongDeviceError);
	// an application area that starts above a bootloader, as the keyed radios' chips show it
	for (const layout of ["@Internal Flash  /0x0800C000/01*016Kg,01*064Kg,07*128Kg", "@Internal Flash  /0x08000000/04*016Kg,01*064Kg,07*128Kg",
		"@Option Bytes  (simulated)", "", null])
		assert.throws(() => romDevice(board, { ...ok, interfaces: [layout] }), WrongDeviceError, String(layout));
	assert.throws(() => romDevice(board, { string1: null, interfaces: [] }), WrongDeviceError);
	assert.throws(() => romDevice(board, null), WrongDeviceError);
	// a layout declared for the board (read on a radio) is the only one taken, spaces aside
	const declared = { ...board, dfu_layout: "@Internal Flash   /0x08000000/8*128Kg,8*128Kg" };
	romDevice(declared, { ...ok, interfaces: ["@Internal Flash /0x08000000/8*128Kg,8*128Kg"] });
	assert.throws(() => romDevice(declared, ok), /not this radio's/);
	const e = (() => { try { romDevice(board, { ...ok, interfaces: ["x"] }); } catch (err) { return err; } })();
	assert.ok(e instanceof DfuError && e.what === "x" && /nothing was sent/.test(e.message));
});

test("a plain install on an unprotected radio: only the image's sectors, every block as sent, leave at the application", async () => {
	const { sim, fake, image, plan } = await installable("blank");
	fake.enterBootloader();
	const dfu = new Dfu(fake.dfuTransport(), noWait);
	assert.equal(await dfu.probeProtected(sim.board), false);
	const seen = [];
	await dfu.run(plan, null, (d, n) => seen.push([d, n]));
	// no command of a keyed bootloader, no mass erase, nothing over unerased bytes, no partial flash word
	assert.deepEqual(fake.vendor, []);
	assert.deepEqual(fake.violations, []);
	const covered = sim.board.sectors.filter((s) => s.addr < sim.board.app_base + image.length);
	assert.equal(covered.length, 2, "a 150 KB image covers two 128 KB sectors");
	assert.deepEqual(fake.erased, covered.map((s) => s.addr));
	// the image as it is (nothing XORed), 0xFF to the end of its last block and beyond
	assert.deepEqual(fake.flash.subarray(0, image.length), image);
	assert.ok(fake.flash.subarray(image.length).every((x) => x === 0xFF));
	// each block after its own Set Address Pointer, as block 2: its address is the pointer under either reading of
	// wTransferSize; whole blocks, so whole flash words. In the plan's order, the vector table's block last
	assert.equal(fake.writes.length, Math.ceil(image.length / BLOCK));
	assert.equal(BLOCK % FLASH_WORD, 0);
	const order = plan.writeOrder.flatMap((st) => st.blocks.map(([n]) => st.sector.addr + (n - FIRST_BLOCK) * BLOCK));
	assert.deepEqual(fake.writes.map(([addr]) => addr), [...order.filter((a) => a !== sim.board.app_base), sim.board.app_base]);
	fake.writes.forEach(([, len, num]) => {
		assert.equal(num, FIRST_BLOCK);
		assert.equal(len, BLOCK);
	});
	assert.deepEqual(seen.at(-1), [plan.totalBlocks, plan.totalBlocks]);
	assert.equal(fake.leftAt, sim.board.app_base);
	assert.ok(fake.exited);
	// the application starts and answers: no codec on this board, the chip family's third value, level 0
	assert.equal(fake.mode, "normal");
	const p = await probe(new Link(fake.serialStream()));
	assert.equal(p.codec, "none");
	assert.equal(p.chip.family, 2);
	assert.equal(p.chip.rdp, 0);
	assert.equal(p.chip.flashKb, (sim.board.flash_end - sim.board.rom_base) / 1024);
	assert.ok(sim.board.debuglink_names.includes(p.board));
	assert.match(supportReport({ chip: p.chip, version: {}, support: null }, []), /^mcu\.family: STM32H7$/m);
});

test("a read protected radio: the probe says so and changes nothing; a write attempt erases nothing", async () => {
	const { sim, fake, plan } = await installable("stock");
	fake.enterBootloader();
	const before = fake.flash.slice();
	const dfu = new Dfu(fake.dfuTransport(), noWait);
	assert.equal(await dfu.probeProtected(sim.board), true);
	assert.equal(await dfu.probeProtected(sim.board), true, "asked twice: the loader is left idle");
	assert.deepEqual(fake.flash, before);
	assert.deepEqual(fake.erased, []);
	assert.equal(fake.unprotects, 0);
	// the page never runs the write on a protected radio; if it did, the loader refuses the first erase
	await assert.rejects(dfu.run(plan, null), /erase 0x[0-9A-F]+ failed: status 11/);
	assert.deepEqual(fake.erased, []);
	assert.deepEqual(fake.writes, []);
	assert.deepEqual(fake.flash, before);
});

test("Read Unprotect: everything erased, the loader gone; entered again it is unprotected and takes the install", async () => {
	const { sim, fake, image, plan } = await installable("stock");
	fake.enterBootloader();
	const tr = fake.dfuTransport();
	const { dfu, stage } = await dfuse.checkLoader(tr, sim.board, noWait);
	assert.equal(stage, "protected");
	await dfu.readUnprotect(sim.board);
	assert.equal(fake.unprotects, 1);
	assert.ok(fake.flash.every((x) => x === 0xFF), "the whole program memory is erased");
	assert.equal(fake.mode, "off");
	await assert.rejects(dfu.getStatus(), /lost/);
	// nothing but the unprotect went: no erase command, no write, no vendor command
	assert.deepEqual([fake.erased, fake.writes, fake.vendor, fake.violations], [[], [], [], []]);
	fake.enterBootloader();
	const again = await dfuse.checkLoader(fake.dfuTransport(), sim.board, noWait);
	assert.equal(again.stage, "ready");
	await again.dfu.run(plan, null);
	assert.deepEqual(fake.flash.subarray(0, image.length), image);
	assert.deepEqual(fake.violations, []);
	assert.equal(fake.running, "rfoundry");
});

test("Read Unprotect is refused for a board with a protected region in its program memory, before anything is sent", async () => {
	const { sim, fake } = await installable("stock");
	sim.board.protect = [{ mem: "progmem", start: 0x1E0000, end: 0x200000, what: "factory data (a test)" }];
	fake.enterBootloader();
	const before = fake.flash.slice();
	const dfu = new Dfu(fake.dfuTransport(), noWait);
	await assert.rejects(dfu.readUnprotect(sim.board), ProtectedError);
	assert.equal(fake.unprotects, 0);
	assert.equal(fake.rdp, 1);
	assert.deepEqual(fake.flash, before);
});

test("a probe without a clear answer is no answer: thrown, and nothing follows it", async () => {
	// a loader that refuses the read with another status than "protected"
	let state = 2, status = 0;
	const sent = [];
	const tr = {
		async out(req, value, data) {
			sent.push([req, value, data ? [...data] : []]);
			if (req === 4 || req === 6) { state = 2; status = 0; } else state = 5;
			return 0;
		},
		async in(req, length) {
			if (req === 3) return new Uint8Array([status, 0, 0, 0, state, 0]);
			state = 10;
			status = 0x01;
			throw new DfuError("USB transfer stall");
		},
	};
	await assert.rejects(new Dfu(tr, noWait).probeProtected(board), /no clear answer.*status 1.*nothing was written/);
	assert.ok(!sent.some(([req, value, data]) => req === 1 && (value !== 0 || data[0] !== 0x21)), "only the address pointer was set");
	// a loader that goes away mid-probe: the USB error, not a verdict
	const dead = { async out() { return 0; }, async in() { throw new DfuError("USB transfer timeout: no reply to request 3 in 10 s"); } };
	await assert.rejects(new Dfu(dead, noWait).probeProtected(board), /USB transfer timeout/);
});

test("protected is concluded only from dfuERROR with errVENDOR, whether the loader stalls the read or answers no bytes", async () => {
	for (const stallUpload of [true, false]) {
		const { sim, fake } = await installable("stock", { stallUpload });
		fake.enterBootloader();
		const before = fake.flash.slice();
		assert.equal(await new Dfu(fake.dfuTransport(), noWait).probeProtected(sim.board), true, `stall ${stallUpload}`);
		assert.deepEqual(fake.flash, before);
		assert.equal(fake.state, 2, "left in dfuIDLE");
	}
	// the vendor status without the error state, and the error state with no status: neither is "protected". The
	// transport plays the probe's GETSTATUS replies in order, then answers the read's with the case
	for (const [status, state] of [[0x0B, 2], [0x0B, 9], [0, 10]]) {
		const replies = [2, 4, 5, 2];	// dfuIDLE; the Set Address Pointer: dfuDNBUSY, dfuDNLOAD-IDLE; dfuIDLE after the ABORT
		const sent = [];
		const tr = {
			async out(req, value, data) { sent.push([req, value, data ? [...data] : []]); return 0; },
			async in(req) {
				if (req !== 3) return new Uint8Array(0);	// the read: no bytes
				return replies.length ? new Uint8Array([0, 0, 0, 0, replies.shift(), 0]) : new Uint8Array([status, 0, 0, 0, state, 0]);
			},
		};
		await assert.rejects(new Dfu(tr, noWait).probeProtected(board), new RegExp(`no clear answer \\(0 bytes, status ${status}, `), `status ${status}, state ${state}`);
		assert.deepEqual(sent, [[1, 0, [0x21, ...[0, 8, 16, 24].map((sh) => (board.app_base >>> sh) & 0xFF)]]], "only the address pointer was set");
	}
});

// The 4 bytes the probe reads are the image's first: the vector table's stack pointer, or 0xFF where there is none.
// rfoundry's Dfu.probe_protected and start_blank
test("the probe reads at the application base and keeps what it read: a vector table, or a blank start", async () => {
	for (const [state, blank] of [["rfoundry", false], ["blank", true]]) {
		const { sim, fake } = await installable(state);
		fake.enterBootloader();
		const dfu = new Dfu(fake.dfuTransport(), noWait);
		assert.equal(dfu.start, null);
		assert.equal(dfu.startBlank, false, "nothing read yet is not a blank start");
		assert.equal(await dfu.probeProtected(sim.board), false);
		assert.deepEqual([...dfu.start], [...fake.flash.subarray(0, 4)]);
		assert.equal(dfu.startBlank, blank, state);
		const found = await dfuse.checkLoader(fake.dfuTransport(), sim.board, noWait);
		assert.deepEqual([found.stage, found.blank], ["ready", blank], state);
	}
	// a protected radio gives no bytes: nothing is kept, and it is not taken for blank
	const locked = await installable("stock");
	locked.fake.enterBootloader();
	const dfu = new Dfu(locked.fake.dfuTransport(), noWait);
	assert.equal(await dfu.probeProtected(locked.sim.board), true);
	assert.deepEqual([dfu.start, dfu.startBlank], [null, false]);
	assert.deepEqual(await dfuse.checkLoader(locked.fake.dfuTransport(), locked.sim.board, noWait).then((f) => [f.stage, f.blank]), ["protected", false]);
	// a probe that follows one that read forgets what that one read
	const again = await installable("blank");
	again.fake.enterBootloader();
	const d2 = new Dfu(again.fake.dfuTransport(), noWait);
	await d2.probeProtected(again.sim.board);
	assert.equal(d2.startBlank, true);
	again.fake.rdp = 1;
	assert.equal(await d2.probeProtected(again.sim.board), true);
	assert.equal(d2.startBlank, false);
	// a board whose image starts above the start of its program flash: the read is at the image's start, where the
	// vector table is, not at another program's bytes below it
	const up = await installable("blank");
	const second = up.sim.board.sectors[1];
	up.sim.board.app_base = second.addr;
	up.fake.flash.set([0x00, 0x00, 0x02, 0x20], 0);	// something at the flash base
	up.fake.enterBootloader();
	const sent = [], tr = up.fake.dfuTransport(), out = tr.out;
	tr.out = (req, value, data) => { if (req === 1 && data?.[0] === 0x21) sent.push(u32le(data, 1)); return out(req, value, data); };
	const d3 = new Dfu(tr, noWait);
	assert.equal(await d3.probeProtected(up.sim.board), false);
	assert.deepEqual(sent, [second.addr]);
	assert.equal(d3.startBlank, true, "blank where the image starts, whatever lies below");
});

test("the application region: a plan that reaches flash past it is refused before any request, and before any unprotect is offered", async () => {
	const { sim, fake } = await installable("stock");
	const b = sim.board, half = b.sectors[b.sectors.length / 2];
	// the region the board's build keeps its image in: the first half of the sectors (a dual-bank chip's first bank)
	b.app_region = [b.app_base, half.addr];
	const fits = testImage(half.addr - b.app_base), over = testImage(half.addr - b.app_base + BLOCK);
	const planOf = async (img) => makePlan(b, segments(await forDevBuild(b, img), img), KEEP, [], b.codec.windows, { vectorLast: true });
	checkPlain(await planOf(fits));
	assert.equal((await planOf(fits)).erase.at(-1).index, half.index - 1);
	// the manifest check and the planner take the longer image (it is inside the chip's flash): the route doesn't
	const manifest = await forDevBuild(b, over);
	assert.equal((await verify(manifest, over, sim.fb, b.id)).id, b.id);
	const plan = await planOf(over);
	assert.equal(plan.erase.at(-1).index, half.index);
	const refusal = new RegExp(`reaches sector ${half.index} .*outside .* application flash`);
	assert.throws(() => checkPlain(plan), refusal);
	assert.throws(() => dfuse.installPlan(b, { manifest, image: over }), refusal);
	fake.rdp = 0;
	fake.enterBootloader();
	const before = fake.flash.slice();
	await assert.rejects(new Dfu(fake.dfuTransport(), noWait).run(plan, null), refusal);
	assert.deepEqual([fake.erased, fake.writes], [[], []]);
	assert.deepEqual(fake.flash, before);
	// the write step: the refusal in place of the loader's steps, no Connect, so no probe and no Read Unprotect
	const locked = await installable("stock");
	locked.sim.board.app_region = b.app_region;
	const { got } = page(locked.fake, locked.sim, over, await forDevBuild(locked.sim.board, over));
	const box = dfuse.writeView();
	assert.match(text(box), refusal);
	assert.equal(got.forward, null);
	assert.ok(!text(box).includes(en["flash.write.then_connect"]));
	assert.equal(locked.fake.unprotects, 0);
	// a board whose data has no region yet is held to its program flash alone
	delete b.app_region;
	checkPlain(await planOf(over));
});

// The board as data/flash-boards.json carries it (site/rfoundrysite/repo.py flash_boards(), here the fixture), with
// no region put on it by the test: rfoundry's check_plain rule holds on the real page. An own image one block past the
// region is refused when the write step is drawn, before any Connect, so before the probe and any Read Unprotect; one
// that fills the region to its last byte is taken
test("the exported board's application region: an image past it is refused before any Read Unprotect is offered", async () => {
	assert.ok(Array.isArray(board.app_region), "data/flash-boards.json carries the board's app_region");
	const [lo, hi] = board.app_region;
	assert.equal(lo, board.app_base);
	assert.ok(hi > lo && hi <= board.flash_end);
	assert.ok(board.sectors.some((s) => s.addr === hi) || hi === board.flash_end, "the region ends on a sector boundary");
	const { sim, fake } = await installable("stock");
	assert.deepEqual(sim.board.app_region, board.app_region, "the simulation's board is the exported one");
	assert.ok(Array.isArray(realBoard.app_region) && realBoard.app_region[0] === realBoard.app_base, "and the registry's own board carries one too");
	const fits = testImage(hi - lo), over = testImage(hi - lo + BLOCK);
	const fitsPlan = dfuse.installPlan(sim.board, { manifest: await forDevBuild(sim.board, fits), image: fits });
	assert.equal(fitsPlan.erase.at(-1).addr + fitsPlan.erase.at(-1).size, hi);
	const overMan = await forDevBuild(sim.board, over);
	// past the region: checkPlain's refusal, or the planner's where the region is the whole program flash
	assert.throws(() => dfuse.installPlan(sim.board, { manifest: overMan, image: over }), /application flash/);
	fake.enterBootloader();
	const before = fake.flash.slice();
	const { got } = page(fake, sim, over, overMan);
	const box = dfuse.writeView();
	assert.match(text(box), /application flash/);
	assert.equal(got.forward, null, "no Connect: nothing is asked of the loader");
	assert.ok(!text(box).includes(en["flash.write.then_connect"]));
	assert.deepEqual([fake.unprotects, fake.erased, fake.writes], [0, [], []]);
	assert.deepEqual(fake.flash, before);
});

test("every block is read back before the start; one that didn't take, or can't be read, leaves the image unstarted", async () => {
	// a loader that takes every block: one read per block, each block 2 after its own pointer, then the start
	let { sim, fake, image, plan } = await installable("blank");
	fake.enterBootloader();
	const reads = [], tr = fake.dfuTransport(), wire = tr.in;
	tr.in = (req, len, value) => {
		if (req === 2) reads.push([len, value]);
		return wire(req, len, value);
	};
	await new Dfu(tr, noWait).run(plan, null);
	assert.equal(reads.length, plan.totalBlocks);
	assert.ok(reads.every(([len, value]) => len === BLOCK && value === FIRST_BLOCK), JSON.stringify(reads.slice(0, 2)));
	assert.ok(fake.exited);
	assert.deepEqual(fake.violations, []);
	// a sector that takes its blocks with no error and programs nothing (write protected, AN3156 5.1)
	({ sim, fake, image, plan } = await installable("blank"));
	fake.enterBootloader();
	const last = plan.writes.at(-1).sector, execute = fake.execute.bind(fake);
	fake.execute = (c) => {
		if (c.kind === "write" && fake.addr >= last.addr && fake.addr < last.addr + last.size) { fake.state = 5; return; }	// dfuDNLOAD-IDLE
		return execute(c);
	};
	await assert.rejects(new Dfu(fake.dfuTransport(), noWait).run(plan, null), new RegExp(`${hex(last.addr)} does not read back.*not started`));
	assert.equal(fake.exited, false);
	assert.equal(fake.leftAt, null);
	// a loader that stalls the read: not checked is not taken as checked
	({ sim, fake, image, plan } = await installable("blank"));
	fake.enterBootloader();
	const stalls = fake.dfuTransport();
	stalls.in = (req, len, value) => (req === 2 ? Promise.reject(new Error("USB transfer stall")) : fake.dfuIn(req, len, value));
	await assert.rejects(new Dfu(stalls, noWait).run(plan, null), /could not be read back.*not started/);
	assert.equal(fake.exited, false);
	// written, only not started, and the vector table not written: nothing to start at a normal power-on
	assert.deepEqual(fake.flash.subarray(BLOCK, image.length), image.subarray(BLOCK), "written, only not started");
	assert.ok(fake.flash.subarray(0, BLOCK).every((x) => x === 0xFF), "no vector table");
});

test("a read-back reply of the wrong length is no check: refused, nothing started, no vector table", async () => {
	for (const [cut, what] of [[16, "1008 of 1024"], [-1, "1025 of 1024"]]) {
		const { fake, plan } = await installable("blank");
		fake.enterBootloader();
		const tr = fake.dfuTransport(), wire = tr.in;
		let uploads = 0;
		tr.in = async (req, len, value) => {
			const r = await wire(req, len, value);
			if (req !== 2 || ++uploads !== 3) return r;
			// a correct prefix, or one byte too many
			return cut > 0 ? r.subarray(0, r.length - cut) : new Uint8Array([...r, 0xFF]);
		};
		await assert.rejects(new Dfu(tr, noWait).run(plan, null), new RegExp(`does not read back.*\\(${what} bytes read\\).*not started`));
		assert.equal(fake.exited, false);
		assert.equal(fake.leftAt, null);
		assert.ok(fake.flash.subarray(0, BLOCK).every((x) => x === 0xFF), "no vector table");
	}
});

test("the vector table last: its sector erased first, its first block written after every other block is read back", async () => {
	const { sim, fake, image, plan } = await installable("blank");
	const base = sim.board.app_base;
	assert.ok(plan.writes.length > 1);
	assert.equal(plan.eraseOrder[0].addr, base);
	assert.equal(plan.writeOrder.at(-1).sector.addr, base);
	assert.ok(plan.describe().includes(`order: sector ${plan.eraseOrder[0].index} erased first and written last (a cut install leaves no application to start)`));
	fake.enterBootloader();
	const events = [], tr = fake.dfuTransport(), out = tr.out, wire = tr.in;
	tr.out = (req, value, data) => {
		if (req === 1 && value === FIRST_BLOCK) events.push(["write", fake.addr]);
		return out(req, value, data);
	};
	tr.in = (req, len, value) => {
		if (req === 2) events.push(["read", fake.addr]);
		return wire(req, len, value);
	};
	await new Dfu(tr, noWait).run(plan, null);
	assert.equal(fake.erased[0], base);
	const at = events.findIndex(([k, a]) => k === "write" && a === base);
	assert.equal(at, events.length - 2, "the vector table's block written last");
	assert.deepEqual(events.at(-1), ["read", base]);
	assert.equal(events.slice(0, at).filter(([k]) => k === "read").length, plan.totalBlocks - 1, "everything else read back first");
	assert.deepEqual(fake.flash.subarray(0, image.length), image);
	assert.ok(fake.exited);
	// the cable goes after any number of blocks before the last: no vector table, nothing started
	for (const n of [0, 1, Math.floor(plan.totalBlocks / 2), plan.totalBlocks - 1]) {
		const r = await installable("blank");
		r.fake.enterBootloader();
		const t2 = r.fake.dfuTransport(), o2 = t2.out;
		let blocks = 0;
		t2.out = (req, value, data) => {
			if (req === 1 && value === FIRST_BLOCK && blocks++ === n) {
				r.fake.mode = "off";
				return Promise.reject(new DfuError("USB transfer timeout: no reply to request 1 in 10 s"));
			}
			return o2(req, value, data);
		};
		// cut at the vector table's own block: the erase is tried and can't reach the radio, and the words don't claim
		// a radio that starts nothing
		const atVectors = n === plan.totalBlocks - 1;
		await assert.rejects(new Dfu(t2, noWait).run(r.plan, null), (e) => {
			assert.ok(e instanceof DfuError && /^USB transfer/.test(e.message), e.message);
			assert.equal(/could not be erased again/.test(e.message), atVectors, e.message);
			assert.equal(/starts nothing/.test(e.message), false, e.message);
			return true;
		});
		assert.ok(r.fake.flash.subarray(0, BLOCK).every((x) => x === 0xFF), `cut after ${n} blocks: no vector table`);
		assert.equal(r.fake.exited, false);
	}
	const blank = (f) => f.fake.flash.subarray(0, plan.eraseOrder[0].size).every((x) => x === 0xFF);
	// the vector table itself does not read back: its sector is erased again
	const v = await installable("blank");
	v.fake.enterBootloader();
	const execute = v.fake.execute.bind(v.fake);
	v.fake.execute = (c) => execute(c.kind === "write" && v.fake.addr === base ? { ...c, data: new Uint8Array(c.data.length) } : c);
	await assert.rejects(new Dfu(v.fake.dfuTransport(), noWait).run(v.plan, null), /does not read back.*\. The vector table's sector is erased again/);
	assert.deepEqual(v.fake.erased.filter((a) => a === base).length, 2);
	assert.ok(blank(v));
	assert.equal(v.fake.exited, false);
	// the vector table's write reported as failed with part of it in flash (the documents describe no such error; the
	// page is still connected, so nothing unchecked is left to start). The loader is in dfuERROR then, where it takes
	// no ABORT: the erase goes after CLRSTATUS
	const f = await installable("blank");
	f.fake.enterBootloader();
	const whole = f.fake.execute.bind(f.fake);
	f.fake.execute = (c) => {
		if (c.kind !== "write" || f.fake.addr !== base) return whole(c);
		whole({ ...c, data: c.data.slice(0, c.data.length / 2) });
		f.fake.fail(0x06);	// errPROG
	};
	await assert.rejects(new Dfu(f.fake.dfuTransport(), noWait).run(f.plan, null),
		new RegExp(`: block at ${hex(base)} failed: status 6, state dfuERROR\\. The vector table's sector is erased again.*install again$`));
	assert.deepEqual(f.fake.erased.filter((a) => a === base).length, 2);
	assert.ok(blank(f));
	assert.deepEqual([f.fake.exited, f.fake.violations], [false, []]);
	// the vector table's read refused: a stalled UPLOAD leaves dfuERROR too
	const u = await installable("blank");
	u.fake.enterBootloader();
	const ut = u.fake.dfuTransport(), uin = ut.in;
	ut.in = (req, len, value) => {
		if (req !== 2 || u.fake.addr !== base) return uin(req, len, value);
		u.fake.fail(0x0F);
		return Promise.reject(new DfuError("USB transfer stall"));
	};
	await assert.rejects(new Dfu(ut, noWait).run(u.plan, null),
		new RegExp(`: the block at ${hex(base)} could not be read back \\(USB transfer stall\\).*\\. The vector table's sector is erased again`));
	assert.deepEqual(u.fake.erased.filter((a) => a === base).length, 2);
	assert.ok(blank(u));
	assert.deepEqual([u.fake.exited, u.fake.violations], [false, []]);
	// an error that is not the page's own at the vector table's block (the browser's, when a transfer fails): the
	// erase is tried all the same, and the error says where it happened and what is left to start. gone: the radio
	// left with it, so the erase can't reach it
	for (const gone of [false, true]) {
		const x = await installable("blank");
		x.fake.enterBootloader();
		const xt = x.fake.dfuTransport(), xout = xt.out;
		let sent = 0;
		xt.out = (req, value, data) => {
			if (req !== 1 || value !== FIRST_BLOCK || ++sent !== x.plan.totalBlocks) return xout(req, value, data);
			if (gone) x.fake.mode = "off";
			return Promise.reject(Object.assign(new Error("A transfer error has occurred"), { name: "NetworkError" }));
		};
		await assert.rejects(new Dfu(xt, noWait).run(x.plan, null), (e) => {
			assert.ok(e instanceof DfuError, `${gone}: an error of the page's own, with what it leaves`);
			assert.match(e.message, /^USB error at the vector table: A transfer error has occurred\. .*install again$/, String(gone));
			assert.equal(/\. The vector table's sector is erased again/.test(e.message), !gone, e.message);
			assert.equal(/could not be erased again .*may start an image whose vector table did not pass its check/.test(e.message), gone, e.message);
			return true;
		});
		assert.equal(x.fake.erased.filter((a) => a === base).length, gone ? 1 : 2, String(gone));
		// the failed transfer never reached the loader, so the table's block is blank either way; erased, its sector is
		assert.ok(x.fake.flash.subarray(0, BLOCK).every((b) => b === 0xFF) && blank(x) === !gone, String(gone));
		assert.deepEqual([x.fake.exited, x.fake.violations], [false, []], String(gone));
	}
	// a plain plan without that order is refused before anything is sent
	const w = await installable("blank");
	w.fake.enterBootloader();
	const unordered = makePlan(w.sim.board, segments(w.manifest, w.image), KEEP);
	await assert.rejects(new Dfu(w.fake.dfuTransport(), noWait).run(unordered, null), /first and write it last/);
	assert.deepEqual([w.fake.erased, w.fake.writes], [[], []]);
});

// As rfoundry's Dfu.to_idle, request for request: a ROM loader stalls a CLRSTATUS outside dfuERROR and an ABORT in it
test("toIdle: CLRSTATUS only out of dfuERROR, ABORT out of a transfer, and a busy loader is waited for", async () => {
	const CLRSTATUS = 4, ABORT = 6;
	for (const [state, want] of [[10, [CLRSTATUS]], [5, [ABORT]], [9, [ABORT]], [4, []], [2, []]]) {
		const replies = [[0, state, 7], [0, 2, 0]], sent = [], naps = [];
		const tr = {
			async out(req) { sent.push(req); },
			async in() {
				const [status, st, poll] = replies.shift();
				return new Uint8Array([status, poll, 0, 0, st, 0]);
			},
		};
		await new Dfu(tr, { wait: async (ms) => { naps.push(ms); } }).toIdle();
		assert.deepEqual([sent, naps], [want, state === 4 ? [7] : []], `from state ${state}`);
	}
});

test("Read Unprotect answered with anything but dfuDNBUSY is refused (AN3156 5.4), whatever the status", async () => {
	// a reply: [status, state]; null: the reply is lost, as when the loader leaves the bus at once
	const loader = (answer, sent) => {
		const replies = [[0, 2]];	// dfuIDLE for toIdle(); then the case, for the GETSTATUS that runs the command
		return {
			async out(req, value, data) {
				sent.push([req, value, data ? [...data] : []]);
				if (req === 1 && data?.[0] === 0x92) replies.push(answer);
				return 0;
			},
			async in(req) {
				assert.equal(req, 3);
				const r = replies.length ? replies.shift() : [0, 2];
				if (!r) throw new DfuError("USB transfer timeout: no reply to request 3 in 10 s");
				return new Uint8Array([r[0], 0, 0, 0, r[1], 0]);
			},
		};
	};
	for (const [status, state] of [[0, 2], [0, 5], [0x0F, 10], [0x0A, 4]]) {
		const sent = [];
		await assert.rejects(new Dfu(loader([status, state], sent), noWait).readUnprotect(board), /read unprotect was refused/, `status ${status}, state ${state}`);
		assert.deepEqual(sent.filter(([req]) => req === 1), [[1, 0, [0x92]]], "only the command");
	}
	// the reply lost: taken as the command taken (rfoundry's read_unprotect too), never a failure that would send the
	// user to switch the radio off while it may be erasing
	const sent = [];
	await new Dfu(loader(null, sent), noWait).readUnprotect(board);
	assert.deepEqual(sent.filter(([req]) => req === 1), [[1, 0, [0x92]]], "only the command");
	// and the loader's own: a Read Unprotect it takes leaves the bus, which the transport says
	const { sim, fake } = await installable("stock");
	fake.enterBootloader();
	const tr = fake.dfuTransport();
	let left = 0;
	const stop = tr.watchLeave(() => left++);
	await new Dfu(tr, noWait).readUnprotect(sim.board);
	stop();
	assert.deepEqual([left, fake.unprotects], [1, 1]);
});

test("the two routes never cross: a keyed board needs its key, a plain one takes none, each loader refuses the other's write", async () => {
	const { sim, fake, plan } = await installable("blank");
	fake.enterBootloader();
	// a key offered to the plain route is a mistake somewhere: refused before anything is sent
	await assert.rejects(new Dfu(fake.dfuTransport(), noWait).run(plan, new Uint8Array(KEY_LEN)), /takes no key/);
	// a board of the family that has a key is a keyed board
	const withKey = { ...plan, board: { ...sim.board, key_sha256: "00" }, erase: plan.erase, writes: plan.writes };
	await assert.rejects(new Dfu(fake.dfuTransport(), noWait).run(withKey, null), /no bootloader key/);
	// a keyed radio's plan against the ROM loader: its first vendor command is not a command there
	const ks = await simulation(fb, keyed.flash.board);
	const kimg = testImage(40 * 1024);
	const kplan = makePlan(ks.board, segments(await forDevBuild(ks.board, kimg), kimg), KEEP);
	await assert.rejects(new Dfu(fake.dfuTransport(), noWait).run(kplan, ks.key));
	assert.deepEqual(fake.vendor, [0x01]);
	assert.deepEqual([fake.erased, fake.writes], [[], []]);
	// and the plain plan against a keyed bootloader: no vendor command went first, so its erase is refused
	const kradio = new FakeRadio(ks, { state: "bootloader" });
	const before = kradio.flash.slice();
	await assert.rejects(new Dfu(kradio.dfuTransport(), noWait).run(plan, null));
	assert.deepEqual(kradio.erased, []);
	assert.deepEqual(kradio.flash, before);
});

test("the plain route sends whole blocks only, stays inside the program flash, and checks the board's protected regions again", async () => {
	const { sim, fake, plan } = await installable("blank");
	fake.enterBootloader();
	// (a plan of one sector: one that erased a second sector of the application and wrote nothing there is refused for that)
	const one = [plan.writes[0].sector];
	const short = { board: sim.board, erase: one, eraseOrder: one, writes: [{ sector: one[0], blocks: [[FIRST_BLOCK, new Uint8Array(1000)]] }],
		writeOrder: [], totalBlocks: 1 };
	await assert.rejects(new Dfu(fake.dfuTransport(), noWait).run(short, null), /is 1000 bytes, not 1024/);
	// a block numbered past its sector's end, or below the first, would set the pointer outside the sector
	const sec = plan.writes[0].sector;
	for (const num of [FIRST_BLOCK + sec.size / BLOCK, FIRST_BLOCK - 1, 0]) {
		const far = { ...short, writes: [{ sector: sec, blocks: [[num, new Uint8Array(BLOCK)]] }] };
		await assert.rejects(new Dfu(fake.dfuTransport(), noWait).run(far, null), /is outside sector/, String(num));
	}
	// a sector outside the program flash (the loader would take an address of its option bytes or its RAM as readily)
	for (const addr of [sim.board.flash_end, sim.board.rom_base - 0x20000, 0x5200201C, 0x20000000]) {
		const out = { index: 99, addr, size: 0x20000 };
		const stray = { board: sim.board, erase: [out], eraseOrder: [out], writes: [], writeOrder: [], totalBlocks: 0 };
		await assert.rejects(new Dfu(fake.dfuTransport(), noWait).run(stray, null), /outside .* program flash/);
		const strayWrite = { ...stray, erase: [], eraseOrder: [], writes: [{ sector: out, blocks: [[FIRST_BLOCK, new Uint8Array(BLOCK)]] }] };
		await assert.rejects(new Dfu(fake.dfuTransport(), noWait).run(strayWrite, null), /outside .* program flash/);
	}
	const guarded = { ...sim.board, protect: [{ mem: "progmem", start: 0, end: 0x100, what: "a test region" }] };
	const p2 = { board: guarded, erase: plan.erase, eraseOrder: plan.eraseOrder, writes: plan.writes, writeOrder: plan.writeOrder, totalBlocks: plan.totalBlocks };
	await assert.rejects(new Dfu(fake.dfuTransport(), noWait).run(p2, null), ProtectedError);
	assert.deepEqual([fake.erased, fake.writes], [[], []]);
	// the application base, where Leave sends the loader, outside the program flash
	for (const app_base of [sim.board.flash_end, sim.board.rom_base - 4]) {
		const away = { board: { ...sim.board, app_base }, erase: plan.erase, eraseOrder: plan.eraseOrder, writes: plan.writes, writeOrder: plan.writeOrder, totalBlocks: plan.totalBlocks };
		await assert.rejects(new Dfu(fake.dfuTransport(), noWait).run(away, null), /application base .* outside its program flash/, hex(app_base));
	}
	assert.deepEqual([fake.erased, fake.writes], [[], []]);
});

test("an image for this board verifies and plans with no codec; one that leaves the flash is refused", async () => {
	const { sim, image, manifest, plan } = await installable("blank");
	assert.equal((await verify(manifest, image, sim.fb, sim.board.id)).id, sim.board.id);
	assert.equal(plan.kept.length, 0);
	assert.ok(!plan.describe().some((l) => /never erased or written/.test(l)), "no bootloader range and no codec sector to keep");
	const big = new Uint8Array(sim.board.flash_end - sim.board.app_base + BLOCK);
	assert.throws(() => makePlan(sim.board, [[sim.board.app_base, big]], KEEP), /outside the application flash/);
});

test("WebUSB: a loader the browser gave no interface names for is identified by the names read from the device", async () => {
	// A configuration descriptor with one interface and two alternate settings whose names are strings 4 and 5, a DFU
	// functional descriptor between them and the end (as a ROM loader's has)
	const config = Uint8Array.from([9, 2, 43, 0, 1, 1, 0, 0xC0, 50,
		9, 4, 0, 0, 0, 0xFE, 1, 2, 4,
		9, 4, 0, 1, 0, 0xFE, 1, 2, 5,
		9, 0x21, 0x0B, 0xFF, 0, 0, 4, 0x1A, 1,
		7, 5, 0x81, 3, 8, 0, 10]);
	assert.deepEqual(interfaceStringIndexes(config), [4, 5]);
	assert.deepEqual(interfaceStringIndexes(config.subarray(0, 20)), [4], "a cut descriptor ends the walk");
	assert.deepEqual(interfaceStringIndexes(Uint8Array.from([9, 4, 0, 0, 0, 0xFE, 1, 2, 0])), [], "no string, no name");
	assert.deepEqual(interfaceStringIndexes(Uint8Array.from([0, 4, 0])), [], "a zero length does not loop");
	const strings = { 1: "STMicroelectronics", 4: romLayout(board), 5: "@Option Bytes  (simulated)" };
	const utf16 = (s) => { const b = new Uint8Array(2 + 2 * s.length); b[0] = b.length; b[1] = 3; for (let i = 0; i < s.length; i++) b[2 + 2 * i] = s.charCodeAt(i); return b; };
	const asked = [];
	const make = ({ named, stall = false }) => ({ configuration: { interfaces: [{ interfaceNumber: 0, alternates: [
		{ interfaceClass: 0xFE, interfaceSubclass: 0x01, interfaceName: named ? romLayout(board) : undefined },
		{ interfaceClass: 0xFE, interfaceSubclass: 0x01, interfaceName: named ? "@Option Bytes  (simulated)" : undefined }] }] },
		open: async () => {}, close: async () => {}, selectConfiguration: async () => {}, claimInterface: async () => {},
		selectAlternateInterface: async () => { throw new Error("an alternate setting was selected"); },
		controlTransferOut: async () => ({ status: "ok", bytesWritten: 0 }),
		controlTransferIn: async (setup) => {
			asked.push(setup.value);
			if (setup.requestType !== "standard" || setup.request !== 6) throw new Error("not a descriptor read");
			if (setup.value === 0x0200) { if (stall) throw new Error("stall"); return { status: "ok", data: new DataView(config.buffer) }; }
			const s = strings[setup.value & 0xFF];
			if (s === undefined) throw new Error("stall");
			return { status: "ok", data: new DataView(utf16(s).buffer) };
		} });
	// no names from the browser: read from the device, in order, and the check passes on alternate 0's layout
	const bare = await WebUsbTransport.open(make({ named: false }), board);
	const info = await bare.fingerprint();
	assert.deepEqual(info.interfaces, [romLayout(board), "@Option Bytes  (simulated)"]);
	romDevice(board, info);
	assert.ok(asked.includes(0x0200) && asked.includes(0x0304) && asked.includes(0x0305));
	// names from the browser: nothing more is read than before (string 1)
	asked.length = 0;
	const named = await WebUsbTransport.open(make({ named: true }), board);
	romDevice(board, await named.fingerprint());
	assert.deepEqual(asked, [0x0301]);
	// a device that answers neither way is still refused: nothing is assumed about what it would erase
	const mute = await WebUsbTransport.open(make({ named: false, stall: true }), board);
	const none = await mute.fingerprint();
	assert.deepEqual(none.interfaces, []);
	assert.throws(() => romDevice(board, none), WrongDeviceError);
});

test("WebUSB: the interface is claimed and no alternate setting is ever selected; the device check reads alternate 0", async () => {
	const ok = async () => ({ status: "ok", data: new DataView(new ArrayBuffer(6)), bytesWritten: 0 });
	const picked = [];
	const dev = { configuration: { interfaces: [{ interfaceNumber: 0, alternates: [
		{ interfaceClass: 0xFE, interfaceSubclass: 0x01, interfaceName: romLayout(board) },
		{ interfaceClass: 0xFE, interfaceSubclass: 0x01, interfaceName: "@Option Bytes  (simulated)" }] }] },
		open: async () => {}, close: async () => {}, selectConfiguration: ok, claimInterface: async () => {},
		selectAlternateInterface: async (intf, alt) => { picked.push([intf, alt]); }, controlTransferOut: ok, controlTransferIn: ok };
	const tr = await WebUsbTransport.open(dev, board);
	romDevice(board, await tr.fingerprint());
	await new Dfu(tr, noWait).getStatus();
	assert.deepEqual(picked, [], "the option bytes alternate is never selected: nothing is");
	// and no module of the route can: the call isn't in their source
	for (const f of ["dfu.js", "dfuse.js"]) {
		const src = readFileSync(fileURLToPath(new URL(`../static/js/flasher/${f}`, import.meta.url)), "utf8");
		assert.ok(!/selectAlternateInterface/.test(src), f);
	}
});

// ---- the write step, drawn

const text = (el) => el.textContent;
// extra.state: more of the page's state (the radio as the registry has it, the codec step's decision, a picked stock
// file, a saved copy of the codec); the rest of extra goes into the backend's context
function page(fake, sim, image, manifest, { state: more = {}, ...extra } = {}) {
	const got = { next: 0, started: [], finished: [], marks: [], forward: undefined, renders: 0, asked: 0, decisions: 0, plans: [] };
	const state = { radio, board: sim.board, task: "install", step: 3, firmware: { manifest, image, origin: "custom" }, fakeRadio: fake,
		forward: (b) => { got.forward = b; }, ...more };
	dfuse.init({ S: state, fb: () => sim.fb, next: () => { got.next++; }, render() { got.renders++; }, reset() {}, helpLink: () => null, manualKitLine: () => null,
		// the codec step's side of the page (app.js): its view, the question before an install without the codec (answer:
		// "go", "stay" or "back", which goes back as the dialog's button does), what the page has to write a codec from
		noteDecision() { got.decisions++; }, codecView: () => "codec",
		confirmNoCodec: async (back) => { got.asked++; if (got.answer === "back") await back(); return (got.answer ?? "go") === "go"; },
		stockGivesCodec: () => !!state.stock && stockfw.profileFor(sim.fb.profiles, state.stock.info?.sha, sim.board.id)?.board === sim.board.id,
		savedKept: () => !!state.savedCodec,
		flashStarted: (what) => { got.started.push(what); return { id: 1 }; }, flashFinished: (s, e = null) => { got.finished.push(e); },
		flashMark: (s, ev) => { got.marks.push(ev); }, errorText: (e) => e.message,
		connectBootloader: async () => {
			if (fake.mode !== "bootloader") throw new Error("the radio is not in update mode");
			return fake.dfuTransport();
		},
		dropSerial: async () => {}, withPicture: (x) => x, driverHelp: () => null, driverWorked() {}, ensureFirmware: async () => state.firmware,
		notePlan(p) { got.plans.push(p); },
		doneView: () => "done", backupView: () => "backup", restoreView: () => "restore", eraseWait: { seconds: 2, tickMs: 5 }, ...extra });
	return { state, got };
}
// The page's wait reads Date.now() on a setInterval: both are the test's own here, moved with clock.tick(ms), so no
// assertion depends on how fast the machine is. setTimeout stays real (the USB transfer timeout, dfu.js). A progress
// bar left moving holds its timer in dom.js, beyond its test: the bars are stopped before and after, since a timer of
// one test's clock, cleared under another's, takes one of that clock's own timers with it
const ownClock = (tc) => {
	stopBars();
	tc.mock.timers.enable({ apis: ["setInterval", "Date"] });
	tc.after(stopBars);
	return tc.mock.timers;
};

test("the write step on a protected radio: nothing is erased before the user confirms, then wait, enter again, install", async (tc) => {
	const clock = ownClock(tc);
	const { sim, fake, image, manifest } = await installable("stock");
	const { got } = page(fake, sim, image, manifest);
	const box = dfuse.writeView();
	box.isConnected = true;
	assert.equal(text(got.forward), en["flash.write.connect"]);
	for (const s of radio.flash.bootloader) assert.ok(text(box).includes(s), s);
	// the plan as rfoundry prints it for a board without a codec route: no codec mode named
	assert.ok(text(box).includes(`board ${sim.board.name}, no codec\n`) && !text(box).includes("codec mode"));
	// Connect before the radio is in its loader: said, and nothing moves on
	await got.forward.click();
	assert.ok(text(box).includes("not in update mode"));
	assert.equal(text(got.forward), en["flash.write.connect"]);
	fake.enterBootloader();
	const before = fake.flash.slice();
	await got.forward.click();
	// protected: what the erase takes, the precondition, the radio's own notice, and a button that waits for the tick
	for (const k of ["protected_title", "protected_text", "erase_warning", "precondition", "confirm"]) assert.ok(text(box).includes(en[`flash.dfuse.${k}`]), k);
	assert.ok(text(box).includes(radio.flash.notice));
	assert.equal(text(got.forward), en["flash.dfuse.unprotect_go"]);
	assert.ok(got.forward.className.includes("btn-danger"));
	assert.equal(got.forward.attrs.disabled, "", "disabled until the box is ticked");
	await got.forward.fire("click");	// a click that got through anyway
	assert.equal(fake.unprotects, 0);
	assert.deepEqual(fake.flash, before);
	assert.equal(got.started.length, 0);
	const agree = all(box).find((e) => e.tagName === "INPUT" && e.attrs.type === "checkbox");
	agree.checked = true;
	await agree.fire("change");
	assert.equal(got.forward.disabled, false);
	await got.forward.click();
	assert.equal(fake.unprotects, 1);
	assert.ok(fake.flash.every((x) => x === 0xFF));
	// the wait: no button, the bar counts; then the loader's steps again
	assert.equal(got.forward, null);
	assert.ok(text(box).includes(en["flash.dfuse.step_erasing"]));
	clock.tick(5);	// one tick: short of the least wait
	assert.equal(got.forward, null);
	clock.tick(5);
	assert.ok(text(box).includes(en["flash.dfuse.step_reenter"]) && text(box).includes(en["flash.dfuse.reenter"]));
	assert.equal(text(got.forward), en["flash.write.connect"]);
	await got.forward.click();	// the radio left the bus and wasn't switched on again
	assert.ok(text(box).includes("not in update mode"));
	fake.enterBootloader();
	await got.forward.click();
	assert.ok(text(box).includes(en["flash.dfuse.unprotected_ok"]));
	assert.equal(text(got.forward), en["flash.write.go"]);
	assert.equal(got.started.length, 0, "the flash log starts with the write, not with the erase");
	await got.forward.click();
	assert.equal(got.next, 1);
	assert.deepEqual(fake.flash.subarray(0, image.length), image);
	assert.deepEqual(fake.violations, []);
	assert.equal(got.started[0].task, "install");
	assert.equal(got.started[0].radio, sim.board.id);
	assert.deepEqual(got.finished, [null]);
	assert.ok(got.marks.includes("first_byte") && got.marks.includes("last_byte"));
});

// The wait follows the loader off the bus, as rfoundry's: "the erase is over" only once it left, and nothing says to
// switch the radio off before the least wait, nor before it left or the longest wait ran out
test("the erase wait: the radio is told to switch off only after it left USB, or with words that don't claim it did", async (tc) => {
	const clock = ownClock(tc);
	for (const how of ["leaves", "stays", "unseen"]) {
		const { sim, fake, image, manifest } = await installable("stock");
		sim.board.mass_erase_max_s = 45;	// not the fixture board's figure: the words must follow the board's data
		const { got } = page(fake, sim, image, manifest, {
			eraseWait: { seconds: 2, maxSeconds: 8, tickMs: 10 },
			connectBootloader: async () => {
				if (fake.mode !== "bootloader") throw new Error("the radio is not in update mode");
				const tr = fake.dfuTransport();
				if (how === "stays") tr.watchLeave = () => () => {};	// no disconnect comes
				if (how === "unseen") delete tr.watchLeave;		// nothing can tell
				return tr;
			} });
		const box = dfuse.writeView();
		box.isConnected = true;
		fake.enterBootloader();
		await got.forward.click();
		const agree = all(box).find((e) => e.tagName === "INPUT" && e.attrs.type === "checkbox");
		agree.checked = true;
		await agree.fire("change");
		await got.forward.click();
		assert.equal(fake.unprotects, 1, how);
		assert.ok(text(box).includes(en["flash.dfuse.step_erasing"]), how);
		// the words name the board's own least wait, twice its processor's mass erase time
		assert.ok(text(box).includes(en["flash.dfuse.erasing"].replace("{s}", "90")), how);
		clock.tick(15);	// 15 ms: short of the least wait (2 ticks of 10 ms)
		assert.equal(got.forward, null, `${how}: no button while it erases`);
		assert.ok(!text(box).includes(en["flash.dfuse.step_reenter"]), how);
		if (how === "leaves") {
			clock.tick(10);	// 25 ms: the least wait is over, and the loader was seen to leave
			assert.ok(text(box).includes(en["flash.dfuse.reenter"]), how);
		} else {
			clock.tick(55);	// 70 ms: past the least wait, short of the longest (8 ticks): still waiting, the bar says for what
			assert.ok(text(box).includes(en["flash.dfuse.step_erasing"]) && !text(box).includes(en["flash.dfuse.step_reenter"]), how);
			assert.ok(text(box).includes(en["flash.dfuse.leaving_bar"].split("{s}")[0]), how);
			clock.tick(10);	// 80 ms: the longest wait
			assert.ok(text(box).includes(en["flash.dfuse.step_reenter"]), how);
			assert.ok(!text(box).includes(en["flash.dfuse.reenter"]), `${how}: never "the erase is over"`);
			// with the board's own figures: the seconds waited, and the erase time that is longer than
			assert.ok(text(box).includes(en["flash.dfuse.reenter_unsure"].replace("{s}", "8").replace("{max}", "45")), how);
		}
		assert.equal(text(got.forward), en["flash.write.connect"], how);
	}
});

// rfoundry's own figures (site/tests/fixtures.py unprotect_waits, from cli.unprotect_wait): the two floors, the waits
// for a row of mass erase times, and the waits of every board of the family
test("rfoundry and the page wait the same after a Read Unprotect, by the processor's own mass erase time", async () => {
	const py = json("unprotect-wait.json");
	assert.deepEqual([dfuse.UNPROTECT_MIN_S, dfuse.UNPROTECT_WAIT_S], [py.min_s, py.wait_s]);
	assert.ok(py.bounds.length >= 4 && py.bounds.some(([, least]) => least > py.min_s), "a row that the floors don't decide");
	for (const [bound, least, most] of py.bounds) assert.deepEqual(dfuse.unprotectWait({ mass_erase_max_s: bound }), { least, most, bound });
	const family = Object.values(fb.boards).filter((b) => b.family === PLAIN_FAMILY);
	assert.deepEqual(family.map((b) => b.id).sort(), Object.keys(py.boards).sort());
	for (const b of family) {
		const w = dfuse.unprotectWait(b);
		assert.deepEqual([w.least, w.most], py.boards[b.id], b.id);
		assert.ok(b.mass_erase_max_s > 0 && w.bound === b.mass_erase_max_s && w.least >= 2 * w.bound && w.most >= 2 * w.least, b.id);
	}
	// a board whose data lost the figure: refused when the step is drawn, so no Read Unprotect is ever offered
	for (const none of [null, undefined, 0]) assert.throws(() => dfuse.unprotectWait({ ...board, mass_erase_max_s: none }), /no mass erase time/);
	const { sim, fake, image, manifest } = await installable("stock");
	delete sim.board.mass_erase_max_s;
	fake.enterBootloader();
	const { got } = page(fake, sim, image, manifest);
	const box = dfuse.writeView();
	assert.match(text(box), /no mass erase time/);
	assert.equal(got.forward, null, "no Connect");
	assert.equal(fake.unprotects, 0);
});

// Once the loader has answered the probe after a Read Unprotect, what the wait left open is settled: a write that
// fails then goes back to the plain steps into the loader, not to the wait's words and its seconds
test("after the probe has answered, a failed write does not repeat what the erase wait said", async (tc) => {
	const clock = ownClock(tc);
	for (const how of ["leaves", "stays"]) {
		const { sim, fake, image, manifest } = await installable("stock");
		const { got } = page(fake, sim, image, manifest, {
			connectBootloader: async () => {
				if (fake.mode !== "bootloader") throw new Error("the radio is not in update mode");
				const tr = fake.dfuTransport();
				if (how === "stays") tr.watchLeave = () => () => {};	// no disconnect comes
				return tr;
			} });
		const box = dfuse.writeView();
		box.isConnected = true;
		fake.enterBootloader();
		await got.forward.click();
		const agree = all(box).find((e) => e.tagName === "INPUT" && e.attrs.type === "checkbox");
		agree.checked = true;
		await agree.fire("change");
		await got.forward.click();
		clock.tick(how === "leaves" ? 10 : 30);	// the least wait (2 ticks of 5 ms), or the longest of practice mode (6)
		const said = how === "leaves" ? en["flash.dfuse.reenter"] : en["flash.dfuse.reenter_unsure"].split("{s}")[0];
		assert.ok(text(box).includes(en["flash.dfuse.step_reenter"]) && text(box).includes(said), how);
		// Connect before the radio is in its loader again: the probe has not answered, the words stay
		await got.forward.click();
		assert.ok(text(box).includes(en["flash.dfuse.step_reenter"]) && text(box).includes(said), how);
		fake.enterBootloader();
		await got.forward.click();
		assert.ok(text(box).includes(en["flash.dfuse.unprotected_ok"]), how);
		fake.mode = "off";	// the cable goes mid-write
		await got.forward.click();
		assert.ok(text(box).includes(en["flash.write.failed"].split("{error}")[0].trim()), how);
		assert.ok(text(box).includes(en["flash.write.bootloader_title"]), how);
		assert.ok(!text(box).includes(en["flash.dfuse.step_reenter"]) && !text(box).includes(said), how);
		assert.ok(!text(box).includes(en["flash.dfuse.reenter_unsure"].split("{s}")[0]) && !text(box).includes(en["flash.dfuse.reenter"]), how);
		assert.equal(text(got.forward), en["flash.write.connect"], how);
		// and the install goes on from there
		fake.enterBootloader();
		await got.forward.click();
		await got.forward.click();
		assert.equal(got.next, 1, how);
		assert.deepEqual(fake.flash.subarray(0, image.length), image, how);
	}
});

test("the write step on a radio that runs RadioFoundry: no unprotect step, and a failed write goes back to Connect", async () => {
	const { sim, fake, image, manifest } = await installable("rfoundry");
	const { got } = page(fake, sim, image, manifest);
	const box = dfuse.writeView();
	box.isConnected = true;
	fake.enterBootloader();
	await got.forward.click();
	assert.ok(text(box).includes(en["flash.dfuse.not_protected"]));
	assert.ok(!text(box).includes(en["flash.dfuse.protected_title"]));
	assert.equal(text(got.forward), en["flash.write.go"]);
	// the cable goes mid-write
	fake.mode = "off";
	await got.forward.click();
	assert.equal(got.next, 0);
	assert.ok(text(box).includes(en["flash.write.failed"].split("{error}")[0].trim()));
	assert.equal(text(got.forward), en["flash.write.connect"]);
	assert.ok(got.finished[0] instanceof Error);
	// again, from the loader
	fake.enterBootloader();
	await got.forward.click();
	await got.forward.click();
	assert.equal(got.next, 1);
	assert.deepEqual(fake.flash.subarray(0, image.length), image);
	assert.equal(fake.unprotects, 0);
});

// ---- the restart into the loader from RadioFoundry, over the debug link (docs/spec/debuglink.md 3.11)

// rfoundry's own request, kinds, words and gate (site/tests/fixtures.py restart_vectors, from rfoundry/debuglink.py)
test("the restart request and its replies are read as rfoundry reads them", async () => {
	const py = json("restart-vectors.json");
	assert.equal(CMD.REBOOT, py.cmd);
	assert.equal(toHex(restartRequest(py.token)), py.request);
	assert.equal(toHex(frame(CMD.REBOOT, py.seq, restartRequest(py.token))), py.frame);
	assert.ok(py.replies.length >= 15 && new Set(py.replies.map((r) => r[2])).size === 8, "every kind is in the vectors");
	for (const [status, data, kind] of py.replies) assert.equal(restartOutcome(status, fromHex(data)), kind, `${status} ${data}`);
	assert.deepEqual(RESTART_WHY, py.why);
	// the host's half of the gate: which chips are asked at all
	assert.ok(py.gate.some((g) => g[2]) && py.gate.some((g) => !g[2]));
	for (const [family, rdp, asked] of py.gate) {
		const sent = [];
		const s = { buf: new Uint8Array(0), clear() { s.buf = new Uint8Array(0); }, take(n) { s.buf = s.buf.slice(n); },
			async write(d) { sent.push(d); const f = parseFrames(d)[0]; s.buf = frame(f.cmd | 0x80, f.seq, new Uint8Array([0])); } };
		const kind = await new Link(s, { timeout: 50 }).restartIntoBootloader({ family, rdp, token: py.token });
		assert.equal(sent.length === 1, asked, `family ${family} rdp ${rdp}`);
		assert.equal(kind, asked ? "ok" : "protected");
		if (asked) assert.equal(toHex(parseFrames(sent[0])[0].payload), py.request);
	}
});

const running = async (opts = {}) => {
	const x = await installable("rfoundry", opts);
	const link = new Link(x.fake.serialStream(), { timeout: 50 });
	return { ...x, link, chip: await link.chip() };
};

test("the simulated radio takes the restart, or refuses it and changes nothing", async () => {
	let { fake, link, chip } = await running();
	assert.equal(chip.token, new DataView(fake.uid.buffer).getUint32(0, true));
	assert.deepEqual([chip.family, chip.rdp, chip.uid], [2, 0, toHex(fake.uid)]);
	// a wrong token is refused by the radio; a read-protected chip is never asked
	assert.equal(await link.restartIntoBootloader({ ...chip, token: chip.token ^ 1 }), "token");
	assert.equal(await link.restartIntoBootloader({ ...chip, rdp: 1 }), "protected");
	assert.equal(await link.restartIntoBootloader(null), "protected");
	assert.equal(fake.reboots.length, 1);
	assert.equal(fake.mode, "normal");
	assert.equal(await link.restartIntoBootloader(chip), "ok");
	assert.equal(fake.mode, "bootloader");
	assert.deepEqual(fake.events, ["restart into the loader"]);
	await assert.rejects(() => link.version(), /lost/);	// its serial port went with the restart
	await checkLoader_(fake);
	// the board's own rule (reason 5), as often as it says; then the user did what the hint says
	({ fake, link, chip } = await running({ restartRefusals: 2 }));
	for (let i = 0; i < 2; i++) {
		assert.equal(await link.restartIntoBootloader(chip), "not-now");
		assert.equal(fake.mode, "normal");
		assert.equal((await link.version()).board, board.debuglink_names[0]);
	}
	assert.equal(await link.restartIntoBootloader(chip), "ok");
	assert.equal(fake.mode, "bootloader");
	// other reasons are what they are; the radio's own protection check comes before the board's rule
	for (const [opts, kind] of [[{ restartReason: 3 }, "transmitting"], [{ restartReason: 4 }, "no-chip"], [{ restartReason: 9 }, "other"]]) {
		({ fake, link, chip } = await running(opts));
		assert.equal(await link.restartIntoBootloader(chip), kind);
		assert.equal(fake.mode, "normal");
	}
	({ fake, link, chip } = await running({ restartRefusals: 1 }));
	fake.rdp = 1;
	assert.equal(await link.restartIntoBootloader(chip), "protected");
	assert.deepEqual([fake.restartRefusals, fake.mode], [1, "normal"]);
	// a refusal carries its status and reason on the error, for whoever asks the command itself
	await assert.rejects(() => link.request(CMD.REBOOT, restartRequest(chip.token)), (e) => e instanceof LinkError && e.status === 7 && e.data[0] === 2);
	// the firmware's order (core/debuglink/dl_chip.c dl_reboot_check, then the platform's reboot_ok): the chip info,
	// the token, transmitting, the protection, the board's rule last. The radio is asked directly: the host's own
	// check would not send to a protected chip
	const ask = (token = chip.token, mode = 1) => link.request(CMD.REBOOT, new Uint8Array([mode, ...restartRequest(token).subarray(1)]));
	const reason = (n) => (e) => e instanceof LinkError && e.status === 7 && e.data[0] === n;
	fake.restartReason = 4;
	await assert.rejects(() => ask(chip.token ^ 1), reason(4));
	fake.restartReason = 3;
	await assert.rejects(() => ask(chip.token ^ 1), reason(1));
	await assert.rejects(() => ask(), reason(3));
	fake.restartReason = 0;
	await assert.rejects(() => ask(), reason(2));
	fake.rdp = 0;
	await assert.rejects(() => ask(), reason(5));
	assert.deepEqual([fake.restartRefusals, fake.mode], [0, "normal"]);
	// a mode the firmware doesn't have is a bad argument
	await assert.rejects(() => ask(chip.token, 2), (e) => e instanceof LinkError && e.status === 2);
	assert.equal(fake.mode, "normal");
	// a normal reset (mode 0) starts the firmware again on a new port, not the loader. The board's rule is about the
	// loader mode only. The reply is the status and reason 0
	fake.restartRefusals = 1;
	assert.deepEqual([...await ask(chip.token, 0)], [0]);
	assert.equal(fake.restartRefusals, 1);
	assert.equal(fake.mode, "normal");
	await assert.rejects(() => link.version(), /lost/);
	assert.ok(await probe(new Link(fake.serialStream(), { timeout: 50 })));
});
async function checkLoader_(fake) {
	const found = await dfuse.checkLoader(fake.dfuTransport(), fake.board, noWait);
	assert.equal(found.stage, "ready");
}

const quick = { waitMs: 20, tickMs: 5 };
test("restartIntoLoader asks only the radio the page was talking to, and says what became of the request", async () => {
	let { sim, fake, link, chip } = await running();
	// another model's name, another radio of the model, a radio without chip info: nothing is asked
	const other = { ...sim.board, debuglink_names: ["SOME OTHER RADIO"] };
	assert.equal((await dfuse.restartIntoLoader(link, other, quick)).kind, "other");
	assert.equal((await dfuse.restartIntoLoader(link, sim.board, { ...quick, uid: "00".repeat(12) })).kind, "other");
	// without chip info there is no token to send: said as that, not as another radio
	const noChip = Object.assign(Object.create(link), { chip: async () => null });
	assert.deepEqual(await dfuse.restartIntoLoader(noChip, sim.board, quick), { kind: "no-chip", why: RESTART_WHY["no-chip"] });
	assert.equal((await dfuse.restartIntoLoader(noChip, other, quick)).kind, "other");
	assert.deepEqual([fake.reboots.length, fake.mode], [0, "normal"]);
	// this radio, by its board name and the UID read when it connected
	assert.deepEqual(await dfuse.restartIntoLoader(link, sim.board, { ...quick, uid: chip.uid }), { kind: "ok" });
	assert.equal(fake.mode, "bootloader");
	// a radio whose port is gone does not answer: thrown, for the caller's words
	await assert.rejects(() => dfuse.restartIntoLoader(link, sim.board, quick));
	({ sim, fake, link } = await running({ restartRefusals: 1 }));
	assert.deepEqual(await dfuse.restartIntoLoader(link, sim.board, quick), { kind: "not-now" });
	({ sim, fake, link } = await running({ restartReason: 3 }));
	assert.deepEqual(await dfuse.restartIntoLoader(link, sim.board, quick), { kind: "transmitting", why: "the radio is transmitting" });
	({ sim, fake, link } = await running({ restartReason: 9 }));
	assert.deepEqual(await dfuse.restartIntoLoader(link, sim.board, quick), { kind: "other", why: "it answered the request with an error" });
	// it answered OK and still runs when the watch ends: said, after a bounded wait
	({ sim, fake, link } = await running({ restartStays: true }));
	const t0 = Date.now();
	assert.deepEqual(await dfuse.restartIntoLoader(link, sim.board, quick), { kind: "stayed" });
	assert.ok(Date.now() - t0 < 2000);
	assert.equal(fake.mode, "normal");
	assert.ok(dfuse.RESTART_CHECK_MS >= 1000 && dfuse.RESTART_CHECK_MS <= 10000 && dfuse.RESTART_TICK_MS < dfuse.RESTART_CHECK_MS);
});

// The write step with the page connected to a radio that runs RadioFoundry, on a board whose firmware restarts into
// its loader. The board's hint is the test's own: the page shows whatever the radio's data says
const HINT = "Turn the knob one step and try again.";
const restartButton = (box) => all(box).find((e) => e.tagName === "BUTTON" && text(e) === en["flash.dfuse.restart_go"]);
async function connected(opts = {}, { flash = { bootloader_restart: true, bootloader_restart_hint: HINT } } = {}) {
	const x = await installable("rfoundry", opts);
	const { state, got } = page(x.fake, x.sim, x.image, x.manifest, { restartWait: quick, dropSerial: async () => { got.dropped = (got.dropped || 0) + 1; state.link = null; } });
	state.radio = { ...radio, flash: { ...radio.flash, ...flash } };
	state.link = new Link(x.fake.serialStream(), { timeout: 50 });
	state.now = { kind: "rfoundry", probe: await probe(state.link), uid: toHex(x.fake.uid) };
	const box = dfuse.writeView();
	box.isConnected = true;
	return { ...x, state, got, box };
}

test("the write step offers the restart to a connected radio, says what follows, and keeps the keys' steps", async () => {
	const { fake, image, state, got, box } = await connected();
	for (const k of ["restart_offer", "restart_next", "restart_by_hand"]) assert.ok(text(box).includes(en[`flash.dfuse.${k}`]), k);
	assert.match(en["flash.dfuse.restart_next"], /Connect.*different USB device/);
	for (const s of state.radio.flash.bootloader) assert.ok(text(box).includes(s), s);
	// the restart is the step's first button while it is offered; Connect is there for the keys' way
	assert.ok(restartButton(box).className.includes("btn-primary"));
	assert.equal(text(got.forward), en["flash.write.connect"]);
	assert.ok(!got.forward.className.includes("btn-primary"));
	await restartButton(box).click();
	assert.equal(fake.mode, "bootloader");
	assert.equal(fake.reboots.length, 1);
	assert.equal(got.dropped, 1, "the serial port went with the restart");
	assert.ok(text(box).includes(en["flash.dfuse.restart_done"]));
	assert.match(en["flash.dfuse.restart_done"], /Connect.*different USB device.*steps below/);
	// the offer is gone, the keys' steps stay, and Connect is the way on
	assert.equal(restartButton(box), undefined);
	assert.ok(!text(box).includes(en["flash.dfuse.restart_offer"]) && !text(box).includes(en["flash.dfuse.restart_by_hand"]));
	for (const s of state.radio.flash.bootloader) assert.ok(text(box).includes(s), s);
	assert.ok(got.forward.className.includes("btn-primary"));
	await got.forward.click();
	// from here the install is the one a radio entered by its keys gets
	assert.ok(text(box).includes(en["flash.dfuse.not_protected"]));
	assert.ok(!text(box).includes(en["flash.dfuse.restart_done"]));
	assert.equal(text(got.forward), en["flash.write.go"]);
	await got.forward.click();
	assert.equal(got.next, 1);
	assert.deepEqual(fake.flash.subarray(0, image.length), image);
	assert.deepEqual([fake.violations, fake.unprotects, fake.vendor], [[], 0, []]);
});

test("a write that fails after a restart goes back to the keys' steps, without the restart's old words", async () => {
	const { fake, image, got, box } = await connected();
	await restartButton(box).click();
	assert.ok(text(box).includes(en["flash.dfuse.restart_done"]));
	await got.forward.click();
	assert.equal(text(got.forward), en["flash.write.go"]);
	// the cable goes mid-write
	fake.mode = "off";
	await got.forward.click();
	assert.equal(got.next, 0);
	assert.ok(text(box).includes(en["flash.write.failed"].split("{error}")[0].trim()));
	// "the radio is restarting" is no longer true, and no port is left to ask a restart on: the keys, then Connect
	assert.ok(!text(box).includes(en["flash.dfuse.restart_done"]) && !text(box).includes(en["flash.dfuse.restart_offer"]));
	assert.equal(restartButton(box), undefined);
	for (const s of radio.flash.bootloader) assert.ok(text(box).includes(s), s);
	assert.equal(text(got.forward), en["flash.write.connect"]);
	assert.ok(got.forward.className.includes("btn-primary"));
	fake.enterBootloader();
	await got.forward.click();
	await got.forward.click();
	assert.equal(got.next, 1);
	assert.deepEqual(fake.flash.subarray(0, image.length), image);
	assert.equal(fake.reboots.length, 1);
});

test("a refused restart is said with the board's hint, changes nothing, and can be tried again", async () => {
	const { fake, image, got, box } = await connected({ restartRefusals: 1 });
	const before = fake.flash.slice();
	await restartButton(box).click();
	assert.ok(text(box).includes(en["flash.dfuse.restart_refused"]) && text(box).includes(HINT));
	assert.ok(text(box).includes(en["flash.sim.restart_note"]), "practice mode says why the next try works");
	// not an error: no error notice, nothing dropped, the radio as it was
	const note = all(box).find((e) => e.className === "notice action" && text(e).includes(HINT));
	assert.ok(note && !all(box).some((e) => /\bbad\b/.test(e.className)));
	assert.deepEqual([fake.mode, got.dropped, fake.flash], ["normal", undefined, before]);
	// the offer and the keys' steps are both still there
	assert.ok(restartButton(box) && !restartButton(box).disabled);
	assert.ok(text(box).includes(en["flash.dfuse.restart_by_hand"]) && text(box).includes(radio.flash.bootloader[0]));
	assert.equal(text(got.forward), en["flash.write.connect"]);
	// the user did what the hint says
	await restartButton(box).click();
	assert.equal(fake.mode, "bootloader");
	assert.ok(text(box).includes(en["flash.dfuse.restart_done"]) && !text(box).includes(en["flash.dfuse.restart_refused"]) && !text(box).includes(HINT));
	await got.forward.click();
	await got.forward.click();
	assert.equal(got.next, 1);
	assert.deepEqual(fake.flash.subarray(0, image.length), image);
});

test("a refused restart leaves the keys' way open, and a board without a hint says the rest all the same", async () => {
	const { fake, image, state, got, box } = await connected({ restartRefusals: 5 }, { flash: { bootloader_restart: true, bootloader_restart_hint: null } });
	await restartButton(box).click();
	assert.ok(text(box).includes(en["flash.dfuse.restart_refused"]) && !text(box).includes(HINT));
	// outside practice mode nothing speaks of a simulated radio
	state.fakeRadio = null;
	await restartButton(box).click();
	assert.ok(text(box).includes(en["flash.dfuse.restart_refused"]) && !text(box).includes(en["flash.sim.restart_note"]));
	state.fakeRadio = fake;
	// off, the key, on: the manual steps as they always were
	fake.enterBootloader();
	await got.forward.click();
	assert.ok(text(box).includes(en["flash.dfuse.not_protected"]) && !text(box).includes(en["flash.dfuse.restart_refused"]));
	await got.forward.click();
	assert.equal(got.next, 1);
	assert.deepEqual(fake.flash.subarray(0, image.length), image);
	assert.equal(fake.events.includes("restart into the loader"), false);
});

test("a restart that is not taken for another reason, or taken and not carried out, is said as what it is", async () => {
	// transmitting: the page's own sentence for it, no hint (the hint is for the board's rule). The user can end it,
	// so it is a notice and the offer stays
	let { fake, got, box } = await connected({ restartReason: 3 });
	await restartButton(box).click();
	assert.ok(text(box).includes(en["flash.dfuse.restart_transmitting"]));
	assert.match(en["flash.dfuse.restart_transmitting"], /transmitting.*Nothing was changed.*steps below/);
	assert.ok(!text(box).includes(en["flash.dfuse.restart_failed"].split("{error}")[0]) && !all(box).some((e) => /\bbad\b/.test(e.className)));
	assert.ok(!text(box).includes(HINT) && fake.mode === "normal" && restartButton(box) && !restartButton(box).disabled);
	assert.deepEqual([fake.reboots.length, got.dropped], [1, undefined]);
	// any other reason: the frame in the page's language, the reason in rfoundry's words
	({ fake, got, box } = await connected({ restartReason: 4 }));
	await restartButton(box).click();
	assert.ok(text(box).includes(en["flash.dfuse.restart_failed"].replace("{error}", RESTART_WHY["no-chip"])));
	assert.ok(!text(box).includes(HINT) && fake.mode === "normal" && restartButton(box));
	// OK, and the radio still runs
	({ fake, got, box } = await connected({ restartStays: true }));
	await restartButton(box).click();
	assert.ok(text(box).includes(en["flash.dfuse.restart_stayed"]));
	assert.match(en["flash.dfuse.restart_stayed"], /steps below/);
	assert.deepEqual([fake.mode, got.dropped], ["normal", undefined]);
	assert.equal(text(got.forward), en["flash.write.connect"]);
	// a radio that no longer answers
	let state;
	({ fake, got, box, state } = await connected());
	state.link = new Link({ buf: new Uint8Array(0), clear() {}, take() {}, async write() {} }, { timeout: 20 });
	await restartButton(box).click();
	assert.ok(text(box).includes(en["flash.dfuse.restart_no_answer"].split("{error}")[0]));
	assert.equal(fake.reboots.length, 0);
	// another radio than the one the page connected to answers: never restarted
	({ fake, got, box, state } = await connected());
	state.now.uid = "ab".repeat(12);
	await restartButton(box).click();
	assert.ok(text(box).includes(en["flash.dfuse.restart_failed"].replace("{error}", "another radio answers on this connection")));
	assert.deepEqual([fake.reboots.length, fake.mode], [0, "normal"]);
});

test("no restart is offered without a connection to RadioFoundry, or on a board whose firmware cannot", async () => {
	const none = (box, why) => {
		assert.equal(restartButton(box), undefined, why);
		for (const k of ["restart_offer", "restart_next", "restart_by_hand"]) assert.ok(!text(box).includes(en[`flash.dfuse.${k}`]), `${why}: ${k}`);
	};
	// the board's data doesn't say so
	let x = await connected({}, { flash: { bootloader_restart: false, bootloader_restart_hint: null } });
	none(x.box, "a board without the restart");
	assert.ok(x.got.forward.className.includes("btn-primary"));
	// nothing connected (an install, a radio on its original firmware), a port that was lost, a radio the page knows
	// only as not RadioFoundry, a radio already in its loader
	for (const [why, change] of [["no link", (s) => { s.link = null; }], ["a lost port", (s) => { s.link.s.lost = true; }],
		["not RadioFoundry", (s) => { s.now = { kind: "stock" }; }], ["nothing known", (s) => { s.now = null; }],
		["in its loader", (s, fake) => fake.enterBootloader()]]) {
		x = await connected();
		change(x.state, x.fake);
		none(dfuse.writeView(), why);
	}
	// and the fixture's install page, as every test above this one drew it
	const { sim, fake, image, manifest } = await installable("stock");
	page(fake, sim, image, manifest);
	none(dfuse.writeView(), "an install");
});

test("the registry says which radios restart into their loader: ROM loader radios only, a hint only beside it", () => {
	for (const r of radios) {
		assert.equal(typeof r.flash.bootloader_restart, "boolean", r.id);
		if (r.flash.bootloader_restart) assert.equal(fb.boards[r.flash.board].family, PLAIN_FAMILY, r.id);
		else assert.equal(r.flash.bootloader_restart_hint, null, r.id);
		assert.ok(r.flash.bootloader_restart_hint === null || r.flash.bootloader_restart_hint.trim(), r.id);
	}
});

test("the write step refuses a device that is not this radio's loader, in the page's words", async () => {
	const { sim, fake, image, manifest } = await installable("stock", { layout: "@Internal Flash  /0x0800C000/01*016Kg,01*064Kg,07*128Kg" });
	const { got } = page(fake, sim, image, manifest);
	const box = dfuse.writeView();
	fake.enterBootloader();
	await got.forward.click();
	assert.ok(text(box).includes(en["flash.dfuse.wrong_device"].split("{what}")[0]));
	assert.equal(text(got.forward), en["flash.write.connect"]);
	assert.deepEqual([fake.erased, fake.writes, fake.unprotects], [[], [], 0]);
});

// ---- the codec step of the route: on the board as the registry has it (skipped where it has no codec route)

// A simulated radio of that board: the synthetic stock file and its codec as the merge places it (fake.js
// romSimulation), an image and its manifest, the file as the codec step holds a picked one, the copy of the codec as a
// browser saves it, the board's codec sectors and a decision to keep by what a running RadioFoundry reported
async function routed(state = "stock", opts = {}) {
	const r = await installable(state, opts, { real: true });
	const b = r.sim.board;
	return { ...r, board: b, secs: codecSectors(b), stock: { name: "stock.bin", data: r.sim.stock, info: { sha: await sha256(r.sim.stock) } },
		saved: { profile: r.sim.profile, segments: r.sim.codecSegments.slice(0, -1), from: "browser", stored: true },
		kept: decideCodec(b, r.manifest, { probe: { board: b.debuglink_names[0], codec: b.codec.id } }) };
}
const routedPage = (r, state = {}, extra = {}) => page(r.fake, r.sim, r.image, r.manifest, { state: { radio: realRadio, ...state }, ...extra });
// the page's own work between two clicks (a file decrypted, a codec placed) is over when the step shows it: waited
// for in real time (setTimeout is never the test's own clock), at most 20 s
const until = async (cond) => {
	for (let i = 0; i < 4000 && !cond(); i++) await new Promise((done) => setTimeout(done, 5));
	assert.ok(cond(), "the step did not get there");
};
const buttons = (box) => all(box).filter((e) => e.tagName === "BUTTON");
const byText = (box, key) => buttons(box).find((e) => text(e) === en[key]);
const agreeTo = async (box) => {
	const agree = all(box).find((e) => e.tagName === "INPUT" && e.attrs.type === "checkbox");
	agree.checked = true;
	await agree.fire("change");
};
// in sector s of a radio's flash
const within = (r, s) => r.fake.flash.subarray(s.addr - r.board.rom_base, s.addr - r.board.rom_base + s.size);
const holdsCodec = (r) => r.sim.codecSegments.every(([a, d]) => Buffer.compare(r.fake.flash.subarray(a - r.board.rom_base, a - r.board.rom_base + d.length), d) === 0);
const noVectors = (r) => r.fake.flash.subarray(0, BLOCK).every((x) => x === 0xFF);
// the whole way from a protected radio to Install: the confirmed Read Unprotect, the wait, the loader again
async function unprotectAndReenter(r, got, box, clock) {
	assert.equal(text(got.forward), en["flash.dfuse.unprotect_go"]);
	await agreeTo(box);
	await got.forward.click();
	assert.equal(r.fake.unprotects, 1);
	clock.tick(10);
	assert.equal(text(got.forward), en["flash.write.connect"]);
	r.fake.enterBootloader();
	await got.forward.click();
	assert.equal(text(got.forward), en["flash.write.go"]);
}

test("the practice radio of a board with a codec route: a synthetic stock file in its container, and RadioFoundry that reports its codec", withRoute, async () => {
	const r = await routed("rfoundry");
	const { sim } = r;
	assert.ok(sim.stock && sim.profile && sim.codecSegments.length === 3);
	assert.equal(sim.key.length, 0, "no bootloader key on this route");
	assert.equal(stockfw.containerKind(sim.stock, sim.fb), sim.profile.container);
	// the page opens it as it opens a real one: by its SHA-256, with its own key, placed as the simulation has it
	assert.equal(sim.fb.profiles[r.stock.info.sha], sim.profile);
	assert.ok(!Object.keys(fbReal.profiles).some((h) => sim.fb.profiles[h]?.board === sim.board.id), "the real file's profile is not the simulation's");
	const got = await stockfw.extract(sim.stock, null, sim.fb, sim.fb.profiles, sim.board.id);
	assert.deepEqual(got.segments, sim.codecSegments);
	assert.deepEqual(sim.codecSegments.map(([a, d]) => [a, d.length]), stockfw.layoutWindows(sim.profile, sim.fb));
	assert.deepEqual(stockfw.layoutWindows(sim.profile, sim.fb), sim.board.codec.windows, "where the board's build expects it");
	// what the radio says of its codec, and reads back for a saved copy
	assert.equal(r.fake.codecReport(), sim.board.codec.id);
	assert.equal((await probe(new Link(r.fake.serialStream()))).codec, sim.board.codec.id);
	assert.equal(new FakeRomRadio(sim, { state: "rfoundry-nocodec", spiSize: 0x200000 }).codecReport(), "absent");
	assert.equal(await stockfw.checkForm(sim.profile, stockfw.forms(sim.profile)[0], r.saved.segments, sim.fb), true);
	// a board without a codec route has no file and reports no vocoder
	const plain = await installable("rfoundry");
	assert.deepEqual([plain.sim.stock, plain.sim.codecSegments, plain.fake.codecReport()], [null, [], "none"]);
});

test("the three plans of a board with a codec route, as rfoundry plans them: keep, merge and blank", withRoute, async () => {
	const image = bin("image.bin");
	const lay = fx.layouts.files.find((f) => f.file === ROM.layout);
	const fw = { manifest: await forDevBuild(realBoard, image), image };
	page(null, { fb: fbReal, board: realBoard }, image, fw.manifest);
	const { profile, segments: codec } = await stockfw.extract(bin(ROM.layout), null, fbReal, { [lay.stock_sha256]: lay.profile }, realBoard.id);
	const keep = dfuse.installPlan(realBoard, fw), merge = dfuse.installPlan(realBoard, fw, { mode: MERGE }, codec, profile),
		blank = dfuse.installPlan(realBoard, fw, { mode: BLANK });
	assert.deepEqual(keep.describe(), ROM.keep);
	assert.deepEqual(merge.describe(), ROM.merge);
	assert.deepEqual(blank.describe(), ROM.blank);
	assert.deepEqual(dfuse.installPlan(realBoard, fw, { mode: KEEP }).describe(), ROM.keep);
	const secs = codecSectors(realBoard), idx = secs.map((s) => s.index), base = realBoard.app_base;
	const of = (plan) => plan.steps.filter((st) => idx.includes(st.sector.index));
	// merge: the codec's sectors written whole with the codec and nothing else, after the image's other sectors and
	// before the sector with the vector table; the descriptor in the last block, after every codec byte
	assert.ok(of(merge).every((st) => st.action === "write" && st.content === "codec" && st.blocks.length === st.sector.size / BLOCK));
	assert.equal(merge.eraseOrder[0].addr, base);
	assert.equal(merge.writeOrder.at(-1).sector.addr, base);
	assert.ok(merge.writeOrder.findIndex((st) => idx.includes(st.sector.index)) < merge.writeOrder.length - 1);
	const last = of(merge).at(-1), d = fbReal.descriptor;
	assert.equal(last.sector.addr + last.blocks.length * BLOCK, d.addr + d.len);
	assert.ok(last.blocks.at(-1)[1].subarray(0, BLOCK - d.len).every((x) => x === 0xFF), "the descriptor's block holds no codec byte");
	assert.ok(merge.erase.length === keep.erase.length + secs.length && merge.kept.length === 0);
	// keep: the codec's sectors are not in the plan
	assert.ok(of(keep).every((st) => st.action === "keep" && !st.blocks.length));
	assert.deepEqual(keep.kept.map((s) => s.index), idx);
	assert.ok(!keep.erase.some((s) => idx.includes(s.index)));
	// blank: erased and left blank, nothing written there
	assert.ok(of(blank).every((st) => st.action === "erase" && !st.blocks.length));
	assert.ok(idx.every((i) => blank.erase.some((s) => s.index === i)) && !blank.writes.some((st) => idx.includes(st.sector.index)));
	assert.equal(blank.totalBlocks, keep.totalBlocks);
	assert.equal(blank.eraseOrder[0].addr, base);
	// codec data belongs to a merge alone, and no image reaches the codec's flash in any mode
	const segs = segments(fw.manifest, image);
	for (const mode of [KEEP, BLANK]) assert.throws(() => makePlan(realBoard, segs, mode, codec), new RegExp(`codec data given in ${mode} mode`));
	assert.throws(() => makePlan(realBoard, segs, "wipe"), /unknown codec mode/);
	const [lo, hi] = realBoard.app_region, over = testImage(hi - lo + 1);
	for (const [mode, c] of [[KEEP, []], [MERGE, codec], [BLANK, []]])
		assert.throws(() => makePlan(realBoard, [[lo, over]], mode, c, stockfw.layoutWindows(profile, fbReal), { vectorLast: true }), /codec (window|sector)/, mode);
	// a board without a codec route: the image alone, and no codec sector to erase by itself
	assert.deepEqual(dfuse.installPlan(board, fw).describe(), ROM.plain.keep);
	assert.throws(() => makePlan(board, segs, BLANK, [], board.codec.windows, { vectorLast: true }), (e) => e.message === ROM.plain.blank_refusal);
	// nor has a keyed radio whose image shares no sector with its codec any use for it here: the mode is this route's
	// by what the page offers, and a board that can't keep its codec apart is refused it by the planner
	assert.throws(() => makePlan({ ...realBoard, keep_codec_supported: false }, segs, BLANK), /no codec sector that can be erased and left blank/);
});

test("the route's limits with a codec: past the application only a codec sector, to hold the codec or nothing", withRoute, async () => {
	const r = await routed("blank");
	routedPage(r);
	const fw = { manifest: r.manifest, image: r.image }, b = r.board;
	const merge = dfuse.installPlan(b, fw, { mode: MERGE }, r.sim.codecSegments, r.sim.profile), blank = dfuse.installPlan(b, fw, { mode: BLANK });
	for (const plan of [r.plan, merge, blank]) checkPlain(plan);
	const like = (plan, change) => ({ board: b, erase: plan.erase, eraseOrder: plan.eraseOrder, writes: plan.writes, writeOrder: plan.writeOrder, totalBlocks: plan.totalBlocks, ...change });
	const past = b.sectors.find((s) => s.addr === r.secs.at(-1).addr + r.secs.at(-1).size);
	const free = b.sectors.find((s) => s.addr > b.app_base && s.addr < b.app_region[1] && !merge.erase.includes(s));
	const outside = /outside .* application flash/;
	const cases = [
		// a codec sector that would get anything but the codec
		["the image in a codec sector", like(merge, { writes: merge.writes.map((st) => (r.secs.includes(st.sector) ? { ...st, content: "app" } : st)) }), outside],
		["the image and the codec in one sector", like(merge, { writes: merge.writes.map((st) => (r.secs.includes(st.sector) ? { ...st, content: "app+codec" } : st)) }), outside],
		// a sector of the application erased and left blank: only a codec sector ever is
		["an application sector erased and not written", like(blank, { erase: [...blank.erase, free], eraseOrder: [...blank.eraseOrder, free] }), /erases sector \d+ .* application flash and writes nothing there/],
		// the vector table's sector written last and not erased first
		["an erase order that does not start at the application base", like(merge, { eraseOrder: [...merge.eraseOrder].reverse() }), /first and write it last/],
	];
	// flash past the codec's sectors (a second bank): never erased, never written, codec or not
	if (past) cases.push(["a sector past the codec's erased", like(blank, { erase: [...blank.erase, past], eraseOrder: [...blank.eraseOrder, past] }), outside],
		["a sector past the codec's written as codec", like(merge, { writes: [...merge.writes, { sector: past, blocks: [[FIRST_BLOCK, new Uint8Array(BLOCK)]], content: "codec" }] }), outside]);
	assert.ok(free, "a sector of the application region that the image leaves free");
	r.fake.enterBootloader();
	const before = r.fake.flash.slice();
	for (const [what, plan, refusal] of cases) {
		assert.throws(() => checkPlain(plan), refusal, what);
		await assert.rejects(new Dfu(r.fake.dfuTransport(), noWait).run(plan, null), refusal, what);
	}
	assert.deepEqual([r.fake.erased, r.fake.writes], [[], []], "refused before any request");
	assert.deepEqual(r.fake.flash, before);
	// an own image that reaches the codec's flash: refused when the step is drawn, before any Connect
	const [lo, hi] = b.app_region, over = testImage(hi - lo + BLOCK);
	const { got } = page(r.fake, r.sim, over, await forDevBuild(b, over), { state: { radio: realRadio, decision: r.kept } });
	const box = dfuse.writeView();
	assert.match(text(box), /codec (window|sector)/);
	assert.equal(got.forward, null);
	assert.ok(!text(box).includes(en["flash.write.then_connect"]));
});

test("a first install with the stock file: the codec step's merge, and the write sends the codec's sectors whole, the vector table last", withRoute, async (tc) => {
	const clock = ownClock(tc);
	const r = await routed("stock");
	// the picked file is this radio's codec source, opened without a key
	const m = await vendorfile.identifyFile(r.sim.stock, r.sim.fb, r.board.id);
	assert.deepEqual([m.kind, m.board, m.how], ["codec", r.board.id, "sha256"]);
	const opened = await vendorfile.openVendorFile("stock.bin", r.sim.stock, r.sim.fb, r.board.id);
	assert.equal(pickProblem(opened, r.board.id), null);
	assert.ok(vendorfile.describe(opened, r.sim.fb, r.board.id)[0].endsWith(": the codec (used)"), "no bootloader key is promised");
	// a radio not known to run RadioFoundry is on its original firmware: without the file there is no decision, with it a merge
	assert.throws(() => decideCodec(r.board, r.manifest, { runsStock: true }), /cannot determine whether the radio has its codec/);
	const decision = decideCodec(r.board, r.manifest, { haveStock: true, runsStock: true });
	assert.equal(decision.mode, MERGE);
	const { got, state } = routedPage(r, { decision, stock: r.stock, now: { kind: "stock", version: null } });
	const box = dfuse.writeView();
	box.isConnected = true;
	assert.equal(got.forward, null, "the codec is taken from the file before the loader's steps are shown");
	await until(() => got.forward);
	assert.equal(text(got.forward), en["flash.write.connect"]);
	assert.ok(text(box).includes(`board ${r.board.name}, codec mode: merge\n`));
	assert.equal(got.plans.at(-1).mode, MERGE);
	r.fake.enterBootloader();
	await got.forward.click();
	assert.ok(text(box).includes(en["flash.dfuse.protected_title"]), "a merge stands whatever the loader says");
	await unprotectAndReenter(r, got, box, clock);
	await got.forward.click();
	assert.equal(got.next, 1);
	assert.equal(got.asked, 0);
	// the image, the codec where the merge puts it with its descriptor, 0xFF around it in its sectors
	const base = r.board.rom_base;
	assert.deepEqual(r.fake.flash.subarray(0, r.image.length), r.image);
	assert.ok(holdsCodec(r));
	const used = new Uint8Array(r.fake.flash.length);
	for (const [a, d] of r.sim.codecSegments) used.fill(1, a - base, a - base + d.length);
	for (const s of r.secs) for (let i = s.addr - base; i < s.addr - base + s.size; i++) if (!used[i]) assert.equal(r.fake.flash[i], 0xFF, `+${i}`);
	// the order at the loader: the vector table's sector erased first, then the image's other sectors and the codec's;
	// every block of a codec sector sent, the descriptor's last of them; the vector table's block after all of it
	assert.equal(r.fake.erased[0], r.board.app_base);
	assert.ok(r.secs.every((s) => r.fake.erased.includes(s.addr)));
	const sent = r.fake.writes.map(([a]) => a);
	assert.equal(sent.at(-1), r.board.app_base);
	for (const s of r.secs) assert.equal(sent.filter((a) => a >= s.addr && a < s.addr + s.size).length, s.size / BLOCK);
	const d = r.sim.fb.descriptor, inCodec = sent.filter((a) => r.secs.some((s) => a >= s.addr && a < s.addr + s.size));
	assert.equal(inCodec.at(-1), d.addr + d.len - BLOCK);
	assert.ok(sent.indexOf(inCodec.at(-1)) < sent.length - 1);
	assert.ok(sent.every((a) => a < r.board.app_region[1] || r.secs.some((s) => a >= s.addr && a < s.addr + s.size)), "nothing past the application but the codec's sectors");
	assert.deepEqual([r.fake.violations, r.fake.vendor], [[], []]);
	// RadioFoundry starts and reports the codec, which the done step expects
	assert.equal((await probe(new Link(r.fake.serialStream()))).codec, r.board.codec.id);
	assert.equal(state.decision.expectCodec, true);
});

const namedFiles = { skip: withRoute.skip || (fx.twin?.unmapped ? false : "no file in the fixtures that is known and not mapped") };
test("a version nobody has mapped is named with the files to use and never offered for sending; a bootloader file is no firmware file", namedFiles, async () => {
	const T = fx.twin, u = T.unmapped, id = u.entry.board;
	const data = bin(T.file).slice();
	for (const [at, mask] of u.change.flips) data[at] ^= mask;
	const fbU = { ...fbReal, unmapped_files: { [u.sha256]: u.entry } };
	// the file of its radio, with the codec and no recipe: the existing kind, by its SHA-256
	assert.deepEqual(await vendorfile.identifyFile(data, fbU, id), { sha: u.sha256, board: id, kind: "codec-unmapped", how: "sha256", known: u.entry.name });
	const opened = await vendorfile.openVendorFile(u.entry.name, data, fbU, id);
	assert.equal(opened.best.match.kind, "codec-unmapped");
	// the codec step's notice: "unmapped", by name, with the files that do give the codec and no word of a key
	assert.deepEqual(pickProblem(opened, id), { problem: "unmapped", file: u.entry.name, known: u.entry.name });
	assert.deepEqual(supportedFiles(fbU, id).files, fbU.boards[id].codec.sources);
	assert.ok(supportedFiles(fbU, id).files.length > 0);
	assert.match(vendorfile.describe(opened, fbU, id)[0], /with the codec, but RadioFoundry has no recipe for this build yet \(used\)$/);
	// no offer to send it: its maker publishes it, and the receiving service takes no file of this container
	assert.equal(vendorfile.analysisCandidate(opened, fbU, id), null);
	assert.equal(vendorfile.offerFile({ path: u.entry.name, data }, fbU, "unmapped"), null);
	// picked on another radio's page it is that radio's file
	assert.deepEqual(pickProblem(await vendorfile.openVendorFile("x.bin", data, fbU, keyed.flash.board), keyed.flash.board),
		{ problem: "other-radio", file: "x.bin", board: id, known: u.entry.name });
	// the extract a merge would run refuses it with rfoundry's sentence
	await assert.rejects(stockfw.extract(data, null, fbU, fbU.profiles, id), (e) => e.code === "unmapped" && e.message === u.message);
	// the maker's bootloader file: no radio's firmware, with its own sentence
	const boot = bin(T.boot.file);
	const b = await vendorfile.identifyFile(boot, fbReal, id);
	assert.deepEqual([b.board, b.problem, b.why], [null, "not-vendor", T.boot.message]);
	const bootOpened = await vendorfile.openVendorFile("boot.bin", boot, fbReal, id);
	assert.equal(pickProblem(bootOpened, id).problem, "not-vendor");
	assert.equal(vendorfile.analysisCandidate(bootOpened, fbReal, id), null);
	assert.ok(vendorfile.describe(bootOpened, fbReal, id)[0].includes(T.boot.message));
});

test("a keep on a read protected radio is void: no Read Unprotect and no Install until the codec has a source or the user leaves it out", withRoute, async (tc) => {
	const clock = ownClock(tc);
	// the void state: the reason, the way back to the codec step and "Install without the codec", nothing else
	const toVoid = async (state = {}, radioState = "stock") => {
		const r = await routed(radioState);
		const p = routedPage(r, { decision: r.kept, ...state });
		const box = dfuse.writeView();
		box.isConnected = true;
		assert.ok(text(box).includes(`board ${r.board.name}, codec mode: keep\n`));
		r.fake.enterBootloader();
		await p.got.forward.click();
		return { r, box, ...p };
	};
	const isVoid = ({ r, box, got }, key) => {
		assert.ok(text(box).includes(en[key]), key);
		assert.equal(got.forward, null, "nothing to go on with");
		for (const k of ["protected_title", "unprotect_go", "confirm", "not_protected"]) assert.ok(!text(box).includes(en[`flash.dfuse.${k}`]), k);
		assert.ok(!byText(box, "flash.write.go") && !byText(box, "flash.dfuse.unprotect_go"));
		assert.ok(byText(box, "flash.nocodec.back") && byText(box, "flash.dfuse.install_no_codec"));
		assert.ok(!text(box).includes("codec mode:"), "the plan that will not run is not shown");
		assert.deepEqual([r.fake.unprotects, r.fake.erased, r.fake.writes], [0, [], []]);
	};
	// whatever made the keep: a radio that reported its codec (a remembered probe, an update), or Keep forced
	for (const state of [{}, { requested: KEEP }, { decision: "forced" }]) {
		const probeLess = state.decision === "forced";
		const v = await toVoid(probeLess ? {} : state);
		if (probeLess) {
			// rebuilt with a forced Keep on a radio taken to run its original firmware
			const forced = decideCodec(v.r.board, v.r.manifest, { requested: KEEP, runsStock: true });
			assert.deepEqual([forced.mode, forced.expectCodec], [KEEP, false]);
			const p = routedPage(v.r, { decision: forced, requested: KEEP });
			const box = dfuse.writeView();
			await p.got.forward.click();
			Object.assign(v, { box, ...p });
		}
		isVoid(v, "flash.dfuse.codec_erased");
		// back to the codec step: the loader is let go, a forced Keep is dropped, the step before this one is drawn
		const step = v.state.step;
		await byText(v.box, "flash.nocodec.back").click();
		assert.deepEqual([v.got.renders, v.state.step, v.state.requested], [1, step - 1, null]);
		assert.equal(v.r.fake.unprotects, 0);
	}
	// with the stock file picked, or a copy of the codec saved from the radio, the keep becomes a merge, said beside
	// the new plan, and the Read Unprotect is offered. The saved copy comes first. The page still holds the report that
	// made the keep (S.now): the decision is made again without the radio's word, as rfoundry's settle_keep makes it
	for (const [what, state, mode] of [["the stock file", (r) => ({ stock: r.stock }), "merge"], ["a saved copy", (r) => ({ savedCodec: r.saved }), "merge_saved"],
		["both", (r) => ({ stock: r.stock, savedCodec: r.saved }), "merge_saved"]]) {
		const r = await routed("stock");
		const now = { kind: "rfoundry", probe: { board: r.board.debuglink_names[0], codec: r.board.codec.id } };
		const { got, state: st } = routedPage(r, { decision: r.kept, now, ...state(r) });
		const box = dfuse.writeView();
		box.isConnected = true;
		r.fake.enterBootloader();
		await got.forward.click();
		assert.equal(st.decision.mode, MERGE, what);
		assert.equal(st.decision.source === "saved", mode === "merge_saved", what);
		assert.ok(text(box).includes(en[`flash.codec.mode.${mode}`]) && text(box).includes(en[st.decision.whyKey]), what);
		assert.ok(text(box).includes(`board ${r.board.name}, codec mode: merge\n`), what);
		assert.ok(text(box).includes(en["flash.dfuse.protected_title"]) && !text(box).includes(en["flash.dfuse.codec_erased"]), what);
		assert.equal(got.decisions, 1, what);
		await unprotectAndReenter(r, got, box, clock);
		await got.forward.click();
		assert.equal(got.next, 1, what);
		assert.ok(holdsCodec(r), what);
		assert.equal((await probe(new Link(r.fake.serialStream()))).codec, r.board.codec.id, what);
	}
	// Keep forced with a file at hand stays a keep, and void: the user asked for it, and decides again
	const forced = await toVoid({ requested: KEEP });
	forced.state.stock = forced.r.stock;
	isVoid(forced, "flash.dfuse.codec_erased");
	const again = await routed("stock");
	const pg = routedPage(again, { decision: again.kept, requested: KEEP, stock: again.stock });
	const bx = dfuse.writeView();
	again.fake.enterBootloader();
	await pg.got.forward.click();
	isVoid({ r: again, box: bx, got: pg.got }, "flash.dfuse.codec_erased");
	// a stock file that fails its check when the merge takes the codec from it: refused with nothing erased
	const bad = await routed("stock");
	const damaged = bad.sim.stock.slice();
	damaged[damaged.length - 1] ^= 1;
	const bp = routedPage(bad, { decision: bad.kept, stock: { ...bad.stock, data: damaged } });
	const bbox = dfuse.writeView();
	bad.fake.enterBootloader();
	await bp.got.forward.click();
	assert.match(text(bbox), /unknown stock file/);
	assert.equal(text(bp.got.forward), en["flash.write.connect"]);
	assert.equal(bp.state.decision, bad.kept, "the decision is not changed by a file that gives nothing");
	assert.deepEqual([bad.fake.unprotects, bad.fake.erased], [0, []]);
});

test("Install without the codec: asked once more, then the codec's sectors are erased and left blank and nothing is written there", withRoute, async (tc) => {
	const clock = ownClock(tc);
	const r = await routed("stock");
	const { got, state } = routedPage(r, { decision: r.kept });
	const box = dfuse.writeView();
	box.isConnected = true;
	r.fake.enterBootloader();
	await got.forward.click();
	const without = () => byText(box, "flash.dfuse.install_no_codec");
	assert.ok(without().className.includes("btn-danger"));
	// "stay": nothing changes
	got.answer = "stay";
	await without().click();
	assert.deepEqual([got.asked, got.forward, state.decision], [1, null, r.kept]);
	assert.ok(text(box).includes(en["flash.dfuse.codec_erased"]));
	// "back": the dialog's own way to the codec step
	got.answer = "back";
	await without().click();
	assert.deepEqual([got.asked, got.renders], [2, 1]);
	assert.equal(state.decision, r.kept);
	// "go": the plan erases the codec's sectors, and the loader's answer stands: this radio is still read protected
	state.step++;
	r.fake.enterBootloader();
	const box2 = dfuse.writeView();
	box2.isConnected = true;
	await got.forward.click();
	got.answer = "go";
	await byText(box2, "flash.dfuse.install_no_codec").click();
	assert.deepEqual([state.decision.mode, state.decision.expectCodec], [BLANK, false]);
	assert.equal(got.plans.at(-1).mode, BLANK);
	assert.ok(text(box2).includes(`board ${r.board.name}, codec mode: blank\n`) && text(box2).includes("ERASE, left blank"));
	assert.ok(text(box2).includes(en["flash.dfuse.protected_title"]) && !text(box2).includes(en["flash.dfuse.codec_erased"]));
	assert.deepEqual([r.fake.unprotects, r.fake.erased], [0, []], "nothing was erased by the answer");
	await unprotectAndReenter(r, got, box2, clock);
	const asked = got.asked;
	await got.forward.click();
	assert.equal(got.asked, asked, "confirmed once, not again at Install");
	assert.equal(got.next, 1);
	assert.deepEqual(r.fake.flash.subarray(0, r.image.length), r.image);
	for (const s of r.secs) {
		assert.ok(r.fake.erased.includes(s.addr));
		assert.ok(within(r, s).every((x) => x === 0xFF), "blank");
		assert.ok(!r.fake.writes.some(([a]) => a >= s.addr && a < s.addr + s.size), "no block sent there");
	}
	assert.deepEqual(r.fake.violations, []);
	// RadioFoundry runs without its codec, and the done step was told not to expect one
	assert.equal((await probe(new Link(r.fake.serialStream()))).codec, "absent");
	assert.equal(state.decision.expectCodec, false);
});

test("a keep on a radio with a vector table stands: the codec's sectors are not sent and are byte for byte as before", withRoute, async () => {
	const r = await routed("rfoundry");
	const { got, state } = routedPage(r, { decision: r.kept, now: { kind: "rfoundry", probe: { board: r.board.debuglink_names[0], codec: r.board.codec.id } } });
	const box = dfuse.writeView();
	box.isConnected = true;
	assert.ok(text(box).includes(`never erased or written: codec sector(s) ${r.secs.map((s) => s.index).join(", ")}`));
	r.fake.enterBootloader();
	const before = r.secs.map((s) => within(r, s).slice());
	await got.forward.click();
	assert.ok(text(box).includes(en["flash.dfuse.not_protected"]));
	assert.equal(text(got.forward), en["flash.write.go"]);
	await got.forward.click();
	assert.deepEqual([got.next, got.asked], [1, 0]);
	assert.equal(state.decision, r.kept);
	for (const [i, s] of r.secs.entries()) {
		assert.ok(!r.fake.erased.includes(s.addr) && !r.fake.writes.some(([a]) => a >= s.addr && a < s.addr + s.size));
		assert.deepEqual(within(r, s), before[i]);
	}
	assert.deepEqual(r.fake.flash.subarray(0, r.image.length), r.image);
	assert.equal((await probe(new Link(r.fake.serialStream()))).codec, r.board.codec.id);
	// a codec known to be missing, kept as selected: Install asks once more, and writes on "go" only
	const none = await routed("rfoundry-nocodec");
	const probed = { board: none.board.debuglink_names[0], codec: "absent" };
	const missing = decideCodec(none.board, none.manifest, { probe: probed, requested: KEEP });
	assert.deepEqual([missing.mode, missing.expectCodec], [KEEP, false]);
	const np = routedPage(none, { decision: missing, requested: KEEP });
	const nbox = dfuse.writeView();
	none.fake.enterBootloader();
	await np.got.forward.click();
	assert.equal(text(np.got.forward), en["flash.write.go"], "a vector table: the keep stands");
	np.got.answer = "stay";
	await np.got.forward.click();
	assert.deepEqual([np.got.asked, np.got.next, none.fake.erased], [1, 0, []]);
	np.got.answer = "go";
	await np.got.forward.click();
	assert.deepEqual([np.got.asked, np.got.next], [2, 1]);
	assert.ok(none.secs.every((s) => !none.fake.erased.includes(s.addr)));
	assert.ok(text(nbox).length > 0);
});

test("the restart into the loader after the codec step: the keep is settled by the loader it reached, as after the keys", withRoute, async () => {
	const r = await routed("rfoundry");
	const flash = { ...realRadio.flash, bootloader_restart: true, bootloader_restart_hint: HINT };
	const { got, state } = routedPage(r, { radio: { ...realRadio, flash }, decision: r.kept }, { restartWait: quick,
		dropSerial: async () => { got.dropped = (got.dropped || 0) + 1; state.link = null; } });
	state.link = new Link(r.fake.serialStream(), { timeout: 50 });
	state.now = { kind: "rfoundry", probe: await probe(state.link), uid: toHex(r.fake.uid) };
	assert.equal(state.now.probe.codec, r.board.codec.id, "the decision rests on what the running radio reported");
	const box = dfuse.writeView();
	box.isConnected = true;
	assert.ok(text(box).includes(en["flash.dfuse.restart_offer"]));
	const before = r.secs.map((s) => within(r, s).slice());
	await restartButton(box).click();
	assert.deepEqual([r.fake.mode, r.fake.reboots.length, got.dropped], ["bootloader", 1, 1]);
	await got.forward.click();
	assert.ok(text(box).includes(en["flash.dfuse.not_protected"]));
	assert.equal(text(got.forward), en["flash.write.go"]);
	await got.forward.click();
	assert.deepEqual([got.next, got.asked], [1, 0]);
	assert.equal(state.decision, r.kept);
	for (const [i, s] of r.secs.entries()) assert.deepEqual(within(r, s), before[i]);
	assert.deepEqual(r.fake.flash.subarray(0, r.image.length), r.image);
});

test("a radio without a vector table did not finish its last install: a keep is void there too, and a merge runs again whole", withRoute, async () => {
	// erased and not protected: the reason is the unfinished install, and the way on is offered at once
	const r = await routed("blank");
	const { got, state } = routedPage(r, { decision: r.kept });
	const box = dfuse.writeView();
	box.isConnected = true;
	r.fake.enterBootloader();
	await got.forward.click();
	assert.ok(text(box).includes(en["flash.dfuse.codec_unknown"]) && !text(box).includes(en["flash.dfuse.codec_erased"]));
	assert.equal(got.forward, null);
	assert.ok(!byText(box, "flash.write.go"));
	await byText(box, "flash.dfuse.install_no_codec").click();
	assert.equal(state.decision.mode, BLANK);
	assert.equal(text(got.forward), en["flash.write.go"], "not protected: Install, no Read Unprotect");
	await got.forward.click();
	assert.deepEqual([got.next, r.fake.unprotects], [1, 0]);
	assert.ok(r.secs.every((s) => r.fake.erased.includes(s.addr) && within(r, s).every((x) => x === 0xFF)));

	// an update that keeps the codec, cut after its erases: the vector table is gone, the codec's sectors untouched. The
	// next Connect can't tell that from a cut merge, so the keep is void; with the copy saved from the radio the codec
	// is written again
	const u = await routed("rfoundry");
	const up = routedPage(u, { decision: u.kept, savedCodec: u.saved });
	const ubox = dfuse.writeView();
	ubox.isConnected = true;
	u.fake.enterBootloader();
	await up.got.forward.click();
	assert.equal(text(up.got.forward), en["flash.write.go"]);
	const whole = u.secs.map((s) => within(u, s).slice());
	const execute = u.fake.execute.bind(u.fake);
	u.fake.execute = (c) => {
		if (c.kind !== "write") return execute(c);
		u.fake.mode = "off";	// the cable goes at the first block
	};
	await up.got.forward.click();
	u.fake.execute = execute;
	assert.equal(up.got.next, 0);
	assert.ok(noVectors(u), "no vector table");
	u.secs.forEach((s, i) => assert.deepEqual(within(u, s), whole[i], "the codec's sector was not touched"));
	assert.equal(text(up.got.forward), en["flash.write.connect"]);
	u.fake.enterBootloader();
	await up.got.forward.click();
	assert.equal(up.state.decision.mode, MERGE);
	assert.equal(up.state.decision.source, "saved");
	assert.ok(text(ubox).includes(en["flash.codec.mode.merge_saved"]) && text(ubox).includes(`board ${u.board.name}, codec mode: merge\n`));
	await up.got.forward.click();
	assert.equal(up.got.next, 1);
	assert.ok(holdsCodec(u) && u.secs.every((s) => u.fake.erased.includes(s.addr)));
	// a cut during the erases. Before the first one ran nothing changed: the vector table is there and the keep stands.
	// After it the vector table is gone, and the keep is void
	for (const [at, stands] of [[0, true], [1, false]]) {
		const c = await routed("rfoundry");
		const cp = routedPage(c, { decision: c.kept });
		const cbox = dfuse.writeView();
		c.fake.enterBootloader();
		await cp.got.forward.click();
		const before = c.fake.flash.slice(), go = c.fake.execute.bind(c.fake);
		let erases = 0;
		c.fake.execute = (cmd) => {
			if (cmd.kind !== "erase" || erases++ !== at) return go(cmd);
			c.fake.mode = "off";	// the cable goes before this erase runs
		};
		await cp.got.forward.click();
		c.fake.execute = go;
		assert.equal(cp.got.next, 0, `cut at erase ${at}`);
		assert.equal(c.fake.erased.length, at);
		if (stands) assert.deepEqual(c.fake.flash, before, "nothing changed");
		c.secs.forEach((sec) => assert.deepEqual(within(c, sec), before.subarray(sec.addr - c.board.rom_base, sec.addr - c.board.rom_base + sec.size)));
		c.fake.enterBootloader();
		await cp.got.forward.click();
		assert.equal(noVectors(c), !stands);
		assert.equal(text(cbox).includes(en["flash.dfuse.codec_unknown"]), !stands, `cut at erase ${at}`);
		assert.equal(cp.got.forward === null, !stands);
		if (stands) assert.equal(text(cp.got.forward), en["flash.write.go"]);
	}
	// without a copy or a file the same cut leaves the void state, with nothing more erased
	const n = await routed("rfoundry");
	const np = routedPage(n, { decision: n.kept });
	const nbox = dfuse.writeView();
	n.fake.enterBootloader();
	await np.got.forward.click();
	const run = n.fake.execute.bind(n.fake);
	n.fake.execute = (c) => {
		if (c.kind !== "write") return run(c);
		n.fake.mode = "off";
	};
	await np.got.forward.click();
	n.fake.execute = run;
	n.fake.enterBootloader();
	const erased = n.fake.erased.length;
	await np.got.forward.click();
	assert.ok(text(nbox).includes(en["flash.dfuse.codec_unknown"]));
	assert.deepEqual([np.got.forward, n.fake.erased.length], [null, erased]);
});

test("a merge cut anywhere in the codec's sectors leaves no vector table, and the same install again writes them whole", withRoute, async () => {
	const probeOf = async (r) => (await probe(new Link(r.fake.serialStream()))).codec;
	const first = await routed("blank");
	const merge = { mode: MERGE, source: "stock", why: "test", whyKey: "flash.codec.why.merge_selected", expectCodec: true };
	routedPage(first, { decision: merge, stock: first.stock });
	const plan = dfuse.installPlan(first.board, { manifest: first.manifest, image: first.image }, merge, first.sim.codecSegments, first.sim.profile);
	// the block numbers, in the order sent, of the codec sectors' first block, one in the middle, the descriptor's
	// block (the last of them) and the one before it
	const order = [...plan.writeOrder.slice(0, -1).flatMap((st) => st.blocks.map(([n]) => st.sector.addr + (n - FIRST_BLOCK) * BLOCK)),
		...plan.writeOrder.at(-1).blocks.slice(1).map(([n]) => first.board.app_base + (n - FIRST_BLOCK) * BLOCK), first.board.app_base];
	const codecAt = order.map((a, i) => [a, i]).filter(([a]) => first.secs.some((s) => a >= s.addr && a < s.addr + s.size)).map(([, i]) => i);
	const d = first.sim.fb.descriptor, desc = order.indexOf(d.addr + d.len - BLOCK);
	assert.equal(desc, codecAt.at(-1), "the descriptor's block is the last one sent to the codec's sectors");
	for (const k of [codecAt[0], codecAt[Math.floor(codecAt.length / 2)], desc - 1, desc]) {
		const r = await routed("blank");
		const { got } = routedPage(r, { decision: merge, stock: r.stock });
		const box = dfuse.writeView();
		box.isConnected = true;
		await until(() => got.forward);
		r.fake.enterBootloader();
		await got.forward.click();
		const execute = r.fake.execute.bind(r.fake);
		let blocks = 0;
		r.fake.execute = (c) => {
			if (c.kind !== "write" || blocks++ !== k) return execute(c);
			r.fake.mode = "off";	// the cable goes before block k is programmed
		};
		await got.forward.click();
		r.fake.execute = execute;
		assert.equal(got.next, 0, `cut at block ${k}`);
		assert.ok(noVectors(r), `cut at block ${k}: no vector table`);
		assert.ok(!holdsCodec(r));
		// the descriptor's place is blank until the last block of its sector: no descriptor over a half-written codec
		assert.ok(r.fake.flash.subarray(d.addr - r.board.rom_base, d.addr - r.board.rom_base + d.len).every((x) => x === 0xFF), `cut at block ${k}: no descriptor`);
		assert.ok(text(box).includes(en["flash.write.failed"].split("{error}")[0].trim()));
		// again: the loader shows a blank start, the merge stands and runs whole
		r.fake.enterBootloader();
		await got.forward.click();
		assert.equal(text(got.forward), en["flash.write.go"]);
		await got.forward.click();
		assert.equal(got.next, 1, `after a cut at block ${k}`);
		assert.ok(holdsCodec(r));
		assert.deepEqual(r.fake.violations, []);
		assert.equal(await probeOf(r), r.board.codec.id);
	}
	// a codec block the loader takes with no error and does not program: the read-back finds it, and the vector table
	// is not written
	const s = await routed("blank");
	const sp = routedPage(s, { decision: merge, stock: s.stock });
	dfuse.writeView();
	await until(() => sp.got.forward);
	s.fake.enterBootloader();
	await sp.got.forward.click();
	const at = s.sim.codecSegments[0][0] - (s.sim.codecSegments[0][0] % BLOCK);	// the block the code starts in
	const run = s.fake.execute.bind(s.fake);
	s.fake.execute = (c) => {
		if (c.kind === "write" && s.fake.addr === at) { s.fake.state = 5; return; }	// dfuDNLOAD-IDLE, nothing programmed
		return run(c);
	};
	await sp.got.forward.click();
	assert.equal(sp.got.next, 0);
	const said = String(sp.got.finished[0]?.message).match(/^the flash at (0x[0-9A-F]{8}) does not read back.*The vector table is not written yet/);
	assert.ok(said && Number(said[1]) >= at && Number(said[1]) < at + BLOCK, `${sp.got.finished[0]?.message} (the block at ${hex(at)})`);
	assert.ok(noVectors(s));
	assert.equal(s.fake.exited, false);
});

test("the write step reached without the codec step's decision decides from what the radio reported, or refuses with nothing asked", withRoute, async () => {
	// a radio that reported its codec: kept
	const r = await routed("rfoundry");
	const { got, state } = routedPage(r, { now: { kind: "rfoundry", probe: { board: r.board.debuglink_names[0], codec: r.board.codec.id } } });
	const box = dfuse.writeView();
	assert.equal(state.decision.mode, KEEP);
	assert.equal(got.decisions, 1);
	assert.equal(text(got.forward), en["flash.write.connect"]);
	assert.ok(text(box).includes("codec mode: keep"));
	// nothing known and no file: no plan, no Connect
	const n = await routed("stock");
	const np = routedPage(n);
	const nbox = dfuse.writeView();
	assert.match(text(nbox), /cannot determine whether the radio has its codec/);
	assert.equal(np.got.forward, null);
	assert.ok(!text(nbox).includes(en["flash.write.then_connect"]));
	// a board without a codec route has no decision and needs none
	const p = await installable("rfoundry");
	const pp = page(p.fake, p.sim, p.image, p.manifest);
	dfuse.writeView();
	assert.deepEqual([pp.state.decision, pp.got.decisions], [undefined, 0]);
	assert.equal(dfuse.voidKeep(null, { stage: "protected", blank: false }), null);
});

// rfoundry's settle_keep, row for row
test("the keep is settled by what the loader says: void when protected or without a vector table, and only a keep", () => {
	const loaders = [["protected", false], ["ready", true], ["ready", false]];
	const want = { [KEEP]: ["flash.dfuse.codec_erased", "flash.dfuse.codec_unknown", null], [MERGE]: [null, null, null], [BLANK]: [null, null, null] };
	for (const [mode, keys] of Object.entries(want))
		loaders.forEach(([stage, blank], i) => assert.equal(dfuse.voidKeep({ mode }, { stage, blank }), keys[i], `${mode}, ${stage}, blank ${blank}`));
	for (const k of ["flash.dfuse.codec_erased", "flash.dfuse.codec_unknown", "flash.dfuse.install_no_codec", "flash.nocodec.back"]) assert.ok(en[k], k);
});

// A maker's file picked where the RadioFoundry image goes is never written as one: a file of either container, whole
// or cut, and a maker's bootloader file
test("a maker's firmware file is not an image: both containers, a cut file and a bootloader file are refused", async () => {
	const refusal = /this is a stock firmware file, not a RadioFoundry image/;
	const files = [["a file of the first container", bin("stock.bin"), stockfw.OUTSECURITY]];
	const T = fx.twin;
	if (T) {
		files.push(["a file of the second container", bin(T.file), stockfw.TWIN], ["the same, cut short", bin(T.file).subarray(0, 0x4000), stockfw.TWIN],
			["the same, padded", new Uint8Array([...bin(T.file).subarray(0, 0x8000), ...new Uint8Array(4096)]), stockfw.TWIN],
			["a bootloader file of that container", bin(T.boot.file), stockfw.TWIN]);
	}
	for (const b of [board, realBoard, fbReal.boards[keyed.flash.board]]) {
		for (const [what, data, kind] of files) {
			assert.equal(stockfw.containerKind(data, fbReal), kind, what);
			const fbB = { ...fbReal, boards: { ...fbReal.boards, [b.id]: b } };
			await assert.rejects(async () => verify(await forDevBuild(b, data), data, fbB, b.id), refusal, `${what} for ${b.id}`);
			// and where the page has not been told which radio the image is for
			await assert.rejects(async () => verify(await forDevBuild(b, data), data, fbB), refusal, `${what}, no radio named`);
		}
	}
	// an image is none of them: its words at those offsets are vector table entries
	const image = testImage(8192);
	assert.equal(stockfw.containerKind(image, fbReal), null);
	assert.equal((await verify(await forDevBuild(board, image), image, fb, board.id)).id, board.id);
	// the page's own-image picker makes the same test before it takes a file for an image (app.js needs the DOM)
	const src = readFileSync(fileURLToPath(new URL("../static/js/flasher/app.js", import.meta.url)), "utf8");
	const own = src.slice(src.indexOf("async function loadOwnImage("));
	assert.match(own.slice(0, own.indexOf("\n}\n")), /if \(stockfw\.containerKind\(f\.data, fb\(\)\)\) \{\n\t\tconst st = await pickedStock\(f\);\n\t\tconst kept = stockForRadio\(st\) && !st\.problem;/);
});

test("the maker's recovery is said in words", async () => {
	const { state } = page(null, await romSimulation(fb, board.id), new Uint8Array(4), {});
	state.fakeRadio = null;
	const el = backendOf(radio).recovery.maker();
	assert.ok(text(el).includes(en["flash.dfuse.maker_text"]) && text(el).includes(en["flash.dfuse.maker_loader"]));
});

test("every string the route uses exists, and every one of its strings is used", () => {
	const src = ["flasher/dfuse.js", "flasher/app.js", "flasher/manual.js"].map((f) => readFileSync(fileURLToPath(new URL(`../static/js/${f}`, import.meta.url)), "utf8")).join("\n");
	const used = new Set([...src.matchAll(/"(flash\.dfuse\.[a-z_]+)"/g)].map((m) => m[1]));
	const have = new Set(Object.keys(en).filter((k) => k.startsWith("flash.dfuse.")));
	assert.deepEqual([...used].sort(), [...have].sort());
	assert.ok(en["flash.win.step3_by_id"].includes("{usb_id}") && !en["flash.win.step3_by_id"].includes("{product}"));
});

// app.js needs the DOM, so what it must keep is checked in its source
test("the page registers the backend, practices against the ROM loader, and expects no codec where the radio has none", () => {
	const src = readFileSync(fileURLToPath(new URL("../static/js/flasher/app.js", import.meta.url)), "utf8");
	assert.match(src, /import \* as dfuse from "\.\/dfuse\.js";/);
	assert.match(src, /dfuse\.init\(\{ S, fb, next, render, reset, [^}]*connectBootloader, dropSerial,\s*[^}]*doneView, backupView, restoreView \}\);/);
	assert.match(src, /new FakeRomRadio\(S\.sim, \{ state: simStock \? "stock" : "rfoundry"/);
	// practice mode shows a refused restart into the loader, then the restart itself
	assert.match(src, /new FakeRomRadio\(S\.sim, \{[^}]*restartRefusals: 1 \}\);/);
	assert.match(src, /S\.sim = \{ \.\.\.\(await romSimulation\(S\.data\.fb, simRadio\.flash\.board\)\), radio: simRadio\.id \};/);
	// the codec step on this route: its view and what it has go to the backend, the steps are asked for the radio, a
	// radio without a bootloader key needs none, and an install without a "current firmware" step takes the backend's word
	assert.match(src, /dfuse\.init\(\{[^}]*notePlan, noteDecision, codecView, confirmNoCodec, stockGivesCodec, savedKept, failureNotice,/);
	assert.match(src, /backendOf\(S\.radio\)\.steps\(S\.task, \{ recovering: !!S\.recovering, radio: S\.radio \}\)/);
	const codec = src.slice(src.indexOf("function codecView("));
	const view = codec.slice(0, codec.indexOf("\n}\n"));
	assert.match(view, /const noKey = !S\.board\.key_sha256;/);
	assert.match(view, /const keyStored = !noKey && \(S\.sim \|\| !!S\.key \|\| !!savedKey\(S\.board\)\);\n\t\tconst keyKnown = noKey \|\| keyStored \|\| /);
	assert.match(view, /const now = S\.now \|\| \(S\.task === "install" && backend\(\)\.installNow \? nowFor\(backend\(\)\.installNow\) : null\);/);
	assert.match(view, /probe: now\?\.kind === "rfoundry" \? now\.probe : null,\n\t\t\t\tstockInstalled: now\?\.kind === "stock" \? now\.version : null,\n\t\t\t\trunsStock: now\?\.kind === "stock",/);
	assert.match(view, /!openfwRecognizedNow\(\) && !backend\(\)\.installNow\) parts\.push\(h\("p", \{ class: "small muted" \}, t\("flash\.codec\.or_connect"\)\)\);/);
	assert.match(src, /const hasCodec = \(\) => !S\.radio\?\.flash\.codec_none;/);
	const done = src.slice(src.indexOf("function doneView("));
	assert.match(done.slice(0, done.indexOf("\n}\n")), /const good = !hasCodec\(\) \|\| /);
	// a restore offers only what the radio's backend takes back; the original firmware by the maker's procedure is a
	// stage of its own in the help task
	assert.match(src, /const can = backend\(\)\.restores\?\.targets;\n\tconst avail = targets\(b\)\.filter\(\(tg\) => !can \|\| can\.includes\(tg\)\);/);
	assert.match(src, /if \(pick === "stock" && rec\.stock === "maker"\) return go\("maker"\);/);
	// the memory chip is claimed only after a backup that holds all of it, and a backend without a channel step gets
	// none: neither the promise of one nor the read and the claim behind it
	assert.match(src, /const whole = pick === "all" && !!S\.backupParts\.find\(\(x\) => x\.what === "spi-flash"\)\?\.complete;/);
	assert.match(src, /\n\t\t\tif \(whole\) await prepareStorage\(status, \{ whole \}\);\n/);
	assert.ok(!/if \(pick === "all"\) await prepareStorage/.test(src));
	assert.match(src, /const bringsChannels = \(\) => backend\(\)\.stockChannels !== false;/);
	const bring = src.slice(src.indexOf("async function bringChannels("));
	assert.match(bring.slice(0, bring.indexOf("connectSerial")), /^async function bringChannels\(status, \{ finish \}\) \{\n\tif \(!bringsChannels\(\)\) \{\n\t\tS\.forward\(finish\(\)\);\n\t\treturn;\n\t\}/);
	assert.match(src, /when === "after" && bringsChannels\(\) && /);
	assert.ok(en["flash.storage.not_prepared"]);
	// the site's radio page offers the flasher for the backend (build.py can_flash)
	const py = readFileSync(fileURLToPath(new URL("../rfoundrysite/build.py", import.meta.url)), "utf8");
	assert.match(py, /fl\["backend"\] == "dfuse" and bool\(self\.flash\["boards"\]\.get\(fl\["board"\], \{\}\)\.get\("dfu_ids"\)\)/);
});
