// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The manual install kit: when the page can't install (a browser without WebUSB or Web Serial, an install that keeps
// failing), a zip with what the user needs to install from the command line: rfoundry from the site's own commit
// (downloads/rfoundry-tool.zip, checked against site.json "tool"), the firmware built here with its manifest, the
// stock file the user gave, and a README.txt with this radio's commands, file names filled in. The README is English,
// as rfoundry's own output is. docs/dev/web-flasher.md "The manual install kit".

import { sha256 } from "./bytes.js";
import { zip, unzip } from "./zip.js";
import { h, t, button, download, filePicker } from "./dom.js";

export class KitError extends Error {}

// How rfoundry installs a radio (registry.json family): "dfu" over USB with the bootloader's key, "serial" over the
// programming cable, "dfuse" over USB through the processor's own ROM bootloader (plain: no key, a first install
// that needs --unprotect, and a stock file only where the radio has a codec route: plainCodec), null where rfoundry
// has no install (a radio installed through the RAM stub: the web flasher or stub/stubtool.py)
export function kitRoute(radio) {
	return { "usb-dfu": "dfu", "serial-loader": "serial", dfuse: "dfuse" }[radio?.flash?.family] || null;
}

// The routes that reach the radio as a USB DFU device: their setup is the WinUSB driver and the udev rule
const usbRoute = (route) => route === "dfu" || route === "dfuse";

// A radio of the "dfuse" route with a codec route (radios.json flash.codec_none: null, or why the radio has none): its
// codec comes from the user's stock file (--stock), and without one the install leaves the codec out (--codec none)
const plainCodec = (radio) => kitRoute(radio) === "dfuse" && !radio.flash.codec_none;

// The browser API an install step needs that this browser lacks: "usb" or "serial", else null. step: the step about
// to be drawn; stub: the install goes through the STM32F4 loader (serial) rather than the bootloader (USB)
export function missingApi(radio, task, step, { usb, serial, stub = false }) {
	if (task !== "install") return null;
	const backend = radio?.flash?.backend;
	if (backend === "anyroad-dfu" && step === "write") return stub ? (serial ? null : "serial") : usb ? null : "usb";
	if (backend === "dfuse" && step === "write") return usb ? null : "usb";
	if (backend === "c7000-stub" && step === "connect") return serial ? null : "serial";
	return null;
}

// The start page's banner, as [i18n key, notice level] pairs: nothing with both APIs, one line with neither. Without
// WebUSB alone (Firefox has Web Serial) radios on a programming cable still install here, so it is a warning, not a
// stop: only the radios that install over USB (DFU) need another browser
export function supportNotices({ usb, serial }) {
	if (!usb && !serial) return [["flash.support.none", "bad"]];
	return [!usb && ["flash.support.usb", "warn"], !serial && ["flash.support.serial", "bad"]].filter(Boolean);
}

// The image's file name without .bin: the manifest's, else rfoundry_<board> (a local build's manifest says "local")
export function kitBase(manifest, boardId) {
	const f = manifest?.image?.file || "";
	return /^[\w.+-]+\.bin$/.test(f) ? f.slice(0, -4) : `rfoundry_${boardId}`;
}

const day = (d) => d.toISOString().slice(0, 10);
export const kitName = (radio, when = new Date()) =>
	`RadioFoundry_${radio.name.replace(/[^A-Za-z0-9-]+/g, "-").replace(/^-|-$/g, "")}_manual_install_${day(when)}.zip`;

// A path for a command line: always quoted (stock file names carry spaces and brackets)
const q = (p) => `"${p}"`;

// The stock files the radio's codec comes from, as the codec step names them; a program flash read is not one
const stockFiles = (board) => (board?.codec?.sources || []).filter((s) => /\.bin$/i.test(s));

// rfoundry's Python and packages (tools/rfoundry/pyproject.toml requires-python and dependencies; the tool zip's
// requirements.txt lists the same, site/tests/manual.test.mjs checks both)
export const PYTHON_MIN = "3.8";
export const PACKAGES = [["pyserial", "the programming cable and the radio's serial port"], ["pyusb", "the bootloader over USB"],
	["libusb-package", "the USB library pyusb needs, built in for Windows, macOS and Linux"], ["certifi", "checks downloads from GitHub"]];

