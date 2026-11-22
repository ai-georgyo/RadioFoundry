// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AnyRoad (TYT / Baofeng STM32) DFU bootloader over WebUSB. Protocol: docs/spec/hardware-dm1701.md 15.1-15.3, as
// tools/rfoundry/rfoundry/dfu.py drives it: only the plan's sectors are erased and written, each 1024-byte block
// XORed with the model key (every sector starts a multiple of 1024 bytes after the application base, so each block
// starts at key index 0). The bootloader can't read flash back (the radios are at read protection level 1).
//
// The plain route (board family "dfuse"): ST's DfuSe download to an MCU's own ROM bootloader (ST AN3156), as
// tools/rfoundry/rfoundry/dfu.py drives it. No key, no vendor command, nothing XORed; the same plan, erased and written
// sector by sector, then Leave at the application base. A ROM loader also answers whether the program memory is read
// protected (probeProtected) and removes the protection by erasing all of it (readUnprotect). Only the internal flash
// is ever addressed. A ROM loader's second alternate setting is the option bytes, where one write can remove the
// loader for good: nothing here selects an alternate setting (the interface stays on 0, the internal flash), and no
// address pointer is ever set outside the board's program flash. Nor is a sector past the board's application region
// erased or written (checkPlain): flash the build keeps unused is flash the loader must not be asked to erase.

import { BLOCK, FIRST_BLOCK } from "./plan.js";
import * as protect from "./protect.js";
import { hex, le32, sha256, sleep } from "./bytes.js";

export const KEY_LEN = 1024;
export const EXIT_ADDR = 0x08000000;	// the keyed route's exit; the plain route leaves at the board's application base
// The board family written plain (tools/boards.py FAMILIES): only a board of it that has no key takes the plain route
export const PLAIN_FAMILY = "dfuse";
// A chip with 256-bit flash words programs 32 bytes at a time, and a partial word needs a forced write (RM0433 4.3.9).
// AN3156 and AN2606 do not say whether a ROM loader forces the last partial word of a download, so the plain route
// never sends one: every block is a whole number of flash words. The plan's blocks are BLOCK bytes each, filled with
// 0xFF past the image's end (plan.js), which pads the image; checkPlain refuses any other block
export const FLASH_WORD = 32;
const DNLOAD = 1, UPLOAD = 2, GETSTATUS = 3, CLRSTATUS = 4, ABORT = 6;
const ST_IDLE = 2, ST_DNBUSY = 4, ST_DNIDLE = 5, ST_ERROR = 10;
const ERR_VENDOR = 0x0B;	// bStatus errVENDOR: a ROM loader's "the memory is read protected" (AN3156 4.1, 5.1, 5.3)
const SET_ADDRESS = 0x21, ERASE = 0x41, READ_UNPROTECT = 0x92;
const STATE_NAMES = ["appIDLE", "appDETACH", "dfuIDLE", "dfuDNLOAD-SYNC", "dfuDNBUSY", "dfuDNLOAD-IDLE",
	"dfuMANIFEST-SYNC", "dfuMANIFEST", "dfuMANIFEST-WAIT-RESET", "dfuUPLOAD-IDLE", "dfuERROR"];

export class DfuError extends Error {}

// The device that answered is not the loader the board installs through (romDevice): nothing was asked of it
export class WrongDeviceError extends DfuError {
	constructor(message, what) {
		super(message);
		this.name = "WrongDeviceError";
		this.what = what;
	}
}

const inside = (s, lo, hi) => s.addr >= lo && s.addr + s.size <= hi;

