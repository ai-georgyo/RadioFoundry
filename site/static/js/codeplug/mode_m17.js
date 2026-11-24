// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The M17 mode module of the Codeplug Editor (docs/spec/m17-codeplug.md): kind 6 and system type 6 on the channel-mode
// layer (channelmodes.js). Bytes to fields and back, and the fields the pages show, as data. The same bytes as the
// firmware's core/m17/m17_codeplug.c and m17_addr.c, tested against the same vectors (tests/vectors/channel-modes.txt).
// Talk targets are M17 addresses in the channel's own block, not contacts. The signing key and the station keys are
// P-256 (docs/spec/p256.md): the curve arithmetic here is exact and public (a key typed or read, a key checked);
// generating a key is WebCrypto's.

import { u8, u16, put16, grow, hex, unhex } from "./modebytes.js";

// The registry id (channel-modes.md 4): the kind and the system type share it
export const M17_KIND = 6;

// --- Addresses (m17.md 10) ---

export const ADDR_BROADCAST = 0xFFFFFFFFFFFF, ADDR_STD_MAX = 0xEE6B27FFFFFF, CALL_MAX = 9;
const ALPHABET = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
export const ADDR_KIND = { reserved: 0, std: 1, extended: 2, broadcast: 3 };

// A character's base-40 value, 0 for space and for one outside the alphabet (letters of either case)
export function addrValue(ch) {
	if (ch === " " || ch.length !== 1) return 0;
	const i = ALPHABET.indexOf(ch >= "a" && ch <= "z" ? ch.toUpperCase() : ch);
	return i < 0 ? 0 : i;
}

// Text to an address: {addr} or {error: "chars" | "long" | "empty"}. Letters upper-cased, trailing spaces dropped; a
// character outside the alphabet is refused, never read as a space (m17_addr_from_text)
export function parseAddr(text) {
	const s = String(text ?? "").replace(/ +$/, "");
	if (!s) return { error: "empty" };
	if ([...s].some((ch) => ch !== " " && addrValue(ch) === 0)) return { error: "chars" };
	if (s.length > CALL_MAX) return { error: "long" };
	let a = 0;
	for (let i = s.length - 1; i >= 0; i--) a = a * 40 + addrValue(s[i]);
	return { addr: a };
}

// An address as text and its kind (m17_addr_text): a standard one as its characters, an extended or reserved one as
// 12 hex digits, broadcast as ""
export function addrText(addr) {
	if (addr === ADDR_BROADCAST) return { kind: ADDR_KIND.broadcast, text: "" };
	if (addr === 0 || addr > ADDR_STD_MAX) {
		return { kind: addr === 0 ? ADDR_KIND.reserved : ADDR_KIND.extended, text: addr.toString(16).toUpperCase().padStart(12, "0") };
	}
	let s = "";
	for (let a = addr; a > 0; a = Math.floor(a / 40)) s += ALPHABET[a % 40];
	return { kind: ADDR_KIND.std, text: s };
}

const addrGet = (b, o) => (b && o + 6 <= b.length ? [...b.subarray(o, o + 6)].reduce((a, x) => a * 256 + x, 0) : 0);
function addrPut(b, o, a) {
	for (let i = 5; i >= 0; i--, a = Math.floor(a / 256)) b[o + i] = a % 256;
}

// The text an address field shows, and what typing in one stores: a callsign, or an address no callsign spells as
// 0x and its 12 hex digits (another tool's)
const addrField = (a) => (a > 0 && a <= ADDR_STD_MAX ? addrText(a).text : "0x" + a.toString(16).toUpperCase().padStart(12, "0"));
function addrTyped(s, t) {
	const raw = /^0x([0-9a-f]{12})$/i.exec(s.trim());
	if (raw) return { value: parseInt(raw[1], 16) };
	// Leading spaces are part of an address (value 0, m17.md 10), as m17_addr_from_text reads them: an address
	// another tool stored with one shows with it and reads back the same when the form is saved untouched
	const r = parseAddr(s.replace(/\s+$/, ""));
	if (r.error === "chars") return { error: t("editor.problem.m17_callsign_chars") };
	if (r.error === "long") return { error: t("editor.problem.m17_callsign_long") };
	return r.error ? { value: 0 } : { value: r.addr };
}
// Words a user may type for "everyone": a callsign of its own, so the editor points at All call (m17_callsign_all)
const ALL_WORDS = ["ALL", "ALLCALL", "BROADCAST", "EVERYONE", "CQ"];

