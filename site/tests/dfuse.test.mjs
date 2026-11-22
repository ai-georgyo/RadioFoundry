// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A radio installed through its processor's own ROM bootloader by plain DfuSe (dfu.js runPlain, dfuse.js), against
// the simulated loader (fake.js FakeRomRadio): the device check, the read protection probe, the confirmed Read
// Unprotect, the plain write and the leave; the backend's contract; and the write step drawn against a small stand-in
// for the DOM. The board is the registry fixture's first one of the family, found by what it is, not by name.
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

const { Dfu, DfuError, WrongDeviceError, WebUsbTransport, romDevice, checkPlain, PLAIN_FAMILY, FLASH_WORD, KEY_LEN } = await import("../static/js/flasher/dfu.js");
const { makePlan, KEEP, BLOCK, FIRST_BLOCK } = await import("../static/js/flasher/plan.js");
const { forDevBuild, segments, verify } = await import("../static/js/flasher/manifest.js");
const { FakeRadio, FakeRomRadio, romSimulation, romLayout, simulation, randomBytes, rng } = await import("../static/js/flasher/fake.js");
const { Link, probe } = await import("../static/js/flasher/link.js");
const { ProtectedError } = await import("../static/js/flasher/protect.js");
const { backendOf } = await import("../static/js/flasher/backends.js");
const { hex } = await import("../static/js/flasher/bytes.js");
const { supportReport } = await import("../static/js/support.js");
const dfuse = await import("../static/js/flasher/dfuse.js");
const { stopBars } = await import("../static/js/flasher/dom.js");

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const fb = json("flash-boards.json");
const radios = json("radios.json").radios;
// the first radio of the family, and a keyed one to tell it from
const radio = radios.find((r) => fb.boards[r.flash.board]?.family === PLAIN_FAMILY);
const keyed = radios.find((r) => fb.boards[r.flash.board]?.family === "usb-dfu");
assert.ok(radio, "a radio of the plain DfuSe family in the fixtures");
assert.ok(keyed, "a keyed USB DFU radio in the fixtures");
const board = fb.boards[radio.flash.board];
const noWait = { wait: async () => {} };

// An image with a vector table: a RAM stack pointer and a reset vector, then firmware-like bytes. 150 KB: two sectors
function testImage(len = 150 * 1024 + 20) {
	const img = randomBytes(rng(7), len);
	img.set([0xFC, 0xFF, 0x01, 0x20, 0x01, 0x02, 0x00, 0x08], 0);
	return img;
}
async function installable(state = "stock", opts = {}) {
	const sim = romSimulation(fb, board.id);
	const fake = new FakeRomRadio(sim, { state, spiSize: 0x200000, ...opts });
	const image = testImage();
	const manifest = await forDevBuild(sim.board, image);
	return { sim, fake, image, manifest, plan: dfuse.installPlan(sim.board, { manifest, image }) };
}

test("the family's board: a ROM loader at the flash base, no key, no codec, nothing protected", () => {
	assert.equal(board.app_base, board.rom_base, "the application starts at the flash base");
	assert.equal(board.key_sha256, null);
	assert.deepEqual(board.protected, []);
	assert.deepEqual(board.codec.windows, []);
	assert.equal(board.sectors[0].addr, board.rom_base);
	assert.equal(board.sectors.reduce((n, s) => n + s.size, 0), board.flash_end - board.rom_base);
	assert.equal(radio.flash.backend, "dfuse");
	assert.equal(typeof radio.flash.codec_none, "string");
	assert.ok(radio.flash.bootloader.length && radio.flash.notice);
	// every other radio has a codec route: the key is there, null
	for (const r of radios) if (r !== radio) assert.equal(r.flash.codec_none, null, r.id);
});

test("backend: install is radio, build, write, done; an update and a reinstall skip what they know", () => {
	const b = backendOf(radio);
	assert.equal(b.id, "dfuse");
	assert.deepEqual(b.steps("install", { recovering: false }), ["radio", "build", "write", "done"]);
	assert.deepEqual(b.steps("install", { recovering: true }), ["build", "write", "done"]);
	assert.deepEqual(b.steps("update", {}), ["update", "write", "done"]);
	for (const task of ["backup", "restore", "restart", "help"]) assert.equal(b.steps(task, {}), null, task);
	for (const s of ["write", "done", "backup", "restore"]) assert.equal(typeof b.views[s], "function", s);
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
		assert.deepEqual(sent, [[1, 0, [0x21, ...[0, 8, 16, 24].map((sh) => (board.rom_base >>> sh) & 0xFF)]]], "only the address pointer was set");
	}
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
	const short = { board: sim.board, erase: plan.erase, eraseOrder: plan.eraseOrder, writes: [{ sector: plan.writes[0].sector, blocks: [[FIRST_BLOCK, new Uint8Array(1000)]] }],
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
function page(fake, sim, image, manifest, extra = {}) {
	const got = { next: 0, started: [], finished: [], marks: [], forward: undefined };
	const state = { radio, board: sim.board, task: "install", firmware: { manifest, image, origin: "custom" }, fakeRadio: fake,
		forward: (b) => { got.forward = b; } };
	dfuse.init({ S: state, fb: () => sim.fb, next: () => { got.next++; }, render() {}, reset() {}, helpLink: () => null, manualKitLine: () => null,
		flashStarted: (what) => { got.started.push(what); return { id: 1 }; }, flashFinished: (s, e = null) => { got.finished.push(e); },
		flashMark: (s, ev) => { got.marks.push(ev); }, errorText: (e) => e.message,
		connectBootloader: async () => {
			if (fake.mode !== "bootloader") throw new Error("the radio is not in update mode");
			return fake.dfuTransport();
		},
		dropSerial: async () => {}, withPicture: (x) => x, driverHelp: () => null, driverWorked() {}, ensureFirmware: async () => state.firmware, notePlan() {},
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

test("the maker's recovery is said in words", () => {
	const { state } = page(null, romSimulation(fb, board.id), new Uint8Array(4), {});
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