const hex4 = (n) => n.toString(16).toUpperCase().padStart(4, "0");

// README.txt's setup section: Python, the packages, and per system what the route needs besides them (the WinUSB
// driver and the udev rule for a USB bootloader, the serial port's permission and name). Not bundled in the kit: pip
// takes the right libusb-package build for the system, which a kit made in the browser can't know
export function kitSetup(route, board) {
	const [vid, pid] = board?.dfu_ids || [0x0483, 0xDF11];
	const usbId = `${hex4(vid)}:${hex4(pid)}`;
	const pip = (py) => [`      ${py} -m venv venv`, `      ${py === "py" ? "venv\\Scripts\\python" : "venv/bin/python"} -m pip install -r requirements.txt`];
	const L = [
		`rfoundry needs Python ${PYTHON_MIN} or later and these packages (requirements.txt). pip installs them from the`,
		"internet into a folder of their own (venv) inside this one, so nothing else on the computer changes:",
		...PACKAGES.map(([n, why]) => `  ${n.padEnd(16)} ${why}`), "",
		"Unpack the zip into a folder and open a terminal in it. Then follow your system.", "",
		"Windows",
		`  - Install Python ${PYTHON_MIN} or later from https://www.python.org/downloads/. In the first screen of the`,
		"    installer tick \"Add python.exe to PATH\", then click Install Now.",
		"  - In the folder's address bar type cmd and press Enter. Check the version, then make the venv:",
		"      py --version", ...pip("py")];
	// the keyed bootloader's device name is the board's, or the one its family shares; a ROM bootloader's name is in the
	// board's data only once it was read on a radio, so without one the device is found by its USB ID
	const device = route === "dfuse" && !board?.dfu_product ? `the device with USB ID ${usbId.replace(":", " ")}`
		: `"${board?.dfu_product || "Digital Radio in USB mode"}" (USB ID ${usbId.replace(":", " ")})`;
	if (usbRoute(route)) L.push(
		"  - The radio's bootloader needs the WinUSB driver, once:",
		"      1. Download Zadig from https://zadig.akeo.ie and run it.",
		"      2. Put the radio in its bootloader (the steps under INSTALL below) and plug it in.",
		`      3. In Zadig's list pick ${device}.`,
		"         Not listed: Options > List All Devices.",
		"      4. Choose WinUSB and click Install Driver (or Replace Driver).",
		"      5. Unplug the radio, put it back in its bootloader and connect again.");
	else L.push("  - The cable's port is COM3, COM4, ...: Device Manager lists it under Ports. A cable missing there needs",
		"    the driver from its chip's maker (CH340: WCH; FTDI; CP210x: Silicon Labs; PL2303: Prolific).");
	L.push("", "macOS",
		`  - Install Python ${PYTHON_MIN} or later from https://www.python.org/downloads/ (or Homebrew: brew install python).`,
		"  - In Terminal:",
		"      cd FOLDER   (type cd and a space, then drag the folder onto the window)",
		"      python3 --version", ...pip("python3"),
		"  - Nothing else to install: libusb-package brings the USB library, and macOS has the cable drivers.");
	if (route === "serial") L.push("  - The cable's port is /dev/cu.usbserial-... (or /dev/cu.wchusbserial-...).");
	L.push("", "Linux",
		`  - Python ${PYTHON_MIN} or later comes with the system. Debian, Ubuntu: sudo apt install python3-venv first.`,
		"  - In a terminal, in the folder:",
		"      python3 --version", ...pip("python3"));
	if (usbRoute(route)) L.push(
		`  - Let your user reach the bootloader (${usbId.toLowerCase()}), once:`,
		`      echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="${hex4(vid).toLowerCase()}", ATTRS{idProduct}=="${hex4(pid).toLowerCase()}", MODE="0666"' | sudo tee /etc/udev/rules.d/50-rfoundry-dfu.rules`,
		"      sudo udevadm control --reload-rules && sudo udevadm trigger");
	L.push("  - Join the dialout group for the serial port, then log out and in again:",
		"      sudo usermod -aG dialout $USER");
	if (route === "serial") L.push("  - The cable's port is /dev/ttyUSB0 (or /dev/ttyACM0).");
	L.push("", "pip needs the internet. If it fails, check the connection and run the pip line again.");
	return L;
}

