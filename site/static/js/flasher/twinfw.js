// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stock firmware container that holds its image twice, in the browser: parse, key recovery, carving the codec
// windows. A port of tools/rfoundry/rfoundry/twinfw.py; a profile names the container with "container": "twin"
// (stockfw.js). The file is a plain 0x80-byte header and a payload XORed with a 256-byte key: the image's length, the
// image, the length again, the same image again, two zero words. Layout, key recovery and the carve recipe are on the
// codec page of each radio whose profile names this container (docs/spec/codec-<id>.md: "Container and encryption",
// "Key recovery", "Extraction recipe").
//
// The key is the container's own, not a bootloader's: it is derived from the file (the two copies give it without a
// guess), checked by its SHA-256 and never stored. Nothing here names a radio: what a file is for is the profile's
// matter. The refusals are rfoundry's, word for word (site/tests/twinfw.test.mjs compares them).

import { hex, sha256, u32le } from "./bytes.js";
import { StockError } from "./stockfw.js";

export const HEADER_LEN = 0x80;		// the plain header; the payload follows it
export const KEY_LEN = 256;
const LEN_AT = 0x14;			// u32: the payload's length
const OFF_AT = 0x44;			// u32: the payload's offset, HEADER_LEN, in a firmware file
export const BOOT_OFF_AT = 0x48;	// the same word in a bootloader file of this container
const TAIL = 8;				// the two zero words that end the payload
const MAX_TRAILER = 16;			// bytes a file may carry after its payload (the profiled files: 2)

const X = (n) => n.toString(16).toUpperCase();

// [payload offset, payload length, N] of a firmware file in this container, N the length of the image it holds twice.
// The strict test, for a codec source; throws StockError for anything else
export function parse(data) {
	if (data.length < HEADER_LEN + 16) throw new StockError("too short for a firmware file", "damaged");
	const off = u32le(data, OFF_AT), length = u32le(data, LEN_AT);
	const after = data.length - HEADER_LEN - length;
	if (off !== HEADER_LEN || length < 16 || (length - 16) % 2 || after < 0 || after > MAX_TRAILER)
		throw new StockError("not a firmware file in this container", "not-vendor");
	return [off, length, (length - 16) / 2];
}

// What a file's header says it is: "firmware" (the payload offset at OFF_AT), "boot" (at BOOT_OFF_AT: a bootloader
// file, which holds no firmware and no codec), or null. It reads the two words and nothing else, so a file that was cut
// short or padded still counts as the maker's. No application image matches: there the two words are vector table
// entries, addresses of handlers, and 0x80 is an address inside the table itself
export function headerKind(data) {
	if (data.length < BOOT_OFF_AT + 4) return null;
	return u32le(data, OFF_AT) === HEADER_LEN ? "firmware" : u32le(data, BOOT_OFF_AT) === HEADER_LEN ? "boot" : null;
}

// n plain bytes from payload offset at: plain[i] = file[off + i] XOR key[i mod 256]
function decrypt(data, off, at, n, key) {
	if (at < 0 || off + at + n > data.length) throw new StockError(`0x${X(at)}+0x${X(n)} is outside the payload`);
	const out = new Uint8Array(n);
	for (let i = 0; i < n; i++) out[i] = data[off + at + i] ^ key[(at + i) % KEY_LEN];
	return out;
}

const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const gcd = (a, b) => (b ? gcd(b, a % b) : a);