// --- A channel's parameter block (m17-codeplug.md 2) ---

export const CONV_LEN = 12, RATE_1600 = 0x04, TX_CAN_SET = 0x80, CONV_RESERVED = 0xF8;
export const ENC = { none: 0, scrambler: 1, aes: 2 };
export const SQUELCH = { can: 0, any: 1, addressed: 2 };

export function decodeConv(b) {
	const f = u8(b, 0), can = u8(b, 1) & 15, t = u8(b, 2), e = u8(b, 9);
	const sq = f & 3;
	return {
		squelch: sq <= 2 ? sq : 0,
		mode1600: (f & RATE_1600) !== 0,
		can,
		txCan: t & TX_CAN_SET ? t & 15 : null,	// null: the CAN
		dst: addrGet(b, 3) || ADDR_BROADCAST,	// a stored zero, or a block cut short of it, is broadcast
		enc: e === ENC.scrambler || e === ENC.aes ? e : ENC.none,
		keyId: b?.length >= 12 ? u16(b, 10) : 0,
	};
}

// prev: the block stored; byte 0's reserved bits and anything after byte 11 stay
export function encodeConv(p, prev = null) {
	const v = grow(prev, CONV_LEN);
	const can = p.can & 15, tx = p.txCan == null ? can : p.txCan & 15;
	v[0] = (v[0] & CONV_RESERVED) | (p.squelch >= 0 && p.squelch <= 2 ? p.squelch : 0) | (p.mode1600 ? RATE_1600 : 0);
	v[1] = can;
	v[2] = tx === can ? 0 : TX_CAN_SET | tx;
	addrPut(v, 3, p.dst === ADDR_BROADCAST ? 0 : p.dst);
	v[9] = p.enc === ENC.scrambler || p.enc === ENC.aes ? p.enc : 0;
	put16(v, 10, p.keyId & 0xFFFF);
	return v;
}

// --- The radio-wide record, key 0x0A06 (m17-codeplug.md 3) ---

export const WIDE_LEN = 60, TEXT_MAX = 52, WIDE_SIGN = 0x01, WIDE_GNSS = 0x02, WIDE_RESERVED = 0xFC;
const STATIONS = [0, 1, 2, 15];

export function decodeWide(b) {
	let n = 0;
	while (n < TEXT_MAX && 8 + n < (b?.length ?? 0) && b[8 + n] !== 0) n++;
	const st = u8(b, 7);
	return {
		src: addrGet(b, 0),
		sign: (u8(b, 6) & WIDE_SIGN) !== 0,
		gnss: (u8(b, 6) & WIDE_GNSS) !== 0,
		station: STATIONS.includes(st) ? st : 0,
		text: n ? new TextDecoder().decode(b.subarray(8, 8 + n)) : "",
	};
}

export function encodeWide(w, prev = null) {
	const v = grow(prev, WIDE_LEN);
	addrPut(v, 0, w.src || 0);
	v[6] = (v[6] & WIDE_RESERVED) | (w.sign ? WIDE_SIGN : 0) | (w.gnss ? WIDE_GNSS : 0);
	v[7] = w.station & 0xFF;
	// The text unchanged: its bytes as stored. Another tool's that aren't UTF-8 read as U+FFFD, which written back
	// would change them when only a switch was
	if (prev && decodeWide(prev).text === (w.text ?? "")) return v;
	const t = new TextEncoder().encode(w.text ?? "").subarray(0, TEXT_MAX);
	v.fill(0, 8, 8 + TEXT_MAX);
	v.set(t.subarray(0, t.indexOf(0) < 0 ? t.length : t.indexOf(0)), 8);
	return v;
}
const wideDefault = (w) => !w.src && !w.sign && !w.gnss && !w.station && !w.text;

// --- P-256 (p256.md 2, 3): exact BigInt arithmetic on public values ---