// README.txt. o: {radio (radios.json), board (flash-boards.json), base, manifest, stock: file name or null, site (the
// page's address, ending in /), repo, community (site.json's), git, date}
export function kitReadme(o) {
	const route = kitRoute(o.radio);
	if (!route) throw new KitError(`rfoundry does not install the ${o.radio.name}`);
	if (route === "dfuse") return plainReadme(o);
	const name = o.radio.name, m = o.manifest || {};
	const img = `firmware/${o.base}.bin`;
	const files = stockFiles(o.board);
	const readout = route === "serial" && !files.length;	// the codec comes from the radio's own program flash
	const stock = o.stock ? `stock/${o.stock}` : readout ? "stock/progmem.bin" : null;
	const docs = (page, anchor = "") => `${o.site}docs/user/${page}.html${anchor ? "#" + anchor : ""}`;
	const L = [];
	const add = (...lines) => L.push(...lines);
	const both = (args) => [`  Windows:        venv\\Scripts\\python ${args}`, `  macOS, Linux:   venv/bin/python ${args}`];

	add(`RadioFoundry for the ${name}: manual install kit`, "",
		`Made ${o.date} by ${o.site}flash/ (RadioFoundry ${o.git || "?"}).`,
		"This kit installs RadioFoundry from the command line with rfoundry, the tool the web installer is built on.",
		"Use it where the web installer can't: a browser without WebUSB or Web Serial (Safari, Firefox), or an install",
		"that keeps failing.", "",
		"WHAT'S INSIDE", "",
		`  ${img.padEnd(40)} the firmware you chose (${[m.version, m.variant].filter(Boolean).join(", ") || "RadioFoundry"})`,
		`  ${`firmware/${o.base}.json`.padEnd(40)} its manifest: rfoundry checks the image against it`);
	if (o.stock) add(`  ${stock.padEnd(40)} your stock firmware file: ${route === "dfu" ? "the codec and the bootloader key" : "the codec"}`);
	add(`  ${"rfoundry.py".padEnd(40)} starts rfoundry from this folder, nothing to install`,
		`  ${"tools/, stub/".padEnd(40)} rfoundry and the tools it uses`,
		`  ${"requirements.txt".padEnd(40)} the Python packages rfoundry needs`,
		`  ${"LICENSE".padEnd(40)} MIT`, "",
		"Nothing here writes the radio's bootloader, so the radio's own update mode always stays available.", "");

	add("1. SET UP (ONCE)", "", ...kitSetup(route, o.board), "");

	// the codec source the commands use, and what to do when the kit has none
	let n = 2;
	if (readout) {
		add(`${n++}. READ THE CODEC FROM THE RADIO`, "",
			`The ${name}'s codec comes from its own program flash. If the radio runs its stock firmware, read it first`,
			"(it is also a backup: keep the file). Switch the radio off, connect the cable, run this and switch the",
			"radio on when the tool says (PORT: the cable's port, see step 1):", "",
			...both(`stub/stubtool.py PORT --board ${o.board?.id || o.radio.flash.board} --progmem ${q(stock)}`), "",
			"It takes about a minute; three green flashes end it. Then switch the radio off and on again.",
			"A radio that runs RadioFoundry with working DMR voice keeps its codec: leave out --stock below.", "");
	} else if (!o.stock) {
		add("THE STOCK FILE", "",
			`The kit has no stock firmware file, which gives the codec${route === "dfu" ? " and the bootloader key" : ""}. Copy yours into the stock folder`,
			`(${files.slice(0, 3).join(", ")}${files.length > 3 ? ", ..." : ""}) and put its name in the commands below.`,
			`A radio that runs RadioFoundry with working DMR voice keeps its codec${route === "dfu" ? " and gives the key" : ""}: leave out --stock.`, "");
	}
	const stockArg = ` --stock ${q(stock || `stock/${files[0] || "STOCK.bin"}`)}`;
	const port = route === "serial" ? " --port PORT" : "";
	const cmd = `rfoundry.py flash ${q(img)}${stockArg}${port}`;

	add(`${n++}. CHECK FIRST (WRITES NOTHING)`, "", ...both(`${cmd} --dry-run`), "",
		"rfoundry checks the image against its manifest and prints exactly what it would erase and write.");
	if (route === "serial") add("Only one USB-serial cable plugged in? rfoundry finds it: you can leave out --port PORT.",
		"List the ports: venv/bin/python -m serial.tools.list_ports (Windows: venv\\Scripts\\python ...).");
	add("");

	const steps = o.radio.flash.bootloader || [];
	add(`${n++}. INSTALL`, "", ...both(cmd), "");
	if (route === "dfu") add(
		"Leave the radio as it is when you start: if it runs RadioFoundry, rfoundry asks it about its codec first.",
		"Then rfoundry asks you to put the radio in its bootloader:");
	else add(
		"A radio that runs RadioFoundry stays on: rfoundry installs over the cable with no keys to hold.",
		"Any other radio: rfoundry asks you to put it in update mode:");
	if (steps.length) steps.forEach((s, i) => add(`  ${i + 1}. ${s}`));
	else add(`  rfoundry names the keys to hold. Your radio's section: ${docs("23-radios")}`);
	add("", "rfoundry writes, waits for the radio to restart and checks what it reports. Keep the cable in until it",
		"says it is done.", "");

	add("IF IT FAILS", "",
		"  - Run the same command again. The radio's update mode is never written, so it can always be reached.",
		`  - Command line troubleshooting: ${docs("20-troubleshooting", "command-line-installs-advanced")}`,
		`  - rfoundry on the command line: ${docs("02-installing", "advanced-rfoundry-on-the-command-line")}`);
	if (o.community) add(`  - Ask the community: ${o.community}`);
	if (o.repo) add(`  - Report a bug: ${o.repo}/issues (paste what rfoundry printed)`);
	return L.join("\n") + "\n";
}