// The plain route's limits on a plan, as rfoundry's dfu.check_plain has them. runPlain checks them before any request,
// and the page before it offers a Read Unprotect (dfuse.js installPlan): an erase of everything must not be followed
// by a refusal to write. A ROM loader takes any address it is sent, so two things are held here:
//
// The program flash only: every sector the plan erases or writes, and every block's address. The loader's address
// pointer also reaches RAM, system memory and the option bytes (AN3156 5.2).
//
// The application region only (board.app_region, [start, end): the linker script's, where data/flash-boards.json
// carries it). Flash past it is flash the build keeps unused, and it can be unused because the loader must not be
// asked to erase it: one version of a dual-bank chip's ROM loader leaves an erase in the second bank early, and a
// flash command after it can hang (AN2606, that loader's known limitations). A build never reaches there; a file
// that is not this board's image can.
//
// And whole blocks only (FLASH_WORD above).
//
// And the vector table last (plan.js vectorLast): the sector at the application base erased first and written last,
// and runPlain writes its first block after everything else is written and read back. At a normal power-on the chip
// starts whatever vector table is there; the ROM loader is entered by the boot pin, not through the vectors. So the
// order costs nothing, and an install cut anywhere before the end leaves no vector table, and no half-written image,
// to start
export function checkPlain(plan) {
	const board = plan.board, name = board.name || "radio";
	const region = board.app_region || null;
	for (const s of [...plan.erase, ...plan.writes.map((st) => st.sector)]) {
		if (!inside(s, board.rom_base, board.flash_end)) throw new DfuError(`sector at ${hex(s.addr)} is outside the ${name}'s program flash`);
		if (region && !(s.addr < region[1] && s.addr + s.size > region[0]))
			throw new DfuError(`the image reaches sector ${s.index} (${hex(s.addr)}), outside the ${name}'s application flash ${hex(region[0])}..${hex(region[1] - 1)}: it is not an image of this radio. Refusing`);
	}
	for (const st of plan.writes) {
		for (const [num, blk] of st.blocks) {
			if (blk.length !== BLOCK || blk.length % FLASH_WORD) throw new DfuError(`block ${num} of sector ${st.sector.index} is ${blk.length} bytes, not ${BLOCK}`);
			const at = st.sector.addr + (num - FIRST_BLOCK) * BLOCK;
			if (!Number.isInteger(num) || num < FIRST_BLOCK || at + BLOCK > st.sector.addr + st.sector.size)
				throw new DfuError(`block ${num} is outside sector ${st.sector.index} (${hex(st.sector.addr)})`);
		}
	}
	const last = plan.writes.length ? plan.writeOrder.at(-1) : null;
	if (last && (last.sector.addr !== board.app_base || last.blocks[0]?.[0] !== FIRST_BLOCK || plan.eraseOrder[0]?.addr !== board.app_base))
		throw new DfuError(`the plan does not erase the sector at the application base ${hex(board.app_base)} first and write it last: an install cut short would leave a vector table to start. Refusing`);
}

export async function checkKey(key, board) {
	if (key.length !== KEY_LEN) throw new DfuError(`the key is ${key.length} bytes, expected ${KEY_LEN}`);
	if (await sha256(key) !== board.key_sha256) throw new DfuError(`this is not the ${board.name} bootloader key`);
	return key;
}

export function xorBlock(block, key) {
	const out = new Uint8Array(block.length);
	for (let i = 0; i < block.length; i++) out[i] = block[i] ^ key[i];
	return out;
}

// A transport has: out(request, value, data?) and in(request, length, value = 0) -> Uint8Array, both class requests on
// the DFU interface. WebUsbTransport is the real one; fake.js has a simulated bootloader.
export class Dfu {
	constructor(transport, { log = () => {}, wait = sleep } = {}) {
		this.t = transport;
		this.log = log;
		this.wait = wait;
	}

	async getStatus() {
		const r = await this.t.in(GETSTATUS, 6);
		if (r.length !== 6) throw new DfuError(`short GETSTATUS reply (${r.length} bytes)`);
		return { status: r[0], pollMs: r[1] | (r[2] << 8) | (r[3] << 16), state: r[4] };
	}

	async waitIdle() {
		let st;
		for (let attempt = 0; attempt < 20; attempt++) {
			st = await this.getStatus();
			if (st.state === ST_IDLE) return;
			await this.t.out(attempt < 10 ? CLRSTATUS : ABORT, 0);
		}
		throw new DfuError(`the bootloader stays in state ${STATE_NAMES[st.state] || st.state}, not dfuIDLE: power-cycle the radio into the bootloader and try again`);
	}

	async waitDone(what) {
		for (let i = 0; i < 4000; i++) {
			const st = await this.getStatus();
			if (st.state === ST_ERROR || st.status !== 0) throw new DfuError(`${what} failed: status ${st.status}, state ${STATE_NAMES[st.state] || st.state}`);
			if (st.state === ST_DNIDLE) return;
			if (st.state !== ST_DNBUSY) throw new DfuError(`${what}: unexpected state ${STATE_NAMES[st.state] || st.state}`);
			await this.wait(Math.max(st.pollMs, 1));
		}
		throw new DfuError(`${what}: the bootloader stayed busy`);
	}