const P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFFn;
export const N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551n;
const B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604Bn;
const G = [0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296n, 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5n];
const mod = (a) => ((a % P) + P) % P;
function pow(a, e) {
	let r = 1n;
	for (a = mod(a); e > 0n; e >>= 1n, a = (a * a) % P) if (e & 1n) r = (r * a) % P;
	return r;
}
const inv = (a) => pow(a, P - 2n);
const big = (b) => b.reduce((a, x) => (a << 8n) | BigInt(x), 0n);
const bytes32 = (x) => Uint8Array.from({ length: 32 }, (_, i) => Number((x >> BigInt(8 * (31 - i))) & 0xFFn));
const onCurve = ([x, y]) => x < P && y < P && mod(y * y - (x * x * x - 3n * x + B)) === 0n;

// Affine points, null the point at infinity (a = -3)
function add(p, q) {
	if (!p) return q;
	if (!q) return p;
	let l;
	if (p[0] === q[0]) {
		if (mod(p[1] + q[1]) === 0n) return null;
		l = mod((3n * p[0] * p[0] - 3n) * inv(2n * p[1]));
	} else {
		l = mod((q[1] - p[1]) * inv(q[0] - p[0]));
	}
	const x = mod(l * l - p[0] - q[0]);
	return [x, mod(l * (p[0] - x) - p[1])];
}
function mul(k, p) {
	let r = null;
	for (; k > 0n; k >>= 1n, p = add(p, p)) if (k & 1n) r = add(r, p);
	return r;
}
const compress = ([x, y]) => Uint8Array.of(2 + Number(y & 1n), ...bytes32(x));

// A public key as typed or stored, in any of the encodings a station gives one (64 bytes X || Y, 65 bytes 04 || X || Y,
// or 33 bytes 02 / 03 || X), checked on the curve: its 33-byte compressed form, null when it is none
export function pubCompressed(b) {
	if (!b) return null;
	let pt;
	if (b.length === 64 || (b.length === 65 && b[0] === 4)) {
		const o = b.length - 64;
		pt = [big(b.subarray(o, o + 32)), big(b.subarray(o + 32, o + 64))];
	} else if (b.length === 33 && (b[0] === 2 || b[0] === 3)) {
		const x = big(b.subarray(1));
		if (x >= P) return null;
		let y = pow(x * x * x - 3n * x + B, (P + 1n) / 4n);	// p = 3 mod 4: the square root, if there is one
		if (Number(y & 1n) !== (b[0] & 1)) y = mod(-y);
		pt = [x, y];
	} else return null;
	return onCurve(pt) && !(pt[0] === 0n && pt[1] === 0n) ? compress(pt) : null;
}

// 65 bytes 04 || X || Y from a compressed key (pubCompressed checked it)
export function pubUncompressed(c) {
	const x = big(c.subarray(1));
	let y = pow(x * x * x - 3n * x + B, (P + 1n) / 4n);
	if (Number(y & 1n) !== (c[0] & 1)) y = mod(-y);
	return Uint8Array.of(4, ...bytes32(x), ...bytes32(y));
}

// A signing key d (32 bytes): valid when 1 <= d <= n - 1 (p256.md 9)
export const privValid = (d) => !!d && d.length === 32 && big(d) >= 1n && big(d) < N;

// Q = d G, compressed: what the editor stores after d. Cached, as validation asks often
const qCache = new Map();
export function pubOfPriv(d) {
	const k = hex(d);
	if (!qCache.has(k)) {
		if (qCache.size > 16) qCache.clear();
		qCache.set(k, privValid(d) ? compress(mul(big(d), G)) : null);
	}
	return qCache.get(k);
}

// The fingerprint of a compressed key (m17_key_fp): the first 8 bytes of its SHA-256, 4 groups of 4 hex digits
export async function fingerprint(c) {
	const h = new Uint8Array(await crypto.subtle.digest("SHA-256", c));
	return hex(h.subarray(0, 8)).match(/..../g).join(" ");
}

// A public key WebCrypto takes: true when it imports (it checks the point on the curve too)
export async function pubImports(c) {
	try {
		await crypto.subtle.importKey("raw", pubUncompressed(c), { name: "ECDSA", namedCurve: "P-256" }, false, ["verify"]);
		return true;
	} catch {
		return false;
	}
}

// A new signing key from WebCrypto's generator: d as 64 hex digits
export async function generateKey() {
	const k = await crypto.subtle.generateKey({ name: "ECDSA", namedCurve: "P-256" }, true, ["sign", "verify"]);
	const jwk = await crypto.subtle.exportKey("jwk", k.privateKey);
	const d = Uint8Array.from(atob(jwk.d.replace(/-/g, "+").replace(/_/g, "/")), (c) => c.charCodeAt(0));
	return hex(d);
}