// README.txt for the "dfuse" route: no key anywhere. What it must say that the other routes' README doesn't: a first
// install erases the radio's whole program memory, its original firmware and that firmware's bootloader with it
// (rfoundry stops and says so until --unprotect is given), and going back is the maker's procedure. The bootloader
// used is the processor's own, which nothing can write. A radio without a codec route has no stock file and no codec,
// so no --stock anywhere. One with a codec route (plainCodec) takes its codec from the user's stock file: the commands
// carry --stock with the kit's file, and --codec none where the kit has none (an install without DMR voice, which the
// README says before any command)
function plainReadme(o) {
	const name = o.radio.name, m = o.manifest || {};
	const img = `firmware/${o.base}.bin`;
	const docs = (page, anchor = "") => `${o.site}docs/user/${page}.html${anchor ? "#" + anchor : ""}`;
	const both = (args) => [`  Windows:        venv\\Scripts\\python ${args}`, `  macOS, Linux:   venv/bin/python ${args}`];
	const codec = plainCodec(o.radio);
	const files = stockFiles(o.board);
	const stock = codec && o.stock ? `stock/${o.stock}` : null;
	const cmd = `rfoundry.py flash ${q(img)}${stock ? ` --stock ${q(stock)}` : codec ? " --codec none" : ""}`;
	// what the kit says of the codec: where it comes from, or that this install leaves it out and how to add it
	const about = !codec ? "No stock firmware file is needed, and none is used."
		: "Its update mode needs no key. The DMR voice codec is not part of RadioFoundry: rfoundry takes it from your stock firmware file.";
	const stockPart = !codec ? [] : stock ? [
		"THE STOCK FILE", "",
		...wrap(`The commands below give rfoundry your stock firmware file (--stock), and it writes the DMR voice codec from it. A radio that runs RadioFoundry with working DMR voice keeps its codec; the stock file is then not used.`), ""]
		: ["THE STOCK FILE", "",
			...wrap(`The kit has no stock firmware file, so the commands below install without the DMR voice codec (--codec none): FM works, DMR voice does not. For DMR voice copy your stock firmware file into the stock folder (${files.slice(0, 3).join(", ")}${files.length > 3 ? ", ..." : ""}) and put --stock "stock/NAME" in place of --codec none in the commands below.`),
			...wrap("A radio that runs RadioFoundry with working DMR voice keeps its codec only if you leave out --codec none: with it rfoundry erases the codec."), ""];
	const steps = o.radio.flash.bootloader || [];
	const enter = steps.length ? steps.map((s, i) => `  ${i + 1}. ${s}`) : [`  rfoundry names the keys to hold. Your radio's section: ${docs("23-radios")}`];
	const L = [`RadioFoundry for the ${name}: manual install kit`, "",
		`Made ${o.date} by ${o.site}flash/ (RadioFoundry ${o.git || "?"}).`,
		"This kit installs RadioFoundry from the command line with rfoundry, the tool the web installer is built on.",
		"Use it where the web installer can't: a browser without WebUSB or Web Serial (Safari, Firefox), or an install",
		"that keeps failing.", "",
		"WHAT'S INSIDE", "",
		`  ${img.padEnd(40)} the firmware you chose (${[m.version, m.variant].filter(Boolean).join(", ") || "RadioFoundry"})`,
		`  ${`firmware/${o.base}.json`.padEnd(40)} its manifest: rfoundry checks the image against it`,
		...(stock ? [`  ${stock.padEnd(40)} your stock firmware file: the codec`] : []),
		`  ${"rfoundry.py".padEnd(40)} starts rfoundry from this folder, nothing to install`,
		`  ${"tools/, stub/".padEnd(40)} rfoundry and the tools it uses`,
		`  ${"requirements.txt".padEnd(40)} the Python packages rfoundry needs`,
		`  ${"LICENSE".padEnd(40)} MIT`, "",
		...wrap(`The ${name} is installed through its processor's own bootloader, which is part of the chip and can't be erased: that update mode always stays available. ${about}`), "",
		"READ THIS BEFORE A FIRST INSTALL", "",
		"A radio on its original firmware is read protected, and its bootloader writes nothing until the protection is",
		"removed. Removing it (a Read Unprotect) erases the radio's whole program memory: the original firmware and",
		"that firmware's bootloader. rfoundry can't put them back. Going back to the original firmware is the maker's",
		"own recovery procedure, with the maker's files: have them, and save your channels with the maker's",
		"programming software, before you install.",
		...(codec ? ["", ...wrap("The voice codec is erased with the original firmware. rfoundry writes it again from your stock firmware file: keep that file.")] : []),
		...(o.radio.flash.notice ? ["", ...wrap(o.radio.flash.notice)] : []), "",
		...stockPart,
		"1. SET UP (ONCE)", "", ...kitSetup("dfuse", o.board), "",
		"2. CHECK FIRST (WRITES NOTHING)", "", ...both(`${cmd} --dry-run`), "",
		"rfoundry checks the image against its manifest and prints exactly what it would erase and write.", "",
		"3. INSTALL", "", ...both(cmd), "",
		...(codec ? ["Leave the radio as it is when you start: if it runs RadioFoundry, rfoundry asks it about its codec first.",
			"Then rfoundry asks you to put the radio in its bootloader:"] : ["rfoundry asks you to put the radio in its bootloader:"]), ...enter, "",
		...(o.radio.flash.bootloader_restart ? [
			"A radio that already runs RadioFoundry needs no keys: leave it switched on and connected, and rfoundry",
			"restarts it into its bootloader by itself. If the radio refuses the restart, rfoundry says what to do and",
			"then asks for the keys as above.", ""] : []),
		"It then asks the radio whether it is read protected. A radio that already runs RadioFoundry is not: rfoundry",
		"writes, the radio restarts, and rfoundry checks what it reports. Keep the cable in until it says it is done.", "",
		"4. A FIRST INSTALL: REMOVE THE READ PROTECTION", "",
		"On a protected radio the command above writes nothing: it stops and says what a Read Unprotect erases. To go",
		"on, run it with --unprotect:", "", ...both(`${cmd} --unprotect`), "",
		"rfoundry asks you to confirm, sends the Read Unprotect and waits for the erase: at least a minute, until the",
		"radio has left USB. Nothing shows on the radio: leave it on and connected until rfoundry says to switch it",
		"off. Then switch the radio off, put it in its bootloader again as above, and rfoundry installs. A radio that",
		"is back in its bootloader by itself is installed at once, and rfoundry says so: leave it on.", "",
		"IF IT FAILS", "",
		"  - Run the same command again. The processor's bootloader is never written, so it can always be reached.",
		`  - Command line troubleshooting: ${docs("20-troubleshooting", "command-line-installs-advanced")}`,
		`  - rfoundry on the command line: ${docs("02-installing", "advanced-rfoundry-on-the-command-line")}`];
	if (o.community) L.push(`  - Ask the community: ${o.community}`);
	if (o.repo) L.push(`  - Report a bug: ${o.repo}/issues (paste what rfoundry printed)`);
	return L.join("\n") + "\n";
}