// The 256-byte key of a firmware file, from the file alone, checked against its SHA-256 (twinfw.derive_key, the same
// steps and tie order, so both give the same key).
//
// The same plain byte is in the file twice, N + 4 bytes apart, so under two key columns s = (N + 4) mod 256 apart: the
// XOR of the two copies gives key[c] XOR key[c + s] for every column c, without error. The columns then form
// gcd(s, 256) chains, and one byte fixes each chain: the one whose derived key bytes are, summed over the chain, the
// most common bytes of their columns (a firmware image is mostly 0x00, so the most common cipher byte of a column is
// its key byte nearly everywhere; a vote alone gets a few columns wrong, the chain sum none). Throws StockError when
// the copies differ or the result is not the key
export async function deriveKey(data, wantSha) {
	const [off, length, n] = parse(data);
	if (n < 64 * KEY_LEN) throw new StockError("the file is too short to derive the key from", "damaged");
	const dist = n + 4, step = dist % KEY_LEN;
	const differ = () => new StockError("the file's two copies differ", "damaged");
	// d[p] = file[off + p] XOR file[off + dist + p] must have period 256: its first 256 bytes are the key's differences
	const x = new Uint8Array(KEY_LEN);
	for (let p = 0; p < dist; p++) {
		const d = data[off + p] ^ data[off + dist + p];
		if (p < KEY_LEN) x[p] = d;
		else if (d !== x[p % KEY_LEN]) throw differ();
	}
	const counts = Array.from({ length: KEY_LEN }, () => new Uint32Array(256));
	for (let i = 0; i < length; i++) counts[i % KEY_LEN][data[off + i]]++;
	const key = new Uint8Array(KEY_LEN);
	const chainLen = KEY_LEN / gcd(step, KEY_LEN);
	for (let first = 0; first < KEY_LEN / chainLen; first++) {	// the chains start at the columns below gcd(step, 256)
		const chain = Array.from({ length: chainLen }, (_, i) => (first + i * step) % KEY_LEN);
		let best = -1, bestVals = null;
		for (let g = 0; g < 256; g++) {
			let v = g, score = 0;
			const vals = [];
			for (const c of chain) {
				vals.push(v);
				score += counts[c][v];
				v ^= x[c];
			}
			if (v !== g) throw differ();	// the XOR around a chain is 0 for two equal copies
			if (score > best) { best = score; bestVals = vals; }	// a tie keeps the lower byte
		}
		chain.forEach((c, i) => { key[c] = bestVals[i]; });
	}
	// the file was identified by its SHA-256 before this: a key that doesn't come out is the flasher's fault
	if ((await sha256(key)) !== wantSha) throw new StockError("could not derive the file's key");
	return key;
}

// The decrypted windows [[addr, bytes]] of a profiled file (twinfw.carve). wins: [[image address, length, SHA-256]],
// the code window first. Every check is against the profile: the file's size, the header's payload and the image
// length in both of its places, the zero words at the end, each window from both copies and its SHA-256, the code's
// first word (codecSig). m: the code's map; its ranges (code.ranges), where it has them, are checked too, each by its
// SHA-256 and first word. Messages name a window by its address. The file was identified by its SHA-256 before this,
// so a check that fails here is the flasher's or its data's (code "internal")
export async function carve(data, prof, wins, key, codecSig, m = null) {
	if (data.length !== prof.size) throw new StockError(`size ${data.length}, expected ${prof.size}`);
	const [off, length, n] = parse(data);
	const [link, wantN] = prof.image;
	if (off !== prof.payload[0] || length !== prof.payload[1])
		throw new StockError(`payload 0x${X(off)}+0x${X(length)}, expected 0x${X(prof.payload[0])}+0x${X(prof.payload[1])}`);
	if (n !== wantN) throw new StockError(`image length 0x${X(n)}, expected 0x${X(wantN)}`);
	if (u32le(decrypt(data, off, 0, 4, key), 0) !== n || u32le(decrypt(data, off, 4 + n, 4, key), 0) !== n)
		throw new StockError("the payload's length words don't hold the image length (wrong key?)");
	if (decrypt(data, off, 8 + 2 * n, TAIL, key).some((b) => b)) throw new StockError("the payload doesn't end with its zero words");
	const out = [];
	for (const [addr, size, want] of wins) {
		if (!(link <= addr && addr + size <= link + n)) throw new StockError(`window ${hex(addr)}+0x${X(size)} is outside the image`);
		const at = 4 + addr - link;
		const plain = decrypt(data, off, at, size, key);
		if (!same(decrypt(data, off, at + n + 4, size, key), plain)) throw new StockError(`the two copies differ in a codec window (${hex(addr)})`);
		const got = await sha256(plain);
		if (got !== want) throw new StockError(`decrypted window ${hex(addr)} does not match (sha256 ${got}, expected ${want})`);
		out.push([addr, plain]);
	}
	const code = out[0][1];
	if (code.length < 4 || u32le(code, 0) !== codecSig) throw new StockError(`codec first word is not ${hex(codecSig)}`);
	for (const r of m?.code?.ranges || []) {
		const part = code.subarray(r.off, r.off + r.len);
		if (part.length !== r.len || (r.sha256 !== undefined && (await sha256(part)) !== r.sha256) || (r.first_word !== undefined && u32le(part, 0) !== r.first_word))
			throw new StockError(`the code's range at ${hex(out[0][0] + r.off)} does not match`);
	}
	return out;
}