	async vendor(b0, b1) {
		await this.t.out(DNLOAD, 0, new Uint8Array([b0, b1]));
		await this.getStatus();
		await this.wait(100);
		await this.getStatus();
		await this.waitIdle();
	}

	async command(cmd, addr, what) {
		const data = new Uint8Array(5);
		data[0] = cmd;
		data.set(le32(addr), 1);
		await this.t.out(DNLOAD, 0, data);
		await this.waitDone(what);
	}

	// dfuIDLE by the standard's own requests (DFU 1.1 A.1): CLRSTATUS leaves dfuERROR, ABORT any idle state of a
	// transfer. waitIdle() sends CLRSTATUS in every state, which the keyed bootloader takes; a ROM loader answers a
	// request its state doesn't allow with a stall and an error
	async toIdle() {
		let st;
		for (let attempt = 0; attempt < 20; attempt++) {
			st = await this.getStatus();
			if (st.state === ST_IDLE) return;
			if (st.state === ST_DNBUSY) await this.wait(Math.max(st.pollMs, 1));
			else await this.t.out(st.state === ST_ERROR ? CLRSTATUS : ABORT, 0);
		}
		throw new DfuError(`the bootloader stays in state ${STATE_NAMES[st.state] || st.state}, not dfuIDLE: power-cycle the radio into the bootloader and try again`);
	}

	// Whether a ROM loader's program memory is read protected, asked without changing anything: the address pointer
	// is set to the start of the flash (allowed with the protection on, AN3156 5.2) and 4 bytes are asked for there. A
	// protected chip does not perform the read and reports dfuERROR with errVENDOR (AN3156 4.1, figures 3 and 4),
	// whether it stalls the request or answers it with no bytes. true: protected, false: the bytes came. Anything
	// else is no answer, and is thrown: "protected" leads to the erase of everything, so it is never concluded from
	// an answer the documents don't give (errVENDOR without the error state is one)
	async probeProtected(board) {
		await this.toIdle();
		await this.command(SET_ADDRESS, board.rom_base, `set address ${hex(board.rom_base)}`);
		await this.toIdle();	// a read is taken in dfuIDLE only (AN3156 figure 3); ABORT leaves dfuDNLOAD-IDLE
		let data = null, refused = null;
		try {
			// block 2: the address is the pointer itself, whatever transfer size the loader multiplies by
			data = await this.t.in(UPLOAD, 4, FIRST_BLOCK);
		} catch (e) {
			refused = e;	// not acknowledged: the status says why
		}
		const st = await this.getStatus();
		if (st.state === ST_ERROR && st.status === ERR_VENDOR) {
			await this.toIdle();
			return true;
		}
		if (refused || st.status !== 0 || st.state === ST_ERROR || data.length !== 4)
			throw new DfuError(`the read protection check got no clear answer (${refused ? refused.message : `${data.length} bytes`}, status ${st.status}, state ${STATE_NAMES[st.state] || st.state}): nothing was written`);
		await this.toIdle();
		return false;
	}

	// Read Unprotect (AN3156 5.4): the loader removes the read protection by erasing the whole program memory and the
	// RAM and resetting the option bytes to their defaults (loader V9.0 of the STM32H74x leaves a PCROP area as it
	// was, AN2606 Table 146), then leaves the bus. It runs on the GETSTATUS after the
	// command, and the device may not answer anything after that: the disconnect is the result, and the next
	// probeProtected() on the loader, entered again, says whether it worked. A board with a protected region in its
	// program memory refuses before anything is sent. The caller asks the user first: nothing here does
	async readUnprotect(board) {
		protect.checkWhole(board.protect, "progmem");
		await this.toIdle();
		this.log("read unprotect: the whole program memory is erased");
		await this.t.out(DNLOAD, 0, new Uint8Array([READ_UNPROTECT]));
		let st;
		try {
			st = await this.getStatus();
		} catch (e) {
			this.log(`the loader left the bus (${e.message})`);
			return;
		}
		if (st.state !== ST_DNBUSY || st.status !== 0) throw new DfuError(`read unprotect was refused: status ${st.status}, state ${STATE_NAMES[st.state] || st.state}`);
	}

