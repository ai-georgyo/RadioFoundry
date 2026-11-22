// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The manual install kit (manual.js): which radios get one and where, its README per install route, its files, the
// tool zip's check against site.json, and the panel and line the page shows, against a small stand-in for the DOM.
// Run with site/tests/run.sh.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { sha256 } from "../static/js/flasher/bytes.js";
import { zip, unzip } from "../static/js/flasher/zip.js";

// ---- just enough DOM for dom.js h()
class El {
	constructor(tag) {
		this.tagName = tag.toUpperCase();
		this.children = [];
		this.attrs = {};
		this.listeners = {};
		this.className = "";
		this.dataset = {};
	}
	append(...cs) { for (const c of cs) this.children.push(c instanceof El ? c : { text: String(c) }); }
	replaceChildren(...cs) { this.children = []; this.append(...cs); }
	setAttribute(k, v) { this.attrs[k] = v; }
	addEventListener(ev, f) { (this.listeners[ev] ||= []).push(f); }
	get textContent() { return this.children.map((c) => (c instanceof El ? c.textContent : c.text)).join(""); }
}
const strings = readFileSync(fileURLToPath(new URL("../i18n/en.json", import.meta.url)), "utf8");
globalThis.Node = El;
globalThis.document = { createElement: (t) => new El(t), body: new El("body"), getElementById: (id) => (id === "i18n" ? { textContent: strings } : null) };
const en = JSON.parse(strings);
const all = (el) => [el, ...el.children.filter((c) => c instanceof El).flatMap(all)];

const kit = await import("../static/js/flasher/manual.js");

const DIR = process.env.RFOUNDRY_SITE_FIXTURES || "build/site-tests";
const json = (n) => JSON.parse(readFileSync(join(DIR, n), "utf8"));
const fb = json("flash-boards.json");
const radios = Object.fromEntries(json("radios.json").radios.map((r) => [r.id, r]));
const SITE = "https://radiofoundry.org/";
const REPO = "https://github.com/radiofoundry/radiofoundry";
const COMMUNITY = "https://community.example/";
const manifest = (board) => ({ format: "rfoundry-release/1", board, version: "v0.2.0", variant: "full", image: { file: `rfoundry-${board}-v0.2.0.bin`, size: 4, sha256: "x" } });
// one radio of each kind, found by what it is, not by name
const all_ = Object.values(radios);
const family = (r) => fb.boards[r.flash.board].family;
const files = (id) => fb.boards[radios[id].flash.board].codec.sources.filter((n) => /\.bin$/i.test(n));
const pick = (f, what) => {
	const r = all_.find(f);
	assert.ok(r, `no radio in the fixtures is ${what}`);
	return r.id;
};
const DFU = pick((r) => family(r) === "usb-dfu" && r.flash.backend === "anyroad-dfu" && files(r.id).length && r.flash.bootloader.length, "a USB DFU radio");
const SERIAL = pick((r) => family(r) === "serial-loader" && r.flash.backend === "c7000-stub" && files(r.id).length && r.flash.bootloader.length, "a cable radio with stock files");
const READOUT = pick((r) => family(r) === "serial-loader" && !files(r.id).length, "a cable radio whose codec comes from a program flash read");
const NONE = pick((r) => !["usb-dfu", "serial-loader"].includes(family(r)) && r.flash.backend === "c7000-stub", "a radio rfoundry doesn't install");
const PLAIN = pick((r) => family(r) === "dfuse" && r.flash.backend === "dfuse" && r.flash.bootloader.length, "a radio installed through its processor's ROM bootloader");
const brd = (id) => fb.boards[radios[id].flash.board];
const readme = (id, stock) => kit.kitReadme({ radio: radios[id], board: brd(id), base: `rfoundry-${id}-v0.2.0`,
	manifest: manifest(id), stock, site: SITE, repo: REPO, community: COMMUNITY, git: "abcd1234", date: "2026-10-26" });

test("kit: a route for each radio rfoundry installs, none for the RAM stub radios", () => {
	for (const r of Object.values(radios)) {
		const family = fb.boards[r.flash.board].family;
		assert.equal(kit.kitRoute(r), { "usb-dfu": "dfu", "serial-loader": "serial", dfuse: "dfuse" }[family] || null, r.id);
	}
	assert.equal(kit.kitRoute(radios[DFU]), "dfu");
	assert.equal(kit.kitRoute(radios[SERIAL]), "serial");
	assert.equal(kit.kitRoute(radios[PLAIN]), "dfuse");
	assert.equal(kit.kitRoute(radios[NONE]), null);
});

