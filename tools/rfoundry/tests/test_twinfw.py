# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Host tests for the stock container that holds its image twice (rfoundry/twinfw.py) and for the placement that joins
a codec's windows into one block and blanks what is not the codec's (rfoundry/stockfw.py): parse, key recovery, carve,
the refusals, the layout rule, the gates, the merge and what checks a copy of it.

Every file here is synthetic: stockfw.build_synthetic_stock makes one with the layout of the shipped profile, random
bytes with the map's sites planted. No stock byte, no key byte. The shipped profile is found by its container, and its
numbers (offsets, lengths, CRC-32s) are those of its codec page.

    python3 -m unittest discover -s tools/rfoundry/tests -v
"""
import contextlib
import copy
import io
import os
import random
import struct
import sys
import tempfile
import types
import unittest
import zlib
from unittest import mock

import home  # noqa: F401  (the settings folder of these tests is a temporary one)

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, TOOLS)

from rfoundry import boards, codecsave, stockfw, twinfw  # noqa: E402
from rfoundry import codecmap as cm  # noqa: E402
import codec_auto  # noqa: E402
import codec_extract  # noqa: E402

REAL_SHA, REAL = next((h, p) for h, p in stockfw.PROFILES.items() if p.get("container") == stockfw.TWIN)
KEY = bytes(random.Random(256).randrange(256) for _ in range(twinfw.KEY_LEN))
STOCK, PROFILES, CODE, FIRST = stockfw.build_synthetic_stock(KEY, random.Random(96), REAL_SHA)
(SHA, PROF), = PROFILES.items()
MAP = PROF["map"]
LINK, N = PROF["image"]
PL = PROF["placement"]
WINS = [w[:2] for w in stockfw.windows(PROF)]
SRC = [a for a, _ in WINS]
DST = [PL["code"], PL["tables"]] + list(PL["extra"])


def crc(d):
    return zlib.crc32(d) & 0xFFFFFFFF


def image_of(stock=STOCK):
    """The synthetic file's image, decrypted with the test's key"""
    return twinfw.decrypt(stock, twinfw.HEADER_LEN, 4, N, KEY)


def variant(change):
    """A synthetic file like STOCK with its image changed by change(image, address -> offset), and its profile: every
    SHA-256 (the windows', and the map's code, ranges, islands and table windows) taken again from the changed bytes,
    the map's CRC-32s and the placement's gates left as they were. Returns (stock, {sha: profile})"""
    image = bytearray(image_of())
    change(image, lambda addr: addr - LINK)
    stock = twinfw.pack(KEY, image, STOCK[-2:])
    data = [bytes(image[a - LINK:a - LINK + n]) for a, n in WINS]
    m = copy.deepcopy(MAP)
    m["code"]["sha256"] = stockfw.sha256(data[0])
    for r in m["code"]["ranges"]:
        part = data[0][r["off"]:r["off"] + r["len"]]
        r["sha256"], r["first_word"] = stockfw.sha256(part), struct.unpack_from("<I", part, 0)[0]
    by_addr = dict(zip(SRC, data))
    for i in m["islands"]:
        i["sha256"] = stockfw.sha256(by_addr[i["start"]])
    for t in m["table_windows"]:
        t["sha256"] = t["min_sha256"] = stockfw.sha256(by_addr[t["flash"]])
    wins = [(a, n, stockfw.sha256(d)) for (a, n), d in zip(WINS, data)]
    prof = dict(PROF, map=m, code=wins[0], tables=wins[1], extra=wins[2:])
    return stock, {stockfw.sha256(stock): prof}


def message(fn, *args, **kw):
    """The text of the StockError a call raises"""
    try:
        fn(*args, **kw)
    except stockfw.StockError as e:
        return str(e)
    raise AssertionError("no StockError")


def tool(argv, profiles=None):
    """tools/codec_extract.py main() with these arguments: (exit code, stdout, stderr). profiles: stand-ins added to
    stockfw.PROFILES for the call"""
    out, err = io.StringIO(), io.StringIO()
    with mock.patch.dict(stockfw.PROFILES, profiles or {}), mock.patch.object(sys, "argv", ["codec_extract.py"] + argv), \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = codec_extract.main()
    return rc, out.getvalue(), err.getvalue()


class TestContainer(unittest.TestCase):
    def test_parse(self):
        length = 2 * N + 16
        self.assertEqual(twinfw.parse(STOCK), (0x80, length, N))
        self.assertEqual((len(STOCK), length, N), (REAL["size"], REAL["payload"][1], REAL["image"][1]),
                         "the synthetic file has the real one's size")
        self.assertEqual(twinfw.parse(STOCK + bytes(14)), (0x80, length, N), "16 bytes after the payload still parse")

        def with_word(at, v):
            b = bytearray(STOCK)
            struct.pack_into("<I", b, at, v)
            return bytes(b)
        bad = {
            "short": STOCK[:0x8F],
            "the offset is not 0x80": with_word(twinfw.OFF_AT, 0x100),
            "an odd image length": with_word(twinfw.LEN_AT, length + 1),
            "no room for the two length words": with_word(twinfw.LEN_AT, 14),
            "a length past the file": with_word(twinfw.LEN_AT, length + 4),
            "17 bytes after the payload": STOCK + bytes(15),
        }
        for what, data in bad.items():
            with self.subTest(what):
                text = message(twinfw.parse, data)
                self.assertEqual(text, "too short for a firmware file" if what == "short" else
                                 "not a firmware file in this container")

    def test_offsets_of_the_codec_page(self):
        code, t1, t20 = REAL["code"][0], REAL["extra"][1][0], REAL["extra"][-1][0]
        for addr, off, phase in ((code, 0x84FA8, 0x28), (t1, 0xA4720, 0xA0), (t20, 0xD5A18, 0x98)):
            with self.subTest(hex(addr)):
                self.assertEqual(twinfw.offset(REAL, addr), off)
                self.assertEqual(twinfw.phase(REAL, off), phase)
                self.assertEqual(twinfw.offset(REAL, addr, 1), off + 0xD6A5A, "the second copy")
        self.assertEqual(twinfw.offset(REAL, REAL["image"][0]), 0x84, "the image follows its length word")

    def test_header_kind(self):
        rng = random.Random(3)
        first_kind, _, _, _ = stockfw.build_synthetic_stock(bytes(1024), rng)
        vectors = struct.pack("<I", 0x24080000) + b"".join(struct.pack("<I", 0x08000401 + 4 * i) for i in range(63))
        boot = twinfw.build_synthetic_boot(rng)
        cases = (("the firmware file", STOCK, "firmware", stockfw.TWIN),
                 ("cut to 0x100 bytes", STOCK[:0x100], "firmware", stockfw.TWIN),
                 ("with 4 KB appended", STOCK + bytes(4096), "firmware", stockfw.TWIN),
                 ("a bootloader file", boot, "boot", stockfw.TWIN),
                 ("a file of the first kind", first_kind, None, stockfw.OUTSECURITY),
                 ("an application image", vectors + stockfw.random_bytes(rng, 4096), None, None),
                 ("64 KB of 0xFF", b"\xFF" * 65536, None, None),
                 ("0x40 bytes", STOCK[:0x40], None, None))
        for what, data, kind, container in cases:
            with self.subTest(what):
                self.assertEqual(twinfw.header_kind(data), kind)
                self.assertEqual(stockfw.container_kind(data), container)
                self.assertEqual(stockfw.is_container(data), container is not None)
        self.assertEqual(struct.unpack_from("<II", boot, twinfw.BOOT_LEN_AT), (len(boot) - 0x80, 0))
        both = bytearray(boot)		# the offset in both places: the firmware file's word decides
        struct.pack_into("<I", both, twinfw.OFF_AT, 0x80)
        self.assertEqual(twinfw.header_kind(bytes(both)), "firmware")
        self.assertIsNone(stockfw.boot_file(bytes(both)))
        self.assertIsNone(stockfw.boot_file(STOCK))
        self.assertIsNone(stockfw.boot_file(first_kind))

    def test_key_from_the_two_copies(self):
        self.assertEqual((N + 4) % 256, 0x5A, "the profiled file: two chains of 128 columns")
        self.assertEqual(twinfw.derive_key(STOCK, stockfw.sha256(KEY)), KEY)
        rng = random.Random(11)
        zeros = bytes(b if b < 100 else 0 for b in range(256))
        for what, n in (("an odd step, one chain", 70001), ("step 0, every column its own chain", 70140),
                        ("step 128, 128 chains of two", 70012)):
            with self.subTest(what):
                key = stockfw.random_bytes(rng, 256)
                data = twinfw.pack(key, stockfw.random_bytes(rng, n).translate(zeros), b"\x01\x02")
                self.assertEqual(twinfw.derive_key(data, stockfw.sha256(key)), key)
        self.assertEqual((70001 + 4) % 2, 1)
        self.assertEqual(((70140 + 4) % 256, (70012 + 4) % 256), (0, 128))

    def test_key_refusals(self):
        want = stockfw.sha256(KEY)
        b = bytearray(STOCK)
        b[twinfw.offset(PROF, LINK + 0x1234, 1)] ^= 0x40		# one byte of the second copy
        self.assertEqual(message(twinfw.derive_key, bytes(b), want), "the file's two copies differ")
        self.assertEqual(message(twinfw.derive_key, STOCK, "00" * 32), "could not derive the file's key")
        small = twinfw.pack(KEY, bytes(1000))
        self.assertEqual(message(twinfw.derive_key, small, want), "the file is too short to derive the key from")
        self.assertEqual(message(twinfw.derive_key, STOCK[:0x100], want), "not a firmware file in this container")

    def test_carve(self):
        segs = twinfw.carve(STOCK, PROF, stockfw.windows(PROF), KEY, MAP)
        self.assertEqual([(a, len(d)) for a, d in segs], WINS)
        self.assertEqual((segs[0][1], segs[1][1]), (CODE, FIRST))
        image = image_of()
        for (a, n), (_, d) in zip(WINS, segs):
            self.assertEqual(d, image[a - LINK:a - LINK + n])
        self.assertEqual(len(segs), 23)
        self.assertEqual(sum(n for _, n in WINS), 0x15C98 + 15875)

        def flipped(at):
            b = bytearray(STOCK)
            b[at] ^= 1
            return bytes(b)
        t5 = WINS[7][0]
        bad = {
            "a byte of a window in the second copy": (flipped(twinfw.offset(PROF, t5 + 3, 1)), "the two copies differ in a codec window"),
            "a byte of a window in both copies": (flipped(twinfw.offset(PROF, t5 + 3)), None),
            "the first length word": (flipped(0x80), "length words"),
            "the second length word": (flipped(0x80 + 4 + N + 1), "length words"),
            "a nonzero byte in the tail words": (flipped(0x80 + 8 + 2 * N + 5), "zero words"),
            "another size": (STOCK + b"\0", "size %d, expected %d" % (len(STOCK) + 1, len(STOCK))),
        }
        both = bytearray(bad["a byte of a window in both copies"][0])
        both[twinfw.offset(PROF, t5 + 3, 1)] ^= 1
        bad["a byte of a window in both copies"] = (bytes(both), "decrypted window 0x%08X does not match" % t5)
        for what, (data, text) in bad.items():
            with self.subTest(what):
                self.assertIn(text, message(twinfw.carve, data, PROF, stockfw.windows(PROF), KEY, MAP))
        self.assertIn("image length", message(twinfw.carve, STOCK, dict(PROF, image=(LINK, N - 2)), stockfw.windows(PROF), KEY))
        self.assertIn("payload", message(twinfw.carve, STOCK, dict(PROF, payload=(0x80, 2 * N)), stockfw.windows(PROF), KEY))
        outside = [(LINK + N - 8, 16, "00" * 32)]
        self.assertIn("outside the image", message(twinfw.carve, STOCK, PROF, outside, KEY))
        wrong = bytes([KEY[0] ^ 1]) + KEY[1:]
        self.assertIn("length words", message(twinfw.carve, STOCK, PROF, stockfw.windows(PROF), wrong))

    def test_carve_checks_the_ranges_of_the_map(self):
        for what, change in (("sha256", {"sha256": "00" * 32}), ("first word", {"first_word": 0x12345678})):
            with self.subTest(what):
                m = copy.deepcopy(MAP)
                m["code"]["ranges"][1].update(change)
                text = message(twinfw.carve, STOCK, PROF, stockfw.windows(PROF), KEY, m)
                self.assertEqual(text, "the code's range at 0x%08X does not match" % (SRC[0] + MAP["code"]["ranges"][1]["off"]))
                # and on the way every merge takes: the profile's map is the one the carve checks
                self.assertEqual(message(stockfw.extract, STOCK, None, {SHA: dict(PROF, map=m)}), text)

        def no_signature(image, at):
            image[at(SRC[0])] ^= 1
        stock, profiles = variant(no_signature)
        self.assertIn("codec first word", message(stockfw.extract, stock, None, profiles))


class TestRefusals(unittest.TestCase):
    """A file that is the maker's but no codec source gets its own sentence on every way in"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def path(self, name, data):
        p = os.path.join(self.tmp.name, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def test_a_version_whose_codec_is_not_mapped(self):
        self.assertEqual(len(stockfw.UNMAPPED_FILES), 3)
        for real in stockfw.UNMAPPED_FILES.values():
            with self.subTest(real["version"]):
                self.assertIn(real["board"], boards.BOARDS)
                self.assertNotIn(real["name"], [p["name"] for p in stockfw.PROFILES.values()])
                data = STOCK[:-1] + bytes([len(real["name"])])	# a stand-in under its own hash
                with mock.patch.dict(stockfw.UNMAPPED_FILES, {stockfw.sha256(data): real}):
                    want = stockfw.unmapped(data)
                    self.assertEqual(want, "this file is %s (%s): the codec in this version is laid out differently and "
                                     "has not been mapped, so it can't be taken from it. Use %s"
                                     % (real["name"], real["version"], REAL["name"]))
                    self.assertEqual(stockfw.unmapped_board(data), real["board"])
                    self.assertEqual(message(stockfw.find_profile, data), want)
                    self.assertEqual(message(stockfw.find_profile, data, board=real["board"]), want)
                    self.assertEqual(message(stockfw.extract, data, None), want)
                    rc, out, err = tool(["--stock", self.path("old.bin", data)])
                    self.assertEqual((rc, out, err), (1, "", "error: %s\n" % want))
        self.assertIsNone(stockfw.unmapped(STOCK))
        self.assertIsNone(stockfw.unmapped_board(STOCK))

    def test_a_bootloader_file(self):
        boot = twinfw.build_synthetic_boot(random.Random(5))
        want = ("this is a bootloader file in the maker's container (its header gives the payload offset at 0x48): it "
                "holds no firmware and no codec")
        self.assertEqual(stockfw.boot_file(boot), want)
        self.assertEqual(message(stockfw.find_profile, boot), want)
        self.assertEqual(message(stockfw.extract, boot, None), want)
        rc, out, err = tool(["--stock", self.path("boot.bin", boot)])
        self.assertEqual((rc, out, err), (1, "", "error: %s\n" % want))

    def test_an_unknown_file_gets_the_message_it_always_got(self):
        for what, data in (("a file of this container nobody knows", STOCK[:-1] + b"\x7F"), ("cut short", STOCK[:0x100]),
                           ("a file of the first kind", stockfw.MAGIC + bytes(0x200))):
            with self.subTest(what):
                digest = stockfw.sha256(data)
                want = "unknown stock file (sha256 %s...), not a supported codec source. Supported: %s" % (
                    digest[:16], ", ".join("%s (%s...)" % (p["name"], h[:8]) for h, p in stockfw.PROFILES.items()))
                self.assertEqual(message(stockfw.find_profile, data), want)
                rc, out, err = tool(["--stock", self.path("x.bin", data)])
                self.assertEqual((rc, out, err), (1, "", "error: %s\n" % want))

    def test_another_radios_file_and_a_plain_image(self):
        other = next(b for b in boards.BOARDS if b != PROF["board"])
        self.assertEqual(message(stockfw.extract, STOCK, None, PROFILES, board=other),
                         "%s is a firmware file for the %s, not the %s" % (PROF["name"], PROF["board"], other))
        rc, _, err = tool(["--stock", self.path("ff.bin", b"\xFF" * 0x10000)])
        self.assertEqual(rc, 1)
        self.assertIn("neither a supported stock file nor a flash readout", err)

    def test_the_makers_file_is_not_an_image(self):
        _, segs = stockfw.extract(STOCK, None, PROFILES)
        want = "the image is an official firmware file; pass the RadioFoundry build (rfoundry_%s.bin)" % PROF["board"]
        first_kind, _, _, _ = stockfw.build_synthetic_stock(bytes(1024), random.Random(4))
        for what, image in (("the firmware file", STOCK), ("cut short", STOCK[:0x1000]), ("a file of the first kind", first_kind),
                            ("a bootloader file", twinfw.build_synthetic_boot(random.Random(5)))):
            with self.subTest(what):
                self.assertEqual(message(stockfw.merge, image, PROF, segs), want)

    def test_the_builds_codec_step(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(codec_auto.self_test(), 0)
        lines = out.getvalue().splitlines()
        self.assertEqual([x for x in lines if x.startswith("ran ")],
                         ["ran   a board without a codec route: its reason, exit 0, no file looked at",
                          "ran   a known file whose codec is not mapped: the sentence, exit 0, built without the codec"])
        self.assertEqual(lines[-1], "codec_auto self-test PASSED")
        # the folder scan: only this board's files, each with its sentence
        entry = {"board": PROF["board"], "name": "old.bin", "version": "V0.1"}
        data = stockfw.random_bytes(random.Random(8), 9000)
        self.path("old.bin", data)
        self.path("other.bin", bytes(9000))
        with mock.patch.dict(stockfw.UNMAPPED_FILES, {stockfw.sha256(data): entry}):
            got = codec_auto.unmapped(self.tmp.name, PROF["board"])
            self.assertEqual(got, [(os.path.join(self.tmp.name, "old.bin"), stockfw.unmapped(data))])
            self.assertEqual(codec_auto.unmapped(self.tmp.name, "another-board"), [])


class TestKeys(unittest.TestCase):
    """The container's key is the file's own: no caller's key has a part in it, and the board has none to give"""

    def test_the_callers_key_is_not_used(self):
        self.assertIsNone(stockfw.codec_only(PROF))
        self.assertIsNone(stockfw.codec_only(REAL))
        self.assertNotIn("file_radio", REAL)
        got = stockfw.extract(STOCK, None, PROFILES)
        self.assertEqual(stockfw.extract(STOCK, b"x" * 1024, PROFILES), got)
        self.assertEqual(stockfw.extract(STOCK, KEY, PROFILES, board=PROF["board"]), got)
        self.assertEqual(stockfw.file_key(STOCK, PROF), KEY)

    def test_the_tool_takes_no_key_for_a_board_without_one(self):
        board = boards.get(PROF["board"])
        self.assertIsNone(board.key_sha256)
        with tempfile.TemporaryDirectory() as d:
            sp, kp = os.path.join(d, "stock.bin"), os.path.join(d, "x.key")
            for p, blob in ((sp, STOCK), (kp, bytes(1024))):
                with open(p, "wb") as f:
                    f.write(blob)
            rc, out, err = tool(["--stock", sp, "--key", kp, "-o", os.path.join(d, "out.bin")], PROFILES)
            self.assertEqual((rc, out, err), (1, "", "error: the %s's bootloader has no key: it takes the image as it "
                                              "is\n" % board.name))
            self.assertEqual(sorted(os.listdir(d)), ["stock.bin", "x.key"])

    def test_the_tool_merges_without_a_key(self):
        image = struct.pack("<II", 0x24080000, 0x08000401) + stockfw.random_bytes(random.Random(2), 5000)
        _, segs = stockfw.extract(STOCK, None, PROFILES)
        with tempfile.TemporaryDirectory() as d:
            sp, ip, op = (os.path.join(d, n) for n in ("stock.bin", "image.bin", "out.bin"))
            for p, blob in ((sp, STOCK), (ip, image)):
                with open(p, "wb") as f:
                    f.write(blob)
            with mock.patch.object(codec_extract.stm32_dfu, "check_image", side_effect=AssertionError("a keyed radio's check")), \
                    mock.patch.object(codec_extract, "derive_key", side_effect=AssertionError("a bootloader key asked for")):
                rc, out, err = tool(["--stock", sp, "--image", ip, "-o", op], PROFILES)
            self.assertEqual((rc, err), (0, ""))
            with open(op, "rb") as f:
                merged = f.read()
            self.assertEqual(merged, stockfw.merge(image, PROF, segs))
            self.assertIn("0x%08X+0x%X, 0x%08X+0x%X, 0x%08X+0x80 verified" % (DST[0], WINS[0][1], PL["tables"], PL["tables_len"],
                                                                         stockfw.DESC_ADDR), out)
            self.assertIn("wrote  %s (1048576 bytes" % op, out)


class TestShippedProfile(unittest.TestCase):
    """The shipped profile and its map, against the numbers of the codec page and the rule the layout follows"""

    def test_the_map(self):
        m = stockfw.code_map(REAL)
        self.assertIs(m, cm.by_code_sha(cm.shipped(), REAL["code"][2])[0], "found by the code's SHA-256")
        self.assertEqual(m["board"], REAL["board"])
        self.assertEqual([len(m[k]) for k in ("self_refs", "island_ptrs", "table_refs", "island_refs", "branches", "objects")],
                         [2, 3, 76, 3, 12, 52])
        self.assertEqual(m["outside_refs"], [])
        self.assertEqual((len(cm.holes(m)), sum(n for _, n in cm.holes(m))), (7, 33368))
        th = cm.table_holes(m)
        self.assertEqual((len(th), sum(n for v in th.values() for _, n in v)), (3, 14))
        self.assertEqual([(r["off"], r["len"]) for r in m["code"]["ranges"]], [(0, 4140), (0x71DC, 14436), (0xCA5C, 37436)])
        self.assertEqual(m["code"]["ranges"][0]["first_word"], stockfw.CODEC_SIG)
        self.assertEqual(sum(1 for r in m["table_refs"] if "anchor" in r), 2)
        self.assertEqual(sum(1 for r in m["table_refs"] if r.get("reachable") is False), 5)
        self.assertEqual(m["stock"]["sha256"], REAL_SHA)
        # the profile's windows are the map's: the code, the islands, the table windows, each with the same hash
        known = {(m["code"]["run"], m["code"]["len"]): m["code"]["sha256"]}
        known.update({(i["start"], i["len"]): i["sha256"] for i in m["islands"]})
        known.update({(w["flash"], w["len"]): w["sha256"] for w in m["table_windows"]})
        self.assertEqual({(a, n): h for a, n, h in stockfw.windows(REAL)}, known)
        self.assertEqual(m["entries"], {"encode": 0x14C, "enc_set": 0x1E0, "enc_init": 0x294, "fec_init": 0x4FC,
                                        "encode_ecc": 0x514, "decode": 0x604, "dec_set": 0x6AC, "dec_init": 0x6F0,
                                        "enc_params": 0x854, "fec_decode": 0xAE0})

    def test_the_layout_follows_the_rule(self):
        for prof in (REAL, PROF):
            pl = prof["placement"]
            wins = stockfw.windows(prof)
            sector = boards.get(prof["board"]).sector_at(stockfw.DESC_ADDR)
            self.assertEqual(pl["code"], sector.addr + prof["code"][0] % 16)
            self.assertEqual([pl["tables"]] + pl["extra"], stockfw.pack_after(pl["code"] + prof["code"][1], wins[1:]))
            placed = [pl["code"], pl["tables"]] + pl["extra"]
            self.assertTrue(all((d - w[0]) % 16 == 0 for d, w in zip(placed, wins)), "every move a multiple of 16")
            self.assertEqual(pl["tables_len"], placed[-1] + wins[-1][1] - pl["tables"])
        self.assertEqual((REAL["placement"]["code"], REAL["placement"]["tables"], REAL["placement"]["tables_len"]),
                         (0x080E0004, 0x080F5CA0, 0x3E83))
        self.assertEqual(stockfw.pack_after(0x1003, [(0x2000, 5), (0x3009, 1), (0x400A, 7)]), [0x1010, 0x1019, 0x101A])
        self.assertEqual(stockfw.pack_after(0x1000, [(0x2000, 5)], modulo=4), [0x1000])

    def test_the_layout_fits_its_sector(self):
        sector = boards.get(REAL["board"]).sector_at(stockfw.DESC_ADDR)
        self.assertEqual(sector.end, stockfw.DESC_ADDR + stockfw.DESC_LEN)
        pl = REAL["placement"]
        spans = sorted(zip([pl["code"], pl["tables"]] + pl["extra"], [w[1] for w in stockfw.windows(REAL)]))
        self.assertGreaterEqual(spans[0][0], sector.addr)
        for (a, n), (b, _) in zip(spans, spans[1:] + [(stockfw.DESC_ADDR, 0)]):
            self.assertLessEqual(a + n, b, hex(a))
        lay = stockfw.layout(REAL)
        self.assertEqual([(a, n) for a, n, _ in lay], [(0x080E0004, 0x15C98), (0x080F5CA0, 0x3E83), (stockfw.DESC_ADDR, 0x80)])
        self.assertEqual(lay[1][0] + lay[1][1], 0x080F9B23)
        self.assertEqual(stockfw.DESC_ADDR - 0x080F9B23, 25693)

    def test_the_gates_follow_from_the_codec_page(self):
        for sha, prof in list(stockfw.PROFILES.items()) + [(SHA, PROF)]:
            if not (prof.get("placement") or {}).get("gates"):
                continue
            with self.subTest(prof["name"], sha=sha[:8]):
                self.assertEqual(stockfw.gates(prof), prof["placement"]["gates"])
        self.assertEqual(REAL["placement"]["gates"], {"code": 0xE24A92CF, "ranges": 0xB4A36974, "tables": 0x7BC061E9})
        self.assertEqual([k for k, _ in stockfw.GATES], ["code", "ranges", "tables"])
        m = copy.deepcopy(MAP)
        del m["table_windows"][4]["crc32"]
        self.assertIsNone(stockfw.gates(dict(PROF, map=m)), "a map without a window's CRC-32 gives no gates")

    def test_the_descriptor(self):
        d = stockfw.descriptor(REAL)
        w = struct.unpack("<32I", d)
        pd = REAL["placed"]
        self.assertEqual(w[:10], (0x43534544, 1, 0x00090096, 0x080E0004, 0x00015C98, pd["code"][0], 1,
                                  0x080F5CA0, 0x00003E83, pd["tables"][0]))
        self.assertEqual(w[10:31], (0,) * 21)
        self.assertEqual(w[31], crc(d[:124]))
        self.assertTrue(all(isinstance(x[1], str) and len(x[1]) == 64 for x in (pd["code"], pd["tables"])),
                        "both placed SHA-256s are set: a copy is checked by them")

    def test_the_profile_is_the_last_and_its_board_has_no_key(self):
        self.assertEqual(list(stockfw.PROFILES)[-1], REAL_SHA)
        self.assertEqual(REAL["segments"], [])
        self.assertEqual(REAL["app_base"], boards.get(REAL["board"]).app_base)
        self.assertEqual(len([p for p in stockfw.PROFILES.values() if p.get("container") == stockfw.TWIN]), 1)
        self.assertTrue(all(p.get("container", stockfw.OUTSECURITY) in (stockfw.OUTSECURITY, stockfw.TWIN)
                            for p in stockfw.PROFILES.values()))
        with self.assertRaises(cm.MapError):	# its tables are no single window: a search for a moved copy never tries it
            cm.table_window(stockfw.code_map(REAL))


class TestMerge(unittest.TestCase):
    def setUp(self):
        self.prof, self.segs = stockfw.extract(STOCK, None, PROFILES)
        self.code, self.block = self.segs[0][1], self.segs[1][1]
        _, self.stock = stockfw.stock_segments(STOCK, None, PROFILES)
        self.sites = cm._layout_sites(MAP, WINS, SRC, DST)

    def test_segments_and_sites(self):
        self.assertIs(self.prof, PROF)
        self.assertEqual([(a, len(d)) for a, d in self.segs],
                         [(0x080E0004, 0x15C98), (0x080F5CA0, 0x3E83), (0x080FFF80, 0x80)])
        self.assertEqual(len(self.sites), 96)
        kinds = {}
        for w, off, r, _, want in self.sites:
            buf, base = (self.code, 0) if w == 0 else (self.block, DST[w] - PL["tables"])
            kinds[(w == 0, r["kind"])] = kinds.get((w == 0, r["kind"]), 0) + 1
            if r["kind"] == "bl":
                self.assertEqual(cm.bl_target(buf, base + off, want[0]), want[1])
                self.assertEqual(want[1] - DST[0], cm.bl_target(self.stock[w][1], off, SRC[w] + off) - SRC[0],
                                 "the call reaches the same offset of the moved code")
            elif r["kind"] == "word":
                self.assertEqual(struct.unpack_from("<I", buf, base + off)[0], want)
            else:
                self.assertEqual(cm.pair_value(buf, dict(r, off=base + off, off2=base + off + r["off2"] - r["off"])), want)
        self.assertEqual(kinds, {(True, "word"): 71, (True, "movw_movt"): 10, (False, "word"): 3, (False, "bl"): 12})

    def test_the_anchored_words_move_with_their_window(self):
        refs = [r for r in MAP["table_refs"] if "anchor" in r]
        self.assertEqual([(r["off"], r["anchor"] - r["target"]) for r in refs], [(0x5C4, 2), (0xC9C, 2)])
        placed = DST[SRC.index(refs[0]["anchor"])]
        self.assertEqual(placed, 0x080F9AA4)
        for r in refs:
            self.assertEqual(struct.unpack_from("<I", self.code, r["off"])[0], placed - 2)

    def test_what_is_blanked_and_what_is_kept(self):
        holes = cm.holes(MAP)
        self.assertEqual(holes, [(0x918, 58), (0x952, 2), (0x102C, 25008), (0xAA40, 8220), (0x14350, 76), (0x15C72, 2),
                                 (0x15C96, 2)])
        skip = {o + i for o, n in holes for i in range(n)}
        self.assertTrue(all(self.code[i] == 0xFF for i in skip))
        for w, off, r, _, _ in self.sites:
            if w == 0:
                skip.update(range(off, off + 4))
                if r["kind"] == "movw_movt":
                    skip.update(range(off + r["off2"] - r["off"], off + r["off2"] - r["off"] + 4))
        self.assertTrue(all(self.code[i] == CODE[i] for i in range(len(CODE)) if i not in skip),
                        "every byte of the code outside its sites and holes is the stock byte")
        th = cm.table_holes(MAP)
        self.assertEqual({a - SRC[0]: v for a, v in th.items()},
                         {0x080F21DC - 0x080A4F24: [(0x44, 4)], 0x080F22A8 - 0x080A4F24: [(0x12, 2)],
                          0x080F5994 - 0x080A4F24: [(0x10, 8)]})
        used, blank = set(), []
        for w, ((a, d), at) in enumerate(zip(self.stock[1:], DST[1:]), 1):
            o = at - PL["tables"]
            used.update(range(o, o + len(d)))
            hole = {x + i for x, n in th.get(a, []) for i in range(n)}
            site = {off + i for sw, off, _, _, _ in self.sites if sw == w for i in range(4)}
            blank += [o + i for i in sorted(hole)]
            self.assertTrue(all(self.block[o + i] == d[i] for i in range(len(d)) if i not in hole and i not in site), hex(a))
        self.assertEqual(blank, list(range(0x3CB0, 0x3CB4)) + list(range(0x3CEA, 0x3CEC)) + list(range(0x3E74, 0x3E7C)))
        self.assertTrue(all(self.block[i] == 0xFF for i in blank))
        gaps = [i for i in range(len(self.block)) if i not in used]
        self.assertEqual(len(gaps), 128)
        self.assertTrue(all(self.block[i] == 0xFF for i in gaps), "0xFF between the windows of the block")

    def test_the_descriptor(self):
        d = self.segs[2][1]
        w = struct.unpack("<32I", d)
        self.assertEqual(w[:10], (0x43534544, 1, 0x00090096, 0x080E0004, 0x15C98, crc(self.code), 1, 0x080F5CA0, 0x3E83,
                                  crc(self.block)))
        self.assertEqual((w[10:31], w[31]), ((0,) * 21, crc(d[:124])))
        self.assertEqual(stockfw.layout(PROF)[2], (stockfw.DESC_ADDR, 0x80, stockfw.sha256(d)))

    def test_the_placed_parts_are_the_segments_before_the_check(self):
        self.assertEqual(stockfw.placed_parts(PROF, self.stock), self.segs[:2])
        self.assertEqual(stockfw.place(PROF, self.stock), self.segs)
        other = dict(PROF, placed={"code": PROF["placed"]["code"], "tables": (0, "00" * 32)})
        self.assertIn("the moved tables doesn't match the profile", message(stockfw.place, other, self.stock))
        self.assertEqual(stockfw.carve(PROF), WINS)
        self.assertEqual(stockfw.gate_crcs(PROF, self.stock), PL["gates"])

    def test_a_site_that_does_not_hold_its_stock_value(self):
        ref = MAP["table_refs"][7]
        self.assertEqual(ref["kind"], "word")

        def stale(image, at):
            struct.pack_into("<I", image, at(SRC[0]) + ref["off"], ref["target"] + 4)
        stock, profiles = variant(stale)
        text = message(stockfw.extract, stock, None, profiles)
        self.assertTrue(text.startswith("can't move the codec: "), text)
        self.assertIn("+0x%X doesn't hold 0x%08X" % (ref["off"], ref["target"]), text)

    def test_the_rewrites_are_taken_back_once(self):
        # the recipe's last step: the placed windows, taken back, must be the stock windows again. A rewrite that
        # doesn't undo itself (here: the way back loses a byte) stops the merge
        real, calls = cm.relocate, []

        def relocate(m, wins, parts, src, dst):
            calls.append((src, dst))
            out = real(m, wins, parts, src, dst)
            return out if len(calls) == 1 else [out[0][:-1] + b"\0"] + out[1:]
        with mock.patch.object(cm, "relocate", relocate):
            self.assertEqual(message(stockfw.placed_parts, PROF, self.stock),
                             "the moved codec doesn't come back to the stock windows")
        self.assertEqual(calls, [(SRC, DST), (DST, SRC)])

    def test_a_layout_that_does_not_hold_together(self):
        def placed(**change):
            return dict(PROF, placement=dict(PL, gates=None, **change))
        extra = list(PL["extra"])
        cases = {
            "the code overlaps the block": (placed(code=PL["code"] + 16), "overlaps the block"),
            "a piece inside the one before": (placed(extra=[extra[0] - 16] + extra[1:]), "inside the window before it"),
            "a piece below the block": (placed(tables=PL["tables"] + 16), "starts below the block"),
            "the last piece ends before the block does": (placed(tables_len=PL["tables_len"] + 1), "the last window ends"),
            "the last piece ends past the block": (placed(tables_len=PL["tables_len"] - 1), "the last window ends"),
            "a window not moved by a multiple of 4": (placed(extra=extra[:-1] + [extra[-1] + 2], tables_len=PL["tables_len"] + 2),
                                                       "can't move the codec"),
        }
        for what, (prof, text) in cases.items():
            with self.subTest(what):
                self.assertIn(text, message(stockfw.placed_parts, prof, self.stock))
        up = stockfw.DESC_ADDR - (PL["tables"] + PL["tables_len"]) + 16
        up -= up % 16
        high = placed(tables=PL["tables"] + up, extra=[a + up for a in extra])
        self.assertIn("reaches the descriptor", message(stockfw.placed_parts, high, self.stock))
        no_holes = copy.deepcopy(MAP)
        no_holes["code"].pop("holes")
        no_holes["code"]["ranges"] = [{"off": 0, "len": no_holes["code"]["len"]}]
        for w in no_holes["table_windows"]:
            w.pop("holes", None)
        self.assertIn("the map lists none", message(stockfw.placed_parts, dict(placed(), map=no_holes), self.stock))
        stray = copy.deepcopy(MAP)	# a hole in a table window that is none of the profile's: nothing would blank it
        stray["table_windows"].append({"flash": SRC[-1] + 0x1000, "run": SRC[-1] + 0x1000, "len": 16,
                                       "holes": [{"off": 4, "len": 2}]})
        cm.check(stray)
        self.assertIn("a table window the profile doesn't carve",
                      message(stockfw.placed_parts, dict(placed(), map=stray), self.stock))
        kept = stockfw.placed_parts(dict(placed(blank=False), map=no_holes), self.stock)
        self.assertEqual({k: crc(d) for k, (_, d) in zip(("code", "tables"), kept)},
                         {k: PL["gates"][k] for k in ("code", "tables")}, "not blanked: the gates are the placed CRC-32s")
        self.assertIn("no codec map", message(stockfw.placed_parts, dict(PROF, map=None, code=(SRC[0], WINS[0][1], "00" * 32)),
                                               self.stock))


class TestGates(unittest.TestCase):
    """Each gate is a CRC-32 that follows from the map alone, compared with the bytes of every merge before a hole is
    blanked: a wrong byte anywhere a pinned value could not catch it afterwards stops the merge here"""

    def gate(self, addr):
        def flip(image, at):
            image[at(addr)] ^= 0x10
        stock, profiles = variant(flip)
        (_, prof), = profiles.items()
        _, segs = stockfw.stock_segments(stock, None, profiles)
        return message(stockfw.extract, stock, None, profiles), stockfw.gate_crcs(prof, segs)

    def test_a_wrong_byte_is_caught_by_its_gate(self):
        want = PL["gates"]
        rs = MAP["code"]["ranges"]
        between = rs[0]["off"] + rs[0]["len"] + 0x100
        self.assertIn((rs[0]["len"], rs[1]["off"] - rs[0]["len"]), cm.holes(MAP))
        inside_hole = next(o for o, n in cm.holes(MAP) if rs[2]["off"] < o < rs[2]["off"] + rs[2]["len"])
        t12 = next(a for a in cm.table_holes(MAP))
        cases = (
            ("a byte of the second range", SRC[0] + rs[1]["off"] + 0x40, "rewritten code", {"code", "ranges"}),
            ("a byte of the hole between two ranges", SRC[0] + between, "rewritten code", {"code"}),
            ("a byte of a hole inside a range", SRC[0] + inside_hole + 1, "rewritten code", {"code", "ranges"}),
            ("a byte of the first island", SRC[1] + 0x30, "joined tables", {"tables"}),
            ("a byte of a table window", SRC[5] + 0x21, "joined tables", {"tables"}),
            ("a byte of a table window's hole", t12 + cm.table_holes(MAP)[t12][0][0], "joined tables", {"tables"}),
        )
        for what, addr, name, wrong in cases:
            with self.subTest(what):
                text, got = self.gate(addr)
                self.assertRegex(text, r"^the %s's CRC-32 0x[0-9A-F]{8} is not the 0x%08X that follows from the map$"
                                 % (name, want[stockfw.GATES[[n for _, n in stockfw.GATES].index(name)][0]]))
                self.assertEqual({k for k in want if got[k] != want[k]}, wrong)

    def test_the_second_gate_is_named_when_only_it_fails(self):
        _, segs = stockfw.stock_segments(STOCK, None, PROFILES)
        for k, name in stockfw.GATES:
            with self.subTest(k):
                prof = dict(PROF, placement=dict(PL, gates=dict(PL["gates"], **{k: PL["gates"][k] ^ 1})))
                self.assertIn("the %s's CRC-32" % name, message(stockfw.placed_parts, prof, segs))


class TestCopies(unittest.TestCase):
    """What recognizes the placed codec again: in a radio's flash, in a merged image, in a saved copy"""

    def setUp(self):
        _, self.segs = stockfw.extract(STOCK, None, PROFILES)
        self.form = stockfw.forms(PROF)[0]

    def test_check_form(self):
        parts = self.segs[:2]
        self.assertTrue(stockfw.check_form(PROF, self.form, parts))
        hole = cm.holes(MAP)[2][0] + 5
        t12 = next(iter(cm.table_holes(MAP)))
        table_hole = DST[SRC.index(t12)] - PL["tables"] + cm.table_holes(MAP)[t12][0][0]
        gap = DST[2] - PL["tables"] - 1
        self.assertEqual(self.segs[1][1][gap], 0xFF)
        for what, w, at in (("the code", 0, 0x300), ("a hole of the code", 0, hole), ("the block", 1, 0x40),
                            ("a gap of the block", 1, gap), ("a table hole", 1, table_hole)):
            with self.subTest(what):
                changed = [bytearray(d) for _, d in parts]
                changed[w][at] ^= 0xFF
                self.assertFalse(stockfw.check_form(PROF, self.form, [(a, bytes(d)) for (a, _), d in zip(parts, changed)]))
        self.assertFalse(stockfw.check_form(PROF, self.form, [(parts[0][0] + 4, parts[0][1]), parts[1]]))
        self.assertIsNone(stockfw.unplace(PROF, self.form, parts), "a blanked copy is never taken back to stock")
        self.assertEqual(stockfw.from_form(PROF, self.form, parts), self.segs)
        _, stock = stockfw.stock_segments(STOCK, None, PROFILES)
        self.assertTrue(stockfw.check_form(PROF, None, stock))
        self.assertEqual(stockfw.from_form(PROF, None, stock), self.segs, "the stock windows, placed")

    def test_from_flash(self):
        flash = bytearray(b"\xFF" * (stockfw.FLASH_END - stockfw.FLASH_BASE))
        for a, d in self.segs:
            flash[a - stockfw.FLASH_BASE:a - stockfw.FLASH_BASE + len(d)] = d
        self.assertEqual(stockfw.from_flash(bytes(flash), profiles=PROFILES), (PROF, self.segs))
        self.assertIsNone(stockfw.from_moved(bytes(flash), profiles=PROFILES))
        flash[self.segs[1][0] - stockfw.FLASH_BASE + 9] ^= 1
        self.assertIsNone(stockfw.from_flash(bytes(flash), profiles=PROFILES))
        # the image as the file holds it, at its link address: the stock windows, found and placed
        flash = bytearray(b"\xFF" * (stockfw.FLASH_END - stockfw.FLASH_BASE))
        flash[LINK - stockfw.FLASH_BASE:LINK - stockfw.FLASH_BASE + N] = image_of()
        self.assertEqual(stockfw.from_flash(bytes(flash), profiles=PROFILES), (PROF, self.segs))

    def test_merge_and_split(self):
        rng = random.Random(6)
        image = struct.pack("<II", 0x24080000, 0x08000401) + stockfw.random_bytes(rng, 300000)
        merged = stockfw.merge(image, PROF, self.segs)
        self.assertEqual(len(merged), 1048576)
        self.assertEqual(merged, stockfw.merge(image, PROF, self.segs), "the same inputs give the same bytes")
        base = PROF["app_base"]
        self.assertEqual(merged[:len(image)], image)
        self.assertEqual(merged.count(0xFF, len(image), PL["code"] - base), PL["code"] - base - len(image))
        for a, d in self.segs:
            self.assertEqual(merged[a - base:a - base + len(d)], d)
        end = PL["tables"] + PL["tables_len"] - base
        self.assertEqual(merged[end:stockfw.DESC_ADDR - base], b"\xFF" * 25693)
        self.assertEqual(merged[PL["code"] + WINS[0][1] - base:PL["tables"] - base], b"\xFF" * 4)
        self.assertEqual(stockfw.split_merged(merged, PROFILES), (image, PROF, self.segs))
        both = dict(stockfw.PROFILES)
        both.update(PROFILES)
        self.assertEqual(stockfw.split_merged(merged, both), (image, PROF, self.segs), "among every other profile")
        self.assertIsNone(stockfw.split_merged(image, both), "a plain image")
        dirty = bytearray(merged)
        dirty[end + 100] = 0
        self.assertIn("other data between or after its windows", message(stockfw.split_merged, bytes(dirty), PROFILES))
        wrong = bytearray(merged)
        wrong[PL["code"] - base + 0x500] ^= 1
        self.assertIn("but not the one from", message(stockfw.split_merged, bytes(wrong), PROFILES))
        self.assertIn("inside the codec window at 0x%08X" % PL["code"],
                      message(stockfw.merge, bytes(PL["code"] - base + 1), PROF, self.segs))
        self.assertIn("boards/%s's linker script" % PROF["board"],
                      message(stockfw.merge, bytes(PL["code"] - base + 1), PROF, self.segs))
        self.assertEqual(len(stockfw.merge(bytes(PL["code"] - base), PROF, self.segs)), 1048576)

    def test_an_image_from_another_base_is_split_by_its_own_profile(self):
        # every other profile's image starts at the keyed bootloaders' application base: a merged image of one is never
        # taken for the other, whatever its length
        key = bytes(random.Random(1).randrange(256) for _ in range(1024))
        stock, profiles, _, _ = stockfw.build_synthetic_stock(key, random.Random(2))
        prof, segs = stockfw.extract(stock, key, profiles)
        image = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(4096)
        merged = stockfw.merge(image, prof, segs)
        both = dict(profiles)
        both.update(PROFILES)
        self.assertEqual(stockfw.split_merged(merged, both), (image, prof, segs))
        self.assertNotIn("app_base", prof)
        self.assertEqual(len(merged), stockfw.FLASH_END - stockfw.APP_BASE)

    def test_a_saved_copy(self):
        board = types.SimpleNamespace(id=PROF["board"], codec=types.SimpleNamespace(id=PROF["codec_profile"]))
        blob = self.segs[0][1] + self.segs[1][1]
        with mock.patch.dict(stockfw.PROFILES, PROFILES, clear=True):
            self.assertEqual(codecsave._check_blob(board, blob), (PROF, self.segs[:2]))
            bad = bytearray(blob)
            bad[len(self.segs[0][1]) + 0x3CB0] = 0	# a table hole that is no longer blank
            self.assertIsNone(codecsave._check_blob(board, bytes(bad)))
            self.assertIsNone(codecsave._check_blob(board, blob[:-1]))


class TestPlacedOption(unittest.TestCase):
    """tools/codec_extract.py --placed: how a new profile's placed values are made"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "stock.bin")
        with open(self.path, "wb") as f:
            f.write(STOCK)
        self.bare = {k: v for k, v in PROF.items() if k != "placed"}

    def test_a_profile_without_placed_values(self):
        rc, out, err = tool(["--stock", self.path, "--placed"], {SHA: self.bare})
        self.assertEqual((rc, err), (0, ""))
        lines = out.splitlines()
        self.assertEqual(len(lines), 5)
        for line, (k, name) in zip(lines, stockfw.GATES):
            self.assertRegex(line, r"^gate   %s +0x%08X  expected 0x%08X  ok$" % (name, PL["gates"][k], PL["gates"][k]))
        pd = PROF["placed"]
        self.assertEqual(lines[3], "placed 0x%08X+0x%X  crc32 0x%08X  sha256 %s" % (PL["code"], WINS[0][1], *pd["code"]))
        self.assertEqual(lines[4], "placed 0x%08X+0x%X  crc32 0x%08X  sha256 %s"
                         % (PL["tables"], PL["tables_len"], *pd["tables"]))
        self.assertEqual(os.listdir(self.tmp.name), ["stock.bin"], "it writes nothing")
        rc2, out2, _ = tool(["--stock", self.path, "--placed"], PROFILES)
        self.assertEqual((rc2, out2), (0, out), "the same for a profile that has them: nothing is compared with them")

    def test_a_gate_that_does_not_hold(self):
        for k, name in stockfw.GATES:
            with self.subTest(k):
                prof = dict(self.bare, placement=dict(PL, gates=dict(PL["gates"], **{k: PL["gates"][k] ^ 0x8000})))
                rc, out, err = tool(["--stock", self.path, "--placed"], {SHA: prof})
                self.assertEqual((rc, err), (1, ""))
                lines = out.splitlines()
                self.assertEqual(len(lines), 3, "no window line")
                self.assertEqual([x.rsplit("  ", 1)[1] for x in lines],
                                 ["MISMATCH" if g == k else "ok" for g, _ in stockfw.GATES])
        self.assertEqual(os.listdir(self.tmp.name), ["stock.bin"])

    def test_a_profile_of_the_first_kind(self):
        key = bytes(random.Random(1).randrange(256) for _ in range(1024))
        stock, profiles, _, _ = stockfw.build_synthetic_stock(key, random.Random(2))
        (_, prof), = profiles.items()
        lines, ok = codec_extract.placed_report(stock, key, profiles=profiles)
        self.assertTrue(ok)
        self.assertEqual(lines, ["placed 0x%08X+0x%X  crc32 0x%08X  sha256 %s" % (a, n, prof["placed"][k][0], prof["placed"][k][1])
                                 for (a, n, _), k in zip(stockfw.placed_windows(prof), ("code", "tables"))])


if __name__ == "__main__":
    unittest.main()