	// The plain route: the plan's sectors erased, then written and read back with the vector table's block last
	// (checkPlain), then Leave at the application base (the loader loads
	// the stack pointer there and jumps to the reset vector, AN3156 5.5). Every block is sent as block 2 right after its
	// own Set Address Pointer: the loader places a block at (block number - 2) x wTransferSize + pointer, and AN3156
	// 5.1 calls wTransferSize "length of the data buffer sent by the host" while the DFU functional descriptor has a
	// field of that name. With block 2 the product is zero under either reading. Whatever fails once the vector
	// table's write has begun, its sector is erased again (unvector): a table that is in flash and did not pass its
	// check is not left to start at the next normal power-on. rfoundry's Dfu.run does the same, step for step
	async runPlain(plan, key, progress = () => {}) {
		const board = plan.board;
		if (key && key.length) throw new DfuError(`the ${board.name || "radio"}'s loader takes no key: a plain download is never XORed`);
		// makePlan refuses or carves these; a plan built another way is checked again before anything is sent
		for (const s of plan.erase) protect.check(board.protect, "progmem", s.addr - board.rom_base, s.addr + s.size - board.rom_base, "erase");
		// the application base is the last address the loader is given (Leave): in the flash too
		if (board.app_base < board.rom_base || board.app_base >= board.flash_end) throw new DfuError(`the ${board.name || "radio"}'s application base ${hex(board.app_base)} is outside its program flash`);
		// every address the loader is given is in the board's program flash and its application region, in whole blocks
		checkPlain(plan);
		await this.toIdle();
		for (const s of plan.eraseOrder) {
			this.log(`erase sector ${s.index} at ${hex(s.addr)}`);
			await this.command(ERASE, s.addr, `erase ${hex(s.addr)}`);
		}
		const total = plan.totalBlocks;
		let done = 0;
		const write = async (st, blocks) => {
			if (blocks.length) this.log(`write sector ${st.sector.index}: ${blocks.length} blocks`);
			for (const [num, blk] of blocks) {
				const addr = st.sector.addr + (num - FIRST_BLOCK) * BLOCK;
				await this.command(SET_ADDRESS, addr, `set address ${hex(addr)}`);
				await this.t.out(DNLOAD, FIRST_BLOCK, blk);
				await this.waitDone(`block at ${hex(addr)}`);
				progress(++done, total);
			}
		};
		if (plan.writes.length) {
			// the vector table's block (checkPlain: the first one of the last sector) after all the rest, which is read
			// back first: until it is written a cut install has nothing to start
			const again = "put the radio in its bootloader again and install again";
			const order = plan.writeOrder, head = order.at(-1);
			const body = [...order.slice(0, -1).map((st) => [st, st.blocks]), [head, head.blocks.slice(1)]];
			for (const [st, blocks] of body) await write(st, blocks);
			try {
				await this.readBack(body);
			} catch (e) {
				if (!(e instanceof DfuError)) throw e;
				throw new DfuError(`${e.message}. The vector table is not written yet, so the radio starts nothing until the install has run again: ${again}`);
			}
			this.log(`write the vector table at ${hex(board.app_base)}`);
			try {
				await write(head, head.blocks.slice(0, 1));
				await this.readBack([[head, head.blocks.slice(0, 1)]], { vectors: true });	// before the start
			} catch (e) {
				// a write the loader reports as failed, a read it refuses, a block that reads back different, a lost
				// device: in each the table may be in flash, and nothing has checked it
				const what = e instanceof DfuError ? e.message : `USB error at the vector table: ${e.message}`;
				throw new DfuError(`${what}${await this.unvector(head)}: ${again}`);
			}
		}
		await this.command(SET_ADDRESS, board.app_base, `set address ${hex(board.app_base)}`);
		await this.t.out(DNLOAD, 0);
		try {
			// the image is written and read back whatever this says: a loader that reports an error here did not start it
			if ((await this.getStatus()).state === ST_ERROR) this.log("the bootloader did not start the image: switch the radio off and on");
		} catch (e) { /* the radio starts the application; it may not answer */ }
	}