// --- System secret 0x0300 + n (m17-codeplug.md 4): d || Q compressed ---

export const SECRET_LEN = 65;
// {d, q} (q "" when the record has d alone, or a Q that isn't a compressed key: the radio derives it); a record of
// another length is no key the radio reads (m17_system_signing_key), kept whole as found
export function decodeSecret(v) {
	if (v.length !== 32 && v.length < SECRET_LEN) return { raw: v };
	const q = v.length >= SECRET_LEN && (v[32] === 2 || v[32] === 3) ? hex(v.subarray(32, 65)) : "";
	return { d: hex(v.subarray(0, 32)), q };
}
// Q is computed from d here, whatever the stored one said: the radio checks d G = Q
export function encodeSecret(s, prev) {
	const d = unhex(s.d);
	const v = grow(prev && prev.length > SECRET_LEN ? prev : null, SECRET_LEN);
	v.set(d, 0);
	v.set(pubOfPriv(d) ?? new Uint8Array(33), 32);
	return v;
}
export function secretValid(s) {
	const d = unhex(s.d);
	if (!privValid(d)) return false;
	return !s.q || s.q === hex(pubOfPriv(d));
}

// --- Traffic keys (channel-modes.md 5.4, this type's algorithm ids) ---

export const ALG = { scrambler: 1, aes: 2 };
export const keyLengthOk = (alg, n) => (alg === ALG.scrambler ? n >= 1 && n <= 3 : alg === ALG.aes && [16, 24, 32].includes(n));
const algOf = (n) => (n >= 1 && n <= 3 ? ALG.scrambler : ALG.aes);
// A key's algorithm: as read, else (a key typed in the editor) its length's
export const keyAlg = (k) => k.alg ?? algOf((unhex(k.key) ?? []).length);
// How the key list names one: AES by its size, a scrambler seed by its width (the algorithm is the select's above it)
const keyName = (k) => (keyAlg(k) === ALG.scrambler ? `${4 * k.key.length}-bit` : `AES-${4 * k.key.length}`);

// --- System entries, class 128: a station's public key (m17-codeplug.md 5) ---

export const ENTRY_STATION = 128, STATION_LEN = 39;

// --- The fields ---

const featureList = (f) => (f == null ? null : Array.isArray(f) ? f : String(f).split(","));
// The encryptions the radio runs: every one with no radio to ask; none in a build without them (the amateur and
// commercial variants)
function encOffered(features) {
	const have = featureList(features);
	return [ENC.scrambler, ENC.aes].filter((e) => !have || have.includes(e === ENC.scrambler ? "M17_SCRAMBLER" : "M17_AES"));
}

// The keys of the channel's system for an encryption
const keysFor = (c, enc) => (c.system && typeof c.system === "object" ? c.system.trafficKeys ?? [] : [])
	.filter((k) => keyAlg(k) === (enc === ENC.scrambler ? ALG.scrambler : ALG.aes)).sort((a, b) => a.id - b.id);