test("kit: offered at the step that needs the API the browser lacks", () => {
	const none = { usb: false, serial: false }, both = { usb: true, serial: true };
	assert.equal(kit.missingApi(radios[DFU], "install", "write", none), "usb");
	assert.equal(kit.missingApi(radios[DFU], "install", "write", { usb: true, serial: false }), null);
	assert.equal(kit.missingApi(radios[DFU], "install", "write", { usb: true, serial: false, stub: true }), "serial");
	assert.equal(kit.missingApi(radios[DFU], "install", "write", both), null);
	for (const step of ["radio", "build", "now", "codec", "done"]) assert.equal(kit.missingApi(radios[DFU], "install", step, none), null, step);
	assert.equal(kit.missingApi(radios[DFU], "backup", "write", none), null);
	assert.equal(kit.missingApi(radios[SERIAL], "install", "connect", none), "serial");
	assert.equal(kit.missingApi(radios[SERIAL], "install", "connect", both), null);
	assert.equal(kit.missingApi(radios[SERIAL], "install", "build", none), null);
	// a radio without a kit is blocked too: the panel then says rfoundry can't install it
	assert.equal(kit.missingApi(radios[NONE], "install", "connect", none), "serial");
	// a ROM bootloader radio: its write step needs USB, never the serial port (no loader route), whatever stub says
	assert.equal(kit.missingApi(radios[PLAIN], "install", "write", none), "usb");
	assert.equal(kit.missingApi(radios[PLAIN], "install", "write", { usb: false, serial: true, stub: true }), "usb");
	assert.equal(kit.missingApi(radios[PLAIN], "install", "write", { usb: true, serial: false }), null);
	for (const step of ["radio", "build", "done"]) assert.equal(kit.missingApi(radios[PLAIN], "install", step, none), null, step);
	assert.equal(kit.missingApi(radios[PLAIN], "backup", "write", none), null);
});

test("kit: file names", () => {
	assert.equal(kit.kitBase(manifest("b1"), "b1"), "rfoundry-b1-v0.2.0");
	assert.equal(kit.kitBase({ image: { file: "local" } }, "b2"), "rfoundry_b2");
	assert.equal(kit.kitBase({ image: { file: "../x.bin" } }, "b2"), "rfoundry_b2");
	assert.equal(kit.kitBase(null, "b3"), "rfoundry_b3");
	assert.equal(kit.kitName({ name: "Maker X-1 (Pro)" }, new Date("2026-10-26T12:00:00Z")), "RadioFoundry_Maker-X-1-Pro_manual_install_2026-10-26.zip");
});

test("kit README: a USB DFU radio with its stock file", () => {
	const r = readme(DFU, files(DFU)[0]);
	const cmd = `rfoundry.py flash "firmware/rfoundry-${DFU}-v0.2.0.bin" --stock "stock/${files(DFU)[0]}"`;
	assert.ok(r.includes(`venv/bin/python ${cmd} --dry-run`), r);
	assert.ok(r.includes(`venv\\Scripts\\python ${cmd} --dry-run`));
	assert.ok(r.includes(`venv/bin/python ${cmd}\n`));
	assert.ok(!r.includes("--port"));
	assert.ok(r.includes("Zadig") && r.includes("50-rfoundry-dfu.rules") && r.includes("dialout"));
	assert.ok(r.includes("py -m venv venv") && r.includes("-m pip install -r requirements.txt"));
	for (const s of radios[DFU].flash.bootloader) assert.ok(r.includes(s), s);
	assert.ok(r.includes("the codec and the bootloader key"));
	assert.ok(r.includes(`${SITE}docs/user/20-troubleshooting.html#command-line-installs-advanced`));
	assert.ok(r.includes(`Ask the community: ${COMMUNITY}`));
	assert.ok(r.includes(`Report a bug: ${REPO}/issues`));
	assert.ok(r.includes("v0.2.0, full"));
	assert.ok(!/\bodr\b|colour|behaviour/i.test(r));
});