	// The plain route's check of what it wrote, as rfoundry's Dfu.read_back: every block of parts ([[step, blocks]])
	// read back and compared with the block sent. A ROM loader reports no error for a write it did not carry out (a
	// write-protected sector, AN3156 5.1 and 5.3), and a block placed at another address than meant shows here too. Each
	// block is read as block 2 right after its own Set Address Pointer, as it was written; UPLOAD is taken in dfuIDLE
	// only (AN3156 figure 3), so ABORT goes before it and again after it. A failure leaves the image unstarted with the
	// radio in its ROM loader, where the same install can run again; until it has, the radio starts nothing at a normal
	// power-on. runPlain says why in its message: before the vector table is written there is none, and a failure of
	// the vector table's own check (vectors: these blocks are it) erases its sector again
	async readBack(parts, { vectors = false } = {}) {
		const n = parts.reduce((a, [, blocks]) => a + blocks.length, 0);
		if (!n) return;
		this.log(`verify: read back ${vectors ? "the vector table" : `${n} blocks`}`);
		for (const [st, blocks] of parts) {
			for (const [num, blk] of blocks) {
				const addr = st.sector.addr + (num - FIRST_BLOCK) * BLOCK;
				await this.command(SET_ADDRESS, addr, `set address ${hex(addr)}`);
				await this.toIdle();
				let got;
				try {
					got = await this.t.in(UPLOAD, blk.length, FIRST_BLOCK);
				} catch (e) {
					throw new DfuError(`the block at ${hex(addr)} could not be read back (${e.message}): the image is written but not checked, and was not started`);
				}
				await this.toIdle();
				let at = 0;
				while (at < blk.length && at < got.length && got[at] === blk[at]) at++;
				if (at < blk.length || got.length !== blk.length)
					throw new DfuError(`the flash at ${hex(addr + at)} does not read back what was written there (${got.length} of ${blk.length} bytes read): the write did not take, and the image was not started`);
			}
		}
	}

	// The vector table's sector (st: its step) erased again after its write or its check failed, and what that leaves
	// to start at a normal power-on, in words for the message. The failure may have left the loader in any state,
	// dfuERROR after a refused write or read among them: toIdle before the erase
	async unvector(st) {
		try {
			await this.toIdle();
			await this.command(ERASE, st.sector.addr, `erase ${hex(st.sector.addr)}`);
		} catch (e) {
			return `. Its vector table could not be erased again (${e.message}): at a normal power-on the radio may start an image whose vector table did not pass its check`;
		}
		return ". The vector table's sector is erased again, so the radio starts nothing until the install has run again";
	}

	// Erase the plan's sectors (in the bootloader's order), write each, exit (the radio restarts). A codec move erases the
	// application's first sector first and writes it last (plan.js writeOrder): a move cut short leaves no application
	// to start. programMode: this
	// bootloader session already took 91 01 (a backup read its SPI flash, origbackup.js); the bootloader takes 91 01 only
	// once, so the write starts at 91 31
	// A board of the plain family that has no key is written plain (runPlain); every other board needs its key
	async run(plan, key, progress = () => {}, { programMode = false } = {}) {
		if (plan.board.family === PLAIN_FAMILY && !plan.board.key_sha256) return this.runPlain(plan, key, progress);
		const base = plan.board.app_base;
		// checked before the erase: every block is XORed with it, so a missing key would stop at the first block with
		// the application area already erased
		if (!key || key.length !== KEY_LEN) throw new DfuError(`no bootloader key for ${plan.board.name || "this radio"}`);
		// makePlan refuses or carves these; a plan built another way is checked again before anything is sent
		for (const s of plan.erase) protect.check(plan.board.protect, "progmem", s.addr - plan.board.rom_base, s.addr + s.size - plan.board.rom_base, "erase");
		for (const st of plan.writes) if ((st.sector.addr - base) % KEY_LEN) throw new DfuError(`sector ${hex(st.sector.addr)} is not key-aligned`);
		await this.waitIdle();
		if (!programMode) await this.vendor(0x91, 0x01);
		await this.vendor(0x91, 0x31);
		for (const s of plan.eraseOrder) {
			this.log(`erase sector ${s.index} at ${hex(s.addr)}`);
			await this.command(0x41, s.addr, `erase ${hex(s.addr)}`);
		}
		const total = plan.totalBlocks;
		let done = 0;
		for (const st of plan.writeOrder) {
			const addr = st.sector.addr;
			this.log(`write sector ${st.sector.index}: ${st.blocks.length} blocks`);
			await this.command(0x21, addr, `set address ${hex(addr)}`);
			for (const [num, blk] of st.blocks) {
				if (blk.length !== BLOCK) throw new DfuError(`block of ${blk.length} bytes`);
				await this.t.out(DNLOAD, num, xorBlock(blk, key));
				await this.waitDone(`block ${num} at ${hex(addr + (num - FIRST_BLOCK) * BLOCK)}`);
				progress(++done, total);
			}
		}
		await this.command(0x21, EXIT_ADDR, `set address ${hex(EXIT_ADDR)}`);
		await this.t.out(DNLOAD, 0);
		try { await this.getStatus(); } catch (e) { /* the radio restarts; it may not answer */ }
	}
}