// A paragraph as README lines of at most 110 characters
function wrap(text, width = 110) {
	const out = [];
	let line = "";
	for (const w of String(text).split(/\s+/).filter(Boolean)) {
		if (line && line.length + 1 + w.length > width) { out.push(line); line = w; } else line = line ? `${line} ${w}` : w;
	}
	if (line) out.push(line);
	return out;
}

// rfoundry from this site: downloads/rfoundry-tool.zip, refused unless its size and SHA-256 are site.json's (a
// cached or damaged copy of another build would refuse this firmware's manifest, or worse, accept a wrong board)
export async function toolEntries(site, root = "", fetchFn = globalThis.fetch) {
	const tool = site?.tool;
	if (!tool?.zip) throw new KitError("this site was built without the rfoundry tool (site.json has no \"tool\")");
	const r = await fetchFn(`${root}${tool.zip}`);
	if (!r.ok) throw new KitError(`could not download the rfoundry tool (${r.status || "network error"})`);
	const data = new Uint8Array(await r.arrayBuffer());
	if (data.length !== tool.size || await sha256(data) !== tool.sha256)
		throw new KitError("the rfoundry tool download does not match this page (size or SHA-256): reload the page and try again");
	return unzip(data);
}

// Every file of the kit, in order: [{name, data}]. tool: toolEntries(); fw: {manifest, image}; stock: {name, data} or
// null; text: kitReadme(). stockFolder false: a radio whose install uses no stock file gets no stock folder
export function kitEntries(tool, fw, base, stock, text, { stockFolder = true } = {}) {
	const out = [{ name: "README.txt", data: text.replace(/\n/g, "\r\n") }];	// CRLF: Notepad of any age
	out.push({ name: `firmware/${base}.bin`, data: fw.image }, { name: `firmware/${base}.json`, data: JSON.stringify(fw.manifest, null, 1) + "\n" });
	if (!stockFolder) { /* nothing of a stock file */ }
	else if (stock) out.push({ name: `stock/${stock.name}`, data: stock.data });
	else out.push({ name: "stock/PUT-YOUR-STOCK-FILE-HERE.txt", data: "Copy your stock firmware file (or program flash read) here. README.txt says which.\r\n" });
	for (const [name, data] of tool) out.push({ name, data });
	return out;
}