// What rfoundry needs, as its package says (tools/rfoundry/pyproject.toml) and the tool zip's requirements.txt lists
const PYPROJECT = readFileSync(fileURLToPath(new URL("../../tools/rfoundry/pyproject.toml", import.meta.url)), "utf8");
const deps = (text) => [...text.matchAll(/^\s*"?([A-Za-z][\w-]*)(?:[<>=!~][^"\n]*)?"?,?\s*$/gm)].map((m) => m[1]);

test("kit README: the setup covers Python, every package and each system's drivers", () => {
	assert.equal(PYPROJECT.match(/requires-python = ">=([\d.]+)"/)[1], kit.PYTHON_MIN);
	const wanted = deps(PYPROJECT.match(/dependencies = \[([^\]]*)\]/)[1]);
	assert.deepEqual(kit.PACKAGES.map(([n]) => n).sort(), wanted.sort());
	const reqs = readFileSync(fileURLToPath(new URL("../rfoundrysite/toolzip.py", import.meta.url)), "utf8").match(/REQUIREMENTS = """\\\n([^]*?)"""/)[1];
	assert.deepEqual(deps(reqs.replace(/^#.*$/gm, "")).sort(), wanted.sort(), "requirements.txt in the tool zip");
	for (const id of [DFU, SERIAL, READOUT]) {
		const r = readme(id, null);
		const setup = r.slice(r.indexOf("1. SET UP"), r.indexOf("\n2. "));
		assert.ok(setup.includes(`Python ${kit.PYTHON_MIN} or later`), setup);
		assert.ok(setup.includes("https://www.python.org/downloads/") && setup.includes("Add python.exe to PATH"));
		for (const n of wanted) assert.ok(setup.includes(n), n);
		assert.ok(setup.includes("py -m venv venv") && setup.includes("venv\\Scripts\\python -m pip install -r requirements.txt"));
		assert.equal(setup.split("python3 -m venv venv").length - 1, 2, "macOS and Linux make the venv");
		assert.equal(setup.split("venv/bin/python -m pip install -r requirements.txt").length - 1, 2);
		assert.ok(setup.includes("python3-venv") && setup.includes("usermod -aG dialout"));
		assert.ok(setup.includes("macOS has the cable drivers"));
		const dfu = family(radios[id]) === "usb-dfu";
		assert.equal(setup.includes("Zadig") && setup.includes("WinUSB"), dfu);
		assert.equal(setup.includes("50-rfoundry-dfu.rules"), dfu);
		if (dfu) {
			const [vid, pid] = brd(id).dfu_ids;
			assert.ok(setup.includes(`ATTRS{idVendor}=="${vid.toString(16).padStart(4, "0")}", ATTRS{idProduct}=="${pid.toString(16).padStart(4, "0")}"`));
		} else assert.ok(setup.includes("Device Manager lists it under Ports") && setup.includes("/dev/ttyUSB0"));
	}
});

test("kit README: a USB DFU radio without a stock file says where to put it", () => {
	const r = readme(DFU, null);
	assert.ok(r.includes("THE STOCK FILE"));
	assert.ok(r.includes(`--stock "stock/${files(DFU)[0]}"`));
});

test("kit README: a serial radio with its stock file", () => {
	const r = readme(SERIAL, files(SERIAL)[0]);
	assert.ok(r.includes(`venv/bin/python rfoundry.py flash "firmware/rfoundry-${SERIAL}-v0.2.0.bin" --stock "stock/${files(SERIAL)[0]}" --port PORT --dry-run`), r);
	assert.ok(!r.includes("Zadig") && !r.includes("50-rfoundry-dfu.rules"));
	assert.ok(r.includes("/dev/ttyUSB0") && r.includes("/dev/cu.usbserial-") && r.includes("COM3"));
	assert.ok(r.includes("serial.tools.list_ports"));
	assert.ok(r.includes("update mode"));
	for (const s of radios[SERIAL].flash.bootloader) assert.ok(r.includes(s), s);
	assert.ok(!r.includes("stubtool.py"));
});

test("kit README: a radio whose codec comes from a program flash read gets the read first", () => {
	const r = readme(READOUT, null);
	assert.ok(r.includes(`stub/stubtool.py PORT --board ${radios[READOUT].flash.board} --progmem "stock/progmem.bin"`), r);
	assert.ok(r.includes('--stock "stock/progmem.bin" --port PORT'));
	for (const step of radios[READOUT].flash.bootloader) assert.ok(r.includes(step), step);
	assert.ok(!r.includes("THE STOCK FILE"));
});

test("kit README: a ROM bootloader radio has no stock file, no key and no codec, and says what a first install erases", () => {
	// a stock file left from another radio's visit changes nothing
	for (const stock of [null, "LEFT-OVER.bin"]) {
		const r = readme(PLAIN, stock);
		const cmd = `rfoundry.py flash "firmware/rfoundry-${PLAIN}-v0.2.0.bin"`;
		assert.ok(r.includes(`venv/bin/python ${cmd} --dry-run\n`), r);
		assert.ok(r.includes(`venv\\Scripts\\python ${cmd} --dry-run\n`));
		assert.ok(r.includes(`venv/bin/python ${cmd}\n`));
		// the unprotect is a command of its own, explained before it is given
		assert.ok(r.includes(`venv/bin/python ${cmd} --unprotect\n`) && r.includes(`venv\\Scripts\\python ${cmd} --unprotect\n`));
		assert.ok(r.indexOf("erases the radio's whole program memory") < r.indexOf("--unprotect"), "what it erases comes first");
		assert.ok(r.indexOf("READ THIS BEFORE A FIRST INSTALL") < r.indexOf("1. SET UP"));
		assert.ok(r.includes("the maker's") && r.includes("rfoundry can't put them back"));
		// the erase wait as rfoundry has it: off only when rfoundry says so, and no fixed time that could end early
		const flat = r.replace(/\n/g, " ");
		assert.ok(flat.includes("leave it on and connected until rfoundry says to switch it off"), r);
		assert.ok(!flat.includes("about a minute"), r);
		// none of the keyed route's texts: no stock file, no codec, no key, no claim about a bootloader in flash
		assert.ok(!r.includes("--stock") && !r.includes("--port") && !r.includes("stock/"), r);
		assert.ok(!r.includes("THE STOCK FILE") && !r.includes("LEFT-OVER"));
		assert.ok(!/bootloader key|gives the codec|the codec and/.test(r));
		assert.ok(!r.includes("Nothing here writes the radio's bootloader"));
		assert.ok(r.replace(/\n/g, " ").includes("its processor's own bootloader, which is part of the chip and can't be erased"));
		for (const s of radios[PLAIN].flash.bootloader) assert.ok(r.includes(s), s);
		// the board's own notice, wrapped, word for word
		assert.ok(r.replace(/\n/g, " ").includes(radios[PLAIN].flash.notice));
		// prose is wrapped; only a command line (indented) may be longer
		assert.ok(r.split("\n").every((l) => l.length <= 110 || l.startsWith("  ")), "no line of prose runs away");
		assert.ok(r.includes(`${SITE}docs/user/20-troubleshooting.html#command-line-installs-advanced`));
		assert.ok(r.includes(`Report a bug: ${REPO}/issues`));
		assert.ok(!/\bodr\b|colour|behaviour/i.test(r));
	}
});

test("kit README: a ROM bootloader radio's setup is the USB one, its device found by USB ID while its name is unknown", () => {
	const b = brd(PLAIN);
	const [vid, pid] = b.dfu_ids;
	const hex4 = (n) => n.toString(16).toUpperCase().padStart(4, "0");
	const r = readme(PLAIN, null);
	const setup = r.slice(r.indexOf("1. SET UP"), r.indexOf("\n2. "));
	assert.ok(setup.includes("Zadig") && setup.includes("WinUSB") && setup.includes("50-rfoundry-dfu.rules"));
	assert.ok(setup.includes(`ATTRS{idVendor}=="${hex4(vid).toLowerCase()}", ATTRS{idProduct}=="${hex4(pid).toLowerCase()}"`));
	assert.ok(!setup.includes("Device Manager lists it under Ports") && !setup.includes("/dev/ttyUSB0"));
	// the keyed bootloader's product name is never offered for another family's device
	assert.ok(!r.includes("Digital Radio in USB mode"));
	if (b.dfu_product) assert.ok(setup.includes(`"${b.dfu_product}"`));
	else assert.ok(setup.includes(`pick the device with USB ID ${hex4(vid)} ${hex4(pid)}.`), setup);
	// once the name was read on a radio it is used
	const named = kit.kitSetup("dfuse", { ...b, dfu_product: "A Loader's Name" }).join("\n");
	assert.ok(named.includes('pick "A Loader\'s Name" (USB ID'));
	// the keyed route's setup is as it was
	assert.ok(kit.kitSetup("dfu", brd(DFU)).join("\n").includes(`pick "${brd(DFU).dfu_product || "Digital Radio in USB mode"}" (USB ID`));
});

test("kit README: none for a radio rfoundry can't install", () => {
	assert.throws(() => readme(NONE, null), kit.KitError);
});

// a stand-in for downloads/rfoundry-tool.zip and the fetch that serves it
async function toolSite(entries = [{ name: "rfoundry.py", data: "print()\n" }, { name: "tools/rfoundry/rfoundry/cli.py", data: "x = 1\n" }]) {
	const bytes = await zip(entries);
	const site = { git: "abcd1234", repo: REPO, tool: { zip: "downloads/rfoundry-tool.zip", sha256: await sha256(bytes), size: bytes.length } };
	const urls = [];
	const fetchFn = async (url) => { urls.push(url); return { ok: true, status: 200, arrayBuffer: async () => bytes.slice().buffer }; };
	return { site, fetchFn, urls, bytes };
}

test("kit: the zip has the README, the firmware, the stock file and the tool", async () => {
	const { site, fetchFn, urls } = await toolSite();
	const image = new Uint8Array([1, 2, 3, 4]);
	const stock = { name: files(DFU)[0], data: new Uint8Array(300).fill(7) };
	const k = await kit.makeKit({ radio: radios[DFU], board: brd(DFU), firmware: { manifest: manifest(DFU), image },
		stock, site, root: "../", url: SITE, when: new Date("2026-10-26T12:00:00Z") }, fetchFn);
	assert.deepEqual(urls, ["../downloads/rfoundry-tool.zip"]);
	assert.equal(k.name, kit.kitName(radios[DFU], new Date("2026-10-26T12:00:00Z")));
	const got = await unzip(k.bytes);
	const fw = `firmware/rfoundry-${DFU}-v0.2.0`;
	assert.deepEqual([...got.keys()], ["README.txt", `${fw}.bin`, `${fw}.json`, `stock/${stock.name}`, "rfoundry.py", "tools/rfoundry/rfoundry/cli.py"]);
	assert.deepEqual(got.get(`${fw}.bin`), image);
	assert.deepEqual(got.get(`stock/${stock.name}`), stock.data);
	assert.deepEqual(JSON.parse(new TextDecoder().decode(got.get(`${fw}.json`))), manifest(DFU));
	const text = new TextDecoder().decode(got.get("README.txt"));
	assert.ok(text.includes("\r\n") && !/[^\r]\n/.test(text));
	assert.ok(text.includes("Made 2026-10-26 by https://radiofoundry.org/flash/ (RadioFoundry abcd1234)"));
});

test("kit: without a stock file the folder is there, with a note", async () => {
	const { site, fetchFn } = await toolSite();
	const k = await kit.makeKit({ radio: radios[SERIAL], board: brd(SERIAL), firmware: { manifest: manifest(SERIAL), image: new Uint8Array(4) },
		stock: null, site, root: "", url: SITE }, fetchFn);
	const names = [...(await unzip(k.bytes)).keys()];
	assert.ok(names.includes("stock/PUT-YOUR-STOCK-FILE-HERE.txt"));
	assert.ok(!names.some((n) => n.startsWith("stock/") && n.endsWith(".bin")));
});

test("kit: a ROM bootloader radio's zip has no stock folder at all", async () => {
	const { site, fetchFn } = await toolSite();
	for (const stock of [null, { name: "LEFT-OVER.bin", data: new Uint8Array(8) }]) {
		const k = await kit.makeKit({ radio: radios[PLAIN], board: brd(PLAIN), firmware: { manifest: manifest(PLAIN), image: new Uint8Array(4) },
			stock, site, root: "", url: SITE }, fetchFn);
		const got = await unzip(k.bytes);
		const fw = `firmware/rfoundry-${PLAIN}-v0.2.0`;
		assert.deepEqual([...got.keys()], ["README.txt", `${fw}.bin`, `${fw}.json`, "rfoundry.py", "tools/rfoundry/rfoundry/cli.py"]);
		assert.ok(new TextDecoder().decode(got.get("README.txt")).includes("--unprotect"));
	}
});

test("kit: a tool zip that doesn't match site.json is refused", async () => {
	const { site, fetchFn } = await toolSite();
	const o = { radio: radios[DFU], board: brd(DFU), firmware: { manifest: manifest(DFU), image: new Uint8Array(4) }, stock: null, root: "", url: SITE };
	await assert.rejects(kit.makeKit({ ...o, site: { ...site, tool: { ...site.tool, sha256: "0".repeat(64) } } }, fetchFn), kit.KitError);
	await assert.rejects(kit.makeKit({ ...o, site: { ...site, tool: { ...site.tool, size: site.tool.size + 1 } } }, fetchFn), kit.KitError);
	await assert.rejects(kit.makeKit({ ...o, site: { ...site, tool: undefined } }, fetchFn), kit.KitError);
	await assert.rejects(kit.makeKit({ ...o, site }, async () => ({ ok: false, status: 404 })), /404/);
	await assert.rejects(kit.makeKit({ ...o, firmware: null, site }, fetchFn), kit.KitError);
});

const args = (id, extra = {}) => () => ({ radio: radios[id], board: fb.boards[radios[id].flash.board], site: {}, root: "", url: SITE,
	firmware: { manifest: manifest(id), image: new Uint8Array(4) }, stock: null, ...extra });

test("kit panel: in place of a step the browser can't run", () => {
	const { el, action } = kit.kitPanel(args(DFU), "usb", { pickStock: () => {} });
	const text = el.textContent;
	assert.ok(text.includes(en["flash.err.no_usb"]));
	assert.ok(text.includes(en["flash.kit.title"]) && text.includes(en["flash.kit.lead"]));
	assert.equal(action.textContent, en["flash.kit.download"]);
	assert.ok(action.className.includes("btn-primary"));
	// no stock file yet: the picker is offered
	assert.ok(all(el).some((e) => e.tagName === "INPUT" && e.attrs.type === "file"));
	const withStock = kit.kitPanel(args(DFU, { stock: { name: "S.bin", data: new Uint8Array(1) } }), "usb").el;
	assert.ok(!all(withStock).some((e) => e.tagName === "INPUT"));
	assert.ok(withStock.textContent.includes("S.bin"));
	// a codec from a program flash read: no file to pick, the README reads it
	const pro = kit.kitPanel(args(READOUT), "serial", { pickStock: () => {} }).el;
	assert.ok(pro.textContent.includes(en["flash.kit.readout"]) && pro.textContent.includes(en["flash.err.no_serial"]));
	assert.ok(!all(pro).some((e) => e.tagName === "INPUT"));
});

test("kit panel: a ROM bootloader radio is told it needs no stock file, and is offered none to pick", () => {
	const { el, action } = kit.kitPanel(args(PLAIN), "usb", { pickStock: () => {} });
	assert.ok(el.textContent.includes(en["flash.dfuse.kit_plain"]) && el.textContent.includes(en["flash.err.no_usb"]));
	assert.ok(!el.textContent.includes(en["flash.kit.readout"]) && !el.textContent.includes(en["flash.kit.no_stock"].split("{files}")[0]));
	assert.ok(!all(el).some((e) => e.tagName === "INPUT"));
	assert.equal(action.textContent, en["flash.kit.download"]);
	const line = kit.kitLine(args(PLAIN));
	assert.ok(line.textContent.includes(en["flash.kit.line_title"]));
});

test("kit panel: a radio rfoundry can't install gets the browser advice, no download", () => {
	const { el, action } = kit.kitPanel(args(NONE), "serial");
	assert.equal(action, null);
	assert.ok(el.textContent.includes(en["flash.kit.none"].replace("{radio}", radios[NONE].name)));
	assert.ok(el.textContent.includes(en["flash.err.no_serial"]));
});

test("kit line: under a failed install, only where it can help", () => {
	const line = kit.kitLine(args(SERIAL));
	assert.ok(line.textContent.includes(en["flash.kit.line_title"]) && line.textContent.includes(en["flash.kit.download"]));
	assert.equal(kit.kitLine(args(NONE)), null);
	assert.equal(kit.kitLine(args(SERIAL, { firmware: null })), null);
});