// ---------------------------------------------------------------------------------------------------------------
// WebUSB

// The radio's own firmware, switched on normally, is a DFU device with the bootloader's IDs and strings, but erasing
// or writing through it can reach the bootloader's sectors (hardware-dm1701.md 15.1). names: the DFU interface strings,
// alt 0 first. A board with a declared bootloader layout (dfu_layout) accepts only that; any other refuses a layout
// that shows only the bootloader's sectors (from 0x08000000, at most BOOT_AREA bytes), as every original firmware's
// does. A bootloader shows the application area, so one that listed the whole flash would still pass. The same rule
// as rfoundry's dfu.normal_mode.
const BOOT_AREA = 0xC000;
const UNIT = { K: 1024, M: 1024 * 1024 };
// The first region of a DfuSe interface string ("@name /0xADDR/NN*SSSKg,..."): [start, bytes], null if it has none
export function dfuRegion(name) {
	const m = /\/0x([0-9a-f]+)\/([^/]*)/i.exec(name || "");
	if (!m) return null;
	let bytes = 0;
	for (const [, n, size, unit] of m[2].matchAll(/(\d+)\*(\d+)\s*([KM]?)/g)) bytes += Number(n) * Number(size) * (UNIT[unit] || 1);
	return [parseInt(m[1], 16), bytes];
}

export function normalMode(names, board) {
	const norm = (n) => (n || "").split(/\s+/).filter(Boolean).join(" ");
	const got = names.filter(Boolean).map(norm);
	if (!got.length) return false;
	if (board?.dfu_layout) return got[0] !== norm(board.dfu_layout);
	return got.some((n) => {
		const r = dfuRegion(n);
		return !!r && r[0] === 0x08000000 && r[1] <= BOOT_AREA;
	});
}

// A ROM loader's device, checked before anything is asked of it (rfoundry's cmd_flash_dfuse makes the same check):
// string 1 must not be the keyed bootloader's of another family, which shares these USB IDs on some radios and would
// take a plain image as an encrypted one; and alternate 0 must show the board's whole program flash, from the flash
// base to its end (or be the board's declared dfu_layout, once that was read on a radio). A device that shows no
// layout is refused too: the page then can't tell what it would erase. info: WebUsbTransport.fingerprint()
export function romDevice(board, info) {
	const norm = (n) => (n || "").split(/\s+/).filter(Boolean).join(" ");
	const string1 = info?.string1 || "";
	if (/anyroad/i.test(string1)) throw new WrongDeviceError(`this is a keyed bootloader ("${string1}"), not the processor's own loader: nothing was sent`, string1);
	const alt0 = norm(info?.interfaces?.[0]);
	if (board.dfu_layout) {
		if (alt0 !== norm(board.dfu_layout)) throw new WrongDeviceError(`the device shows the layout "${alt0 || "none"}", not this radio's: nothing was sent`, alt0 || "?");
		return;
	}
	const r = dfuRegion(alt0);
	if (!r || r[0] !== board.rom_base || r[1] !== board.flash_end - board.rom_base)
		throw new WrongDeviceError(`the device shows the layout "${alt0 || "none"}", not ${(board.flash_end - board.rom_base) / 1024} KB of flash from ${hex(board.rom_base)}: nothing was sent`, alt0 || "?");
}

export class NormalModeError extends DfuError {
	constructor(layout) {
		super(`the radio is switched on normally, not in its bootloader (layout ${layout})`);
		this.name = "NormalModeError";
		this.layout = layout;
	}
}

export const usbSupported = () => typeof navigator !== "undefined" && "usb" in navigator;

// A WebUSB call the radio never answers waits forever, and the page with it (#446). rfoundry gives each control transfer
// 5 s (dfu.py), a sector erase included; twice that with no reply is a lost connection. errorText reads the "USB
// transfer" prefix as one
export const USB_TIMEOUT_MS = 10000;
function timed(promise, what, ms) {
	let timer;
	const late = new Promise((_, reject) => {
		timer = setTimeout(() => reject(new DfuError(`USB transfer timeout: no reply to ${what} in ${ms / 1000} s`)), ms);
	});
	return Promise.race([promise, late]).finally(() => clearTimeout(timer));
}