// The kit's zip. o: {radio, board, firmware: {manifest, image}, stock: {name, data} | null, site (site.json), root (the
// page's path to the site root), url (the site's address), when}
export async function makeKit(o, fetchFn = globalThis.fetch) {
	if (!o.firmware?.image) throw new KitError("no firmware yet: choose and build it first");
	const when = o.when || new Date();
	const base = kitBase(o.firmware.manifest, o.radio.flash.board);
	// no stock file for a radio without a codec route, even if one was picked for another radio
	const plain = kitRoute(o.radio) === "dfuse" && !plainCodec(o.radio);
	const stock = o.stock?.data && !plain ? { name: o.stock.name.split(/[\\/]/).pop(), data: o.stock.data } : null;
	const text = kitReadme({ radio: o.radio, board: o.board, base, manifest: o.firmware.manifest, stock: stock?.name || null,
		site: o.url, repo: o.site?.repo, community: o.site?.community, git: o.site?.git, date: day(when) });
	const tool = await toolEntries(o.site, o.root, fetchFn);
	return { name: kitName(o.radio, when), bytes: await zip(kitEntries(tool, o.firmware, base, stock, text, { stockFolder: !plain }), when) };
}

// ---------------------------------------------------------------------------------------------------------------
// on the page

// The download button: builds the kit and saves it, or says why not. args(): makeKit's o at the time of the click
export function kitButton(args, { primary = false, label = t("flash.kit.download"), status = null } = {}) {
	const b = button(label, async () => {
		b.disabled = true;
		status?.replaceChildren(h("span", { class: "muted" }, t("flash.kit.making")));
		try {
			const kit = await makeKit(args());
			download(kit.name, kit.bytes, "application/zip");
			status?.replaceChildren(h("span", {}, "✓ ", t("flash.kit.saved", { name: kit.name })));
		} catch (e) {
			status?.replaceChildren(h("span", { class: "kit-error" }, t("flash.kit.failed", { error: e.message })));
		}
		b.disabled = false;
	}, { primary });
	return b;
}

