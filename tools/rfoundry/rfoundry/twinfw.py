# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The stock firmware container that holds its image twice: parse, key recovery, carving the codec windows.

A profile of stockfw.PROFILES names it with "container": "twin". The file is a plain 0x80-byte header and a payload
XORed with a 256-byte key: the image's length, the image, the length again, the same image again, two zero words.
Layout, key recovery and the carve recipe are on the codec page of each radio whose profile names this container
(docs/spec/codec-<id>.md: "Container and encryption", "Key recovery", "Extraction recipe").

The key is the container's own, not a bootloader's: it is derived from the file (the two copies give it without a
guess), checked by its SHA-256 and never stored. Nothing here names a radio: what a file is for is the profile's
matter (stockfw.py). The web flasher's twinfw.js is a port of this file.
"""
import collections
import copy
import math
import struct
import zlib

from . import codecmap
from . import stockfw

HEADER_LEN = 0x80		# the plain header; the payload follows it
KEY_LEN = 256
LEN_AT = 0x14			# u32: the payload's length
OFF_AT = 0x44			# u32: the payload's offset, HEADER_LEN, in a firmware file
BOOT_OFF_AT = 0x48		# the same word in a bootloader file of this container
BOOT_LEN_AT = 0x18
TAIL = 8			# the two zero words that end the payload
MAX_TRAILER = 16		# bytes a file may carry after its payload (the profiled files: 2)


def _xor(a, b):
    return (int.from_bytes(a, "little") ^ int.from_bytes(b, "little")).to_bytes(len(a), "little")


def _stream(key, phase, n):
    """n key bytes from key index phase on"""
    return (key[phase:] + key * (n // KEY_LEN + 1))[:n]


def parse(data):
    """(payload offset, payload length, N) of a firmware file in this container, N the length of the image it holds
    twice. The strict test, for a codec source; raises StockError for anything else"""
    if len(data) < HEADER_LEN + 16:
        raise stockfw.StockError("too short for a firmware file")
    off = struct.unpack_from("<I", data, OFF_AT)[0]
    length = struct.unpack_from("<I", data, LEN_AT)[0]
    if off != HEADER_LEN or length < 16 or (length - 16) % 2 or not 0 <= len(data) - HEADER_LEN - length <= MAX_TRAILER:
        raise stockfw.StockError("not a firmware file in this container")
    return off, length, (length - 16) // 2


def header_kind(data):
    """What a file's header says it is: "firmware" (the payload offset at OFF_AT), "boot" (at BOOT_OFF_AT: a
    bootloader file, which holds no firmware and no codec), or None. It reads the two words and nothing else, so a
    file that was cut short or padded still counts as the maker's. No application image matches: there the two words
    are vector table entries, addresses of handlers, and 0x80 is an address inside the table itself"""
    if len(data) < BOOT_OFF_AT + 4:
        return None
    a, b = struct.unpack_from("<II", data, OFF_AT)
    return "firmware" if a == HEADER_LEN else "boot" if b == HEADER_LEN else None


def offset(prof, addr, copy_no=0):
    """The file offset of image address addr, in the first copy or (copy_no 1) the second"""
    off, _ = prof["payload"]
    link, n = prof["image"]
    return off + 4 + (addr - link) + copy_no * (n + 4)


def phase(prof, file_off):
    """The key index of the byte at a file offset"""
    return (file_off - prof["payload"][0]) % KEY_LEN


def decrypt(data, off, at, n, key):
    """n plain bytes from payload offset at: plain[i] = file[off + i] XOR key[i mod 256]"""
    if at < 0 or off + at + n > len(data):
        raise stockfw.StockError("0x%X+0x%X is outside the payload" % (at, n))
    return _xor(data[off + at:off + at + n], _stream(key, at % KEY_LEN, n))


def derive_key(data, want_sha256):
    """The 256-byte key of a firmware file, from the file alone, checked against its SHA-256.

    The same plain byte is in the file twice, N + 4 bytes apart, so under two key columns s = (N + 4) mod 256 apart:
    the XOR of the two copies gives key[c] XOR key[c + s] for every column c, without error. The columns then form
    gcd(s, 256) chains, and one byte fixes each chain: the one whose derived key bytes are, summed over the chain, the
    most common bytes of their columns (a firmware image is mostly 0x00, so the most common cipher byte of a column
    is its key byte nearly everywhere; a vote alone gets a few columns wrong, the chain sum none). Raises StockError
    when the copies differ or the result is not the key"""
    off, length, n = parse(data)
    if n < 64 * KEY_LEN:
        raise stockfw.StockError("the file is too short to derive the key from")
    dist = n + 4
    step = dist % KEY_LEN
    d = _xor(data[off:off + dist], data[off + dist:off + 2 * dist])
    if d[KEY_LEN:] != d[:-KEY_LEN]:
        raise stockfw.StockError("the file's two copies differ")
    x = d[:KEY_LEN]
    payload = data[off:off + length]
    counts = [collections.Counter(payload[c::KEY_LEN]) for c in range(KEY_LEN)]
    key = bytearray(KEY_LEN)
    chain_len = KEY_LEN // math.gcd(step, KEY_LEN)
    for first in range(KEY_LEN // chain_len):		# the chains start at the columns below gcd(step, 256)
        chain = [(first + i * step) % KEY_LEN for i in range(chain_len)]
        best, best_vals = -1, None
        for g in range(256):
            v, score, vals = g, 0, []
            for c in chain:
                vals.append(v)
                score += counts[c][v]
                v ^= x[c]
            if v != g:		# the XOR around a chain is 0 for two equal copies
                raise stockfw.StockError("the file's two copies differ")
            if score > best:	# a tie keeps the lower byte
                best, best_vals = score, vals
        for c, v in zip(chain, best_vals):
            key[c] = v
    key = bytes(key)
    if stockfw.sha256(key) != want_sha256:
        raise stockfw.StockError("could not derive the file's key")
    return key


def carve(data, prof, wins, key, m=None):
    """The decrypted windows [(addr, bytes)] of a profiled file. wins: [(image address, length, SHA-256)], the code
    window first. Every check is against the profile: the file's size, the header's payload and the image length in
    both of its places, the zero words at the end, each window from both copies and its SHA-256, the code's first
    word. m: the code's map; its ranges (code.ranges), where it has them, are checked too, each by its SHA-256 and
    first word. Messages name a window by its address"""
    if len(data) != prof["size"]:
        raise stockfw.StockError("size %d, expected %d" % (len(data), prof["size"]))
    off, length, n = parse(data)
    link, want_n = prof["image"]
    if (off, length) != tuple(prof["payload"]):
        raise stockfw.StockError("payload 0x%X+0x%X, expected 0x%X+0x%X" % ((off, length) + tuple(prof["payload"])))
    if n != want_n:
        raise stockfw.StockError("image length 0x%X, expected 0x%X" % (n, want_n))
    word = struct.pack("<I", n)
    if decrypt(data, off, 0, 4, key) != word or decrypt(data, off, 4 + n, 4, key) != word:
        raise stockfw.StockError("the payload's length words don't hold the image length (wrong key?)")
    if decrypt(data, off, 8 + 2 * n, TAIL, key) != bytes(TAIL):
        raise stockfw.StockError("the payload doesn't end with its zero words")
    out = []
    for addr, size, want in wins:
        if not (link <= addr and addr + size <= link + n):
            raise stockfw.StockError("window 0x%08X+0x%X is outside the image" % (addr, size))
        at = 4 + addr - link
        plain = decrypt(data, off, at, size, key)
        if decrypt(data, off, at + n + 4, size, key) != plain:
            raise stockfw.StockError("the two copies differ in a codec window (0x%08X)" % addr)
        if stockfw.sha256(plain) != want:
            raise stockfw.StockError("decrypted window 0x%08X does not match (sha256 %s, expected %s)"
                                     % (addr, stockfw.sha256(plain), want))
        out.append((addr, plain))
    code = out[0][1]
    if len(code) < 4 or struct.unpack_from("<I", code, 0)[0] != stockfw.CODEC_SIG:
        raise stockfw.StockError("codec first word is not 0x%08X" % stockfw.CODEC_SIG)
    for r in (m or {"code": {}})["code"].get("ranges", []):
        part = code[r["off"]:r["off"] + r["len"]]
        if len(part) != r["len"] or ("sha256" in r and stockfw.sha256(part) != r["sha256"]) or \
                ("first_word" in r and struct.unpack_from("<I", part, 0)[0] != r["first_word"]):
            raise stockfw.StockError("the code's range at 0x%08X does not match" % (out[0][0] + r["off"]))
    return out


def pack(key, image, trailer=b""):
    """A firmware file of this container around an image, for tests: the header (its length and offset words, and the
    three other bytes the profiled files have there, whose meaning is not known), the payload under key, a trailer"""
    n = len(image)
    plain = struct.pack("<I", n) + bytes(image) + struct.pack("<I", n) + bytes(image) + bytes(TAIL)
    header = bytearray(HEADER_LEN)
    struct.pack_into("<I", header, LEN_AT, len(plain))
    struct.pack_into("<I", header, OFF_AT, HEADER_LEN)
    header[0x02], header[0x6D], header[0x6F] = 0x0D, 0x01, 0x18
    return bytes(header) + _xor(plain, _stream(key, 0, len(plain))) + bytes(trailer)


def _plant(buf, off, r, v):
    """Write what a layout implies at one site of codecmap._layout_sites into a synthetic window"""
    if r["kind"] == "movw_movt":
        off2 = off + r["off2"] - r["off"]
        struct.pack_into("<HH", buf, off, codecmap._MOVW, 0)
        struct.pack_into("<HH", buf, off2, codecmap._MOVT, 0)
        codecmap._put_pair(buf, dict(r, off=off, off2=off2), v)
    else:
        codecmap._put(buf, off, r, v)


def build_synthetic(key, rng, real, real_map):
    """A synthetic firmware file in this container with the layout of a real profile, for tests: an image of the real
    length with a firmware's statistics (mostly zero bytes; the key recovery relies on them), random bytes in every
    codec window at its stock address, the code's first word the signature, and every site of the real map holding
    what the stock layout implies, so the merge moves it as it moves the real one. The profile it returns carries its
    own map: the real one with every hash, CRC-32 and first word replaced by the synthetic bytes', its gates derived
    from that map and its "placed" values from the merge's own code. No stock byte and no key byte is in it.
    Returns (stock bytes, {its SHA-256: profile}, code, first window after the code), as
    stockfw.build_synthetic_stock"""
    key = bytes(key[:KEY_LEN])
    link, n = real["image"]
    raw = stockfw.random_bytes(rng, n)
    image = bytearray(raw.translate(bytes(b if b < 100 else 0 for b in range(256))))
    stock_wins = stockfw.windows(real)
    wins = [(a, size) for a, size, _ in stock_wins]
    data = [bytearray(stockfw.random_bytes(rng, size)) for _, size in wins]
    struct.pack_into("<I", data[0], 0, stockfw.CODEC_SIG)
    src = [a for a, _ in wins]
    for w, off, r, have, _ in codecmap._layout_sites(real_map, wins, src, src):
        _plant(data[w], off, r, have)
    data = [bytes(d) for d in data]
    for (a, size), d in zip(wins, data):
        image[a - link:a - link + size] = d

    def sums(d):
        return {"sha256": stockfw.sha256(d), "crc32": zlib.crc32(d) & 0xFFFFFFFF}

    by_addr = dict(zip(src, data))
    m = copy.deepcopy(real_map)
    m["profile"] = "synthetic-" + real["codec_profile"]
    m["code"].update(sums(data[0]))
    for r in m["code"].get("ranges", []):
        part = data[0][r["off"]:r["off"] + r["len"]]
        r.update(sums(part), first_word=struct.unpack_from("<I", part, 0)[0])
    for i in m["islands"]:
        if i["start"] in by_addr:
            i.update(sums(by_addr[i["start"]]))
    for t in m["table_windows"]:
        if t["flash"] in by_addr:
            t.update(sums(by_addr[t["flash"]]), min_sha256=stockfw.sha256(by_addr[t["flash"]]))
    codecmap.check(m)

    stock = pack(key, image, stockfw.random_bytes(rng, 2))
    prof = dict(real, map=m, key_sha256=stockfw.sha256(key), size=len(stock), payload=(HEADER_LEN, 2 * n + 16),
                code=(wins[0][0], wins[0][1], stockfw.sha256(data[0])),
                tables=(wins[1][0], wins[1][1], stockfw.sha256(data[1])),
                extra=[(a, size, stockfw.sha256(d)) for (a, size), d in zip(wins[2:], data[2:])])
    prof.pop("placed", None)
    if real.get("placement"):
        pl = dict(real["placement"])
        prof["placement"] = pl
        if "gates" in pl:
            pl["gates"] = stockfw.gates(prof)
        parts = [d for _, d in stockfw.placed_parts(prof, list(zip(src, data)))]
        prof["placed"] = {"code": (zlib.crc32(parts[0]) & 0xFFFFFFFF, stockfw.sha256(parts[0])),
                          "tables": (zlib.crc32(parts[1]) & 0xFFFFFFFF, stockfw.sha256(parts[1]))}
        if parts[2:]:		# a placement that keeps further windows apart: their CRC-32s, as the descriptor carries them
            prof["placed"]["extra"] = [zlib.crc32(d) & 0xFFFFFFFF for d in parts[2:]]
    return stock, {stockfw.sha256(stock): prof}, data[0], data[1]


def build_synthetic_boot(rng, length=0x4000):
    """A stand-in for a bootloader file of this container, for the refusal tests: the two facts its codec pages state
    about that header (the payload's offset at BOOT_OFF_AT, its length at BOOT_LEN_AT) and random bytes. It is not the
    maker's file and holds nothing of it"""
    header = bytearray(HEADER_LEN)
    struct.pack_into("<I", header, BOOT_LEN_AT, length)
    struct.pack_into("<I", header, BOOT_OFF_AT, HEADER_LEN)
    return bytes(header) + stockfw.random_bytes(rng, length)