export const m17Kind = {
	id: M17_KIND,
	feature: "M17",
	systemType: M17_KIND,
	optionalSystem: true,
	contacts: false,	// the destination is in the block: an M17 address, not a contact
	ownFreqs: true,
	defaultTx: true,
	recordFields: [],
	flags: {},
	newParams: () => ({ squelch: SQUELCH.can, mode1600: false, can: 0, txCan: null, dst: ADDR_BROADCAST, enc: ENC.none, keyId: 0 }),
	decode: decodeConv,
	encode: encodeConv,
	fields: ({ features = null } = {}) => {
		const encs = encOffered(features);
		const out = [
			{ key: "can", label: "editor.m17.can", type: "int", min: 0, max: 15, blank: 0 },
			// squelch 1 is every CAN; switched off, a channel set to "addressed" by another tool stays so
			{ key: "squelch", label: "editor.m17.can_any", type: "select", options: (p) => [[SQUELCH.can, "editor.general.off"],
				[SQUELCH.any, "editor.general.on"], ...(p?.squelch === SQUELCH.addressed ? [[SQUELCH.addressed, "editor.m17.can_addressed"]] : [])] },
			{ key: "mode1600", label: "editor.m17.rate", type: "select", options: () => [[false, "editor.m17.rate3200"], [true, "editor.m17.rate1600"]] },
			{ key: "dst", label: "editor.m17.destination", type: "text", help: "editor.m17.destination_help",
				format: (v, t) => (v === ADDR_BROADCAST ? t("editor.m17.broadcast") : addrField(v)),
				parse: (s, t) => {
					if (!s.trim() || s.trim().toLowerCase() === t("editor.m17.broadcast").toLowerCase()) return { value: ADDR_BROADCAST };
					const r = addrTyped(s, t);
					return r.error || r.value ? r : { value: ADDR_BROADCAST };
				} },
		];
		if (encs.length) {
			out.push(
				{ key: "enc", label: "editor.m17.encryption", type: "select", options: (p) => [[ENC.none, "editor.general.off"],
					...encs.map((e) => [e, e === ENC.scrambler ? "editor.m17.scrambler" : "editor.m17.aes"]),
					...(p?.enc && !encs.includes(p.enc) ? [[p.enc, p.enc === ENC.scrambler ? "editor.m17.scrambler" : "editor.m17.aes"]] : [])] },
				{ key: "keyId", label: "editor.m17.key", type: "select", dynamic: true, show: (p) => !!p.enc,
					options: (p, c) => {
						const ks = c ? keysFor(c, p?.enc) : [];
						const out2 = ks.map((k) => [k.id, `${k.id} · ${keyName(k)}`]);
						if (p?.enc && !ks.some((k) => k.id === p.keyId)) out2.push([p.keyId, "editor.m17.key_missing"]);
						return out2;
					} },
			);
		}
		return out;
	},
	validate(c, p, problem) {
		if (!p) return;
		const { text } = addrText(p.dst);
		if (p.dst !== ADDR_BROADCAST && ALL_WORDS.includes(text.trim())) problem.warn("dst", "m17_callsign_all");
		if (p.enc && !keysFor(c, p.enc).some((k) => k.id === p.keyId)) problem.err("keyId", "m17_key_missing");
	},
	settings: {
		key: "m17",
		decode: decodeWide,
		encode: (s, prev) => (wideDefault(s) && !prev ? null : encodeWide(s, prev)),
		fields: [
			{ key: "src", label: "editor.m17.callsign", type: "text", help: "editor.m17.callsign_help",
				format: (v) => (v ? addrField(v) : ""), parse: (s, t) => addrTyped(s, t) },
			{ key: "text", label: "editor.m17.call_text", type: "text", help: "editor.m17.call_text_help",
				format: (v) => v ?? "", parse: (s, t) => {
					// as typed (spaces kept but the trailing ones); the radio sends it as it is, in up to four blocks
					const v = String(s).replace(/\s+$/, "");
					if (!/^[\x20-\x7E]*$/.test(v)) return { error: t("editor.problem.m17_text_ascii") };
					if (v.length > TEXT_MAX) return { error: t("editor.err.too_long", { max: TEXT_MAX }) };
					return { value: v };
				} },
			{ key: "gnss", label: "editor.m17.send_position", type: "bool" },
			{ key: "sign", label: "editor.m17.sign_calls", type: "bool", help: "editor.m17.sign_calls_help" },
		],
		validate(s, problem, m) {
			if (s.src && s.src > ADDR_STD_MAX) problem.err("src", "m17_callsign_chars");
			if (!/^[\x20-\x7E]*$/.test(s.text ?? "")) problem.err("text", "m17_text_ascii");
			const signers = (m?.systems ?? []).filter((x) => x.type === M17_KIND && x.secret && !x.secret.raw && secretValid(x.secret));
			if (s.sign && !signers.length) problem.err("sign", "m17_sign_no_key");
		},
	},
};