// A step the browser can't run (missingApi): what is missing, and the kit in its place. args(): makeKit's o;
// pickStock(f): takes the user's stock file (app.js pickedStock) and redraws. Returns {el, action}: the panel, and the
// download button for the nav bar (null where rfoundry can't install the radio)
export function kitPanel(args, api, { pickStock = null } = {}) {
	const o = args();
	const route = kitRoute(o.radio);
	const why = t(api === "usb" ? "flash.err.no_usb" : "flash.err.no_serial");
	if (!route) {
		return { el: h("section", { class: "panel kit" }, h("h2", {}, t("flash.kit.none_title")),
			h("p", { class: "notice bad" }, why), h("p", {}, t("flash.kit.none", { radio: o.radio.name }))), action: null };
	}
	const status = h("p", { class: "small", "aria-live": "polite" });
	const files = stockFiles(o.board);
	// the plain route without a codec route uses no stock file: said, and no picker. With one, the shared branch in
	// this route's words: the file gives the codec and no key, and without it the install leaves the codec out
	const dfuse = route === "dfuse", plain = dfuse && !plainCodec(o.radio);
	const stockPart = plain ? h("li", {}, t("flash.dfuse.kit_plain")) : o.stock?.data
		? h("li", {}, t(dfuse ? "flash.dfuse.kit_stock" : "flash.kit.has_stock", { name: o.stock.name }))
		: files.length ? h("li", {}, t(dfuse ? "flash.dfuse.kit_no_stock" : "flash.kit.no_stock", { files: files.slice(0, 3).join(", ") }),
			pickStock ? filePicker(t("flash.codec.stock_pick"), ".bin,.zip", pickStock) : null)
			: h("li", {}, t("flash.kit.readout"));
	const el = h("section", { class: "panel kit" },
		h("h2", {}, t("flash.kit.title")),
		h("div", { class: "notice info" }, h("p", {}, h("strong", {}, why)), h("p", {}, t("flash.kit.lead"))),
		h("ul", { class: "kit-list" },
			h("li", {}, t("flash.kit.has_firmware", { version: o.firmware?.manifest?.version || "", radio: o.radio.name })),
			stockPart,
			h("li", {}, t("flash.kit.has_tool")),
			h("li", {}, t("flash.kit.has_readme"))),
		h("p", { class: "small muted" }, t("flash.kit.needs")),
		status);
	return { el, action: kitButton(args, { primary: true, status }) };
}

// Under a failed install: one line offering the kit. null where rfoundry can't install the radio or there is no
// firmware to put in it
export function kitLine(args) {
	const o = args();
	if (!kitRoute(o.radio) || !o.firmware?.image) return null;
	const status = h("span", { class: "small", "aria-live": "polite" });
	return h("div", { class: "notice info kit-line" },
		h("p", {}, h("strong", {}, t("flash.kit.line_title")), " ", t("flash.kit.line")),
		h("div", { class: "actions" }, kitButton(args, { status }), status));
}
