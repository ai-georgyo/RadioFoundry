# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Host tests for the codec maps (rfoundry/codecmap.py): loading, the checks, moving code and tables. The vectors in
codecmap_vectors.json are synthetic (random bytes with planted address words) and shared with the web flasher's port
(site/tests/codecmap.test.mjs), so both move a codec to the same bytes.

    python3 -m unittest discover -s tools/rfoundry/tests -v
"""
import copy
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from rfoundry import boards, stockfw  # noqa: E402
from rfoundry import codecmap as cm  # noqa: E402

with open(os.path.join(HERE, "codecmap_vectors.json")) as f:
    V = json.load(f)
CODE = bytes.fromhex(V["code"])
NEW = int(V["new_run"], 0)


def word(b, off):
    return struct.unpack_from("<I", b, off)[0]


def broken(**change):
    raw = copy.deepcopy(V["map"])
    for path, value in change.items():
        node = raw
        keys = path.split("__")
        for k in keys[:-1]:
            node = node[int(k)] if k.isdigit() else node[k]
        node[int(keys[-1]) if keys[-1].isdigit() else keys[-1]] = value
    return raw


class TestCodecMap(unittest.TestCase):
    def setUp(self):
        self.m = cm.normalize(copy.deepcopy(V["map"]))

    def test_numbers_as_hex_strings(self):
        self.assertEqual(self.m["code"]["run"], 0x1CEB0)
        self.assertEqual(self.m["entries"]["encode"], 0x60)
        self.assertEqual(self.m["self_refs"][0]["value"], 0x1CEC0)

    def test_move_code_rewrites_only_the_self_refs(self):
        out = cm.move_code(CODE, self.m, NEW)
        self.assertEqual(out.hex(), V["moved"])
        delta = NEW - 0x1CEB0
        for r in self.m["self_refs"]:
            self.assertEqual(word(out, r["off"]), r["value"] + delta)
        listed = {r["off"] + i for r in self.m["self_refs"] for i in range(4)}
        self.assertTrue(all(out[i] == CODE[i] for i in range(len(CODE)) if i not in listed))
        self.assertEqual(cm.move_code(CODE, self.m, 0x1CEB0), CODE, "moving to the stock address changes nothing")
        self.assertEqual(cm.relocations(self.m), [0x40, 0x84, 0xFC])

    def test_move_code_refuses_another_build(self):
        other = bytearray(CODE)
        other[5] ^= 1
        with self.assertRaises(cm.MapError):
            cm.move_code(bytes(other), self.m, NEW)
        with self.assertRaises(cm.MapError):
            cm.move_code(CODE[:-4], self.m, NEW)
        # without a hash in the map, a listed word that doesn't hold its value still stops the move
        m = cm.normalize(broken(code__sha256=None))
        other = bytearray(CODE)
        struct.pack_into("<I", other, 0x40, 0)
        with self.assertRaises(cm.MapError):
            cm.move_code(bytes(other), m, NEW)

    def test_move_tables(self):
        moves = {int(k, 0): int(v, 0) for k, v in V["table_moves"].items()}
        out = cm.move_tables(CODE, self.m, moves)
        self.assertEqual(out.hex(), V["tables_moved"])
        self.assertEqual(word(out, 0x24), 0x18002000 + 0xC, "a ref into an object keeps its offset in it")
        self.assertEqual(cm.move_tables(CODE, self.m, {}), CODE)
        with self.assertRaises(cm.MapError):
            cm.move_tables(CODE, self.m, {0x18001600: 0x18003000})	# its group partner stays

    def test_checks(self):
        bad = {
            "unaligned self ref": broken(self_refs__0__off="0x41"),
            "self ref outside the code": broken(self_refs__0__off="0x100"),
            "self ref value outside the code": broken(self_refs__0__value="0x1"),
            "unknown ref kind": broken(self_refs__0__kind="pair"),
            "movw/movt on C-SKY": broken(self_refs__0__kind="movw_movt", self_refs__0__off2="0x44"),
            "entry outside the code": broken(entries__encode="0x200"),
            "table ref into no object": broken(table_refs__0__target="0x18009000"),
            "refs outside the codec": broken(outside_refs=["0x40021000"]),
            "word listed twice": broken(table_refs__0__off="0x40"),
            "other format": broken(format="rfoundry-codec-map/0"),
            "unknown isa": broken(isa="mips"),
            "not a number": broken(code__len="lots"),
        }
        for what, raw in bad.items():
            with self.subTest(what):
                with self.assertRaises(cm.MapError):
                    cm.normalize(raw)

    def test_move_tables_refuses_pairs_and_island_refs_only_when_they_move(self):
        T = cm.normalize(copy.deepcopy(V["thumb"]["map"]))
        code = bytes.fromhex(V["thumb"]["code"])
        self.assertEqual(cm.move_tables(code, T, {}), code)
        t = copy.deepcopy(T)
        t["island_refs"] = [{"kind": "word", "at": 0x080D19F0 + 0x40, "target": 0x080D19F0 + 0x190}]
        with self.assertRaises(cm.MapError):
            cm.move_tables(code, t, {0x080D19F0 + 0x180: 0x080D19F0 + 0x1C0})

    def test_load_maps(self):
        with tempfile.TemporaryDirectory() as d:
            os.mkdir(os.path.join(d, "test"))
            with open(os.path.join(d, "test", "test-csky.json"), "w") as f:
                json.dump(V["map"], f)
            maps = cm.load_maps(d)
            self.assertEqual(list(maps), ["test-csky"])
            self.assertEqual(cm.by_code_sha(maps, V["map"]["code"]["sha256"])[0]["profile"], "test-csky")
            os.mkdir(os.path.join(d, "other"))
            with open(os.path.join(d, "other", "x.json"), "w") as f:
                json.dump(V["map"], f)
            with self.assertRaises(cm.MapError):	# filed under the wrong board, and a second map for the profile
                cm.load_maps(d)

    def test_shipped_maps_load(self):
        for p, m in cm.load_maps().items():
            with self.subTest(p):
                self.assertEqual(m["profile"], p)

    def test_a_map_without_the_optional_keys_gets_none(self):
        # ranges and holes of the code, holes of a table window and a ref's anchor are converted where a map has them
        # and never added: a map without them is loaded to the data it was loaded to before they existed
        n = 0
        for board in sorted(os.listdir(cm.MAPS_DIR)):
            d = os.path.join(cm.MAPS_DIR, board)
            for name in sorted(os.listdir(d)) if os.path.isdir(d) else ():
                if not name.endswith(".json"):
                    continue
                with open(os.path.join(d, name)) as f:
                    raw = json.load(f)
                m = cm.normalize(raw)
                with self.subTest(name):
                    self.assertEqual(set(m["code"]), set(raw["code"]))
                    for rw, w in zip(raw.get("table_windows", []), m["table_windows"]):
                        self.assertEqual(set(w), set(rw))
                    for k in ("self_refs", "island_ptrs", "table_refs", "island_refs"):
                        for rr, r in zip(raw.get(k, []), m[k]):
                            self.assertEqual(set(r) - {"kind"}, set(rr) - {"kind"})
                    if "ranges" not in raw["code"] and "holes" not in raw["code"]:
                        n += 1
                        self.assertEqual(cm.holes(m), [])
                        self.assertIsNone(cm.ranges_crc(m, [(m["code"]["run"], m["code"]["len"])], [0], [0]))
                    if not any("holes" in w for w in raw.get("table_windows", [])):
                        self.assertEqual(cm.table_holes(m), {})
        self.assertGreater(n, 100)


class TestShippedLayouts(unittest.TestCase):
    """Every stock profile that places its codec, against its shipped map: what a move relies on"""

    def placing(self):
        out = [p for p in stockfw.PROFILES.values() if len(stockfw.forms(p)) > 1]
        self.assertTrue(out)
        return out

    def test_maps_are_read_once(self):
        self.assertIs(cm.shipped(), cm.shipped())
        self.assertEqual(sorted(cm.shipped()), sorted(cm.load_maps()))

    def test_every_listed_site_moves_with_its_window(self):
        # _layout_sites refuses a site or a value outside the carved windows: every address the map lists has a
        # window to follow, and nothing it doesn't list points outside them (no outside_refs)
        for prof in self.placing():
            with self.subTest(prof["name"]):
                m = stockfw.code_map(prof)
                self.assertIsNotNone(m)
                self.assertEqual(m["outside_refs"], [])
                cur = stockfw.forms(prof)[0]
                wins = stockfw.carve(prof, cur)
                stock = [a for a, _ in wins]
                for f in stockfw.forms(prof):
                    sites = cm._layout_sites(m, wins, stock, [a for a, _, _ in stockfw.placed_windows(prof, f)])
                    self.assertTrue(sites)

    def test_current_crcs_follow_from_the_former_ones(self):
        # the CRC of each window where the codec goes now, worked out from where an earlier release put it and the map
        # alone, is the one the firmware pins: the two layouts and the map agree
        for prof in self.placing():
            fs = stockfw.forms(prof)
            wins = stockfw.carve(prof, fs[0])
            for f in fs[1:]:
                with self.subTest(prof["name"], former=f["codec_profile"]):
                    got = cm.relocate_crcs(stockfw.code_map(prof), wins, stockfw._crcs(f),
                                           [a for a, _, _ in stockfw.placed_windows(prof, f)],
                                           [a for a, _, _ in stockfw.placed_windows(prof, fs[0])])
                    self.assertEqual(list(got), stockfw._crcs(fs[0]))

    def test_placed_windows_leave_the_application_alone(self):
        for prof in self.placing():
            with self.subTest(prof["name"]):
                lo, hi = boards.get(prof["board"]).app_region
                wins = sorted((a, n) for a, n, _ in stockfw.layout(prof))
                for a, n in wins:
                    self.assertTrue(a >= hi or a + n <= lo, hex(a))
                for (a, n), (b, _) in zip(wins, wins[1:]):
                    self.assertLessEqual(a + n, b, hex(a))		# no two windows (or the descriptor) overlap


class TestThumb(unittest.TestCase):
    """Thumb-2: movw/movt self references, and the BL calls from code kept in a table window (an island) into the
    code. The encoders agree with the GNU assembler (checked by hand); the vector is shared with codecmap.js."""

    def setUp(self):
        self.t = V["thumb"]
        self.m = cm.normalize(copy.deepcopy(self.t["map"]))
        self.code = bytes.fromhex(self.t["code"])
        self.win = bytes.fromhex(self.t["window"])
        self.at = int(self.t["window_addr"], 0)
        self.new = int(self.t["new_run"], 0)
        self.delta = self.new - self.m["code"]["run"]

    def test_bl_round_trip(self):
        b = bytearray(4)
        for at, target in ((0x080D1A00, 0x080E3AF4), (0x080D1A04, 0x08074794), (0x080D1A10, 0x07F00000),
                           (0x08000000, 0x08000004), (0x08FFFFF0, 0x08000000)):
            cm.put_bl(b, 0, at, target)
            self.assertEqual(cm.bl_target(b, 0, at), target)
        with self.assertRaises(cm.MapError):
            cm.put_bl(b, 0, 0x08000000, 0x0A000000)		# out of reach
        self.assertIsNone(cm.bl_target(b"\0\0\0\0", 0, 0x08000000))

    def test_move_code(self):
        out = cm.move_code(self.code, self.m, self.new)
        self.assertEqual(out.hex(), self.t["moved"])
        self.assertEqual(word(out, 0x40), self.m["self_refs"][0]["value"] + self.delta)
        self.assertEqual(cm.pair_value(out, self.m["self_refs"][1]), self.m["self_refs"][1]["value"] + self.delta)
        self.assertEqual(word(out, 0x80), word(self.code, 0x80), "a pointer into an island that stays is kept")
        changed = [i for i in range(len(out)) if out[i] != self.code[i]]
        self.assertTrue(all(0x40 <= i < 0x44 or 0x100 <= i < 0x108 for i in changed))
        with self.assertRaises(cm.MapError):
            cm.move_code(self.code, self.m, self.new + 2)		# Align(PC, 4) literal pools: whole words only
        with self.assertRaises(cm.MapError):
            cm.relocations(self.m)				# not a move by adding to words

    def test_move_branches(self):
        out = cm.move_branches(self.at, self.win, self.m, self.new)
        self.assertEqual(out.hex(), self.t["window_moved"])
        for b in self.m["branches"]:
            self.assertEqual(cm.bl_target(out, b["at"] - self.at, b["at"]), b["target"] + self.delta)
        self.assertEqual(cm.branches_in(self.m, self.at, len(self.win)), 2)
        self.assertEqual(cm.move_branches(self.at + 0x1000, self.win, self.m, self.new), self.win, "no branch in it")
        stale = bytearray(self.win)
        stale[0x10:0x14] = b"\0\0\0\0"
        with self.assertRaises(cm.MapError):
            cm.move_branches(self.at, bytes(stale), self.m, self.new)

    def test_checks(self):
        def bad(**change):
            raw = copy.deepcopy(self.t["map"])
            for k, v in change.items():
                node = raw
                keys = k.split("__")
                for x in keys[:-1]:
                    node = node[int(x)] if x.isdigit() else node[x]
                node[int(keys[-1]) if keys[-1].isdigit() else keys[-1]] = v
            return raw
        for what, raw in {"branch outside an island": bad(branches__0__at="0x080D1B00"),
                          "branch target outside the code": bad(branches__0__target="0x08000000"),
                          "pair without off2": bad(self_refs__1__off2=None),
                          "odd pair site": bad(self_refs__1__off="0x101"),
                          "island pointer into no island": bad(island_ptrs__0__target="0x08000000")}.items():
            with self.subTest(what):
                if raw["self_refs"][1].get("off2") is None:
                    del raw["self_refs"][1]["off2"]
                with self.assertRaises(cm.MapError):
                    cm.normalize(raw)


class TestWindowMove(unittest.TestCase):
    """The code and its table window moved as two blocks (move_window), and the move read back from a copy found in a
    flash read (unmove_window). The vector is shared with codecmap.js."""

    def setUp(self):
        self.w = V["window"]
        self.m = cm.normalize(copy.deepcopy(self.w["map"]))
        self.code, self.tables = bytes.fromhex(self.w["code"]), bytes.fromhex(self.w["tables"])
        self.cr, self.tr = int(self.w["code_run"], 0), int(self.w["tables_run"], 0)
        self.lo = cm.table_window(self.m)[0]

    def test_move_window(self):
        mc, mt = cm.move_window(self.code, self.tables, self.m, self.cr, self.tr)
        self.assertEqual(mc.hex(), self.w["code_moved"])
        self.assertEqual(mt.hex(), self.w["tables_moved"])
        dt = self.tr - self.lo
        self.assertEqual(word(mc, 0x84), 0x080D1B70 + dt, "a table ref follows the tables")
        self.assertEqual(word(mc, 0x80), 0x080D1A11 + dt, "an island pointer follows the tables")
        self.assertEqual(word(mt, 0x40), 0x080D1B78 + dt, "an island ref follows the tables")
        for b in self.m["branches"]:
            self.assertEqual(cm.bl_target(mt, b["at"] - self.lo, b["at"] + dt), b["target"] + self.cr - self.m["code"]["run"])
        with self.assertRaises(cm.MapError):
            cm.move_window(self.code, self.tables + b"\0", self.m, self.cr, self.tr)	# not the map's cut
        with self.assertRaises(cm.MapError):
            cm.move_window(self.code, self.tables, self.m, self.cr, self.tr + 2)

    def flash(self, cr, tr, tables=None):
        mc, mt = cm.move_window(self.code, self.tables, self.m, cr, tr)
        b = bytearray(b"\xff" * 0x100000)
        b[cr - 0x08000000:cr - 0x08000000 + len(mc)] = mc
        b[tr - 0x08000000:tr - 0x08000000 + len(mt)] = mt if tables is None else tables
        return b

    def test_unmove_window(self):
        """Wherever the copy is: the code anywhere, the tables at their stock address or moved, the islands' calls
        into the code moved with it or left as stock had them"""
        for cr, tr, tables in ((self.cr, self.tr, None), (0x0807537C, self.lo, None), (0x0807537C, self.lo, self.tables),
                               (self.m["code"]["run"], self.lo, None), (0x08060004, 0x08098010, None)):
            with self.subTest(code=hex(cr), tables=hex(tr)):
                got = cm.unmove_window(self.flash(cr, tr, tables), 0x08000000, self.m, cr)
                self.assertEqual(got, (self.code, self.tables, tr))
        b = self.flash(self.cr, self.tr)
        self.assertIsNone(cm.unmove_window(b, 0x08000000, self.m, self.cr + 4), "not where the code is")
        for off in (self.cr + 0x200, self.tr + 0x100, self.tr + 0x40):
            c = bytearray(b)
            c[off - 0x08000000] ^= 1
            self.assertIsNone(cm.unmove_window(c, 0x08000000, self.m, self.cr), "changed at 0x%08X" % off)

    def test_window_must_hold_every_ref(self):
        t = copy.deepcopy(self.m)
        t["table_windows"][0]["min_len"] = 0x100		# the object at +0x180 is outside
        with self.assertRaises(cm.MapError):
            cm.table_window(t)
        self.assertIsNone(cm.unmove_window(bytes(0x100000), 0x08000000, t, self.cr))


class TestJoined(unittest.TestCase):
    """A code window that names its ranges and holes, a table ref anchored to a window it points just below, and
    every window moved as its own block and then joined into one (the "joined" vector, shared with codecmap.js)"""

    def setUp(self):
        self.j = V["joined"]
        self.m = cm.normalize(copy.deepcopy(self.j["map"]))
        self.wins = [(int(a, 0), len(d) // 2) for a, d in self.j["windows"]]
        self.parts = [bytes.fromhex(d) for _, d in self.j["windows"]]
        self.src = [a for a, _ in self.wins]
        self.dst = [int(a, 0) for a in self.j["layout"]]
        self.at = int(self.j["block_at"], 0)

    def block(self, pieces):
        out = bytearray(b"\xFF" * (len(self.j["block"]) // 2))
        for a, d in zip(self.dst[1:], pieces):
            out[a - self.at:a - self.at + len(d)] = d
        return bytes(out)

    def bad(self, **change):
        raw = copy.deepcopy(self.j["map"])
        for path, value in change.items():
            node = raw
            keys = path.split("__")
            for k in keys[:-1]:
                node = node[int(k)] if k.isdigit() else node[k]
            node[int(keys[-1]) if keys[-1].isdigit() else keys[-1]] = value
        return raw

    def test_relocate_blank_and_join(self):
        out = cm.relocate(self.m, self.wins, self.parts, self.src, self.dst)
        self.assertEqual([d.hex() for d in out], self.j["relocated"])
        self.assertEqual(cm.relocate(self.m, self.wins, out, self.dst, self.src), self.parts, "and back to stock")
        self.assertEqual(cm.blank(out[0], cm.holes(self.m)).hex(), self.j["code_blanked"])
        self.assertEqual(self.block(out[1:]).hex(), self.j["block"])
        th = cm.table_holes(self.m)
        self.assertEqual(th, {self.src[3]: [(0x20, 4)]})
        blanked = self.block([cm.blank(d, th.get(a, [])) for a, d in zip(self.src[1:], out[1:])])
        self.assertEqual(blanked.hex(), self.j["block_blanked"])
        # what changed: the listed sites in the code, the holes when blanked, nothing else
        code = cm.blank(out[0], cm.holes(self.m))
        sites = {o + i for o in (0x20, 0x24, 0x40, 0x50, 0x54, 0x58, 0x1E0, 0x1E8) for i in range(4)}
        holes = {o + i for o, n in cm.holes(self.m) for i in range(n)}
        self.assertEqual(cm.holes(self.m), [(0x100, 0x80), (0x1C0, 0x10)])
        for i, (a, b) in enumerate(zip(code, self.parts[0])):
            if i in holes:
                self.assertEqual(a, 0xFF)
            elif i not in sites:
                self.assertEqual(a, b, hex(i))
        diff = [i for i, (a, b) in enumerate(zip(blanked, self.block(out[1:]))) if a != b]
        t2 = self.dst[3] - self.at
        self.assertTrue(set(diff) <= set(range(t2 + 0x20, t2 + 0x24)))
        gap = range(self.dst[3] + self.wins[3][1] - self.at, self.dst[4] - self.at)
        self.assertEqual(len(gap), 8)
        self.assertTrue(all(blanked[i] == 0xFF for i in gap), "0xFF between two windows of the block")

    def test_an_anchored_value_moves_with_its_window(self):
        r = self.m["table_refs"][2]
        self.assertEqual((r["target"], r["anchor"]), (self.src[4] - 2, self.src[4]))
        self.assertEqual(cm.held(r), self.src[4])
        self.assertEqual(cm.held(self.m["table_refs"][0]), self.src[2], "a ref without an anchor: its value")
        out = cm.relocate(self.m, self.wins, self.parts, self.src, self.dst)
        self.assertEqual(word(out[0], 0x58), self.dst[4] - 2, "two bytes below the window, where the window went")
        self.assertEqual(cm.pair_value(out[0], self.m["table_refs"][3]), self.dst[4] + 8)
        self.assertEqual(cm.bl_target(out[1], 0x20, self.dst[1] + 0x20), self.dst[0] + 0x1A0)
        self.assertEqual(word(out[1], 0x40), self.dst[3])
        stale = [bytearray(d) for d in self.parts]
        struct.pack_into("<I", stale[0], 0x58, self.src[4])	# the window's start, not the value the map lists
        with self.assertRaises(cm.MapError):
            cm.relocate(self.m, self.wins, [bytes(d) for d in stale], self.src, self.dst)

    def test_crcs_follow_without_the_bytes(self):
        out = cm.relocate(self.m, self.wins, self.parts, self.src, self.dst)
        crc = lambda d: zlib.crc32(d) & 0xFFFFFFFF
        stock = [cm.stock_crc(self.m["code"]), cm.stock_crc(self.m["islands"][0])] + \
                [cm.stock_crc(w) for w in self.m["table_windows"]]
        self.assertEqual(stock, [crc(d) for d in self.parts])
        moved = cm.relocate_crcs(self.m, self.wins, stock, self.src, self.dst)
        self.assertEqual(moved, [crc(d) for d in out])
        only = cm.ranges_only(out[0], self.m)
        self.assertEqual(only[0x100:0x180], b"\xFF" * 0x80, "the hole between the ranges")
        self.assertEqual(only[:0x100] + only[0x180:], out[0][:0x100] + out[0][0x180:], "a hole inside a range stays")
        self.assertEqual(cm.ranges_crc(self.m, self.wins, self.src, self.dst), crc(only))
        # a concatenation's CRC-32 from its parts': the block from its windows and the 0xFF between them
        parts, at = [], self.at
        for a, d, c in zip(self.dst[1:], out[1:], moved[1:]):
            if a > at:
                parts.append((b"\xFF" * (a - at), a - at))
            parts.append((c, len(d)))
            at = a + len(d)
        self.assertEqual(cm.crc_join(parts), crc(self.block(out[1:])))
        self.assertEqual(cm.crc_join([(b"abc", 3), (crc(b""), 0), (crc(b"defgh"), 5)]), crc(b"abcdefgh"))
        self.assertEqual(cm.crc_join([]), crc(b""))
        no_crc = copy.deepcopy(self.m)
        del no_crc["code"]["ranges"][1]["crc32"]
        self.assertIsNone(cm.ranges_crc(no_crc, self.wins, self.src, self.dst))
        self.assertIsNone(cm.stock_crc({}))

    def test_checks(self):
        t3 = self.src[4]
        self.assertEqual(cm.normalize(self.bad(table_refs__2__anchor=hex(t3 + 13)))["table_refs"][2]["anchor"], t3 + 13)
        island_ref = dict(self.j["map"]["island_refs"][0], anchor=hex(self.src[3] + 4))
        branch = dict(self.j["map"]["branches"][0], anchor=hex(self.src[0] + 0x1A4))
        bad = {
            "anchor 16 away": self.bad(table_refs__2__anchor=hex(t3 + 14)),
            "anchor equal to the value": self.bad(table_refs__2__anchor=hex(t3 - 2)),
            "anchor in no object": self.bad(table_refs__2__anchor=hex(t3 - 12)),
            "anchor on an island ref": self.bad(island_refs=[island_ref]),
            "anchor on a self ref": self.bad(self_refs__0__anchor=hex(self.src[0] + 0x204)),
            "anchor on a branch": self.bad(branches=[branch]),
            "hole outside the code": self.bad(code__holes__1__off="0x2F8"),
            "empty hole": self.bad(code__holes__1__len=0),
            "overlapping holes": self.bad(code__holes__1__off="0x170"),
            "a gap between ranges that is no hole": self.bad(code__ranges__1__off="0x190", code__ranges__1__len=0x170),
            "a hole between ranges that is too short": self.bad(code__holes__0__len=0x70),
            "holes without ranges": self.bad(code__ranges=[]),
            "the code doesn't start with a range": self.bad(code__ranges__0__off="0x10", code__ranges__0__len=0xF0),
            "the ranges don't reach the code's end": self.bad(code__ranges__1__len=0x170),
            "overlapping ranges": self.bad(code__ranges__0__len=0x190),
            "a site in a hole": self.bad(code__holes__1__off="0x1E4"),
            "the second half of a pair in a hole": self.bad(code__holes__1__off="0x1EA", code__holes__1__len=2),
            "an entry in a hole": self.bad(entries__encode="0x1C2"),
            "an entry's second byte in a hole": self.bad(entries__encode="0x1BF"),
            "a table window's hole outside the window": self.bad(table_windows__1__holes__0__off="0x2E"),
            "a ref into a table window's hole": self.bad(table_windows__1__holes__0__off="0x10"),
            "an island ref into a table window's hole": self.bad(table_windows__1__holes__0__off="0x0"),
            "an anchor in a table window's hole": self.bad(table_windows__2__holes=[{"off": "0x0", "len": 2}]),
            "a range's first word is not a number": self.bad(code__ranges__0__first_word="push"),
            "an anchor that is the value itself": self.bad(table_refs__0__anchor=self.j["map"]["table_refs"][0]["target"]),
            "a branch into a hole": self.bad(code__holes__1__off="0x1A0"),
            "a self ref into a hole (its Thumb bit set)": self.bad(code__holes__1__off="0x1F8", code__holes__1__len=9),
        }
        # the code starts with a listed hole, not with a range
        raw = self.bad(code__ranges__0__off="0x10", code__ranges__0__len=0xF0)
        raw["code"]["holes"].insert(0, {"off": "0x0", "len": 16})
        bad["a hole before the first range"] = raw

        def island_window(off, n):
            # a table window that holds the island, as the builds with one table window have it, with a hole in it
            raw = self.bad()
            raw["table_windows"].append({"id": "TI", "flash": hex(self.src[1]), "run": hex(self.src[1]), "len": self.wins[1][1],
                                         "holes": [{"off": off, "len": n}]})
            return raw
        bad["an island ref's site in a table window's hole"] = island_window(0x43, 2)
        bad["a branch's site in a table window's hole"] = island_window(0x1E, 3)
        bad["an island's entry in a table window's hole"] = island_window(0x10, 1)
        for what, raw in bad.items():
            with self.subTest(what):
                with self.assertRaises(cm.MapError):
                    cm.normalize(raw)
        # each of them is one step from a map that loads
        cm.normalize(self.bad(code__holes__1__off="0x1F0"))
        cm.normalize(self.bad(code__holes__1__off="0x1F8", code__holes__1__len=8))
        cm.normalize(self.bad(code__holes__1__off="0x1A4"))
        cm.normalize(self.bad(table_windows__1__holes__0__off="0x24"))
        cm.normalize(self.bad(entries__encode="0x1BE"))
        cm.normalize(island_window(0x60, 4))
        cm.normalize(island_window(0x44, 0x3C))
        cm.normalize(island_window(0x12, 0xE))

    def test_a_reader_without_the_anchor_refuses_the_map(self):
        # what keeps a tool that doesn't know the key from moving this codec wrongly: without the anchor the value
        # lies in no object and in no window
        raw = self.bad()
        del raw["table_refs"][2]["anchor"]
        with self.assertRaises(cm.MapError):
            cm.normalize(raw)
        m = copy.deepcopy(self.m)
        del m["table_refs"][2]["anchor"]
        with self.assertRaises(cm.MapError):
            cm._layout_sites(m, self.wins, self.src, self.dst)


if __name__ == "__main__":
    unittest.main()