export const m17System = {
	id: M17_KIND,
	label: "editor.m17.kind",
	idLen: 0,
	conventional: true,
	defaultTx: true,
	sharingHelp: "editor.m17.sharing_help",
	newFields: () => ({}),
	decode: () => ({}),
	encode: (_, prev) => grow(prev, 0),
	fields: () => [],
	identity: () => "",
	ownedKeys: () => [],
	classes: [
		{
			cls: ENTRY_STATION, name: "m17_station", group: false,
			label: "editor.m17.station_keys", help: "editor.m17.station_keys_help", addLabel: "editor.m17.add_station",
			newEntry: () => ({ addr: 0, pub: "" }),
			decode: (body) => ({ addr: addrGet(body, 0), pub: body.length >= STATION_LEN ? hex(body.subarray(6, STATION_LEN)) : "" }),
			encode: (e, body) => {
				const v = grow(body, STATION_LEN);
				addrPut(v, 0, e.addr);
				const c = unhex(e.pub);
				if (c && c.length === 33) v.set(c, 6);
				return v;
			},
			fields: [
				{ key: "addr", label: "editor.m17.callsign", type: "text", size: 9,
					format: (v) => (v ? addrField(v) : ""),
					parse: (s, t) => {
						const r = addrTyped(s, t);
						return r.error || r.value ? r : { error: t("editor.problem.m17_callsign_chars") };
					} },
				{ key: "pub", label: "editor.m17.public_key", type: "text", size: 30,
					format: (v) => v ?? "",
					// any of the encodings a station gives, stored compressed
					parse: (s, t) => {
						const c = pubCompressed(unhex(s.replace(/[\s:-]/g, "")));
						return c ? { value: hex(c) } : { error: t("editor.problem.m17_public_key") };
					} },
				// the radio shows the same (m17_key_fp); a key WebCrypto won't take says so here too
				{ key: "fp", label: "editor.m17.fingerprint", type: "note",
					text: async (e, t) => {
						const c = unhex(e.pub);
						if (!c || c.length !== 33 || !pubCompressed(c)) return "";
						return (await pubImports(c)) ? fingerprint(c) : t("editor.problem.m17_public_key");
					} },
			],
		},
	],
	secret: {
		optional: true,	// signing is the station's choice: the system works without its key
		label: "editor.m17.signing_key",
		keypair: { generate: generateKey, publicOf: (d) => pubOfPriv(unhex(d)), uncompressed: pubUncompressed, fingerprint },
		decode: decodeSecret,
		encode: encodeSecret,
		fields: [{ key: "d", label: "editor.m17.signing_key", type: "hexbytes", bytes: 32, help: "editor.m17.signing_key_help" }],
		valid: secretValid,
		empty: () => ({ d: "", q: "" }),
	},
	validate(s, f, entries, problem) {
		for (const e of entries.filter((x) => x.cls === ENTRY_STATION)) {
			if (!(e.addr > 0 && e.addr <= ADDR_STD_MAX)) problem.err("entries", "m17_callsign_chars");
			const c = unhex(e.pub);
			if (!c || c.length !== 33 || !pubCompressed(c)) problem.err("entries", "m17_public_key");
		}
		for (const k of s.trafficKeys ?? []) {
			const b = unhex(k.key) ?? new Uint8Array(0);
			if (!keyLengthOk(keyAlg(k), b.length)) problem.err("trafficKeys", "m17_key_length");
			else if (keyAlg(k) === ALG.scrambler && b.every((x) => x === 0)) problem.err("trafficKeys", "m17_key_zero");
		}
	},
	traffic: {
		max: 64,
		labels: { show: "editor.m17.show_keys" },
		columns: [
			{ key: "id", label: "editor.nxdn.key_id", type: "int", min: 0, max: 0xFFFF },
			{ key: "key", label: "editor.m17.key", type: "text", secret: true, size: 24,
				format: (v) => v ?? "",
				parse: (s, t) => {
					const b = unhex(s.replace(/[\s:-]/g, ""));
					if (!b || !(keyLengthOk(ALG.scrambler, b.length) || keyLengthOk(ALG.aes, b.length))) return { error: t("editor.problem.m17_key_length") };
					if (b.length <= 3 && b.every((x) => x === 0)) return { error: t("editor.problem.m17_key_zero") };
					return { value: hex(b) };
				} },
		],
		// Any length is read (validation says when it is wrong); the algorithm is the key's length's
		decode(v) {
			if (v.length < 5 || (v[1] !== ALG.scrambler && v[1] !== ALG.aes)) return null;
			return { id: u16(v, 2), key: hex(v.subarray(5)), alg: v[1], flags: v[4] };
		},
		encode(system, k) {
			const { id, key, flags = 0 } = k;
			const b = unhex(key) ?? new Uint8Array(0);
			const v = new Uint8Array(5 + b.length);
			v[0] = system;
			v[1] = keyAlg(k);
			put16(v, 2, id);
			v[4] = flags;
			v.set(b, 5);
			return v;
		},
	},
};