export class WebUsbTransport {
	constructor(device, intf, timeoutMs = USB_TIMEOUT_MS) {
		this.device = device;
		this.intf = intf;
		this.timeoutMs = timeoutMs;
	}

	static async request(board) {
		const [vendorId, productId] = board.dfu_ids;
		const device = await navigator.usb.requestDevice({ filters: [{ vendorId, productId }] });
		return WebUsbTransport.open(device, board);
	}

	// board: refuse the radio switched on normally before claiming anything or sending a DFU request
	// timeoutMs: how long any one USB call may go unanswered
	static async open(device, board = null, { timeoutMs = USB_TIMEOUT_MS } = {}) {
		await timed(device.open(), "open", timeoutMs);
		if (device.configuration === null) await timed(device.selectConfiguration(1), "select configuration", timeoutMs);
		const names = [];
		for (const i of device.configuration.interfaces)
			for (const alt of i.alternates)
				if (alt.interfaceClass === 0xFE && alt.interfaceSubclass === 0x01) names.push(alt.interfaceName);
		if (board && normalMode(names, board)) {
			try { await device.close(); } catch (e) { /* gone */ }
			throw new NormalModeError(names[0]);
		}
		let intf = null;
		for (const i of device.configuration.interfaces)
			for (const alt of i.alternates)
				if (alt.interfaceClass === 0xFE && alt.interfaceSubclass === 0x01 && intf === null) intf = i.interfaceNumber;
		if (intf === null) throw new DfuError("the device has no DFU interface");
		try {
			await timed(device.claimInterface(intf), "claim interface", timeoutMs);
		} catch (e) {
			if (e instanceof DfuError) throw e;
			throw new DfuError(`cannot open the bootloader (${e.message}). Close other programs that use the radio.`);
		}
		// the interface is claimed and no alternate setting is ever selected: it stays on 0, which on a ROM loader is
		// the internal flash (its alternate 1 is the option bytes, never selected or written)
		return new WebUsbTransport(device, intf, timeoutMs);
	}

	async out(request, value, data) {
		const r = await timed(this.device.controlTransferOut({ requestType: "class", recipient: "interface", request, value, index: this.intf },
			data || new Uint8Array(0)), `request ${request}`, this.timeoutMs);
		if (r.status !== "ok") throw new DfuError(`USB transfer ${r.status}`);
		return r.bytesWritten;
	}

	async in(request, length, value = 0) {
		const r = await timed(this.device.controlTransferIn({ requestType: "class", recipient: "interface", request, value, index: this.intf }, length),
			`request ${request}`, this.timeoutMs);
		if (r.status !== "ok") throw new DfuError(`USB transfer ${r.status}`);
		return new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
	}

	async stringDescriptor(index) {
		try {
			const r = await timed(this.device.controlTransferIn({ requestType: "standard", recipient: "device", request: 6, value: 0x0300 | index, index: 0x0409 }, 255),
				"string descriptor", this.timeoutMs);
			const b = new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
			return new TextDecoder("utf-16le").decode(b.subarray(2, b[0]));
		} catch (e) {
			return null;
		}
	}

	// What the bootloader shows: the strings the model fingerprint table will be built from
	async fingerprint() {
		const d = this.device;
		const names = [];
		for (const i of d.configuration.interfaces) for (const a of i.alternates) if (a.interfaceName) names.push(a.interfaceName);
		return { string1: await this.stringDescriptor(1), manufacturer: d.manufacturerName, product: d.productName,
			serial: d.serialNumber, version: `${d.deviceVersionMajor}.${d.deviceVersionMinor}.${d.deviceVersionSubminor}`, interfaces: names };
	}

	async close() {
		try { await this.device.close(); } catch (e) { /* already gone: the radio restarted */ }
	}

	// left() once this device has left the bus: WebUSB's disconnect event for it, which comes for a device the page
	// was given even after close(). Returns the function that stops the watch, or null where there is no WebUSB
	watchLeave(left) {
		const usb = typeof navigator !== "undefined" ? navigator.usb : null;
		if (!usb?.addEventListener) return null;
		const dev = this.device;
		const on = (ev) => { if (ev.device === dev) left(); };
		usb.addEventListener("disconnect", on);
		return () => usb.removeEventListener("disconnect", on);
	}
}
