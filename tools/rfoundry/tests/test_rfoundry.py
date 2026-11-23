# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Host tests for rfoundry: sector planning, manifests, the codec merge library, key derivation, the codec decision,
and end-to-end runs against the fake bootloader. No USB, no radio, no real key or stock file.

    python3 -m unittest discover -s tools/rfoundry/tests -v        (tools/ci.sh runs this)
"""
import collections
import contextlib
import hashlib
import io
import json
import os
import random
import re
import shutil
import struct
import sys
import tempfile
import unittest
import zlib
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

sys.path.insert(0, HERE)
import home  # noqa: E402,F401 (the settings folder in a temporary folder)
from rfoundry import boards, bootkey, cli, codecmap, codecsave, debuglink, fakedfu, manifest as mf, plan, state, stockfw  # noqa: E402
from rfoundry import dfu as dfumod  # noqa: E402

DM1701 = boards.get("dm1701")
KEY = fakedfu.synthetic_key()


def app_image(n, seed=1):
    rng = random.Random(seed)
    return struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + stockfw.random_bytes(rng, n - 8)


def run_cli(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = cli.main(argv)
    return rc, out.getvalue(), err.getvalue()


_STOCK = {}


def synthetic_stock():
    if not _STOCK:
        _STOCK["v"] = stockfw.build_synthetic_stock(KEY, random.Random(7))
    return _STOCK["v"]


def placed():
    """The synthetic DM-1701 codec as the merge writes it: (profile, segments: code moved into sector 11, the tables
    at their stock address, the descriptor)"""
    _, profiles, code, tables = synthetic_stock()
    prof = list(profiles.values())[0]
    return prof, stockfw.place(prof, [(prof["code"][0], code), (prof["tables"][0], tables)])


class NoDefaultBoard(unittest.TestCase):
    """Nothing given names the radio: rfoundry asks on a terminal and refuses otherwise, never assumes one."""

    def test_refuses_without_a_terminal(self):
        with mock.patch("sys.stdin.isatty", return_value=False):
            with self.assertRaises(cli.UsageError) as e:
                cli.pick_board(None, "download")
        for b in boards.BOARDS:
            self.assertIn(b, str(e.exception))

    def test_asks_on_a_terminal(self):
        ids = sorted(boards.BOARDS)
        with mock.patch("sys.stdin.isatty", return_value=True), mock.patch("builtins.input", return_value="2"), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cli.pick_board(None, "download").id, boards.by()[1].id)
        self.assertEqual(cli.pick_board(ids[0], "download").id, ids[0])

    def test_one_candidate_needs_no_question(self):
        b = boards.by()[0]
        self.assertIs(cli.pick_board(None, "x", [b]), b)

    def test_the_stock_file_names_the_board(self):
        stock, profiles, _, _ = synthetic_stock()
        prof = list(profiles.values())[0]
        with mock.patch.object(stockfw, "PROFILES", profiles):
            self.assertEqual(cli.stock_board(stock, None).id, prof["board"])
            other = next(b for b in boards.BOARDS if b != prof["board"])
            with self.assertRaises(cli.UsageError):
                cli.stock_board(stock, other)


class BuildOptions(unittest.TestCase):
    """Hardware build options (board.toml [[build_options]]): the image names, the radio's board name, mismatches."""

    def setUp(self):
        self.b = next((b for b in boards.BOARDS.values() if b.build_options), None)
        if self.b is None:
            self.skipTest("no board has build options")
        self.o = self.b.build_options[0]
        self.default, self.other = self.o["values"][0], self.o["values"][1]

    def args(self, *opts):
        return type("A", (), {"option": list(opts)})()

    def test_image_id_names_only_non_default_values(self):
        self.assertEqual(self.b.image_id({}), self.b.id)
        self.assertEqual(self.b.image_id({self.o["name"]: self.default["id"]}), self.b.id)
        self.assertEqual(self.b.image_id({self.o["name"]: self.other["id"]}), self.b.id + "-" + self.other["id"])

    def test_file_names(self):
        """The Makefile's names (BUILD, TARGET) and release.py's, from rfoundry/names.py"""
        oth = {self.o["name"]: self.other["id"]}
        sfx = "-" + self.other["id"]
        self.assertEqual(self.b.build_image({}), "build/%s/rfoundry_%s.bin" % (self.b.id, self.b.id))
        self.assertEqual(self.b.build_image(oth, "amateur"), "build/%s-amateur%s/rfoundry_%s%s.bin"
                         % (self.b.id, sfx, self.b.id, sfx))
        self.assertEqual(self.b.release_id(oth, "full"), self.b.id + sfx)
        self.assertEqual(mf.image_name(self.b.release_id(oth, "amateur"), "v1"), "rfoundry-%s-amateur%s-v1.bin" % (self.b.id, sfx))
        db = next((b for b in boards.BOARDS.values() if b.first_stage), None)
        if db:
            self.assertEqual(db.build_image({}), "build/%s/rfoundry_%s_install.bin" % (db.id, db.id))

    def test_the_board_name_gives_the_value(self):
        probe = {"board": self.other["board_name"]}
        self.assertEqual(cli.resolve_options(self.args(), self.b, probe), {self.o["name"]: self.other["id"]})
        self.assertEqual(self.b.options_for_name(self.b.id), {})
        self.assertIsNone(self.b.options_for_name("no such radio"))

    def test_option_and_radio_disagree(self):
        probe = {"board": self.other["board_name"]}
        with self.assertRaises(cli.UsageError):
            cli.resolve_options(self.args("%s=%s" % (self.o["name"], self.default["id"])), self.b, probe)

    def test_unknown_value_refused(self):
        with self.assertRaises(cli.UsageError):
            cli.resolve_options(self.args("%s=nope" % self.o["name"]), self.b, None)

    def test_release_needs_every_value(self):
        with mock.patch("sys.stdin.isatty", return_value=False), self.assertRaises(cli.UsageError) as e:
            cli.resolve_options(self.args(), self.b, None, need_all=True)
        self.assertIn(self.o["name"], str(e.exception))

    def test_an_image_for_other_hardware_is_refused(self):
        want = {self.o["name"]: self.other["id"]}
        cli.check_options(self.b, {"build_options": dict(want)}, want)
        with self.assertRaises(cli.UsageError):
            cli.check_options(self.b, {}, want)		# no build_options: the default build


class StockPlacement(unittest.TestCase):
    """The registry's stock-file boards ([codec] stock): tools/boards.py leaves their codec windows to stockfw, so the
    checks it makes for a linker codec slot are made here."""

    def test_stock_boards(self):
        stock = [b for b in boards.BOARDS.values() if b.codec_stock]
        self.assertTrue(stock)
        for b in stock:
            for h in b.codec_stock:
                prof = stockfw.PROFILES[h]
                self.assertEqual(prof["board"], b.id)
                self.assertEqual(prof["codec_profile"], b.codec.id, "%s: every stock file holds the board's codec" % b.id)
                if stockfw.codec_only(prof):
                    self.assertNotEqual(prof["key_sha256"], b.key_sha256, "%s: a codec-only file" % b.id)
                else:
                    self.assertEqual(prof["key_sha256"], b.key_sha256, "%s: the DFU key is the stock file's" % b.id)
            for a, n in b.codec.windows:
                self.assertTrue(b.app_region[1] <= a and a + n <= b.flash_end,
                                "%s: codec window 0x%08X+0x%X outside the flash the linker keeps free" % (b.id, a, n))


    def test_firmware_knows_every_placement(self):
        """boards/<radio>/vocoder_<radio>.c pins each stock build where the merge puts it: its tag, the code's address,
        length and CRC, the tables' address, length and CRC, and the codec id the board reports."""
        import re
        root = os.path.join(HERE, "..", "..", "..")
        for b in [b for b in boards.BOARDS.values() if b.codec_stock]:
            voc = open(os.path.join(root, "boards", b.id, "vocoder_%s.c" % b.id)).read()
            nums = {int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)u", voc)}
            self.assertIn('"%s"' % b.codec.id, voc)
            for h in b.codec_stock:
                prof = stockfw.PROFILES[h]
                if not prof.get("placement"):
                    continue
                pl, pd = prof["placement"], prof["placed"]
                for what, v in (("tag", pl["tag"]), ("code", pl["code"]), ("code length", prof["code"][1]),
                                ("code CRC", pd["code"][0]), ("tables", pl.get("tables", prof["tables"][0])),
                                ("tables length", pl.get("tables_len", prof["tables"][1])),
                                ("tables CRC", pd["tables"][0])):
                    self.assertIn(v, nums, "%s %s: %s 0x%X is not in vocoder_%s.c" % (b.id, prof["version"], what, v,
                                                                                       b.id))


class TwoStockFiles(unittest.TestCase):
    """A board whose codec comes from either of two stock files ([codec] stock lists both): the same codec, its tables
    at different addresses. The board's windows are the union of both layouts; each merge plans against its own."""

    A = {"name": "A.bin", "board": "dm1701", "codec_profile": "dm1701-two", "key_sha256": "",
         "code": (0x080E0000, 0x1000, "0" * 64), "tables": (0x080E4000, 0x2000, "1" * 64)}
    B = {"name": "B.bin", "board": "dm1701", "codec_profile": "dm1701-two", "key_sha256": "",
         "code": (0x080E0000, 0x1000, "2" * 64), "tables": (0x080C0000, 0x2000, "3" * 64)}

    def board(self, profiles):
        e = json.load(open(os.path.join(os.path.dirname(boards.__file__), "registry.json")))["boards"]["dm1701"]
        e = dict(e, codec_stock=list(profiles))
        with mock.patch.dict(stockfw.PROFILES, profiles):
            return boards.Board(e)

    def segs(self, prof):
        return [(a, bytes([i + 1]) * n) for i, (a, n, _) in enumerate(stockfw.windows(prof))]

    def test_union_and_each_merge(self):
        b = self.board({"a" * 64: self.A, "b" * 64: self.B})
        self.assertEqual(b.codec.windows, [(0x080E0000, 0x1000), (0x080E4000, 0x2000), (0x080C0000, 0x2000)])
        self.assertEqual(b.codec.sources, ["A.bin", "B.bin"])
        self.assertEqual(b.codec_codes, [0x080E0000, 0x080E0000])
        img = app_image(5000)
        for prof in (self.A, self.B):
            segs = self.segs(prof)
            p = plan.make_plan(b, [(b.app_base, img)], plan.MERGE, segs, b.merge_windows(prof))
            written = b"".join(bl for st in p.writes for _, bl in st.blocks)
            for a, d in segs:
                self.assertIn(d, written)
            # against the union, one file's codec never covers the other's tables: the planner refuses
            with self.assertRaises(plan.PlanError):
                plan.make_plan(b, [(b.app_base, img)], plan.MERGE, segs)

    def test_keep_protects_both(self):
        b = self.board({"a" * 64: self.A, "b" * 64: self.B})
        p = plan.make_plan(b, [(b.app_base, app_image(5000))], plan.KEEP)
        self.assertEqual([s.index for s in p.kept], [10, 11])	# B's tables in sector 10, A's in sector 11
        only_a = self.board({"a" * 64: self.A})
        self.assertEqual([s.index for s in plan.make_plan(only_a, [(b.app_base, app_image(5000))], plan.KEEP).kept],
                         [11])

    def test_different_codecs_refused(self):
        with self.assertRaises(ValueError):
            self.board({"a" * 64: self.A, "b" * 64: dict(self.B, codec_profile="dm1701-other")})


class SectorMap(unittest.TestCase):
    def test_f405_layout(self):
        s = DM1701.sectors
        self.assertEqual([x.index for x in s], list(range(3, 12)))
        self.assertEqual(s[0].addr, 0x0800C000)
        self.assertEqual(s[-1].end, 0x08100000)
        for a, b in zip(s, s[1:]):
            self.assertEqual(a.end, b.addr)

    def test_codec_sectors(self):
        self.assertEqual([x.index for x in DM1701.codec_sectors()], [10, 11])
        ld = open(os.path.join(HERE, "..", "..", "..", "boards", "dm1701", "memory.ld")).read()
        self.assertNotIn("FLASH2", ld)			# one region: no second image any more

    def test_every_sector_key_aligned(self):
        for s in DM1701.sectors:
            self.assertEqual((s.addr - DM1701.app_base) % 1024, 0)

    def test_linker_keeps_app_out_of_codec_sectors(self):
        # the region starts at board.mk APP_BASE (__app_start) and ends below the codec sectors
        ld = open(os.path.join(HERE, "..", "..", "..", "boards", "dm1701", "memory.ld")).read()
        self.assertIn("ORIGIN = __app_start, LENGTH = 0x080C0000 - __app_start", ld)
        self.assertEqual(DM1701.app_region, (0x0800C000, 0x080C0000))
        first_codec = min(s.addr for s in DM1701.codec_sectors())
        self.assertEqual(first_codec, 0x080C0000)
        # the firmware looks for the codec where the merge puts it
        voc = open(os.path.join(HERE, "..", "..", "..", "boards", "dm1701", "vocoder_dm1701.c")).read()
        prof = stockfw.PROFILES["0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"]
        self.assertIn("#define CODEC_TAG\t\t0x%04Xu" % prof["placement"]["tag"], voc)
        self.assertIn("#define CODEC_CODE\t\t0x%08Xu" % prof["placement"]["code"], voc)
        self.assertIn("#define CODEC_CRC\t\t0x%08Xu" % prof["placed"]["code"][0], voc)
        self.assertIn("#define CODEC_TABLES_LEN\t0x%05Xu" % prof["placement"]["tables_len"], voc)
        self.assertIn("#define CODEC_TABLES_CRC\t0x%08Xu" % prof["placed"]["tables"][0], voc)
        self.assertIn('"%s"' % prof["codec_profile"], voc)
        desc = open(os.path.join(HERE, "..", "..", "..", "platform", "stm32", "codec_desc.h")).read()
        self.assertIn("#define CODEC_DESC_ADDR\t\t0x%08Xu" % stockfw.DESC_ADDR, desc)
        self.assertIn("#define CODEC_DESC_MAGIC\t0x%08Xu" % stockfw.DESC_MAGIC, desc)

    def test_dm1801pro_matches_its_linker_script(self):
        b = boards.get("dm1801pro")
        self.assertEqual((b.app_base, b.flash_end, b.family), (0x0300C000, 0x03100000, boards.get("dm32").family))
        self.assertEqual([(s.addr, s.size) for s in b.codec_sectors()], [(0x030E8000, 0x18000)])
        self.assertEqual(b.codec.windows, boards.get("dm32").codec.windows)
        self.assertEqual(b.protected, boards.get("dm32").protected)
        ld = open(os.path.join(HERE, "..", "..", "..", "boards", "dm1801pro", "memory.ld")).read()
        self.assertIn("ORIGIN = __app_start, LENGTH = __codec_slot - __app_start", ld)
        self.assertIn("__codec_slot = 0x030E8000;", ld)
        self.assertIs(boards.from_debuglink_name("DM-1801 Pro"), b)

    def test_h743_layout(self):
        """The CS7000 M17 PLUS: two banks of eight 128 KB sectors from the flash base, written through the ROM
        bootloader (no first stage, no stub, no key); the linker keeps the image in the first bank"""
        b = boards.get("cs7000p")
        self.assertEqual([(x.index, x.addr, x.size) for x in b.sectors],
                         [(i, 0x08000000 + i * 0x20000, 0x20000) for i in range(16)])
        self.assertEqual((b.family, b.rom_base, b.app_base, b.flash_end), ("dfuse", 0x08000000, 0x08000000, 0x08200000))
        self.assertEqual(b.app_region, (0x08000000, 0x08100000))
        self.assertEqual((b.protected, b.first_stage, b.stub, b.key_sha256, b.key_prefix), ([], None, None, None, None))
        self.assertEqual(b.dfu_ids, (0x0483, 0xDF11))
        self.assertEqual(fakedfu.rom_layout(b), "@Internal Flash  /0x08000000/16*128Kg")
        self.assertEqual(dfumod.region(fakedfu.rom_layout(b)), (0x08000000, 0x200000))
        for s in b.sectors:						# every sector starts on a block and a flash word
            self.assertEqual((s.addr - b.app_base) % plan.BLOCK, 0)
        self.assertEqual(plan.BLOCK % dfumod.FLASH_WORD, 0)

    def test_open_firmware_cps_data(self):
        """Every board carries the MCU flash's base (area 5's origin in the open-firmware CPS) and its CPS radio
        types from board.toml [cps]; the base is where the protected bootloader range starts"""
        for b in boards.BOARDS.values():
            with self.subTest(board=b.id):
                # a bootloader in the MCU's ROM leaves the whole flash to the image: nothing is protected
                self.assertEqual(b.protected == [], b.rom_base == b.app_base)
                if b.protected:
                    self.assertEqual(b.rom_base, b.protected[0][0])
                self.assertLessEqual(b.rom_base, b.app_base)
                self.assertTrue(all(isinstance(t, int) and 0 <= t < 64 for t in b.cps_radio_types))
                toml = open(os.path.join(HERE, "..", "..", "..", "boards", b.id, "board.toml")).read()
                self.assertEqual(bool(b.cps_radio_types), "[cps]" in toml)


class Planning(unittest.TestCase):
    def test_keep_small_image(self):
        img = app_image(5000)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.KEEP)
        self.assertEqual([s.index for s in p.erase], [3])
        self.assertEqual([s.index for s in p.kept], [10, 11])
        st = p.writes[0]
        self.assertEqual([n for n, _ in st.blocks], [2, 3, 4, 5, 6])
        self.assertEqual(st.blocks[-1][1], img[4096:] + b"\xFF" * (5 * 1024 - len(img)))

    def test_keep_release_sized_image(self):
        img = app_image(220780)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.KEEP)
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6])
        self.assertEqual(len(p.writes[0].blocks), 16)
        self.assertEqual(len(p.writes[1].blocks), 64)
        self.assertEqual(len(p.writes[2].blocks), 128)
        self.assertEqual(len(p.writes[3].blocks), (220780 - 0x34000 - 1) // 1024 + 1)
        self.assertEqual(b"".join(b for st in p.writes for _, b in st.blocks)[:len(img)], img)
        skipped = [st.sector.index for st in p.steps if st.action == "skip"]
        self.assertEqual(skipped, [7, 8, 9])	# application sectors the image doesn't reach

    def test_keep_fills_whole_app_area(self):
        img = app_image(0x080C0000 - 0x0800C000)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.KEEP)
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6, 7, 8, 9])

    def test_data_byte_ranges_leave_the_plan_alone(self):
        """A [[flash.protect]] entry is a byte range of its memory: data flash entries, a range the firmware keeps
        inside a sector included, change nothing of a program flash plan; the program flash's own still refuse"""
        import copy
        from rfoundry import protect
        img = app_image(220780)
        b = copy.copy(DM1701)
        b.protect = [{"mem": "data", "start": 0x1000, "end": 0x1200, "what": "test (x.md 2.1)"},
                     {"mem": "data", "start": 0x1E000, "end": 0x20000, "what": "test (x.md 2.4)"}]
        want = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.KEEP)
        got = plan.make_plan(b, [(b.app_base, img)], plan.KEEP)
        self.assertEqual([(st.sector.index, st.action) for st in got.steps],
                         [(st.sector.index, st.action) for st in want.steps])
        self.assertEqual(protect.carved_units(b, "data", 0, 0x21000, 0x1000),
                         [a for a in range(0, 0x21000, 0x1000) if a != 0x1000 and not 0x1E000 <= a < 0x20000])
        sec = b.sectors[2]
        b.protect = b.protect + [{"mem": "progmem", "start": sec.addr - b.rom_base + 0x100,
                                  "end": sec.addr - b.rom_base + 0x200, "what": "test (x.md 1)"}]
        with self.assertRaises(plan.PlanError):	# a sector holding part of a progmem entry is refused whole
            plan.make_plan(b, [(b.app_base, img)], plan.KEEP)

    def test_second_range_refused(self):
        """The firmware is one image from the base: a second range (as releases from before the codec moved carried for
        sector 9) is refused anywhere, in both modes and on every board."""
        img = app_image(5000)
        for addr in (0x08060000, 0x080A0000, 0x080C0000, 0x080E0000, 0x0800F000, 0x08000000):
            for mode in (plan.KEEP, plan.MERGE):
                csegs = placed()[1] if mode == plan.MERGE else []
                with self.assertRaises(plan.PlanError, msg="%s %s" % (hex(addr), mode)):
                    plan.make_plan(DM1701, [(DM1701.app_base, img), (addr, b"d" * 100)], mode, csegs)
        with self.assertRaises(plan.PlanError):
            plan.make_plan(boards.get("md380"), [(boards.get("md380").app_base, img), (0x080A0000, b"d" * 100)], plan.MERGE,
                           [(a, b"\0" * n) for a, n in boards.get("md380").codec.windows])

    def test_image_into_codec_sector_refused(self):
        img = app_image(0x080C0000 - 0x0800C000 + 1)
        for mode in (plan.KEEP, plan.MERGE):
            with self.assertRaises(plan.PlanError):
                plan.make_plan(DM1701, [(DM1701.app_base, img)], mode)

    def test_merge_writes_codec_sectors(self):
        prof, segs = placed()
        (ca, mc), (ta, mt), (da, desc) = segs
        img = app_image(100000)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.MERGE, segs, DM1701.merge_windows(prof))
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 10, 11])
        self.assertEqual(p.kept, [])
        s11 = [st for st in p.writes if st.sector.index == 11][0]
        flat = b"".join(b for _, b in s11.blocks)
        flat += b"\xFF" * (0x20000 - len(flat))
        end = ta + len(mt) - 0x080E0000
        self.assertEqual(flat[:end], mt[0x080E0000 - ta:])			# the tables' tail
        at = ca - 0x080E0000
        self.assertEqual(flat[end:at], b"\xFF" * (at - end))
        self.assertEqual(flat[at:at + len(mc)], mc)				# the code, moved
        self.assertEqual(flat[0x1FF80:], desc)					# the descriptor
        self.assertEqual(flat[at + len(mc):0x1FF80], b"\xFF" * (0x1FF80 - at - len(mc)))

    def test_merge_needs_whole_codec(self):
        prof, segs = placed()
        with self.assertRaises(plan.PlanError):
            plan.make_plan(DM1701, [(DM1701.app_base, app_image(1000))], plan.MERGE, segs[:2],
                           DM1701.merge_windows(prof))

    def test_keep_rejects_codec_data(self):
        with self.assertRaises(plan.PlanError):
            plan.make_plan(DM1701, [(DM1701.app_base, app_image(1000))], plan.KEEP, [(0x08074714, b"\0" * 4)])

    def test_bootloader_and_bounds(self):
        with self.assertRaises(plan.PlanError):
            plan.make_plan(DM1701, [(0x08008000, b"\0" * 2048)], plan.KEEP)
        with self.assertRaises(plan.PlanError):
            plan.make_plan(DM1701, [(0x0800C400, b"\0" * 2048)], plan.KEEP)	# not at the vector table
        with self.assertRaises(plan.PlanError):
            plan.make_plan(DM1701, [], plan.KEEP)

    def test_sector_order(self):
        """An install runs in the board's sector order, as before; only a codec move (vector_last) erases the vector
        table's sector first and writes it last"""
        for bid in ("dm1701", "mduv380", "md380", "md9600"):
            b = boards.get(bid)
            p = plan.make_plan(b, [(b.app_base, app_image(0x30000))], plan.KEEP)
            self.assertEqual(p.erase_order, p.erase, bid)
            self.assertEqual(p.write_order, p.writes, bid)
            self.assertNotIn("order:", "\n".join(p.describe()))
            m = plan.make_plan(b, [(b.app_base, app_image(0x30000))], plan.KEEP, vector_last=True)
            self.assertEqual(m.erase_order[0].addr, b.app_base, bid)
            self.assertEqual(m.write_order[-1].sector.addr, b.app_base, bid)
            self.assertEqual(sorted(s.addr for s in m.erase_order), sorted(s.addr for s in p.erase))
            self.assertIn("written last", "\n".join(m.describe()))
            self.assertEqual(cli.Decision(plan.MERGE, "x").moves, False)

    def test_describe_names_every_sector(self):
        p = plan.make_plan(DM1701, [(DM1701.app_base, app_image(30000))], plan.KEEP)
        text = "\n".join(p.describe())
        for i in range(3, 12):
            self.assertIn("sector %2d" % i, text)
        self.assertIn("erase 2 sector(s): 3, 4", text)
        self.assertIn("never erased or written: codec sector(s) 10, 11", text)


class RenamedFolder(unittest.TestCase):
    def test_old_settings_folder_is_copied_once(self):
        with tempfile.TemporaryDirectory() as home:
            env = {"HOME": home, "XDG_CONFIG_HOME": os.path.join(home, ".config"), "APPDATA": os.path.join(home, "appdata")}
            with mock.patch.dict(os.environ, env):
                old = state._config_dir("odrflash")
                os.makedirs(os.path.join(old, "codecs"))
                with open(os.path.join(old, "codecs", "x.bin"), "wb") as f:
                    f.write(b"codec")
                new = state.config_dir()
                self.assertNotEqual(new, old)
                with open(os.path.join(new, "codecs", "x.bin"), "rb") as f:
                    self.assertEqual(f.read(), b"codec")
                os.remove(os.path.join(new, "codecs", "x.bin"))
                state.config_dir()						# the new folder exists: nothing is copied again
                self.assertFalse(os.path.exists(os.path.join(new, "codecs", "x.bin")))
                self.assertTrue(os.path.exists(os.path.join(old, "codecs", "x.bin")))


class Manifests(unittest.TestCase):
    def test_generate_and_verify(self):
        img = app_image(220780)
        m = mf.generate(DM1701, img, "v1.2.3", "a" * 40, "FM,DMR", "gcc 13.3")
        self.assertEqual(m["sectors"], [3, 4, 5, 6])
        self.assertEqual(m["codec"]["sectors"], [10, 11])
        self.assertEqual(m["codec"]["profile"], "dm1701-v0206-csv-s11")
        self.assertFalse(m["codec"]["included"])
        self.assertEqual(m["ranges"], [{"addr": "0x0800C000", "size": 220780}])
        self.assertEqual(m["image"]["file"], "rfoundry-dm1701-v1.2.3.bin")
        m2 = json.loads(mf.dumps(m))
        self.assertIs(mf.verify(m2, img), DM1701)
        self.assertIs(mf.verify(m2, img, "dm1701"), DM1701)

    def test_verify_refusals(self):
        img = app_image(4096)
        m = mf.generate(DM1701, img, "v1", "b" * 40)
        with self.assertRaises(mf.ManifestError):
            mf.verify(m, img[:-1] + b"\0")				# hash
        with self.assertRaises(mf.ManifestError):
            mf.verify(m, img, "dm32")				# wrong radio
        old = json.loads(mf.dumps(m))
        old["format"] = "odr-release/1"			# written before the project was renamed
        self.assertIs(mf.verify(old, img), DM1701)
        bad = json.loads(mf.dumps(m))
        bad["format"] = "rfoundry-release/9"
        with self.assertRaises(mf.ManifestError):
            mf.verify(bad, img)
        bad = json.loads(mf.dumps(m))
        bad["codec"]["profile"] = "dm1701-v0206-csv"	# a release from before the codec moved
        with self.assertRaises(mf.ManifestError):
            mf.verify(bad, img)

    def test_generate_refuses_codec_overlap(self):
        with self.assertRaises(mf.ManifestError):
            mf.generate(DM1701, app_image(0x080C0000 - 0x0800C000 + 4), "v1", "c" * 40)

    def test_release_tool(self):
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        img = app_image(12345)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "in.bin")
            open(bp, "wb").write(img)
            out = os.path.join(d, "dist")
            with contextlib.redirect_stdout(io.StringIO()):
                release.main(["--board", "dm1701", "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", "d" * 40,
                              "--features", "FM", "--toolchain", "test"])
                release.main(["--board", "dm1701", "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", "d" * 40,
                              "--features", "FM", "--toolchain", "test"])	# again: SHA256SUMS not duplicated
            m = mf.load(os.path.join(out, "rfoundry-dm1701-v9.9.9.json"))
            self.assertIs(mf.verify(m, open(os.path.join(out, "rfoundry-dm1701-v9.9.9.bin"), "rb").read()), DM1701)
            sums = open(os.path.join(out, "SHA256SUMS")).read().splitlines()
            self.assertEqual(len(sums), 2)
            self.assertIn(hashlib.sha256(img).hexdigest() + "  rfoundry-dm1701-v9.9.9.bin", sums)
            # a merged image is refused: the codec where this layout puts it (its first word), or where merges before
            # the move did (the stock code, here a stand-in profile's)
            code = struct.pack("<I", stockfw.CODEC_SIG) + bytes(random.Random(5).randrange(256) for _ in range(252))
            fake = {"board": "dm1701", "code": (0x08074714, len(code), hashlib.sha256(code).hexdigest())}
            for at, data in ((0x080E3A74, code[:4]), (0x08074714, code)):
                merged = bytearray(b"\xFF" * (at - 0x0800C000 + len(data)))
                merged[:len(img)] = img
                merged[at - 0x0800C000:] = data
                open(bp, "wb").write(merged)
                with mock.patch.dict(stockfw.PROFILES, {"stand-in": fake}):
                    with self.assertRaises(SystemExit) as e:
                        release.check_no_codec(DM1701, bytes(merged))
                    self.assertIn("codec signature", str(e.exception))
                    with self.assertRaises(SystemExit):
                        release.main(["--board", "dm1701", "--version", "v9", "--bin", bp, "--out", out, "--git", "d" * 40,
                                      "--features", "", "--toolchain", ""])
            # the image's own code may hold the codec's first word at a stock address: that's a push, not the codec
            mine = bytearray(b"\xFF" * (0x08074714 - 0x0800C000 + 4))
            mine[:len(img)] = img
            struct.pack_into("<I", mine, 0x08074714 - 0x0800C000, stockfw.CODEC_SIG)
            release.check_no_codec(DM1701, bytes(mine))

    def test_release_build_option(self):
        """--build-option: a non-default value of a board's [[build_options]] gets its own names and manifest field;
        an unknown option or value is refused."""
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        b = next(b for b in boards.BOARDS.values() if b.build_options)
        opt = b.build_options[0]
        value = opt["values"][1]["id"]
        self.assertIsNotNone(b.build_option(opt["name"], value))
        self.assertIsNone(b.build_option(opt["name"], "no-such-value"))
        img = app_image(4096)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "in.bin")
            open(bp, "wb").write(img)
            out = os.path.join(d, "dist")
            common = ["--board", b.id, "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", "d" * 40,
                      "--features", "FM", "--toolchain", "test"]
            with contextlib.redirect_stdout(io.StringIO()):
                release.main(common + ["--build-option", "%s=%s" % (opt["name"], value)])
            m = mf.load(os.path.join(out, "rfoundry-%s-%s-v9.9.9.json" % (b.id, value)))
            self.assertEqual(m["build_options"], {opt["name"]: value})
            for bad in ("%s=no-such-value" % opt["name"], "NO_SUCH_OPTION=%s" % value):
                with self.assertRaises(SystemExit):
                    release.main(common + ["--build-option", bad])
            with self.assertRaises(SystemExit):
                release.main(common[:1] + ["no-such-board"] + common[2:])

    def test_release_needs_the_first_stage(self):
        """The DB25-D's image goes to 0x0300C000, its first stage's sector: the application alone is refused, the
        install image (first stage, then the application) is released."""
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        stage = bytes([0x2C, 0x11, 0x01, 0xC0]) + bytes(0x1000 - 4)
        app = app_image(20000)
        with tempfile.TemporaryDirectory() as d:
            bp, out = os.path.join(d, "rfoundry_db25d.bin"), os.path.join(d, "dist")
            args = ["--board", "db25d", "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", "d" * 40,
                    "--features", "FM", "--toolchain", "test"]
            open(bp, "wb").write(app)
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as e:
                release.main(args)
            self.assertIn("_install.bin", str(e.exception))
            open(bp, "wb").write(stage)
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
                release.main(args)							# the first stage alone
            open(bp, "wb").write(stage + app)
            with contextlib.redirect_stdout(io.StringIO()):
                release.main(args)
            self.assertEqual(open(os.path.join(out, "rfoundry-db25d-v9.9.9.bin"), "rb").read(), stage + app)


class SingleImage(unittest.TestCase):
    """The DM-1701 image is one range, sectors 3-9 (docs/spec/codec-placement.md); a release from before the codec
    moved (two ranges, the old codec profile) is refused by rfoundry and by the old route."""

    def run_release(self, d, size=30000, name="rfoundry_dm1701"):
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        bp = os.path.join(d, name + ".bin")
        open(bp, "wb").write(app_image(size))
        out = os.path.join(d, "dist")
        with contextlib.redirect_stdout(io.StringIO()):
            release.main(["--board", "dm1701", "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", "d" * 40,
                          "--features", "FM", "--toolchain", "test"])
        return out

    def old_release(self, d):
        """A release laid out as before the codec moved: application + sector 9 data, the old codec profile"""
        img, data = app_image(30000), app_image(9000, seed=4)
        m = mf.generate(DM1701, img, "v0.9.0", "e" * 40)
        m["ranges"].append({"addr": "0x080A0000", "size": len(data)})
        m["image"]["size"] = len(img) + len(data)
        m["image"]["sha256"] = hashlib.sha256(img + data).hexdigest()
        m["codec"]["profile"] = "dm1701-v0206-csv"
        bp = os.path.join(d, "rfoundry-dm1701-v0.9.0.bin")
        open(bp, "wb").write(img + data)
        open(os.path.join(d, "rfoundry-dm1701-v0.9.0.json"), "w").write(mf.dumps(m))
        return bp, m, img + data

    def test_release_is_one_range(self):
        with tempfile.TemporaryDirectory() as d:
            out = self.run_release(d, size=0x080C0000 - 0x0800C000)		# all of sectors 3-9
            whole = open(os.path.join(out, "rfoundry-dm1701-v9.9.9.bin"), "rb").read()
            m = mf.load(os.path.join(out, "rfoundry-dm1701-v9.9.9.json"))
            self.assertEqual(m["sectors"], [3, 4, 5, 6, 7, 8, 9])
            self.assertEqual(m["ranges"], [{"addr": "0x0800C000", "size": len(whole)}])
            self.assertIs(mf.verify(m, whole), DM1701)
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit):
                self.run_release(d, size=0x080C0000 - 0x0800C000 + 4)		# into codec sector 10

    def test_old_release_refused(self):
        with tempfile.TemporaryDirectory() as d:
            _, m, whole = self.old_release(d)
            with self.assertRaises(mf.ManifestError) as e:
                mf.verify(m, whole)
            self.assertIn("2 flash ranges", str(e.exception))
            m1 = json.loads(mf.dumps(m))
            m1["ranges"] = m1["ranges"][:1]
            m1["ranges"][0]["size"] = len(whole)
            with self.assertRaises(mf.ManifestError) as e:
                mf.verify(m1, whole)
            self.assertIn("dm1701-v0206-csv", str(e.exception))

    def merge_cli(self, image, extra=()):
        """rfoundry merge with the synthetic stock file and key (the real profile and key hash are patched out)."""
        stock, profiles, _, _ = synthetic_stock()
        with tempfile.TemporaryDirectory() as d:
            sp, op = os.path.join(d, "stock.bin"), os.path.join(d, "merged.bin")
            open(sp, "wb").write(stock)
            with mock.patch.dict(stockfw.PROFILES, profiles, clear=True), \
                    mock.patch.object(dfumod, "check_key", lambda k, b: k), \
                    mock.patch.object(stockfw, "derive_key", lambda s, want=None: KEY):
                rc, out, err = run_cli(["merge", image, "--stock", sp, "-o", op] + list(extra))
            return rc, out, err, (open(op, "rb").read() if os.path.exists(op) else b"")

    def test_cli_merge_of_a_release_image(self):
        with tempfile.TemporaryDirectory() as d:
            out = self.run_release(d)
            rc, so, se, merged = self.merge_cli(os.path.join(out, "rfoundry-dm1701-v9.9.9.bin"))
        self.assertEqual(rc, 0, so + se)
        self.assertEqual(merged[:30000], app_image(30000))
        self.assertEqual(merged[30000:0x080D19F0 - 0x0800C000], b"\xFF" * (0x080D19F0 - 0x0800C000 - 30000))
        for a, data in placed()[1]:
            self.assertEqual(merged[a - 0x0800C000:][:len(data)], data)
        self.assertEqual(len(merged), 0x08100000 - 0x0800C000)
        with tempfile.TemporaryDirectory() as d:
            bp, _, _ = self.old_release(d)
            rc, so, se, _ = self.merge_cli(bp)
        self.assertNotEqual(rc, 0)


class OtherRadioStock(unittest.TestCase):
    def test_flash_refuses_another_radios_file_by_name_before_the_key(self):
        """rfoundry flash --stock with another radio's file and no --key: named as such, not "could not derive the
        bootloader key" (the key is derived before the codec is extracted)."""
        stock, profiles, _, _ = synthetic_stock()
        digest, prof = next(iter(profiles.items()))
        img = app_image(220780)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "rfoundry-dm1701-v0.0.1.bin")
            open(bp, "wb").write(img)
            open(os.path.join(d, "rfoundry-dm1701-v0.0.1.json"), "w").write(
                mf.dumps(mf.generate(DM1701, img, "v0.0.1", "e" * 40)))
            sp = os.path.join(d, "other.bin")
            open(sp, "wb").write(stock)
            for codec in ("merge", "keep"):
                with mock.patch.dict(stockfw.PROFILES, {digest: dict(prof, board="md380")}), \
                     mock.patch.object(cli, "get_probe", return_value=None), \
                     mock.patch.object(state, "key_path", return_value=os.path.join(d, "no.key")):
                    rc, out, err = run_cli(["flash", bp, "--stock", sp, "--codec", codec, "--yes"])
                self.assertEqual(rc, 2, out + err)
                self.assertIn("is a firmware file for the %s, not the %s" % (boards.get("md380").name, DM1701.name),
                              err, codec)
                self.assertNotIn("derive", err, codec)


class NoCodecFile(unittest.TestCase):
    """A file known to hold no codec (stockfw.NO_CODEC_FILES) is named and refused, as the flasher does
    (site/tests/flasher.test.mjs). The real file is never in the repository: a stand-in registered under its hash."""
    REAL = "a09ab4d364d199853204ebdf068ba0886d7e7bb99689f08437aae52104474a7e"

    def stand_in(self):
        data = app_image(96 * 1024, seed=1702)
        entry = {stockfw.sha256(data): stockfw.NO_CODEC_FILES[self.REAL]}
        return data, mock.patch.dict(stockfw.NO_CODEC_FILES, entry)

    def test_entry(self):
        self.assertEqual(stockfw.NO_CODEC_FILES[self.REAL]["board"], "dm1701")
        for h in stockfw.NO_CODEC_FILES:		# never also a source or a stock file
            self.assertNotIn(h, stockfw.PROFILES)
            self.assertNotIn(h, stockfw.KNOWN_FILES)

    def test_refused_by_name(self):
        data, patch = self.stand_in()
        self.assertIsNone(stockfw.no_codec(data))
        with patch:
            msg = stockfw.no_codec(data)
            self.assertIn(stockfw.NO_CODEC_FILES[self.REAL]["name"], msg)
            with self.assertRaises(stockfw.StockError) as e:
                stockfw.find_profile(data)
            self.assertEqual(str(e.exception), msg)
            with self.assertRaises(cli.UsageError) as e:
                cli.stock_codec(data, None, "dm1701")
            self.assertEqual(str(e.exception), msg)

    def test_flash_with_it_as_the_stock_file(self):
        data, patch = self.stand_in()
        img = app_image(220780)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "rfoundry-dm1701-v0.0.1.bin")
            open(bp, "wb").write(img)
            open(os.path.join(d, "rfoundry-dm1701-v0.0.1.json"), "w").write(
                mf.dumps(mf.generate(DM1701, img, "v0.0.1", "e" * 40)))
            sp = os.path.join(d, "open.bin")
            open(sp, "wb").write(data)
            with patch, mock.patch.object(cli, "get_probe", return_value=None), \
                    mock.patch.object(state, "key_path", return_value=os.path.join(d, "no.key")):
                rc, out, err = run_cli(["flash", bp, "--stock", sp, "--codec", "merge", "--yes"])
            self.assertEqual(rc, 2, out + err)
            self.assertIn(stockfw.NO_CODEC_FILES[self.REAL]["name"], err)

    @unittest.skipUnless(os.environ.get("RFOUNDRY_NOCODEC_FILE", "").endswith(".bin"), "set RFOUNDRY_NOCODEC_FILE (the .bin)")
    def test_real_file(self):
        """The file holds neither the codec's code nor its tables (docs/spec/codec-dm1701.md "Files without the
        codec"): where it expects the code, a code window's length is blank, and it ends before the stock tables"""
        data = open(os.environ["RFOUNDRY_NOCODEC_FILE"], "rb").read()
        self.assertEqual(stockfw.sha256(data), self.REAL)
        self.assertIsNotNone(stockfw.no_codec(data))
        self.assertIsNone(stockfw.from_flash(data, base=stockfw.APP_BASE))
        code = 0x0807537C - stockfw.APP_BASE
        self.assertEqual(data[code:code + 0x1021C], b"\xFF" * 0x1021C)
        self.assertLess(stockfw.APP_BASE + len(data), 0x080D19F0)


class WaitForDfu(unittest.TestCase):
    def test_no_bootloader_raises_the_hint_not_a_format_error(self):
        with mock.patch.object(dfumod, "find", return_value=[]):
            with self.assertRaises(cli.UsageError) as cm:
                cli.wait_for_dfu(None, boards.get("md380"), 0)
        self.assertIn("no radio bootloader found (USB 0483:DF11)", str(cm.exception))


class NormalMode(unittest.TestCase):
    """The radio's own firmware switched on normally is a DFU device with the bootloader's IDs and strings; writing
    through it can erase the bootloader. Both DFU tools refuse it before any DFU request."""
    NORMAL = ["@Internal Flash   /0x08000000/03*016Kg", "@SPI Flash Memory /0x00000000/16*064Kg"]
    LOADER = ["@Internal Flash   /0x0800C000/01*016Kg,01*064Kg,07*128Kg", "@SPI Flash Memory /0x00000000/16*064Kg"]

    def device(self, interfaces):
        board = boards.get("dm1701")
        dev = fakedfu.FakeUsbDevice(board, b"\0" * dfumod.KEY_LEN)
        dev.strings = dict(dev.strings, interfaces=interfaces)
        dev.ctrl_transfer = mock.Mock(side_effect=AssertionError("a DFU request reached a radio in normal mode"))
        return board, dev

    def test_rule(self):
        self.assertTrue(dfumod.normal_mode(self.NORMAL))
        self.assertFalse(dfumod.normal_mode(self.LOADER))
        self.assertFalse(dfumod.normal_mode([None]))			# no layout string: nothing to go by
        # a bootloader that showed the whole flash, not only the application area, is no normal mode
        self.assertFalse(dfumod.normal_mode(["@Internal Flash  /0x08000000/03*016Kg,01*016Kg,01*064Kg,07*128Kg"]))
        self.assertEqual(dfumod.region("@SPI Flash Memory /0x00000000/16*064Kg"), (0, 0x100000))
        declared = mock.Mock(dfu_layout="@Internal Flash /0x0800C000/01*016Kg,01*064Kg,07*128Kg")
        self.assertFalse(dfumod.normal_mode(self.LOADER, declared))	# white space doesn't count
        self.assertTrue(dfumod.normal_mode(["@Internal Flash /0x0800C000/01*016Kg"], declared))
        for b in ("md380", "dm1701"):					# their boards declare the layout read on a radio
            self.assertFalse(dfumod.normal_mode(self.LOADER, boards.get(b)))
            self.assertTrue(dfumod.normal_mode(self.NORMAL, boards.get(b)))

    def test_rfoundry_refuses_without_a_dfu_request(self):
        board, dev = self.device(self.NORMAL)
        with self.assertRaises(cli.UsageError) as cm:
            cli.check_bootloader(fakedfu.FakeUsb([dev]), dev, board, log=lambda s: None)
        self.assertIn("switched on normally", str(cm.exception))
        dev.ctrl_transfer.assert_not_called()
        board, dev = self.device(self.LOADER)
        cli.check_bootloader(fakedfu.FakeUsb([dev]), dev, board, log=lambda s: None)

    def test_stm32_dfu_refuses_without_a_dfu_request(self):
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import stm32_dfu
        _, dev = self.device(self.NORMAL)
        for force in (False, True):				# --force is for string 1, not this
            with self.assertRaises(stm32_dfu.DfuError) as cm:
                stm32_dfu.open_device(fakedfu.FakeUsb([dev]), force, "dm1701")
            self.assertIn("switched on normally", str(cm.exception))
        dev.ctrl_transfer.assert_not_called()


class OldRouteGuard(unittest.TestCase):
    def test_stm32_dfu_refuses_a_release_image_with_two_ranges(self):
        """A release from before the codec moved holds the application and its sector 9 data back to back; written as
        one image the data would land over the application. The manifest next to it says so."""
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import stm32_dfu
        with tempfile.TemporaryDirectory() as d:
            bp, _, _ = SingleImage().old_release(d)
            err = io.StringIO()
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                rc = stm32_dfu.main([bp, "--model", "dm1701", "--dry-run"])
            self.assertEqual(rc, 2)
            self.assertIn("2 flash ranges", err.getvalue())
            os.makedirs(os.path.join(d, "x"))
            out1 = SingleImage().run_release(os.path.join(d, "x"))
            err = io.StringIO()
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                rc = stm32_dfu.main([os.path.join(out1, "rfoundry-dm1701-v9.9.9.bin"), "--model", "dm1701", "--dry-run"])
            self.assertEqual(rc, 0, err.getvalue())					# one range: as before


# The MD-9600's stock builds (board.toml [codec] stock), the first two being 06.09 CSV and 16.45 CSV, their tables at
# different addresses; the MD-9600's own placement of these files (the same files are DM-1701 codec sources:
# stockfw.profile_key)
MD9600_STOCK = tuple(k for k, p in stockfw.PROFILES.items() if p["board"] == "md9600")
# The files with an MD-9600 placement and no DM-1701 one yet (the DM-1701's firmware profile list doesn't take them)
MD9600_ONLY = {"MD9600-CSV-V06.03.bin", "MD9600-CSV-GPS-P06.03.bin", "MD9600-REC-D06.03.bin", "MD9600-GPS-REC-S06.03.bin",
               "MD9600-REC-D03.28.bin", "MD9600-REC-D03.29.bin", "MD9600-REC-D04.03.bin",
               "RT90-CSV(2571V5)-V26.41.bin", "RT90-CSV-GPS(2571V5)-P26.41.bin", "RT90-REC(2571V5)-D26.38.bin",
               "RT90-GPS-REC(2571V5)-S26.42.bin", "MD9600-AES-GPS-REC(2571V5)-S126.61.bin"}


class StockLibrary(unittest.TestCase):
    def test_extract_and_merge(self):
        stock, profiles, code, tables = synthetic_stock()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual(segs, placed()[1])
        self.assertEqual([(a, len(d)) for a, d in segs], [(0x080E3A74, len(code)), (0x080D19F0, 0x12082),
                                                         (stockfw.DESC_ADDR, 0x80)])
        img = app_image(5000)
        merged = stockfw.merge(img, prof, segs)
        self.assertEqual(len(merged), 0x08100000 - 0x0800C000)
        self.assertEqual(merged[:len(img)], img)
        for a, d in segs:
            self.assertEqual(merged[a - 0x0800C000:a - 0x0800C000 + len(d)], d)
        self.assertEqual(merged[0x08074714 - 0x0800C000:0x080D19F0 - 0x0800C000].count(0xFF),
                         0x080D19F0 - 0x08074714)				# nothing at the stock code address

    def test_place_rewrites_and_checks(self):
        """The move rewrites the self reference and the island's call, and nothing else; a wrong map or profile stops"""
        _, profiles, code, tables = synthetic_stock()
        prof = list(profiles.values())[0]
        (ca, mc), (ta, mt), _ = placed()[1]
        delta = ca - prof["code"][0]
        word = lambda b, o: struct.unpack_from("<I", b, o)[0]
        self.assertEqual(word(mc, stockfw.SYN_SELF_REF), word(code, stockfw.SYN_SELF_REF) + delta)
        self.assertEqual(codecmap.bl_target(mt, stockfw.SYN_BL, ta + stockfw.SYN_BL),
                         codecmap.bl_target(tables, stockfw.SYN_BL, ta + stockfw.SYN_BL) + delta)
        diff = [i for i in range(len(mc)) if mc[i] != code[i]]
        self.assertTrue(diff and all(stockfw.SYN_SELF_REF <= i < stockfw.SYN_SELF_REF + 4 for i in diff))
        diff = [i for i in range(len(mt)) if mt[i] != tables[i]]
        self.assertTrue(diff and all(stockfw.SYN_BL <= i < stockfw.SYN_BL + 4 for i in diff))
        segs = [(prof["code"][0], code), (prof["tables"][0], tables)]
        for bad in (dict(prof, map=None, code=prof["code"][:2] + ("0" * 64,)),			# no map for this code
                    dict(prof, placed=dict(prof["placed"], code=(0, "0" * 64))),		# placed hash wrong
                    dict(prof, placement=dict(prof["placement"], code=prof["placement"]["code"] + 2))):	# alignment
            with self.assertRaises(stockfw.StockError):
                stockfw.place(bad, segs)

    def test_refusals(self):
        stock, profiles, _, _ = synthetic_stock()
        with self.assertRaises(stockfw.StockError):
            stockfw.extract(stock, bytes([KEY[0] ^ 1]) + KEY[1:], profiles)
        with self.assertRaises(stockfw.StockError):
            stockfw.extract(stock, KEY)				# not the real file
        # another radio's file is named as such before any decryption, never blamed on the key
        other = next(b for b in ("dm1701", "md380", "mduv380") if b != list(profiles.values())[0]["board"])
        with self.assertRaisesRegex(stockfw.StockError, "firmware file for the .*, not the " + other):
            stockfw.extract(stock, bytes(len(KEY)), profiles, board=other)
        with self.assertRaises(stockfw.StockError):
            stockfw.parse_container(b"x" * 300)
        with self.assertRaises(stockfw.StockError):
            stockfw.merge(stock, list(profiles.values())[0], [(0x08074714, b"")])

    def test_split_merged(self):
        stock, profiles, code, tables = synthetic_stock()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        img = app_image(5000)
        app, got, csegs = stockfw.split_merged(stockfw.merge(img, prof, segs), profiles)
        self.assertEqual((app, got["codec_profile"], csegs), (img, prof["codec_profile"], segs))
        self.assertIsNone(stockfw.split_merged(img, profiles))
        self.assertIsNone(stockfw.split_merged(app_image(0x80000), profiles))	# past the codec start, no codec there
        bad = bytearray(stockfw.merge(img, prof, segs))
        bad[0x080E3A74 - 0x0800C000 + 100] ^= 1
        with self.assertRaises(stockfw.StockError):
            stockfw.split_merged(bytes(bad), profiles)				# a codec, but not this profile's
        extra = bytearray(stockfw.merge(img, prof, segs))
        extra[0x080F8000 - 0x0800C000] = 0
        with self.assertRaises(stockfw.StockError):
            stockfw.split_merged(bytes(extra), profiles)			# data between the windows

    def test_derive_key(self):
        stock, _, _, _ = synthetic_stock()
        k = stockfw.derive_key(stock, hashlib.sha256(KEY).hexdigest())
        self.assertEqual(k, KEY)
        with self.assertRaises(stockfw.StockError):
            stockfw.derive_key(stock, "0" * 64)

    def test_derive_key_corrects_wrong_columns(self):
        """Columns where the most common plain byte isn't zero are fixed by the hash-checked search."""
        stock, _, _, _ = synthetic_stock()
        s = bytearray(stock)
        segs = stockfw.parse_container(stock)
        total = sum(n for _, n in segs)
        rng = random.Random(5)
        # columns 77 and 300: just over half of their plain zero bytes become 0x5A, so 0x5A narrowly wins the vote
        for col in (77, 300):
            for i in range(col, total, 1024):
                if s[stockfw.HEADER_LEN + i] == KEY[col] and rng.random() < 0.53:
                    s[stockfw.HEADER_LEN + i] = 0x5A ^ KEY[col]
        cols = collections.Counter(s[stockfw.HEADER_LEN + 77:stockfw.HEADER_LEN + total:1024])
        self.assertEqual(cols.most_common(1)[0][0], 0x5A ^ KEY[77])	# the plain vote alone is wrong
        k = stockfw.derive_key(bytes(s), hashlib.sha256(KEY).hexdigest())
        self.assertEqual(k, KEY)

    def test_derive_key_ff_padding(self):
        """A file mostly 0xFF (erased flash padding, as Retevis's RT82 EXXT_V2) votes key ^ 0xFF: the flipped vote."""
        stock, _, _, _ = synthetic_stock()
        s = bytearray(stock)
        total = sum(n for _, n in stockfw.parse_container(stock))
        for i in range(total * 3 // 5):
            s[stockfw.HEADER_LEN + i] = 0xFF ^ KEY[i % 1024]
        k = stockfw.derive_key(bytes(s), hashlib.sha256(KEY).hexdigest())
        self.assertEqual(k, KEY)

    def test_single_segment_container(self):
        """The MD-380 files: count word 0xFFFFFFFF, one segment in the first table entry."""
        hdr = bytearray(stockfw.HEADER_LEN)
        hdr[:len(stockfw.MAGIC)] = stockfw.MAGIC
        struct.pack_into("<I", hdr, 0x7C, 0xFFFFFFFF)
        struct.pack_into("<II", hdr, 0x80, 0x0800C000, 0x400)
        data = bytes(hdr) + bytes(0x400) + bytes(0x100)
        self.assertEqual(stockfw.parse_container(data), [(0x0800C000, 0x400)])
        with self.assertRaises(stockfw.StockError):
            stockfw.parse_container(data[:0x300])		# the segment runs past the end

    def test_md380_board(self):
        b = boards.get("md380")
        self.assertTrue(b.releasable)
        self.assertTrue(b.keep_codec_supported)	# the application has sectors 3-10, the codec 11
        self.assertEqual(b.codec.id, "md380-d2032-s11v2")
        self.assertEqual(b.codec_former, ["md380-d2032-s11"])	# sectors 8-11, before the application took 8-10
        # every build's codec is all in sector 11: an update keeps it
        self.assertEqual([s.index for s in b.codec_sectors()], [11])
        ld = open(os.path.join(HERE, "..", "..", "..", "boards", "md380", "memory.ld")).read()
        self.assertIn("ORIGIN = __app_start, LENGTH = 0x080E0000 - __app_start", ld)
        self.assertEqual(b.app_region, (0x0800C000, 0x080E0000))
        self.assertIn((stockfw.DESC_ADDR, stockfw.DESC_LEN), b.codec.windows)
        # one entry per stock file, each at the start of sector 11 (its stock address modulo 16)
        self.assertEqual(b.codec_codes[:3], [0x080E0004, 0x080E0000, 0x080E0004])
        self.assertEqual(b.codec_codes, [stockfw.placed_windows(stockfw.PROFILES[h])[0][0] for h in b.codec_stock])
        for h in b.codec_stock:
            prof = stockfw.PROFILES[h]
            for a, n, _ in stockfw.placed_windows(prof):
                self.assertTrue(0x080E0000 <= a and a + n <= stockfw.DESC_ADDR, prof["name"])
                self.assertEqual(a % 16, stockfw.windows(prof)[[w[0] for w in stockfw.placed_windows(prof)].index(a)][0]
                                 % 16, prof["name"])
        self.assertIs(boards.from_debuglink_name("MD-380 VHF"), b)

    def test_md380_profile(self):
        p = stockfw.PROFILES["e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43"]
        self.assertEqual((p["board"], p["key_sha256"]), ("md380", stockfw.MD380_KEY_SHA256))
        self.assertEqual(p["segments"], [(0x0800C000, 0xF2C00)])
        self.assertEqual([(a, n) for a, n, _ in stockfw.windows(p)],
                         [(0x08050C44, 0x10884), (0x0808FC64, 0xAFF8), (0x080D0FC8, 0x24), (0x080F9698, 0x18),
                          (0x080F9E74, 0x14), (0x080FD490, 0x20)])
        self.assertEqual(stockfw.profile_for_board("md380"),
                         "e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43")
        self.assertEqual(stockfw.identify(b"x"), None)

    def test_md380_extract_merge_and_plan(self):
        """A synthetic file with the D002.032 layout: the code moved to sector 11, the five table windows at their stock
        addresses, the descriptor; the merge plan writes sectors 3-7 and the codec sectors, a keep plan only 3-7."""
        b = boards.get("md380")
        stock, profiles, code, tables = stockfw.build_synthetic_stock(KEY, random.Random(380),
                                                                      stockfw.profile_for_board("md380"))
        self.assertEqual(stockfw.parse_container(stock), [(0x0800C000, 0xF2C00)])
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual([a for a, _ in segs], [0x080E0004, 0x080F0894, 0x080FB898, 0x080FB8C8, 0x080FB8E4, 0x080FB900,
                                                stockfw.DESC_ADDR])
        self.assertEqual(len(segs[0][1]), len(code))
        self.assertNotEqual(segs[0][1], code)		# the references moved with the windows
        img = app_image(0x080E0000 - 0x0800C000)
        merged = stockfw.merge(img, prof, segs)
        self.assertEqual(merged[:len(img)], img)
        for a, d in segs:
            self.assertEqual(merged[a - 0x0800C000:a - 0x0800C000 + len(d)], d)
        with self.assertRaises(stockfw.StockError):
            stockfw.merge(img + b"\0" * 5, prof, segs)
        p = plan.make_plan(b, [(b.app_base, img)], plan.MERGE, segs, b.merge_windows(prof))
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6, 7, 8, 9, 10, 11])
        p = plan.make_plan(b, [(b.app_base, img)], plan.KEEP)
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6, 7, 8, 9, 10])

    def test_md380_3514_merge(self):
        """An MD-380 D35.14 file: the UV380 family's codec with three table windows, all moved into sector 11 after the
        code; the same application fits."""
        b = boards.get("md380")
        d3514 = "ff6c3d79b205391c9c2096f8e4367c514c1e1e4dac7d421ed69077e5b42eaf2f"
        self.assertEqual(stockfw.PROFILES[d3514]["codec_profile"], b.codec.id)
        stock, profiles, _, _ = stockfw.build_synthetic_stock(KEY, random.Random(35), d3514)
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual([a for a, _ in segs], [0x080E0000, 0x080F0368, 0x080FD0D8, 0x080FD954, stockfw.DESC_ADDR])
        self.assertEqual(struct.unpack_from("<I", segs[-1][1], 24)[0], 3)	# table windows in the descriptor
        img = app_image(0x080E0000 - 0x0800C000)
        merged = stockfw.merge(img, prof, segs)
        self.assertEqual(stockfw.split_merged(merged, profiles)[1], prof)
        p = plan.make_plan(b, [(b.app_base, img)], plan.MERGE, segs, b.merge_windows(prof))
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6, 7, 8, 9, 10, 11])
        with self.assertRaises(plan.PlanError):		# the application can't reach the codec sector
            plan.make_plan(b, [(b.app_base, app_image(0x080E0000 - 0x0800C000 + 4))], plan.KEEP)

    def test_md9600_board(self):
        b = boards.get("md9600")
        self.assertTrue(b.releasable)
        self.assertTrue(b.keep_codec_supported)		# the application has sectors 3-9, the codec 10-11
        self.assertEqual(b.codec.id, "md9600-s11v2")
        self.assertEqual(b.codec_former, ["md9600-s11"])		# the tables in sector 9, before the application took it
        self.assertEqual([s.index for s in b.codec_sectors()], [10, 11])
        self.assertEqual(tuple(b.app_region), (0x0800C000, 0x080C0000))	# memory.ld: sectors 3-9
        self.assertEqual(tuple(b.codec_stock), MD9600_STOCK)		# board.toml lists every profile, the first two first
        # The board's windows are the union of every build's: every window of each file's layout lies inside one of
        # them, and an image reaching any table range is refused
        for sha in MD9600_STOCK:
            for a, n, _ in stockfw.layout(stockfw.PROFILES[sha]):
                self.assertTrue(any(lo <= a and a + n <= lo + ln for lo, ln in b.codec.windows),
                                "0x%08X+0x%X of %s outside the board's codec windows" % (a, n, sha[:8]))
        self.assertEqual(sorted(b.codec.sources), sorted(stockfw.PROFILES[h]["name"] for h in MD9600_STOCK))
        self.assertIs(boards.from_debuglink_name("MD-9600"), b)
        self.assertEqual(b.key_sha256, stockfw.MD9600_KEY_SHA256)

    def test_md9600_profiles(self):
        mine = [p for p in stockfw.PROFILES.values() if p["board"] == "md9600"]
        self.assertEqual([p["version"] for p in mine][:2], ["V06.09 CSV", "V16.45 CSV"])
        self.assertEqual(len(mine), 21 + len(MD9600_ONLY))
        self.assertEqual(len(mine), len({p["placement"]["tag"] for p in mine}), "one tag per build")
        self.assertEqual(len(mine), len({p["name"] for p in mine}))
        for h, p in stockfw.PROFILES.items():
            if p["board"] != "md9600":
                continue
            sha = stockfw.key_sha(h)
            dm = stockfw.PROFILES.get(sha)
            self.assertEqual((p["key_sha256"], p["codec_profile"]), (stockfw.MD9600_KEY_SHA256, "md9600-s11v2"))
            self.assertEqual([f["codec_profile"] for f in p["former"]], ["md9600-s11"])
            self.assertEqual(p["segments"][0], (0x00060000, 0x59800))
            if dm is None:		# the 03.xx to 06.03 builds are the MD-9600's own sources only so far
                self.assertIn(p["name"], MD9600_ONLY)
                continue
            # the same file as the DM-1701's codec source: the same windows, only placed differently
            self.assertEqual((p["name"], p["size"], p["segments"], p["code"], p["tables"]),
                             (dm["name"], dm["size"], dm["segments"], dm["code"], dm["tables"]))
            self.assertEqual(dm["file_radio"], "MD-9600")
            self.assertEqual("MD-9600 " + p["version"], dm["version"])
            # the code window is a codec the firmware calls at fixed offsets; the tables move to sector 10 as one block
            # (their stock address modulo 16), the code to sector 11, the descriptor is above the code
            self.assertTrue(0x08074000 <= p["code"][0] and p["code"][1] % 2 == 0 and 0xF1E0 <= p["code"][1] <= 0x10300)
            self.assertLessEqual(p["tables"][0] + p["tables"][1], 0x080E0000)
            self.assertGreaterEqual(p["tables"][0], 0x080A0000)
            self.assertLessEqual(0x080E0000 + p["code"][1], stockfw.DESC_ADDR)
            self.assertEqual(p["placement"], {"code": 0x080E0000, "tables": 0x080C0000 + p["tables"][0] % 16,
                                              "tag": p["placement"]["tag"]})
            self.assertLessEqual(p["placement"]["tables"] + p["tables"][1], 0x080E0000)
            self.assertEqual(p["former"][0]["placement"], {"code": 0x080E0000, "tag": p["placement"]["tag"]})
            # the codec map: found by the code's hash, with the entry offsets vocoder_md9600.c calls
            maps = codecmap.by_code_sha(codecmap.load_maps(), p["code"][2])
            self.assertEqual(len(maps), 1, p["name"])
            self.assertEqual(maps[0]["board"], "md9600")
            self.assertEqual({k: v for k, v in maps[0]["entries"].items() if k != "fec_decode"},
                             dict(encode=0x130, encode_ecc=0x4E8, decode=0x5D8, enc_init=0x284, fec_init=0x4CC,
                                  dec_init=0x6D0, enc_params=0x848, enc_set=0x1C0, dec_set=0x688))
            self.assertEqual(maps[0]["code"]["len"], p["code"][1])
        self.assertEqual(stockfw.profile_for_board("md9600"), MD9600_STOCK[0])
        # the same file on the DM-1701 is a codec source with the DM-1701's placement; the board picks the profile
        sha = stockfw.key_sha(MD9600_STOCK[0])
        stock, profiles, _, _ = stockfw.build_synthetic_stock(KEY, random.Random(9600), MD9600_STOCK[0])
        self.assertEqual(stockfw.find_profile(stock, profiles, "md9600")["board"], "md9600")
        self.assertEqual(stockfw.PROFILES[sha]["board"], "dm1701")
        self.assertEqual(stockfw.identify(bytes.fromhex("00")), None)
        self.assertIn("S16.45 GPS-REC", stockfw.KNOWN_FILES["3d71b4358267fe5fdae8980f340b6a158e7b229c9d32c8ab894c4985964e8aff"])
        # the firmware's profiles pin the same placed CRCs, windows and tags (boards/md9600/vocoder_md9600.c)
        with open(os.path.join(HERE, "..", "..", "..", "boards", "md9600", "vocoder_md9600.c")) as f:
            src = f.read().lower()
        self.assertEqual(src.count(".tag = "), len(mine))
        self.assertIn("{ profiles, %d }" % len(mine), src)
        for p in mine:
            self.assertIn("0x%08xu" % p["placed"]["code"][0], src)
            self.assertIn("0x%08xu" % p["placed"]["tables"][0], src)
            self.assertIn("{ 0x%08xu, 0x%xu, 0x%08xu }" % (p["placement"]["tables"], p["tables"][1], p["placed"]["tables"][0]), src)
            self.assertIn(".tag = 0x%04xu" % p["placement"]["tag"], src)
            self.assertIn(".code_len = 0x%xu" % p["code"][1], src)

    def test_md9600_extract_merge_and_plan(self):
        """A synthetic file with each MD-9600 layout: the code moved to sector 11, the tables to sector 10, the
        descriptor; the merge plan writes sectors 3-7 and 10-11, a keep plan only 3-7; an application filling 3-9
        fits."""
        b = boards.get("md9600")
        for sha in MD9600_STOCK:
            tab_at = stockfw.PROFILES[sha]["placement"]["tables"]
            stock, profiles, code, tables = stockfw.build_synthetic_stock(KEY, random.Random(9600), sha)
            prof, segs = stockfw.extract(stock, KEY, profiles)
            self.assertEqual([a for a, _ in segs], [0x080E0000, tab_at, stockfw.DESC_ADDR])
            self.assertNotEqual(segs[0][1], code)		# the self reference moved with the code
            img = app_image(0x08080000 - 0x0800C000)
            merged = stockfw.merge(img, prof, segs)
            for a, d in segs:
                self.assertEqual(merged[a - 0x0800C000:a - 0x0800C000 + len(d)], d)
            p = plan.make_plan(b, [(b.app_base, img)], plan.MERGE, segs, b.merge_windows(prof))
            self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6, 7, 10, 11], stockfw.PROFILES[sha]["name"])
            p = plan.make_plan(b, [(b.app_base, img)], plan.KEEP)
            self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6, 7])
            full = app_image(0x080C0000 - 0x0800C000)
            self.assertEqual([s.index for s in plan.make_plan(b, [(b.app_base, full)], plan.KEEP).erase],
                             [3, 4, 5, 6, 7, 8, 9])
            with self.assertRaises(plan.PlanError):		# the application can't reach the codec sectors
                plan.make_plan(b, [(b.app_base, app_image(0x080C0000 - 0x0800C000 + 4))], plan.KEEP)

    def test_derive_key_refines_a_bad_vote(self):
        """Plain text like Thumb code (a Markov chain over a vocabulary of halfwords) where 0x00 is common but not
        dominant, as in the MD-380 image: the vote gets about a third of the columns wrong, the pair model fixes them."""
        rng = random.Random(1)
        vocab = [rng.randrange(1, 65536) for _ in range(48)] + [0]
        weights = [1.0 / (i + 1) for i in range(48)] + [1.0]
        n = 0xF2C00
        nxt = {w: rng.choices(vocab, weights, k=3) for w in vocab}
        pool = rng.choices(vocab, weights, k=n // 2)
        hw = []
        cur = 0
        for i in range(n // 2):
            cur = nxt[cur][i % 3] if rng.random() < 0.6 else pool[i]
            hw.append(cur)
        plain = struct.pack("<%dH" % (n // 2), *hw)
        hdr = bytearray(stockfw.HEADER_LEN)
        hdr[:len(stockfw.MAGIC)] = stockfw.MAGIC
        struct.pack_into("<I", hdr, 0x7C, 0xFFFFFFFF)
        struct.pack_into("<II", hdr, 0x80, 0x0800C000, n)
        stream = (KEY * (n // 1024 + 1))[:n]
        stock = bytes(hdr) + bytes(a ^ b for a, b in zip(plain, stream)) + bytes(0x100)
        payload = stock[stockfw.HEADER_LEN:stockfw.HEADER_LEN + n]
        vote = bytes(collections.Counter(payload[j::1024]).most_common(1)[0][0] for j in range(1024))
        self.assertGreater(sum(a != b for a, b in zip(vote, KEY)), 200)	# the plain vote is far off
        self.assertEqual(stockfw.derive_key(stock, hashlib.sha256(KEY).hexdigest()), KEY)

    def test_real_profile_table(self):
        p = stockfw.PROFILES["0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"]
        self.assertEqual(p["codec_profile"], DM1701.codec.id)
        # the board's windows: V02.06's layout, then V02.01 to V02.03's (one layout for the three), 0x40 lower, then
        # V02.06 noCSV's and V02.03 noCSV's, then the MD-9600 files' (code and tables moved)
        md = [w for h in DM1701.codec_stock if stockfw.codec_only(stockfw.PROFILES[h])
              for w in DM1701.merge_windows(stockfw.PROFILES[h])[:2]]
        self.assertEqual(len(md), 42)
        self.assertEqual(DM1701.codec.windows, [(0x080E3A74, 0x1021C), (0x080D19F0, 0x12082), (0x080FFF80, 0x80),
                                                (0x080E3A34, 0x1021C), (0x080D19B0, 0x12082),
                                                (0x080E6828, 0x10200), (0x080D4794, 0x12092),
                                                (0x080E67E8, 0x10200), (0x080D4754, 0x12092)] + md)
        self.assertEqual(DM1701.merge_windows(p), [(0x080E3A74, 0x1021C), (0x080D19F0, 0x12082), (0x080FFF80, 0x80)])
        # the real map covers the real code, and the placement keeps the move whole words
        m = stockfw.code_map(p)
        self.assertEqual(m["profile"], "dm1701-v0206-csv")
        self.assertEqual((p["placement"]["code"] - p["code"][0]) % 4, 0)
        self.assertEqual(p["placement"]["code"], (p["tables"][0] + p["placement"]["tables_len"] + 3) & ~3)
        w = m["table_windows"][0]
        self.assertEqual((w["min_start"], w["min_len"]), (p["tables"][0], p["placement"]["tables_len"]))
        self.assertEqual(codecmap.branches_in(m, p["tables"][0], p["placement"]["tables_len"]), len(m["branches"]))


def flash_readout(stock, key, profiles, placed_image=None):
    """A synthetic stub readout of the internal flash (1 MB from 0x08000000): a radio on its stock firmware (the
    application segment decrypted at its address), or with placed_image (a merged image) at 0x0800C000."""
    out = bytearray(stockfw.random_bytes(random.Random(4), 0xC000)) + b"\xFF" * (0x100000 - 0xC000)
    if placed_image is not None:
        out[0xC000:0xC000 + len(placed_image)] = placed_image
        return bytes(out)
    off = stockfw.HEADER_LEN
    for a, n in stockfw.parse_container(stock):
        if a >= stockfw.FLASH_BASE:
            out[a - stockfw.FLASH_BASE:a - stockfw.FLASH_BASE + n] = stockfw.decrypt(stock, off, n, key)
        off += n
    return bytes(out)


class FirmwareProfiles(unittest.TestCase):
    """Every placed stock profile is one the radio's firmware accepts: boards/<radio>/vocoder_<radio>.c lists its tag,
    code window and table windows with the CRC-32s the merge's descriptor carries, and nothing else."""

    WIN = re.compile(r"\{ (\w+), (\w+), (\w+) \}")

    def firmware(self, board):
        src = open(os.path.join(HERE, "..", "..", "..", "boards", board, "vocoder_%s.c" % board)).read()
        defs = dict(re.findall(r"#define (\w+)\s+(\w+)u\b", src))
        num = lambda v: int(defs.get(v, v).rstrip("u"), 0)	# a number, or a #define of one
        tables = {m.group(1): [tuple(num(x) for x in w) for w in self.WIN.findall(m.group(2))]
                  for m in re.finditer(r"codec_desc_window_t (\w+)\[\w*\] = \{(.*?)\};", src, re.S)}
        structs = {}
        for m in re.finditer(r"stm32_codec_profile_t (\w+) = \{(.*?)\n\};", src, re.S):
            f = dict(re.findall(r"\.(\w+) = (\w+)", m.group(2)))
            wins = tables[f["tables"]]
            self.assertEqual(len(wins), num(f["windows"]))
            structs[m.group(1)] = (num(f["tag"]), num(f["code"]), num(f["code_len"]), num(f["code_crc"]), tuple(wins))
        # only what the profiles[] array lists is accepted: a profile defined but not listed is not
        m = re.search(r"stm32_codec_profile_t \*const profiles\[\] = \{(.*?)\};", src, re.S)
        listed = re.findall(r"&(\w+)", m.group(1))
        self.assertEqual(len(listed), len(set(listed)), board)
        count = re.search(r"stm32_codec_profiles_t stm32_codec_profiles = \{ profiles, (\d+) \}", src)
        if count:
            self.assertEqual(int(count.group(1)), len(listed), board)
        return {structs[n] for n in listed}

    def test_every_profile_in_the_firmware(self):
        for board in ("mduv380", "md380", "dm1701"):
            want = set()
            for prof in stockfw.PROFILES.values():
                if prof["board"] != board:
                    continue
                d = stockfw.descriptor(prof)
                w = struct.unpack_from("<%dI" % (7 + 3 * stockfw.DESC_WINDOWS), d)
                want.add((w[2], w[3], w[4], w[5], tuple(tuple(w[7 + 3 * i:10 + 3 * i]) for i in range(w[6]))))
            self.assertEqual(self.firmware(board), want, board)


class Dm1701Builds(unittest.TestCase):
    """The DM-1701's stock files: V02.06 CSV, and V02.01, V02.02 and V02.03 CSV, which hold one codec 0x40 below
    V02.06's (codec-dm1701.md "Firmware files"): one map and one placement for the three. V02.06 and V02.03 noCSV hold
    it higher up, its code 0x1C bytes shorter: a map and a placement each."""

    V0206 = "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"
    V0201 = "cd9ff4bdb7ba1c464299f3e366bfe72f2e948f3505a8fe54ac19047bff4d6713"
    V0202 = "643650e821db3adf3e0b203437fcdd776d6a7754bc01c0359bd30785ff2478f7"
    V0203 = "35a045d7a71164183b164d6ecfdd9e349c757b3539be67a16fecd68ca6f65948"
    V0206N = "4230c8d0aabf15f3f165c29a118a5f8f9b906a2d9b7d215c1d8a7bc0e3978bd9"
    V0203N = "0a23d563fba04a1874c1e4eb4c0c23bf86e1d17da37cd3681f06ea71ad9c800a"

    def test_board(self):
        own = [self.V0206, self.V0201, self.V0202, self.V0203, self.V0206N, self.V0203N]
        self.assertEqual(DM1701.codec_stock[:6], own)
        self.assertEqual(DM1701.codec_stock[6:], list(Md9600Files.FILES))
        self.assertEqual(DM1701.codec.id, "dm1701-v0206-csv-s11")
        self.assertEqual([s.index for s in DM1701.codec_sectors()], [10, 11])
        for h in own:
            p = stockfw.PROFILES[h]
            self.assertRegex(stockfw.KNOWN_FILES[h], r"^DM-1701 V02\.0[1236] (no)?CSV$")
            self.assertEqual(stockfw.KNOWN_FILES[h], "DM-1701 " + p["version"])
            self.assertIn(p["name"], DM1701.codec.sources)
        # every known DM-1701 file is a codec source
        self.assertEqual({h for h, v in stockfw.KNOWN_FILES.items() if v.startswith("DM-1701 ")}, set(own))
        # one tag per layout, and every layout in sectors 10 and 11, the code below the descriptor
        tags = {}
        for h in DM1701.codec_stock:
            p = stockfw.PROFILES[h]
            tags.setdefault(p["placement"]["tag"], set()).add(tuple(DM1701.merge_windows(p)))
            for a, n in DM1701.merge_windows(p):
                self.assertTrue(0x080C0000 <= a and a + n <= 0x08100000, p["name"])
            self.assertLessEqual(p["placement"]["code"] + p["code"][1], stockfw.DESC_ADDR)
        self.assertEqual(sorted(tags), [0x0203, 0x0206, 0x10203, 0x10206] + sorted(Md9600Files.FILES.values()))
        self.assertTrue(all(len(v) == 1 for v in tags.values()), tags)

    def test_readout_named_by_layout(self):
        """A readout can't tell files with the same codec layout apart, so it is named after all of them"""
        for h in (self.V0201, self.V0202, self.V0203):
            self.assertEqual(stockfw.layout_name(stockfw.PROFILES[h]), "V02.01/V02.02/V02.03 CSV")
        for h, v in ((self.V0206, "V02.06 CSV"), (self.V0206N, "V02.06 noCSV"), (self.V0203N, "V02.03 noCSV")):
            self.assertEqual(stockfw.layout_name(stockfw.PROFILES[h]), v)
            self.assertEqual(stockfw.same_layout(stockfw.PROFILES[h]), [stockfw.PROFILES[h]])

    def test_one_map_for_three_files(self):
        """The three older files: one code window (by its hash), the dm1701-v0203-csv map, one placement and tag."""
        maps = codecmap.load_maps()
        profs = [stockfw.PROFILES[h] for h in (self.V0201, self.V0202, self.V0203)]
        for p in profs:
            self.assertEqual({k: p[k] for k in ("code", "tables", "placement", "placed", "segments", "size")},
                             {k: profs[0][k] for k in ("code", "tables", "placement", "placed", "segments", "size")})
            m = codecmap.by_code_sha(maps, p["code"][2])
            self.assertEqual([x["profile"] for x in m], ["dm1701-v0203-csv"], p["name"])
            m = m[0]
            self.assertEqual(m["stock"]["sha256"], self.V0203)
            self.assertEqual((m["code"]["flash"], m["code"]["len"]), p["code"][:2])
            tw = m["table_windows"][0]
            self.assertEqual((tw["flash"], tw["len"], tw["sha256"]), p["tables"])
            self.assertEqual((tw["min_start"], tw["min_len"]), (p["tables"][0], p["placement"]["tables_len"]))
            # the tables cut to the codec's data, the code right after them, word aligned, below the descriptor
            self.assertEqual(p["placement"]["code"], (p["tables"][0] + p["placement"]["tables_len"] + 3) & ~3)
            self.assertLessEqual(p["placement"]["code"] + p["code"][1], stockfw.DESC_ADDR)
            self.assertEqual(codecmap.branches_in(m, p["tables"][0], p["placement"]["tables_len"]), len(m["branches"]))
            self.assertEqual(p["placement"]["tag"], 0x0203)
        self.assertNotEqual(profs[0]["placement"]["tag"], stockfw.PROFILES[self.V0206]["placement"]["tag"])

    def test_map_is_v0206_relinked(self):
        """The V02.03 map is the V02.06 one with every address 0x40 lower: the same entries, the same sites in the code
        and in the islands, every target and value shifted, the same object lengths except where a bound depends on
        the stock code around it. A map that disagrees with the V02.06 one anywhere else is wrong. The same holds for
        the two noCSV maps."""
        maps = codecmap.load_maps()
        self.relinked(maps["dm1701-v0203-csv"], maps["dm1701-v0206-csv"], -0x40)
        self.relinked(maps["dm1701-v0203-nocsv"], maps["dm1701-v0206-nocsv"], -0x40)

    def relinked(self, a, b, d):
        self.assertEqual(a["entries"], b["entries"])
        self.assertEqual((a["code"]["flash"] - b["code"]["flash"], a["code"]["len"], a["code"]["reachable_len"]),
                         (d, b["code"]["len"], b["code"]["reachable_len"]))
        for k in ("self_refs", "island_ptrs", "table_refs", "island_refs", "branches"):
            self.assertEqual(len(a[k]), len(b[k]), k)
            for x, y in zip(a[k], b[k]):
                self.assertEqual(set(x), set(y), k)
                for f in x:
                    if f in ("kind", "off", "off2", "reachable"):
                        self.assertEqual(x[f], y[f], (k, f))
                    else:
                        self.assertEqual(x[f] - y[f], d, (k, f, x))
        self.assertEqual([(i["start"] - d, i["len"]) for i in a["islands"]],
                         [(i["start"], i["len"]) for i in b["islands"]])
        self.assertEqual([o["start"] - d for o in a["objects"]], [o["start"] for o in b["objects"]])
        self.assertEqual([[v - d for v in g] for g in a["groups"]], b["groups"])
        ta, tb = a["table_windows"][0], b["table_windows"][0]
        self.assertEqual((ta["min_start"] - d, ta["min_len"], ta["len"]), (tb["min_start"], tb["min_len"], tb["len"]))
        self.assertEqual(a["outside_refs"], [])

    def test_nocsv_map_matches_csv(self):
        """The noCSV maps against the CSV map of the same version, site for site: the same entries and counts; every
        site in the code at the same offset, 0x1C lower past the 0x1C bytes the noCSV code lacks (stock offset 0x4A00);
        every reference into the tables to the same object at the same offset in it, every island call to the same code
        offset. A map that finds another site or target anywhere is wrong."""
        maps = codecmap.load_maps()
        for n, c in (("dm1701-v0206-nocsv", "dm1701-v0206-csv"), ("dm1701-v0203-nocsv", "dm1701-v0203-csv")):
            a, b = maps[n], maps[c]
            self.assertEqual(a["entries"], b["entries"])
            self.assertEqual((a["code"]["len"], a["code"]["reachable_len"]),
                             (b["code"]["len"] - 0x1C, b["code"]["reachable_len"] - 0x1C))
            self.assertEqual([o["len"] for o in a["objects"][:43]], [o["len"] for o in b["objects"][:43]])

            def norm(m, nocsv):
                code, n_ = m["code"]["flash"], m["code"]["len"]
                def off(o):		# a code offset as the CSV code has it
                    self.assertFalse(nocsv and 0x4A00 <= o < 0x4A1C, o)
                    return o + 0x1C if nocsv and o >= 0x4A00 else o
                def at(v):		# where an address points: code offset, or (object or island, index, offset in it)
                    if code <= v < code + n_:
                        return ("code", off(v - code))
                    for kind in ("objects", "islands"):
                        for i, o in enumerate(m[kind]):
                            if o["start"] <= v < o["start"] + max(o["len"], 1):
                                return (kind, i, v - o["start"])
                    self.fail("0x%08X is in no window of %s" % (v, m["profile"]))
                out = {}
                for k in ("self_refs", "island_ptrs", "table_refs", "island_refs", "branches"):
                    out[k] = [tuple(sorted((f, (off(x[f]) if f in ("off", "off2") else
                                                at(x[f] & ~1 if f == "value" or k == "island_ptrs" else x[f])
                                                if f not in ("kind", "reachable") else x[f])) for f in x))
                              for x in m[k]]
                return out
            na, nb = norm(a, True), norm(b, False)
            for k in na:
                self.assertEqual(na[k], nb[k], (n, k))
            self.assertEqual(a["outside_refs"], [])
            # groups by object index: the same, but for objects 6 and 7, which the CSV builds keep together because the
            # second starts where the first ends; in noCSV 0x7C bytes the codec doesn't use lie between them
            # (docs/spec/codec-dm1701.md, "Codec in the V02.06 CSV file")
            def groups(m):
                index = {o["start"]: i for i, o in enumerate(m["objects"])}
                return sorted(tuple(index[s] for s in g) for g in m["groups"])
            self.assertEqual(groups(b), sorted(groups(a) + [(6, 7)]), n)
            for m, gap in ((a, 0x7C), (b, 0)):
                o6, o7 = m["objects"][6], m["objects"][7]
                self.assertEqual((o6["len"], o7["len"]), (96, 3072), m["profile"])
                self.assertEqual(o7["start"] - (o6["start"] + o6["len"]), gap, m["profile"])

    def test_synthetic_merge(self):
        """A V02.01 file merges, splits and reads back from a readout like V02.06; its descriptor carries tag 0x0203."""
        stock, profiles, _, _ = stockfw.build_synthetic_stock(KEY, random.Random(201), self.V0201)
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual([(a, len(d)) for a, d in segs], [(0x080E3A34, 0x1021C), (0x080D19B0, 0x12082),
                                                         (stockfw.DESC_ADDR, stockfw.DESC_LEN)])
        self.assertEqual(struct.unpack_from("<4I", segs[-1][1], 8), (0x0203, 0x080E3A34, 0x1021C,
                                                                      prof["placed"]["code"][0]))
        img = app_image(0x080C0000 - 0x0800C000)
        merged = stockfw.merge(img, prof, segs)
        self.assertEqual(stockfw.split_merged(merged, profiles)[1], prof)
        self.assertEqual(stockfw.from_flash(flash_readout(stock, KEY, profiles), profiles=profiles), (prof, segs))
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.MERGE, segs, DM1701.merge_windows(prof))
        self.assertEqual([s.index for s in p.erase if s.index >= 10], [10, 11])

    @unittest.skipUnless(os.path.isdir(os.environ.get("RFOUNDRY_DM1701_STOCK_DIR", "")),
                         "set RFOUNDRY_DM1701_STOCK_DIR (a folder with the DM-1701 stock files)")
    def test_real_files(self):
        """Every supported DM-1701 stock file in the folder: the key derived from it, every window decrypted, the codec
        placed by its map and checked against the profile's hashes (extract() raises on any mismatch)."""
        found = set()
        for root, _, files in os.walk(os.environ["RFOUNDRY_DM1701_STOCK_DIR"]):
            for n in files:
                data = open(os.path.join(root, n), "rb").read()
                h = stockfw.sha256(data)
                if h not in DM1701.codec_stock:
                    continue
                key = stockfw.derive_key(data)
                prof, segs = stockfw.extract(data, key, board="dm1701")
                self.assertEqual([a for a, _ in segs], [a for a, _ in DM1701.merge_windows(prof)])
                found.add(h)
        self.assertEqual(found, set(DM1701.codec_stock))


class Rt10Map(unittest.TestCase):
    def test_map_is_db25d_020_relinked(self):
        """The RT10's codec is the DB25-D 020 compile with one function inside the window 0x14 bytes shorter
        (at +0x614): every site and entry past it 0x14 lower, every table target 0x2BFC higher (the tables at 0x1800420C
        instead of 0x18001610), the same objects. Two maps made independently that disagree anywhere else are wrong."""
        maps = codecmap.load_maps()
        a, b = maps["rt10-112"], maps["db25d-020"]
        cut, d, dt = 0x614, -0x14, 0x1800420C - 0x18001610

        def moved(o):
            return o + d if o >= cut else o
        self.assertEqual(a["entries"], {k: moved(v) for k, v in b["entries"].items()})
        self.assertEqual(a["code"]["reachable_len"], b["code"]["reachable_len"] + d)
        self.assertEqual(len(a["self_refs"]), len(b["self_refs"]))
        for x, y in zip(a["self_refs"], b["self_refs"]):
            self.assertEqual((x["off"], x["value"] - a["code"]["run"], x["reachable"]),
                             (moved(y["off"]), moved(y["value"] - b["code"]["run"]), y["reachable"]))
        self.assertEqual([(r["off"], r["target"]) for r in a["table_refs"]],
                         [(moved(r["off"]), r["target"] + dt) for r in b["table_refs"]])
        self.assertEqual([(o["start"], o["len"]) for o in a["objects"]], [(o["start"] + dt, o["len"]) for o in b["objects"]])
        ta, tb = a["table_windows"][0], b["table_windows"][0]
        self.assertEqual((ta["run"], ta["len"], ta["sha256"]), (tb["run"] + dt, tb["len"], tb["sha256"]))
        for k in ("island_ptrs", "island_refs", "branches", "islands", "outside_refs"):
            self.assertEqual(a[k], [], k)


class Md9600Files(unittest.TestCase):
    """The TYT MD-9600 files as codec sources for the DM-1701 (codec-dm1701.md "MD-9600 files"): an open-firmware
    install leaves their codec on the radio. They give the codec only, encrypted with the MD-9600's key; the merge
    moves the codec's table window with the code into sector 10, since its stock address is the application's."""

    FILES = {	# file SHA-256: descriptor tag
        "de368437de05307d3e217f049bb5e30b1a8ec201e50b8e81aa5c506089c53532": 0x96001645,	# MD-9600 V16.45 CSV
        "7f4d760288f0c1875c7c5dc83dcdf3f355f5188becde2b505eedf609fdbe665f": 0x96011645,	# MD-9600 P16.45 CSV-GPS
        "d2ef7a7e9c484dc1a8777488816f4da47181899ff8b7299a0ead7d141d55e10c": 0x96021645,	# MD-9600 D16.45 REC
        "3d71b4358267fe5fdae8980f340b6a158e7b229c9d32c8ab894c4985964e8aff": 0x96031645,	# MD-9600 S16.45 GPS-REC
        "79a561eda2f5843334ea5974f5de511b6e172a1643f9163806d54a94404f0f09": 0x96000609,	# MD-9600 V06.09 CSV
        "9454eb221ae1f206fca5a15ec1b540bf06b3db4991f466b1626a3a3073ca91d5": 0x96010609,	# MD-9600 P06.09 CSV-GPS
        "7b1b1b33e661766a5b0c0b3ddab9c31c96e3855b09f4e280b07f4990b563b68f": 0x96020609,	# MD-9600 D06.09 REC
        "4610ed115e80efa04be554a94915ec2e053ae00f6b64dfa4815451966840dba7": 0x96030609,	# MD-9600 S06.09 GPS-REC
        "194526d60f796cc0f7339d14c1efeedc267731d108fd66a96876b459408de0ba": 0x96000624,	# MD-9600 V06.24 CSV
        "1eafdb774fd71cb7bc3334d06617ed42264ee25df1c73f61744f6b21742bbd20": 0x96010624,	# MD-9600 P06.24 CSV-GPS
        "ec44929e855442ad248f1a5c171a467922d6cb5742fc35f759db269c645e22ab": 0x96030624,	# MD-9600 S06.24 GPS-REC
        "6643261e62b9778a5df43b1e5c3428d6d308bf54b34478513df3e08f6f4dbb63": 0x96020624,	# MD-9600 D06.24 REC
        "98302fbc26fa0db93bd7ecbd29a410ac9fbb42cbd7d6ce15d54ce085fefff329": 0x96001624,	# MD-9600 V16.24 CSV
        "e86b9e9775758548dd2d01334cfacc96c640fbf843aa6c5fa009b4bc7598e98d": 0x96011624,	# MD-9600 P16.24 CSV-GPS
        "4e1c5c081d5bdf4bef8f205668fd7381391187d8b07150df7e99275b030b811b": 0x96031624,	# MD-9600 S16.24 GPS-REC
        "e21c72e30716fb43e4d15798012aec15e964b9ec8c1a534ede5d0b0fe73f3acb": 0x96021624,	# MD-9600 D16.24 REC
        "02e366687f122ed752581344db8316666d21782d9353334518b69237a81e74dc": 0x96012645,	# MD-9600 P26.45 CSV-GPS
        "6df34152e9786530d61032ab71f1962cf194aca0b99cf0a54e3802fd2cd4ac72": 0x96032645,	# MD-9600 S26.45 GPS-REC
        "baee36b0fd852dfc1fb14952242a1231dbfa4fabc37cba57c61f1b8d3e0d830d": 0x96022645,	# MD-9600 D26.45 REC
        "2522008737d7845e43ab47a307d38498ee783c788a377fac855dff94dea53af0": 0x96022657,	# MD-9600 D26.57 REC
        "d8a653307222e576ee416ab6d4704d14758fa71c1b0e826ffd840f26266cf11f": 0x96002645,	# MD-9600 V26.45 CSV
    }

    def test_profiles(self):
        maps = codecmap.load_maps()
        for h, tag in self.FILES.items():
            p = stockfw.PROFILES[h]
            with self.subTest(p["name"]):
                self.assertEqual((p["board"], p["file_radio"], p["codec_profile"]), ("dm1701", "MD-9600", DM1701.codec.id))
                self.assertEqual(p["key_sha256"], stockfw.MD9600_KEY_SHA256)
                self.assertNotEqual(p["key_sha256"], DM1701.key_sha256)
                self.assertIn("not the bootloader key", stockfw.codec_only(p))
                self.assertEqual(stockfw.KNOWN_FILES[h], p["version"])
                self.assertTrue(p["version"].startswith("MD-9600 "))
                m = codecmap.by_code_sha(maps, p["code"][2])
                self.assertEqual(len(m), 1)
                m = m[0]
                self.assertEqual((m["board"], m["stock"]["sha256"]), ("md9600", h))
                self.assertEqual((m["code"]["flash"], m["code"]["len"]), p["code"][:2])
                # the profile's table window is the map's cut, which holds everything the codec refers to
                tw = m["table_windows"][0]
                self.assertEqual((tw["min_start"], tw["min_len"], tw["min_sha256"]), p["tables"])
                self.assertEqual(codecmap.table_window(m), p["tables"][:2])
                # placed in sectors 10 and 11, the tables from 0x080D0000 at their stock page offset, the code in the
                # page after them at its own, below the descriptor
                pl = p["placement"]
                self.assertEqual(pl["tag"], tag)
                self.assertNotIn("tables_len", pl)
                self.assertEqual(pl["tables"], 0x080D0000 + (p["tables"][0] & 0xFFF))
                self.assertEqual(pl["code"] & 0xFFF, p["code"][0] & 0xFFF)
                self.assertLessEqual(pl["tables"] + p["tables"][1], pl["code"] - 0x1000 + 0x1000)
                self.assertLess(pl["code"] - (pl["tables"] + p["tables"][1]), 0x2000)
                self.assertLessEqual(pl["code"] + p["code"][1], stockfw.DESC_ADDR)
                for a, n in DM1701.merge_windows(p):
                    self.assertTrue(0x080C0000 <= a and a + n <= 0x08100000)
                self.assertEqual(stockfw.placed_windows(p)[1][:2], (pl["tables"], p["tables"][1]))

    def synthetic(self):
        """A synthetic codec-only profile and its file: the shared Thumb vector's code and table window (codecmap_vectors
        "window") in a container encrypted with a key other than the radio's"""
        with open(os.path.join(HERE, "codecmap_vectors.json")) as f:
            w = json.load(f)["window"]
        m = codecmap.normalize(w["map"])
        code, tables = bytes.fromhex(w["code"]), bytes.fromhex(w["tables"])
        lo, n = codecmap.table_window(m)
        other = bytes(random.Random(9600).randrange(256) for _ in range(1024))
        app = bytearray(0x80000)
        app[m["code"]["run"] - 0x08060000:m["code"]["run"] - 0x08060000 + len(code)] = code
        app[lo - 0x08060000:lo - 0x08060000 + n] = tables
        hdr = bytearray(stockfw.HEADER_LEN)
        hdr[:len(stockfw.MAGIC)] = stockfw.MAGIC
        struct.pack_into("<III", hdr, 0x7C, 1, 0x08060000, len(app))
        stock = bytes(hdr) + bytes(b ^ other[i % 1024] for i, b in enumerate(app)) + bytes(0x100)
        CR, TR = 0x080E2C00, 0x080D0230
        mc, mt = codecmap.move_window(code, tables, m, CR, TR)
        prof = {"name": "synthetic-md9600.bin", "version": "MD-9600 synthetic", "board": "dm1701", "file_radio": "MD-9600",
                "key_sha256": hashlib.sha256(other).hexdigest(), "codec_profile": DM1701.codec.id, "size": len(stock),
                "segments": [(0x08060000, len(app))], "code": (m["code"]["run"], len(code), m["code"]["sha256"]),
                "tables": (lo, n, hashlib.sha256(tables).hexdigest()), "map": m,
                "placement": {"code": CR, "tables": TR, "tag": 0x96FF0000},
                "placed": {"code": (zlib.crc32(mc), hashlib.sha256(mc).hexdigest()),
                           "tables": (zlib.crc32(mt), hashlib.sha256(mt).hexdigest())}}
        return prof, stock, code, tables, mc, mt

    def test_extract_with_the_files_own_key(self):
        prof, stock, _, _, mc, mt = self.synthetic()
        profiles = {hashlib.sha256(stock).hexdigest(): prof}
        got, segs = stockfw.extract(stock, KEY, profiles, board="dm1701")	# the radio's key is not the file's
        self.assertIs(got, prof)
        self.assertEqual(segs[:2], [(0x080E2C00, mc), (0x080D0230, mt)])
        self.assertEqual(segs[2], (stockfw.DESC_ADDR, stockfw.descriptor(prof)))
        d = struct.unpack_from("<10I", segs[2][1])
        self.assertEqual(d[2:10], (0x96FF0000, 0x080E2C00, len(mc), zlib.crc32(mc), 1, 0x080D0230, len(mt), zlib.crc32(mt)))

    def test_found_wherever_another_firmware_put_it(self):
        """stockfw.from_moved: the codec in a flash read, its code anywhere, its tables at their stock address or
        moved, the islands' calls into the code moved or not; placed as a merge from the file places it"""
        prof, stock, code, tables, mc, mt = self.synthetic()
        profiles = {hashlib.sha256(stock).hexdigest(): prof}
        want = stockfw.place(prof, [(prof["code"][0], code), (prof["tables"][0], tables)])
        m, lo = prof["map"], prof["tables"][0]
        for cr, tr, verbatim in ((0x0807537C, lo, False), (0x0807537C, lo, True), (0x0807537C, lo - 0x91C, False),
                                 (prof["code"][0], lo, False), (0x08060004, 0x08098010, False)):
            with self.subTest(code=hex(cr), tables=hex(tr), verbatim=verbatim):
                c2, t2 = codecmap.move_window(code, tables, m, cr, tr)
                flash = bytearray(b"\xff" * 0x100000)
                flash[cr - 0x08000000:cr - 0x08000000 + len(c2)] = c2
                flash[tr - 0x08000000:tr - 0x08000000 + len(t2)] = tables if verbatim else t2
                got = stockfw.from_moved(bytes(flash), profiles=profiles)
                self.assertIsNotNone(got)
                self.assertEqual((got[0], got[1], got[2]), (prof, want, cr))
                flash[cr - 0x08000000 + 0x123] ^= 0x10
                self.assertIsNone(stockfw.from_moved(bytes(flash), profiles=profiles), "a changed byte")
        self.assertIsNone(stockfw.from_moved(b"\xff" * 0x100000, profiles=profiles))

    def test_no_key_from_it(self):
        """The CLI never derives the DM-1701 key from an MD-9600 file, and says why"""
        prof = stockfw.PROFILES[next(iter(self.FILES))]
        self.assertIn("MD-9600", stockfw.codec_only(prof))
        hint = cli.stock_hint(DM1701)
        self.assertIn("for the codec only, one of 21 MD-9600 firmware files", hint)
        self.assertNotIn(prof["name"], hint)
        self.assertNotIn("MD-9600", cli.stock_hint(DM1701, key=True), "the key's hint names only files that give it")


class MdUv380(unittest.TestCase):
    """The MD-UV380 family: TYT and Retevis files from 18.11 to 219.74, one codec, each build's tables at its own
    address."""

    B = boards.get("mduv380")
    V1926 = "75e4d470b451cb5d191a647c0836c71801433b826cc4a7fe2c48d750036bd98e"
    V21960 = "447ed4e09482c45661cd6e4c83d1e9f777463993ab7dd48b0e0597bb14f519ee"	# RT3S, the code before the tables

    def test_board(self):
        b = self.B
        self.assertEqual(b.codec.id, "mduv380-1926-s11")
        self.assertEqual(len(b.codec_stock), 41)
        self.assertEqual(b.codec_stock[0], self.V1926)		# the first: what the simulations use
        self.assertEqual([s.index for s in b.codec_sectors()], [10, 11])
        self.assertEqual(b.app_region, (0x0800C000, 0x080C0000))
        self.assertEqual(b.key_prefix, bytes.fromhex("00AA8989"))
        self.assertEqual(b.key_sha256, stockfw.MDUV380_KEY_SHA256)
        self.assertEqual(b.image_id({"POWER": "10w"}), "mduv380-10w")
        self.assertEqual(b.image_id({}), "mduv380")
        self.assertEqual(b.options_for_name("MD-UV380 10W"), {"POWER": "10w"})
        self.assertIs(boards.from_debuglink_name("MD-UV380"), b)
        self.assertTrue(any("PTT" in s for s in b.bootloader_steps))
        self.assertIn("PTT", cli.bootloader_steps(b))
        hint = cli.stock_hint(b)
        for h in b.codec_stock:
            self.assertIn(stockfw.PROFILES[h]["name"], hint)
            self.assertRegex(stockfw.KNOWN_FILES[h], r"^(MD-UV380|RT3S) ")

    def test_placements(self):
        """Each file's tables at their stock address cut to the codec's data, the code right after them, word aligned,
        or at the start of sector 10 when it doesn't fit there; everything inside sectors 10 and 11 below the
        descriptor; each its own tag; its map found by the code's hash."""
        maps = codecmap.load_maps()
        tags = set()
        for h in self.B.codec_stock:
            prof = stockfw.PROFILES[h]
            pl = prof["placement"]
            tab = prof["tables"][0]
            after = (tab + pl["tables_len"] + 3) & ~3
            if after + prof["code"][1] <= stockfw.DESC_ADDR:
                self.assertEqual(pl["code"], after, prof["name"])
            else:
                self.assertEqual(pl["code"], 0x080C0000, prof["name"])
                self.assertLessEqual(pl["code"] + prof["code"][1], tab, prof["name"])
            self.assertLessEqual(pl["code"] + prof["code"][1], stockfw.DESC_ADDR, prof["name"])
            self.assertGreaterEqual(tab, 0x080C0000)
            tags.add(pl["tag"])
            m = codecmap.by_code_sha(maps, prof["code"][2])
            self.assertEqual(len(m), 1, prof["name"])
            self.assertEqual(m[0]["stock"]["sha256"], h)
            self.assertEqual((m[0]["code"]["flash"], m[0]["code"]["len"]), prof["code"][:2])
            tw = m[0]["table_windows"][0]
            self.assertEqual((tw["flash"], tw["len"], tw["sha256"]), prof["tables"])
            self.assertEqual(tw["min_len"], pl["tables_len"])
            wins = stockfw.layout(prof)
            self.assertEqual(wins[-1][:2], (stockfw.DESC_ADDR, stockfw.DESC_LEN))
        self.assertEqual(len(tags), 41)

    def synthetic(self):
        return stockfw.build_synthetic_stock(KEY, random.Random(19), self.V1926)

    def test_synthetic_merge(self):
        stock, profiles, code, _ = self.synthetic()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual([a for a, _ in segs], [0x080EB300, 0x080D8C94, stockfw.DESC_ADDR])
        self.assertEqual(len(segs[1][1]), 0x1266A)
        merged = stockfw.merge(app_image(0x080C0000 - 0x0800C000), prof, segs)
        self.assertEqual(stockfw.split_merged(merged, profiles)[1], prof)

    def test_code_before_the_tables(self):
        """A build whose code doesn't fit after its tables (RT3S 219.60): the code at the start of sector 10, below
        the tables; merged, split and read back from a readout like any other."""
        stock, profiles, _, _ = stockfw.build_synthetic_stock(KEY, random.Random(20), self.V21960)
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual([a for a, _ in segs], [0x080C0000, 0x080E322C, stockfw.DESC_ADDR])
        self.assertLess(0x080C0000 + len(segs[0][1]), 0x080E322C)
        desc = segs[-1][1]
        self.assertEqual(struct.unpack_from("<4I", desc, 8), (0x80561960, 0x080C0000, len(segs[0][1]),
                                                              prof["placed"]["code"][0]))
        merged = stockfw.merge(app_image(0x080C0000 - 0x0800C000), prof, segs)
        self.assertEqual(stockfw.split_merged(merged, profiles)[1], prof)
        self.assertEqual(stockfw.from_flash(flash_readout(stock, KEY, profiles), profiles=profiles), (prof, segs))
        b = self.B
        p = plan.make_plan(b, [(b.app_base, app_image(5000))], plan.MERGE, segs, b.merge_windows(prof))
        self.assertEqual([s.index for s in p.erase if s.index >= 10], [10, 11])
        with self.assertRaises(stockfw.StockError):	# an image reaching into sector 10 meets the code
            stockfw.merge(app_image(0x080C0000 - 0x0800C000 + 4), prof, segs)

    def test_codec_from_a_flash_readout(self):
        """A stub readout of a radio still on its stock firmware gives the codec as the stock file's merge does; so does
        one of a radio running RadioFoundry with the codec placed. Anything else gives nothing."""
        stock, profiles, _, _ = self.synthetic()
        want = stockfw.extract(stock, KEY, profiles)
        flash = flash_readout(stock, KEY, profiles)
        self.assertEqual(stockfw.from_flash(flash, profiles=profiles), want)
        merged = stockfw.merge(app_image(5000), *want)
        self.assertEqual(stockfw.from_flash(flash_readout(None, None, None, merged), profiles=profiles), want)
        bad = bytearray(flash)
        bad[0x0807B830 - stockfw.FLASH_BASE + 0x400] ^= 1
        self.assertIsNone(stockfw.from_flash(bytes(bad), profiles=profiles))
        self.assertIsNone(stockfw.from_flash(flash[:0x80000], profiles=profiles))
        self.assertIsNone(stockfw.from_flash(flash))			# not the real build
        # the DM-1701 the same way
        dstock, dprofiles, _, _ = synthetic_stock()
        self.assertEqual(stockfw.from_flash(flash_readout(dstock, KEY, dprofiles), profiles=dprofiles),
                         stockfw.extract(dstock, KEY, dprofiles))

    def test_cli_merge_and_flash_from_a_readout(self):
        stock, profiles, _, _ = self.synthetic()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        img = app_image(60000)
        with tempfile.TemporaryDirectory() as d, mock.patch.dict(stockfw.PROFILES, profiles):
            fp, ip, op = (os.path.join(d, n) for n in ("flash.bin", "rfoundry_mduv380.bin", "out.bin"))
            open(fp, "wb").write(flash_readout(stock, KEY, profiles))
            open(ip, "wb").write(img)
            rc, out, err = run_cli(["merge", ip, "--stock", fp, "-o", op])
            self.assertEqual(rc, 0, out + err)
            self.assertEqual(open(op, "rb").read(), stockfw.merge(img, prof, segs))
            rc, out, err = run_cli(["key", "--stock", fp])
            self.assertEqual(rc, 2)
            self.assertIn("never from a flash readout", err)
            rc, out, err = run_cli(["flash", ip, "--board", "mduv380", "--simulate", "rfoundry-nocodec", "--stock", fp,
                                    "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("a flash readout of a radio with V19.26 CSV", out)
            self.assertIn("ERASE + WRITE codec", out)
            self.assertIn("sim      PASS", out)
            # the 10 W release image, its build option in the manifest, onto a radio in its bootloader
            m = dict(mf.generate(self.B, img, "v0.0.1", "e" * 40), build_options={"POWER": "10w"})
            rp = os.path.join(d, "rfoundry-mduv380-10w-v0.0.1.bin")
            open(rp, "wb").write(img)
            open(rp[:-4] + ".json", "w").write(mf.dumps(m))
            rc, out, err = run_cli(["flash", rp, "--simulate", "blank", "--option", "POWER=10w", "--stock", fp, "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("ERASE + WRITE codec", out)
            self.assertIn("sim      PASS", out)
            rc, out, err = run_cli(["flash", rp, "--simulate", "blank", "--option", "POWER=5w", "--stock", fp, "--yes"])
            self.assertEqual(rc, 2, out + err)
            open(fp, "wb").write(b"\xFF" * 0x100000)
            rc, out, err = run_cli(["merge", ip, "--stock", fp, "-o", op, "--board", "mduv380"])
            self.assertEqual(rc, 2)
            self.assertIn("flash readout", err)

    def test_simulated_flash_from_a_stock_file(self):
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "rfoundry_mduv380.bin")
            open(bp, "wb").write(app_image(0x080C0000 - 0x0800C000 - 100))
            rc, out, err = run_cli(["flash", bp, "--board", "mduv380", "--simulate", "rfoundry-nocodec", "--stock",
                                    "synthetic", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("MERGE", out)
            self.assertIn("sim      PASS", out)


class CodecDecision(unittest.TestCase):
    man = {"codec": {"profile": "dm1701-v0206-csv-s11"}}
    NEW, OLD = "dm1701-v0206-csv-s11", "dm1701-v0206-csv"	# the codec where this layout keeps it, where older RadioFoundry did

    def d(self, **kw):
        args = dict(requested="auto", have_stock=False, probe=None, stock_installed=None, ask=None, runs_stock=False)
        args.update(kw)
        return cli.decide_codec(DM1701, self.man, args["requested"], args["have_stock"], args["probe"],
                                args["stock_installed"], args["ask"], runs_stock=args["runs_stock"])

    def probe(self, codec, board="DM-1701"):
        return {"board": board, "codec": codec, "git": "x"}

    def test_probe_with_codec_keeps_even_with_stock(self):
        self.assertEqual(self.d(probe=self.probe(self.NEW)).mode, plan.KEEP)
        self.assertEqual(self.d(probe=self.probe(self.NEW), have_stock=True).mode, plan.KEEP)

    def test_probe_without_codec(self):
        self.assertEqual(self.d(probe=self.probe("absent"), have_stock=True).mode, plan.MERGE)
        with self.assertRaises(cli.UsageError):
            self.d(probe=self.probe("absent"))
        d = self.d(probe=self.probe("absent"), requested="keep")
        self.assertEqual(d.mode, plan.KEEP)
        self.assertFalse(d.expect_codec)
        with self.assertRaises(cli.UsageError):
            self.d(probe=self.probe(None))				# older firmware: unknown, no stock

    def test_codec_in_the_old_place_is_written_again(self):
        """RadioFoundry from before the codec moved runs its codec from sectors 7 and 8, which this application overwrites"""
        self.assertEqual(self.d(probe=self.probe(self.OLD), have_stock=True).mode, plan.MERGE)
        with self.assertRaises(cli.UsageError):
            self.d(probe=self.probe(self.OLD))

    def test_probe_other_board_refused(self):
        with self.assertRaises(cli.UsageError):
            self.d(probe=self.probe(self.NEW, board="DM-32UV"))

    def test_stock_installed(self):
        """No stock firmware holds the codec where RadioFoundry keeps it: the stock file is always needed"""
        self.assertEqual(DM1701.codec.installed, [])
        for v in ("V02.06 CSV", "v02.06-csv", "V02.03 CSV"):
            with self.assertRaises(cli.UsageError):
                self.d(stock_installed=v)
            self.assertEqual(self.d(stock_installed=v, have_stock=True).mode, plan.MERGE)

    def test_keep_on_stock_expects_no_codec(self):
        """Keeping the codec of a radio on stock firmware keeps none here: the result must not be reported as a failure"""
        for kw in (dict(runs_stock=True), dict(stock_installed="V02.06 CSV"), dict(runs_stock=True, have_stock=True)):
            d = self.d(requested="keep", **kw)
            self.assertEqual(d.mode, plan.KEEP, kw)
            self.assertFalse(d.expect_codec, kw)
            self.assertIn("DMR voice", d.risk)
        self.assertTrue(self.d(requested="keep").expect_codec)		# not known to run stock: unverified
        self.assertEqual(self.d(runs_stock=True, have_stock=True).mode, plan.MERGE)
        with self.assertRaises(cli.UsageError):
            self.d(runs_stock=True)

    def test_unknown(self):
        with self.assertRaises(cli.UsageError):
            self.d()
        self.assertEqual(self.d(have_stock=True).mode, plan.MERGE)
        self.assertEqual(self.d(requested="keep").mode, plan.KEEP)
        asked = []
        with self.assertRaises(cli.UsageError):
            self.d(ask=lambda q: asked.append(q) or True)		# nothing a user could say makes it safe
        self.assertEqual(asked, [])

    def test_merge_requested(self):
        self.assertEqual(self.d(requested="merge", have_stock=True, probe=self.probe(self.NEW)).mode,
                         plan.MERGE)
        with self.assertRaises(cli.UsageError):
            self.d(requested="merge")

    def test_dm32_always_merges(self):
        man = {"codec": {"profile": "dm32-049"}}
        self.assertEqual(cli.decide_codec(boards.get("dm32"), man, "auto", True, None, None, None).mode, plan.MERGE)
        with self.assertRaises(cli.UsageError):
            cli.decide_codec(boards.get("dm32"), man, "auto", False, None, None, None)

    def test_dm1801pro_always_merges(self):
        man = {"codec": {"profile": "dm1801pro-049"}}
        self.assertEqual(cli.decide_codec(boards.get("dm1801pro"), man, "auto", True, None, None, None).mode, plan.MERGE)


class StateFile(unittest.TestCase):
    def test_probe_memory(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(state.recent_probe(d))
            state.remember_probe({"board": "DM-1701", "codec": "x"}, d, now=1000.0)
            self.assertEqual(state.recent_probe(d, now=1000.0 + 60)["codec"], "x")
            self.assertIsNone(state.recent_probe(d, now=1000.0 + state.PROBE_MAX_AGE_S + 1))
            state.forget_probe(d)
            self.assertIsNone(state.recent_probe(d, now=1001.0))


class FakeBootloaderEndToEnd(unittest.TestCase):
    def flash_with(self, mode, initial_codec=True):
        rng = random.Random(11)
        codec = [(a, stockfw.random_bytes(rng, n)) for a, n in DM1701.codec.windows]
        flash = fakedfu.stock_flash(DM1701, rng, codec if initial_codec else [])
        dev = fakedfu.FakeAnyRoad(DM1701, KEY, flash)
        before = bytes(dev.flash)
        img = app_image(150000)
        segs, wins = [], None
        if mode == plan.MERGE:
            prof, segs = placed()
            wins = DM1701.merge_windows(prof)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], mode, segs, wins)
        dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None).run(p, KEY)
        return dev, before, img, p, segs

    def test_keep_leaves_codec_and_bootloader(self):
        dev, before, img, p, _ = self.flash_with(plan.KEEP)
        self.assertEqual(dev.vendor, [0x01, 0x31])
        self.assertEqual(dev.erased, [0x0800C000, 0x08010000, 0x08020000])
        self.assertTrue(dev.exited)
        self.assertEqual(dev.violations, [])
        self.assertEqual(dev.read(0x0800C000, len(img)), img)
        for s in DM1701.codec_sectors():				# 10 and 11
            self.assertEqual(dev.read(s.addr, s.size), before[s.addr - 0x08000000:s.end - 0x08000000], s)
        self.assertEqual(dev.read(0x08000000, 0xC000), before[:0xC000])

    def test_merge_writes_codec(self):
        dev, before, img, p, segs = self.flash_with(plan.MERGE, initial_codec=False)
        self.assertEqual(dev.erased, [0x0800C000, 0x08010000, 0x08020000, 0x080C0000, 0x080E0000])
        self.assertEqual(dev.violations, [])
        for a, data in segs:
            self.assertEqual(dev.read(a, len(data)), data)
        self.assertEqual(dev.read(0x080C0000, 0x080D19F0 - 0x080C0000), b"\xFF" * (0x080D19F0 - 0x080C0000))
        self.assertEqual(dev.read(0x08000000, 0xC000), before[:0xC000])

    def test_keyless_write_detected(self):
        """Blocks are XORed with the key: the fake decrypts with its own key, so the wrong key gives other bytes."""
        rng = random.Random(2)
        dev = fakedfu.FakeAnyRoad(DM1701, KEY, fakedfu.stock_flash(DM1701, rng, []))
        p = plan.make_plan(DM1701, [(DM1701.app_base, app_image(3000))], plan.KEEP)
        other = bytes(b ^ 1 for b in KEY)
        dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None).run(p, other)
        self.assertNotEqual(dev.read(0x0800C000, 3000), app_image(3000))

    def test_cli_simulations(self):
        img = app_image(220780)
        m = mf.generate(DM1701, img, "v0.0.1", "e" * 40)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "rfoundry-dm1701-v0.0.1.bin")
            open(bp, "wb").write(img)
            open(os.path.join(d, "rfoundry-dm1701-v0.0.1.json"), "w").write(mf.dumps(m))
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-codec", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("KEEP", out)
            self.assertIn("sim      PASS", out)
            self.assertIn("codec sector 11 unchanged", out)
            rc, out, err = run_cli(["flash", bp, "--simulate", "stock", "--stock-installed", "V02.06 CSV", "--yes"])
            self.assertEqual(rc, 2, out + err)			# stock keeps the codec elsewhere: --stock is needed
            self.assertIn("--stock FILE", err)
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-nocodec", "--stock", "synthetic", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("MERGE", out)
            self.assertIn("sim      PASS", out)
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-nocodec", "--yes"])
            self.assertEqual(rc, 2)
            self.assertIn("no matching codec", err)
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-codec", "--dry-run"])
            self.assertEqual(rc, 0)
            self.assertIn("dry run: nothing was erased or written", out)
            self.assertIn("erase 4 sector(s): 3, 4, 5, 6", out)
            # a manifest for another radio is refused
            m2 = dict(m, board="dm32")
            open(os.path.join(d, "rfoundry-dm1701-v0.0.1.json"), "w").write(mf.dumps(m2))
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-codec", "--yes"])
            self.assertEqual(rc, 2)
            # an official firmware file as the image is refused
            sp = os.path.join(d, "stock.bin")
            open(sp, "wb").write(stockfw.MAGIC + b"\0" * 300)
            rc, out, err = run_cli(["flash", sp, "--board", "dm1701", "--simulate", "rfoundry-codec", "--yes"])
            self.assertEqual(rc, 2)
            self.assertIn("official", err)

    def test_md380_merge_simulation(self):
        """A local MD-380 build filling sectors 3-10: merged from a synthetic D002.032 file."""
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "rfoundry_md380.bin")
            open(bp, "wb").write(app_image(0x080E0000 - 0x0800C000 - 100))
            rc, out, err = run_cli(["flash", bp, "--board", "md380", "--simulate", "rfoundry-nocodec", "--stock", "synthetic",
                                    "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("MERGE", out)
            self.assertIn("sector 11  0x080E0000..0x080FFFFF   128 KB  ERASE + WRITE codec", out)
            self.assertIn("sector 10  0x080C0000..0x080DFFFF   128 KB  ERASE + WRITE", out)
            self.assertIn("sim      PASS", out)

    def test_md9600_merge_simulation(self):
        """A local MD-9600 build filling sectors 3-7: merged from a synthetic 06.09 file."""
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "rfoundry_md9600.bin")
            with open(bp, "wb") as f:
                f.write(app_image(0x08080000 - 0x0800C000 - 100))
            rc, out, err = run_cli(["flash", bp, "--board", "md9600", "--simulate", "rfoundry-nocodec", "--stock", "synthetic",
                                    "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("MERGE", out)
            self.assertIn("sector 11  0x080E0000..0x080FFFFF   128 KB  ERASE + WRITE codec", out)
            self.assertIn("sector 10  0x080C0000..0x080DFFFF   128 KB  ERASE + WRITE codec", out)
            self.assertIn("sector  9  0x080A0000..0x080BFFFF   128 KB  not touched (free)", out)
            self.assertIn("sim      PASS", out)

    def test_merged_image_simulation(self):
        """An image with the codec merged in: no manifest, no --board, no --stock; the codec comes from the image."""
        stock, profiles, _, _ = synthetic_stock()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        with tempfile.TemporaryDirectory() as d, mock.patch.dict(stockfw.PROFILES, profiles):
            bp = os.path.join(d, "rfoundry_dm1701_codec.bin")
            open(bp, "wb").write(stockfw.merge(app_image(40000), prof, segs))
            rc, out, err = run_cli(["flash", bp, "--simulate", "blank", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("codec merged in", out)
            self.assertIn("MERGE: the image carries the codec", out)
            self.assertIn("ERASE + WRITE codec", out)
            self.assertIn("sim      PASS", out)
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-codec", "--codec", "keep", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("codec mode: keep", out)
            self.assertIn("sim      PASS", out)
            rc, out, err = run_cli(["flash", bp, "--board", "md380", "--simulate", "blank", "--yes"])
            self.assertEqual(rc, 2)						# the codec says DM-1701
            rc, out, err = run_cli(["flash", bp, "--simulate", "blank", "--stock", "synthetic", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("only gives the bootloader key", out)		# the codec still comes from the image
            self.assertIn("MERGE: the image carries the codec", out)

    def test_dev_build_without_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "rfoundry_dm1701.bin")
            open(bp, "wb").write(app_image(40000))
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-codec", "--yes"])
            self.assertEqual(rc, 2)						# no manifest, no --board
            rc, out, err = run_cli(["flash", bp, "--board", "dm1701", "--simulate", "rfoundry-codec", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("no manifest", err)


def rom_board():
    """A board installed through its MCU's ROM bootloader by plain DfuSe"""
    return next(b for _, b in sorted(boards.BOARDS.items()) if b.family == "dfuse")


def rom_image(n, seed=1):
    return stockfw.random_bytes(random.Random(seed), n)


class NoCodecBoard(unittest.TestCase):
    """A board without a codec route (board.toml [codec] none): it loads, plans and gets a manifest, with nothing to
    keep or merge."""

    def setUp(self):
        self.b = next((b for _, b in sorted(boards.BOARDS.items()) if b.codec_none), None)
        if self.b is None:
            self.skipTest("every board has a codec route")

    def test_loads_without_a_codec(self):
        b = self.b
        self.assertFalse(b.has_codec)
        self.assertIsInstance(b.codec_none, str)
        self.assertEqual((b.codec.id, b.codec.windows, b.codec.installed, b.codec.sources), (None, [], [], []))
        self.assertEqual((b.codec_tool, b.codec_stock, b.codec_layouts, b.codec_codes, b.codec_former),
                         (None, [], {}, [], []))
        self.assertEqual(b.codec_sectors(), [])
        self.assertEqual(b.merge_windows(), [])
        self.assertFalse(b.keep_codec_supported)
        # a build that reports no codec id reports None, which is this board's (missing) id: not a codec to keep
        for reported in (None, "absent", b.id + "-049"):
            self.assertFalse(b.codec_present(reported))
        for other in boards.BOARDS.values():				# every other board says it has one
            self.assertEqual(other.has_codec, other.codec_none is None)

    def test_plans_and_manifests(self):
        b = self.b
        img = rom_image(150004)
        p = plan.make_plan(b, [(b.app_base, img)], plan.KEEP)
        self.assertEqual(p.kept, [])
        self.assertEqual(p.erase, b.sectors_for(b.app_base, b.app_base + len(img)))
        text = "\n".join(p.describe())
        self.assertIn("board %s, no codec" % b.name, text)
        self.assertNotIn("codec mode", text)
        if not b.protected:
            self.assertNotIn("never erased", text)
        with self.assertRaises(plan.PlanError):			# nothing to merge from
            plan.make_plan(b, [(b.app_base, img)], plan.KEEP, [(b.app_base + 0x40000, b"x" * 16)])
        m = mf.generate(b, img, "v0.0.1", "e" * 40)
        self.assertEqual(m["codec"], {"included": False, "profile": None, "sectors": [], "windows": [],
                                      "keep_supported": False})
        self.assertIs(mf.verify(m, img, b.id), b)
        self.assertIs(mf.verify(mf.for_dev_build(b, img), img), b)

    def test_release_tool(self):
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        b = self.b
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            with open(bp, "wb") as f:
                f.write(rom_image(30000))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = release.main(["--board", b.id, "--version", "v9.9.9", "--bin", bp, "--out", os.path.join(d, "dist"),
                                   "--git", "d" * 40, "--features", "FM", "--toolchain", "test"])
            self.assertEqual(rc, 0)
            self.assertIn("no codec for this radio", out.getvalue())
            m = mf.load(os.path.join(d, "dist", "rfoundry-%s-v9.9.9.json" % b.id))
            self.assertIsNone(m["codec"]["profile"])
            self.assertEqual(m["ranges"], [{"addr": "0x%08X" % b.app_base, "size": 30000}])

    def test_after_the_flash_and_probe_say_nothing_about_a_codec(self):
        b = self.b
        p = {"board": b.debuglink_names[0], "port": "/dev/ttyACM0", "git": "abc", "codec": None}
        out = io.StringIO()
        with mock.patch.object(debuglink, "find_ports", return_value=["/dev/x"]), \
                mock.patch.object(debuglink, "probe", return_value=p), mock.patch.object(cli, "hwopt_prompt"), \
                mock.patch.object(state, "remember_probe"), mock.patch.object(cli.time, "sleep"), \
                contextlib.redirect_stdout(out):
            rc = cli.verify_after(mock.Mock(port=None), b, {}, None)
            cli.cmd_probe(mock.Mock(port=None))
        self.assertEqual(rc, 0)
        self.assertIn("running RadioFoundry git abc", out.getvalue())
        self.assertIn("note     " + b.codec_none, out.getvalue())
        for wrong in ("DMR voice is ready", "--stock", "can be kept"):
            self.assertNotIn(wrong, out.getvalue())


class RomBootloader(unittest.TestCase):
    """The plain DfuSe route to an MCU's ROM bootloader (family dfuse), against fakedfu.FakeRom: no key and no vendor
    command, whole flash words, the read protection probe, a Read Unprotect only when asked for, and never the option
    bytes' alternate setting."""

    def setUp(self):
        self.b = rom_board()
        self.keyed = next(b for _, b in sorted(boards.BOARDS.items()) if b.family == "usb-dfu")

    def dfu(self, dev):
        return dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None)

    def stock(self, **kw):
        dev = fakedfu.FakeRomUsbDevice(self.b, fakedfu.rom_flash(self.b, random.Random(5)), **kw)
        return dev, bytes(dev.flash)

    def image_plan(self, n=150020):
        img = rom_image(n)
        return img, plan.make_plan(self.b, [(self.b.app_base, img)], plan.KEEP, vector_last=True)

    def test_plain_run(self):
        b = self.b
        dev, before = self.stock()
        img, p = self.image_plan()
        self.assertEqual(len(img) % dfumod.FLASH_WORD, 4)		# the image ends on a 4-byte boundary only
        log = []
        dfumod.Dfu(dev, 0, log=log.append, sleep=lambda s: None).run(p)
        secs = b.sectors_for(b.app_base, b.app_base + len(img))
        self.assertEqual(dev.erased, [s.addr for s in secs])		# only the image's sectors
        self.assertEqual(dev.read(b.app_base, len(img)), img)		# as they are: nothing XORed
        end = b.app_base + len(img)
        self.assertEqual(dev.read(end, secs[-1].end - end), b"\xFF" * (secs[-1].end - end))
        off = secs[-1].end - b.rom_base
        self.assertEqual(bytes(dev.flash[off:]), before[off:])		# every other sector as it was
        self.assertEqual((dev.refused, dev.violations, dev.partial, dev.alt_selected), ([], [], [], []))
        self.assertTrue(dev.exited)
        self.assertEqual(dev.addr, b.app_base)				# left at the application base
        self.assertIn("leave: start the image at 0x%08X" % b.app_base, log)
        # no vendor command and nothing but DFU class requests on the one interface
        cmds = [r[3] for r in dev.requests if r[1] == dfumod.DFU_DNLOAD and r[2] == 0 and r[3]]
        self.assertEqual({c[0] for c in cmds}, {dfumod.CMD_ERASE, dfumod.CMD_SET_ADDRESS})
        self.assertEqual({r[0] for r in dev.requests}, {0x21, 0xA1})

    def test_every_block_is_block_2_after_its_own_set_address(self):
        """As the web flasher writes: the bootloader places a block at (block number - 2) x wTransferSize + pointer, and
        AN3156 5.1 leaves open whether that is the length of the block or the DFU functional descriptor's field. With
        block 2 after a Set Address Pointer of the block's own address the product is zero either way"""
        b = self.b
        img, p = self.image_plan()
        dev, _ = self.stock()
        self.dfu(dev).run(p)
        pointer, placed = None, []
        for rtype, req, value, data in dev.requests:
            if req != dfumod.DFU_DNLOAD or not data:
                continue
            if value == 0:
                pointer = struct.unpack("<I", data[1:5])[0] if data[0] == dfumod.CMD_SET_ADDRESS else None
            else:
                self.assertEqual(value, plan.FIRST_BLOCK)
                self.assertIsNotNone(pointer, "a block without a Set Address Pointer of its own")
                placed.append(pointer)
                pointer = None
        want = [st.sector.addr + (n - plan.FIRST_BLOCK) * plan.BLOCK for st in p.write_order for n, _ in st.blocks]
        self.assertEqual(sorted(placed), sorted(want))
        self.assertEqual(len(placed), p.total_blocks)
        # a bootloader that multiplies by another transfer size than the block's length places every block the same
        for size in (2048, 256):
            dev, _ = self.stock(transfer_size=size)
            self.dfu(dev).run(p)
            self.assertEqual(dev.read(b.app_base, len(img)), img, size)
            self.assertEqual((dev.violations, dev.exited), ([], True), size)
        # the fake itself: block 3 lands by the transfer size it was given
        fake = fakedfu.FakeRom(b, transfer_size=2048)
        fake.addr = b.app_base
        fake.ctrl_transfer(0x21, dfumod.DFU_DNLOAD, plan.FIRST_BLOCK + 1, 0, b"\x00" * plan.BLOCK)
        self.assertEqual(fake.read(b.app_base + 2048, 4), b"\x00" * 4)
        self.assertEqual(fake.read(b.app_base + plan.BLOCK, 4), b"\xFF" * 4)

    def test_blocks_are_whole_flash_words(self):
        dev, _ = self.stock()
        img, p = self.image_plan(70020)
        self.dfu(dev).run(p)
        self.assertTrue(dev.sent)
        self.assertEqual({len(d) % dfumod.FLASH_WORD for d in dev.sent}, {0})
        sent = dev.sent[-1:] + dev.sent[:-1]			# the vector table's block was sent last
        self.assertTrue(sent[-1].endswith(b"\xFF" * (dfumod.FLASH_WORD - 4)))
        self.assertEqual(b"".join(sent)[:len(img)], img)
        self.assertEqual(dfumod.flash_words(b"abcde"), b"abcde" + b"\xFF" * 27)
        self.assertEqual(dfumod.flash_words(b"x" * 64), b"x" * 64)
        self.assertEqual(dfumod.flash_words(b""), b"")
        # the fake shows what the fill is for: the last partial word of a short block stays unwritten
        blank = fakedfu.FakeRom(self.b)
        blank._program(self.b.app_base, b"\x00" * 36)
        self.assertEqual(blank.read(self.b.app_base, 36), b"\x00" * 32 + b"\xFF" * 4)
        self.assertEqual(blank.partial, [self.b.app_base + 32])

    def test_the_route_follows_the_board(self):
        """A key on a board without one, or none on a keyed board, is refused before any request"""
        dev, _ = self.stock()
        _, p = self.image_plan()
        with self.assertRaises(dfumod.DfuError):
            self.dfu(dev).run(p, KEY)
        self.assertEqual(dev.requests, [])
        keyed = fakedfu.FakeAnyRoad(DM1701, KEY)
        with self.assertRaises(dfumod.DfuError):
            self.dfu(keyed).run(plan.make_plan(DM1701, [(DM1701.app_base, app_image(3000))], plan.KEEP))
        self.assertEqual((keyed.erased, keyed.vendor, keyed.blocks), ([], [], 0))

    def test_probe(self):
        dev, before = self.stock()
        self.assertFalse(self.dfu(dev).probe_protected(self.b))
        self.assertEqual(dev.state, dfumod.ST_IDLE)
        for stall in (False, True):					# a refused read may come back empty or stall
            dev, before = self.stock(protected=True, stall_upload=stall)
            self.assertTrue(self.dfu(dev).probe_protected(self.b))
            self.assertEqual(dev.state, dfumod.ST_IDLE)
            self.assertEqual((bytes(dev.flash), dev.erased, dev.blocks, dev.unprotected), (before, [], 0, False))
            # the only commands: set address, to the start of the program flash
            cmds = [r[3] for r in dev.requests if r[1] == dfumod.DFU_DNLOAD]
            self.assertEqual(cmds, [bytes([dfumod.CMD_SET_ADDRESS]) + struct.pack("<I", self.b.rom_base)])

    def test_a_bootloader_left_in_another_state(self):
        """A run that was interrupted leaves the bootloader where it was: in the middle of a download or an upload,
        or in dfuERROR. The next command's first requests take it to dfuIDLE by the standard's own requests (to_idle),
        as the web flasher's: a ROM bootloader stalls a CLRSTATUS outside dfuERROR and an ABORT inside it"""
        b = self.b
        img, p = self.image_plan(9000)
        for state_ in (dfumod.ST_DNIDLE, dfumod.ST_UPIDLE, dfumod.ST_ERROR):
            for first in ("probe", "run", "unprotect"):
                dev, _ = self.stock(protected=first == "unprotect")
                dev.state, dev.status = state_, 0x06 if state_ == dfumod.ST_ERROR else 0
                what = (state_, first)
                if first == "probe":
                    self.assertFalse(self.dfu(dev).probe_protected(b), what)
                elif first == "run":
                    self.dfu(dev).run(p)
                    self.assertEqual((dev.read(b.app_base, len(img)), dev.exited), (img, True), what)
                else:
                    self.dfu(dev).read_unprotect(b)
                    self.assertTrue(dev.unprotected, what)
                # nothing was stalled on the way: CLRSTATUS only out of the error, ABORT only out of a transfer
                sent = [r[1] for r in dev.requests[:2]]
                self.assertEqual(sent, [dfumod.DFU_GETSTATUS, dfumod.DFU_CLRSTATUS if state_ == dfumod.ST_ERROR
                                        else dfumod.DFU_ABORT], what)
                self.assertEqual(dev.status, 0, what)

    def test_probe_never_guesses_protected(self):
        """"Protected" leads to an erase of everything: an error the documents don't describe is not taken for it"""
        class Odd(fakedfu.FakeRom):
            def _error(self, status):
                super()._error(0x08)					# errPROG, not errVENDOR

        dev = Odd(self.b, protected=True)
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).probe_protected(self.b)
        self.assertIn("nothing was changed", str(cm.exception))

        class Short(fakedfu.FakeRom):					# no error, but not the 4 bytes either
            def ctrl_transfer(self, rtype, req, value, intf, data, timeout=None):
                r = super().ctrl_transfer(rtype, req, value, intf, data, timeout)
                return r[:2] if req == dfumod.DFU_UPLOAD else r

        with self.assertRaises(dfumod.DfuError):
            self.dfu(Short(self.b)).probe_protected(self.b)

    def test_protected_refuses_and_changes_nothing(self):
        dev, before = self.stock(protected=True)
        _, p = self.image_plan()
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).run(p)
        self.assertIn("erase", str(cm.exception))
        self.assertEqual((bytes(dev.flash), dev.erased, dev.blocks, dev.exited), (before, [], 0, False))

    def test_read_unprotect(self):
        dev, before = self.stock(protected=True)
        d = self.dfu(dev)
        d.read_unprotect(self.b)
        self.assertEqual(bytes(dev.flash), b"\xFF" * len(before))	# everything, the maker's bootloader included
        self.assertTrue(dev.unprotected and dev.gone and not dev.protected)
        self.assertEqual([r[3] for r in dev.requests if r[1] == dfumod.DFU_DNLOAD], [b"\x92"])
        with self.assertRaises(fakedfu.Gone):				# off the bus until it is switched off and on
            d.get_status()
        self.assertEqual(fakedfu.FakeUsb([dev]).core.find(), [])
        dev.power_cycle()
        self.assertFalse(d.probe_protected(self.b))
        _, p = self.image_plan()
        d.run(p)
        self.assertTrue(dev.exited)
        self.assertEqual(dev.alt_selected, [])

    def test_read_unprotect_refused_by_a_protected_region(self):
        """Read Unprotect erases the whole program flash: a board that lists anything protected there never gets it"""
        import copy
        from rfoundry import protect
        b = copy.copy(self.b)
        sec = b.sectors[-1]
        b.protect = [{"mem": "progmem", "start": sec.addr - b.rom_base, "end": sec.end - b.rom_base,
                      "what": "test (x.md 1)"}]
        dev = fakedfu.FakeRomUsbDevice(b, fakedfu.rom_flash(b, random.Random(5)), protected=True)
        before = bytes(dev.flash)
        with self.assertRaises(protect.ProtectedError) as cm:
            self.dfu(dev).read_unprotect(b)
        self.assertIn("test (x.md 1)", str(cm.exception))
        self.assertEqual((dev.requests, bytes(dev.flash), dev.protected), ([], before, True))
        # the command line stops at the same place, before its question
        usb = fakedfu.FakeUsb([dev])
        with mock.patch.object(fakedfu, "FakeRomUsbDevice", return_value=dev), \
                mock.patch.object(boards, "get", return_value=b), mock.patch.object(fakedfu, "FakeUsb", return_value=usb):
            rc, out, err = run_cli(["flash", "--board", b.id, "--simulate", "stock", "--unprotect"])
        self.assertEqual(rc, 2, out + err)
        self.assertIn("protected regions", err)
        self.assertEqual((bytes(dev.flash), dev.protected, dev.unprotected), (before, True, False))

    def test_read_unprotect_not_taken(self):
        class Deaf(fakedfu.FakeRom):
            def ctrl_transfer(self, rtype, req, value, intf, data, timeout=None):
                if req == dfumod.DFU_DNLOAD and bytes(data or b"") == b"\x92":
                    self._error(0x0F)
                    return 1
                return super().ctrl_transfer(rtype, req, value, intf, data, timeout)

        dev = Deaf(self.b, protected=True)
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).read_unprotect(self.b)
        self.assertIn("did not take", str(cm.exception))

    def test_option_bytes_are_never_selected(self):
        """Alternate setting 1 of a ROM bootloader is the chip's option bytes. Nothing selects an alternate setting, on
        any path; the fake fails every request that follows one"""
        dev, _ = self.stock(protected=True)
        usb = fakedfu.FakeUsb([dev])
        cli.check_rom_loader(usb, dev, self.b, log=lambda s: None)
        intf = dfumod.open_device(usb, dev)
        self.assertEqual(intf, 0)
        d = dfumod.Dfu(dev, intf, log=lambda *a: None, sleep=lambda s: None)
        self.assertTrue(d.probe_protected(self.b))
        d.read_unprotect(self.b)
        dev.power_cycle()
        self.assertFalse(d.probe_protected(self.b))
        _, p = self.image_plan()
        d.run(p)
        dfumod.release(usb, dev)
        self.assertEqual((dev.alt_selected, dev.alt), ([], 0))
        # DFU class requests to the interface, nothing else (no standard SET_INTERFACE, bmRequestType 0x01)
        self.assertLessEqual({(r[0], r[1]) for r in dev.requests},
                             {(0x21, dfumod.DFU_DNLOAD), (0x21, dfumod.DFU_ABORT), (0x21, dfumod.DFU_CLRSTATUS),
                              (0xA1, dfumod.DFU_GETSTATUS), (0xA1, dfumod.DFU_UPLOAD)})
        # every address sent is in the program flash: the pointer also reaches the option bytes
        for r in dev.requests:
            if r[1] == dfumod.DFU_DNLOAD and r[2] == 0 and len(r[3] or b"") == 5:
                a = struct.unpack("<I", r[3][1:])[0]
                self.assertTrue(self.b.rom_base <= a < self.b.flash_end, hex(a))
        # the fake itself: a selected alternate setting is recorded and fails the next request
        dev, _ = self.stock()
        dev.set_interface_altsetting(0, 1)
        self.assertEqual(dev.alt_selected, [1])
        with self.assertRaises(AssertionError):
            dev.ctrl_transfer(0xA1, dfumod.DFU_GETSTATUS, 0, 0, 6)
        dev, _ = self.stock()
        dev.ctrl_transfer(0x01, 0x0B, 1, 0, None)			# the standard SET_INTERFACE request
        self.assertEqual(dev.alt_selected, [1])
        # and a sector outside the program flash never reaches the bootloader
        dev, _ = self.stock()
        _, p = self.image_plan(5000)
        p.steps[0].sector = boards.Sector(0, 0x1FF00000, 0x20000)
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).run(p)
        self.assertIn("outside the program flash", str(cm.exception))
        self.assertEqual(dev.requests, [])

    def test_device_checks(self):
        """The two bootloaders share their USB IDs: each route refuses the other's device before any DFU request"""
        b = self.b
        quiet = lambda s: None
        rom, _ = self.stock()
        rom.ctrl_transfer = mock.Mock(side_effect=AssertionError("a DFU request before the device check passed"))
        cli.check_rom_loader(fakedfu.FakeUsb([rom]), rom, b, log=quiet)
        # a radio maker's keyed bootloader on the plain route
        keyed = fakedfu.FakeUsbDevice(self.keyed, KEY)
        keyed.ctrl_transfer = rom.ctrl_transfer
        with self.assertRaises(cli.UsageError) as cm:
            cli.check_rom_loader(fakedfu.FakeUsb([keyed]), keyed, b, log=quiet)
        self.assertIn("maker's own bootloader", str(cm.exception))
        # a ROM bootloader on the keyed route
        with self.assertRaises(cli.UsageError) as cm:
            cli.check_bootloader(fakedfu.FakeUsb([rom]), rom, self.keyed, log=quiet)
        self.assertIn("not the radio's own bootloader", str(cm.exception))
        # a keyed board whose registry entry lost the string has nothing to tell the two apart by: refused as well
        import copy
        nameless = copy.copy(self.keyed)
        nameless.dfu_string1 = ""
        with self.assertRaises(cli.UsageError):
            cli.check_bootloader(fakedfu.FakeUsb([rom]), rom, nameless, log=quiet)
        # another chip's ROM bootloader: an STM32F405's flash layout, or no layout string at all
        for names in (["@Internal Flash  /0x08000000/04*016Kg,01*064Kg,07*128Kg", "@Option Bytes  /0x1FFFC000/01*016 e"],
                      ["@Internal Flash  /0x08000000/08*128Kg"], ["@Internal Flash  /0x08020000/15*128Kg"], [None, None]):
            other, _ = self.stock()
            other.strings = dict(other.strings, interfaces=names)
            other.ctrl_transfer = rom.ctrl_transfer
            with self.assertRaises(cli.UsageError) as cm:
                cli.check_rom_loader(fakedfu.FakeUsb([other]), other, b, log=quiet)
            self.assertIn("flash layout", str(cm.exception))
        rom.ctrl_transfer.assert_not_called()
        self.assertTrue(dfumod.rom_layout_ok(["@Internal  Flash /0x%08X/%d*128Kg" % (b.rom_base, len(b.sectors))], b))
        declared = mock.Mock(dfu_layout="@Internal Flash  /0x08000000/16*128Kg", rom_base=0, flash_end=0)
        self.assertTrue(dfumod.rom_layout_ok(["@Internal Flash /0x08000000/16*128Kg", "x"], declared))
        self.assertFalse(dfumod.rom_layout_ok(["@Internal Flash /0x08000000/16*128Ka"], declared))

    def cli_run(self, *argv):
        """A simulated install, with the fake device it ran against"""
        made = []
        real = fakedfu.FakeRomUsbDevice

        class Spy(real):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                made.append(self)

        with mock.patch.object(fakedfu, "FakeRomUsbDevice", Spy):
            rc, out, err = run_cli(["flash", "--board", self.b.id] + list(argv))
        return rc, out, err, (made[0] if made else None)

    def test_cli_stops_at_a_protected_radio(self):
        rc, out, err, dev = self.cli_run("--simulate")			# stock: read protected
        self.assertEqual(rc, 2, out + err)
        self.assertIn("read protected", err)
        self.assertIn("--unprotect", err)
        self.assertIn("erases the whole program memory", err)
        self.assertEqual((dev.erased, dev.blocks, dev.unprotected, dev.exited, dev.alt_selected), ([], 0, False, False, []))
        self.assertTrue(dev.protected)
        self.assertNotEqual(dev.read(self.b.app_base, 4096), b"\xFF" * 4096)	# the maker's firmware is still there
        # the board's own install notice follows when the registry carries it
        with mock.patch.object(self.b, "notice", "Back up first.", create=True):
            rc, out, err, _ = self.cli_run("--simulate", "stock")
        self.assertTrue(err.rstrip().endswith("--unprotect\nBack up first."), err)

    def test_cli_unprotects_when_asked(self):
        rc, out, err, dev = self.cli_run("--simulate", "--unprotect")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("board %s, no codec" % self.b.name, out)
        self.assertIn("erases its program memory", out)
        self.assertIn("leave: start the image at 0x%08X" % self.b.app_base, out)
        self.assertIn("sim      PASS", out)
        self.assertTrue(dev.unprotected and dev.exited)
        self.assertEqual((dev.alt_selected, dev.refused, dev.partial, dev.violations), ([], [], [], []))
        used = b"".join(d for d in dev.sent)
        self.assertEqual(len(used) % dfumod.FLASH_WORD, 0)
        # every sector the image didn't take is blank: nothing of the maker's firmware is left
        for s in self.b.sectors:
            if s.addr not in dev.erased:
                self.assertEqual(dev.read(s.addr, s.size), b"\xFF" * s.size)

    def test_cli_unprotected_radio(self):
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            img = rom_image(140000)
            with open(bp, "wb") as f:
                f.write(img)
            for state_ in ("blank", "rfoundry-nocodec"):
                rc, out, err, dev = self.cli_run(bp, "--simulate", state_)
                self.assertEqual(rc, 0, out + err)
                self.assertIn("sim      PASS", out)
                self.assertEqual(dev.read(self.b.app_base, len(img)), img)
                self.assertFalse(dev.unprotected)
            rc, out, err, dev = self.cli_run(bp, "--simulate", "blank", "--unprotect")
            self.assertEqual(rc, 0, out + err)
            self.assertIn("not read protected: nothing to remove", out)
            self.assertFalse(dev.unprotected)
            rc, out, err, dev = self.cli_run(bp, "--simulate", "stock", "--dry-run")
            self.assertEqual(rc, 0, out + err)
            self.assertIn("dry run: nothing was erased or written", out)
            self.assertIsNone(dev)
            # what belongs to the keyed bootloaders and the codec doesn't apply
            for extra in (["--stock", "synthetic"], ["--codec", "keep"], ["--key", bp], ["--stock-installed", "V1"]):
                rc, out, err, dev = self.cli_run(bp, "--simulate", "blank", *extra)
                self.assertEqual(rc, 2, out + err)
                self.assertIn("don't apply", err)
                self.assertIsNone(dev)

    def test_unprotect_is_for_this_route_only(self):
        rc, out, err = run_cli(["flash", "--board", self.keyed.id, "--simulate", "rfoundry-codec", "--unprotect"])
        self.assertEqual(rc, 2, out + err)
        self.assertIn("--unprotect is for", err)
        # without it a keyed board's simulation needs no image file either
        rc, out, err = run_cli(["flash", "--board", self.keyed.id, "--simulate", "rfoundry-codec", "--yes"])
        self.assertEqual(rc, 0, out + err)
        self.assertIn("sim      PASS", out)

    def test_real_run_asks_before_the_unprotect(self):
        """Not simulated: the question is asked, "no" (or no terminal) changes nothing, and after a "yes" the radio is
        waited for a second time"""
        b = self.b
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            img = rom_image(9000)
            with open(bp, "wb") as f:
                f.write(img)
            argv = ["flash", bp, "--board", b.id, "--unprotect", "--no-probe", "--no-verify", "--no-reporting"]
            dev, before = self.stock(protected=True)
            usb = fakedfu.FakeUsb([dev])

            def left(u, board, was, sent):				# the radio leaves when the erase is over
                self.assertEqual((was.bus, was.address), (dev.bus, dev.address))
                self.assertTrue(dev.gone)
                return True, None

            def user(s):						# the user's part: off, the key, on
                if dev.gone:
                    dev.power_cycle()

            with mock.patch.object(dfumod, "import_usb", return_value=usb), \
                    mock.patch.object(cli.time, "sleep", side_effect=user), \
                    mock.patch.object(cli, "wait_unprotect", side_effect=left) as gone:
                with mock.patch.object(cli, "ask_yes", return_value=False) as ask:
                    rc, out, err = run_cli(argv)
                self.assertEqual(rc, 1, out + err)
                ask.assert_called_once()
                self.assertIn("Remove the read protection now?", ask.call_args[0][0])
                self.assertEqual((bytes(dev.flash), dev.protected, dev.unprotected), (before, True, False))
                gone.assert_not_called()
                with mock.patch.object(cli, "ask_yes", return_value=True) as ask:
                    rc, out, err = run_cli(argv)
                self.assertEqual(rc, 0, out + err)
                ask.assert_called_once()				# the one question covers the install
                gone.assert_called_once()
                self.assertIn("Now switch the radio off.", out)
                # the last instruction before the write is to leave the radio on
                off = out.index("Now switch the radio off.")
                self.assertLess(off, out.index("leave it on and connected while it is written"))
                self.assertLess(out.index("leave it on and connected while it is written"), out.index("erase sector"))
                self.assertTrue(dev.unprotected and dev.exited)
                self.assertEqual(dev.read(b.app_base, len(img)), img)
                self.assertEqual(dev.alt_selected, [])

    def test_detect(self):
        ports = mock.patch.object(debuglink, "find_ports", return_value=None)	# the host's serial ports stay out of it
        ports.start()
        self.addCleanup(ports.stop)
        dev, before = self.stock(protected=True)
        with mock.patch.object(dfumod, "import_usb", return_value=fakedfu.FakeUsb([dev])):
            rc, out, err = run_cli(["detect", "--no-probe"])
        self.assertEqual(rc, 0, out + err)
        self.assertIn("the bootloader in a processor's ROM", out)
        self.assertIn("%s (--board %s)" % (self.b.name, self.b.id), out)
        self.assertIn("read protection: on", out)
        self.assertEqual((bytes(dev.flash), dev.unprotected, dev.alt_selected), (before, False, []))
        dev, _ = self.stock()
        with mock.patch.object(dfumod, "import_usb", return_value=fakedfu.FakeUsb([dev])):
            rc, out, err = run_cli(["detect", "--no-probe", "--board", self.b.id])
        self.assertIn("read protection: off", out)
        # a keyed bootloader is still what it was, and gets no request
        keyed = fakedfu.FakeUsbDevice(self.keyed, KEY)
        keyed.ctrl_transfer = mock.Mock(side_effect=AssertionError("a DFU request from detect"))
        with mock.patch.object(dfumod, "import_usb", return_value=fakedfu.FakeUsb([keyed])):
            rc, out, err = run_cli(["detect", "--no-probe"])
        self.assertIn("AnyRoad bootloader", out)
        self.assertNotIn("read protection", out)

    def test_no_key_to_get(self):
        rc, out, err = run_cli(["key", "--board", self.b.id])
        self.assertEqual(rc, 2)
        self.assertIn("has no key", err)

    def test_nothing_past_the_application_region(self):
        """Flash past the linker script's application region is never erased or written (a ROM bootloader may not
        survive an erase there): a file that reaches it is refused before any request, and before a Read Unprotect"""
        b = self.b
        lo, hi = b.app_region
        if hi >= b.flash_end:
            self.skipTest("the application region is the whole program flash")
        img = rom_image(hi - lo + 5000)
        p = plan.make_plan(b, [(b.app_base, img)], plan.KEEP)		# the planner takes the whole program flash
        self.assertEqual(p.erase[-1].addr, hi)
        dev, before = self.stock()
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).run(p)
        self.assertIn("application flash", str(cm.exception))
        self.assertEqual((dev.requests, bytes(dev.flash)), ([], before))
        # an image that fills the region to its last byte is this radio's
        dfumod.check_plain(plan.make_plan(b, [(b.app_base, rom_image(hi - lo))], plan.KEEP, vector_last=True))
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            with open(bp, "wb") as f:
                f.write(img)
            for extra in (["--unprotect"], ["--dry-run"]):
                rc, out, err, dev = self.cli_run(bp, "--simulate", "stock", *extra)
                self.assertEqual(rc, 2, out + err)
                self.assertIn("application flash", err)
                self.assertIsNone(dev)				# no device was looked for: nothing unprotected

    def test_block_outside_its_sector(self):
        """A plan whose block would land past its sector's end (in the next, unerased one), or below the first block
        (block 0 is the DfuSe command channel: its first byte would be taken as Set Address, Erase or Read Unprotect,
        AN3156 5), is refused before any request, as the web flasher's checkPlain refuses it"""
        for num in (None, plan.FIRST_BLOCK - 1, 0):
            _, p = self.image_plan()
            st = p.writes[-1]
            n = plan.FIRST_BLOCK + st.sector.size // plan.BLOCK if num is None else num
            # for block 0, bytes that would be an erase of the sector if they reached the bootloader as a command
            st.blocks.append((n, (bytes([dfumod.CMD_ERASE]) + struct.pack("<I", st.sector.addr)).ljust(plan.BLOCK,
                                                                                                    b"\xFF")))
            dev, before = self.stock()
            with self.assertRaises(dfumod.DfuError, msg=n) as cm:
                self.dfu(dev).run(p)
            self.assertIn("block %d is outside sector %d" % (n, st.sector.index), str(cm.exception))
            self.assertEqual((dev.requests, bytes(dev.flash)), ([], before), n)

    def test_read_back_before_the_start(self):
        """Every block is read back before the image is started: a ROM bootloader reports no error for a write it
        didn't carry out (AN3156 5.1, 5.3). A block that reads back different, or can't be read, leaves the image
        unstarted with the radio still in its bootloader"""
        b = self.b
        img, p = self.image_plan()
        dev, _ = self.stock()
        self.dfu(dev).run(p)
        ups = [r for r in dev.requests if r[1] == dfumod.DFU_UPLOAD]
        self.assertEqual(len(ups), p.total_blocks)
        self.assertTrue(all(r[0] == 0xA1 and r[2] == plan.FIRST_BLOCK and r[3] == plan.BLOCK for r in ups), ups[:2])
        self.assertTrue(dev.exited)
        self.assertEqual((dev.refused, dev.violations, dev.alt_selected), ([], [], []))
        # a sector that takes its blocks without an error and without programming them (write protected)
        dev, _ = self.stock()
        last = p.writes[-1].sector
        real = dev._program
        dev._program = lambda a, d: None if last.addr <= a < last.end else real(a, d)
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).run(p)
        self.assertIn("0x%08X does not read back" % last.addr, str(cm.exception))
        self.assertIn("not started", str(cm.exception))
        self.assertFalse(dev.exited)
        self.assertEqual(dev.alt_selected, [])
        # a read the bootloader stalls: not checked is not taken as checked
        dev, _ = self.stock()
        wire = dev.ctrl_transfer

        def no_reads(rtype, req, *a):
            if req == dfumod.DFU_UPLOAD:
                raise fakedfu.Stall("Pipe error")
            return wire(rtype, req, *a)
        dev.ctrl_transfer = no_reads
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).run(p)
        self.assertIn("could not be read back", str(cm.exception))
        self.assertFalse(dev.exited)
        # written, only not started, and the vector table not written: nothing to start at a normal power-on
        self.assertEqual(dev.read(b.app_base + plan.BLOCK, len(img) - plan.BLOCK), img[plan.BLOCK:])
        self.assertEqual(dev.read(b.app_base, plan.BLOCK), b"\xFF" * plan.BLOCK)

    def test_waits_for_the_radio_to_leave(self):
        b = self.b
        dev, _ = self.stock(protected=True)
        usb = fakedfu.FakeUsb([dev])
        was = mock.Mock(bus=dev.bus, address=dev.address)
        naps = []
        self.assertFalse(cli.wait_gone(usb, b, was, 0, sleep=naps.append))	# still there when the time is up
        self.assertEqual(naps, [])

        def leave(s):
            naps.append(s)
            dev.gone = True

        self.assertTrue(cli.wait_gone(usb, b, was, 60, sleep=leave))		# it leaves while rfoundry waits
        self.assertEqual(naps, [1.0])
        dev.power_cycle()							# back as another device: the old place is empty
        self.assertTrue(cli.wait_gone(usb, b, was, 0, sleep=naps.append))

    def test_radio_that_stays_on_usb_after_the_unprotect(self):
        """The radio took the command and is still on USB when the wait ends: that device is passed over, and the
        install goes on only with the one that appears after the radio was switched off and on"""
        b = self.b
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            img = rom_image(9000)
            with open(bp, "wb") as f:
                f.write(img)
            argv = ["flash", bp, "--board", b.id, "--unprotect", "--yes", "--wait", "0", "--no-probe", "--no-verify",
                    "--no-reporting"]
            for comes_back in (False, True):
                dev, _ = self.stock(protected=True)

                def stays(u, board, was, wait_s, sleep=None, dev=dev, comes_back=comes_back):
                    dev.gone = False				# on the bus where it was
                    if comes_back:
                        dev.power_cycle()
                    return False

                with mock.patch.object(dfumod, "import_usb", return_value=fakedfu.FakeUsb([dev])), \
                        mock.patch.object(cli.time, "sleep"), mock.patch.object(cli, "wait_gone", side_effect=stays):
                    rc, out, err = run_cli(argv)
                self.assertIn("still on USB", err)
                # past twice the longest erase: the words say it is over, not to wait longer, before "switch off"
                self.assertIn("the erase is over, or the radio did not take the command", err)
                self.assertNotIn("another minute", err)
                self.assertTrue(dev.unprotected)
                if comes_back:
                    self.assertEqual(rc, 0, out + err)
                    self.assertEqual(dev.read(b.app_base, len(img)), img)
                    self.assertTrue(dev.exited)
                else:
                    self.assertEqual(rc, 2, out + err)
                    self.assertIn("did not leave USB", err)
                    self.assertNotIn("is empty", err)
                    self.assertEqual((dev.erased, dev.blocks, dev.exited), ([], 0, False))
                self.assertEqual(dev.alt_selected, [])

    def test_wait_after_the_unprotect(self):
        """The radio is told it may be switched off only once it has left USB and UNPROTECT_MIN_S have passed (past
        twice the chip's longest mass erase), and is waited for up to UNPROTECT_WAIT_S; a bootloader back on the bus by
        itself is found before anything says to switch off"""
        b = self.b
        least, most = cli.unprotect_wait(b)
        self.assertGreater(b.mass_erase_max_s, 0)
        self.assertGreaterEqual(least, max(cli.UNPROTECT_MIN_S, 2 * b.mass_erase_max_s))
        self.assertGreaterEqual(most, max(cli.UNPROTECT_WAIT_S, 2 * least))
        for leaves_at, comes_back in ((5, False), (90, False), (None, False), (5, True)):
            dev, _ = self.stock(protected=True)
            usb = fakedfu.FakeUsb([dev])
            was = mock.Mock(bus=dev.bus, address=dev.address)
            clock = [1000.0]

            def nap(s, dev=dev, leaves_at=leaves_at, comes_back=comes_back):
                clock[0] += s
                if leaves_at is not None and clock[0] - 1000.0 >= leaves_at and not dev.unprotected:
                    dev.gone = dev.unprotected = True
                    if comes_back:
                        dev.power_cycle()			# reset into its bootloader at once, a new device

            with mock.patch.object(cli.time, "monotonic", side_effect=lambda: clock[0]), \
                    mock.patch.object(cli.time, "sleep", side_effect=nap):
                left, back = cli.wait_unprotect(usb, b, was, 1000.0)
            what = (leaves_at, comes_back)
            self.assertEqual(left, leaves_at is not None, what)
            self.assertIs(back, dev if comes_back else None, what)
            took = clock[0] - 1000.0
            self.assertGreaterEqual(took, least, what)
            self.assertLessEqual(took, most + 1, what)
            if leaves_at == 90:
                self.assertLess(took, 92, "on as soon as it left")
            if leaves_at is None:
                self.assertGreaterEqual(took, most)

    def test_the_wait_follows_the_chip_s_mass_erase_time(self):
        """Both waits come from the registry's mass erase time of the board's processor (platform.toml
        mass_erase_max_s), never from a figure of the tool's own: twice the bound and at least UNPROTECT_MIN_S before
        the switch-off, twice that and at least UNPROTECT_WAIT_S for the radio to leave USB"""
        import copy
        b = copy.copy(self.b)
        for bound, want in ((1, (60, 120)), (26, (60, 120)), (30, (60, 120)), (31, (62, 124)), (100, (200, 400))):
            b.mass_erase_max_s = bound
            self.assertEqual(cli.unprotect_wait(b), want, bound)
        # every board of the route has the figure, and the words quote the board's
        for x in boards.BOARDS.values():
            if x.family == "dfuse":
                self.assertGreater(x.mass_erase_max_s, 0, x.id)
        b.mass_erase_max_s = 45
        dev, _ = self.stock(protected=True)
        clock = [0.0]
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            with open(bp, "wb") as f:
                f.write(rom_image(9000))
            argv = ["flash", bp, "--board", b.id, "--unprotect", "--yes", "--wait", "0", "--no-probe", "--no-verify",
                    "--no-reporting"]
            with mock.patch.object(dfumod, "import_usb", return_value=fakedfu.FakeUsb([dev])), \
                    mock.patch.object(boards, "get", return_value=b), \
                    mock.patch.object(cli.time, "monotonic", side_effect=lambda: clock[0]), \
                    mock.patch.object(cli.time, "sleep", side_effect=lambda s: clock.__setitem__(0, clock[0] + s)):
                dev.ctrl_transfer = self.stays_on_usb(dev)
                rc, out, err = run_cli(argv)
            self.assertIn("(at least 90 s)", out)
            self.assertIn("still on USB after 180 s, longer than its erase takes (at most 45 s)", err)
            self.assertGreaterEqual(clock[0], 180)
            # a board whose registry entry lost the figure: refused before the Read Unprotect, nothing erased
            b.mass_erase_max_s = None
            dev, before = self.stock(protected=True)
            with mock.patch.object(dfumod, "import_usb", return_value=fakedfu.FakeUsb([dev])), \
                    mock.patch.object(boards, "get", return_value=b), mock.patch.object(cli.time, "sleep"):
                rc, out, err = run_cli(argv)
            self.assertEqual(rc, 2, out + err)
            self.assertIn("no mass erase time", err)
            self.assertEqual((dev.protected, dev.unprotected), (True, False))
            self.assertTrue(bytes(dev.flash) == before, "nothing erased")

    @staticmethod
    def stays_on_usb(dev):
        """dev's ctrl_transfer for a radio that takes the Read Unprotect and is still on USB afterwards"""
        wire = dev.ctrl_transfer

        def stays(rtype, req, value, intf, data, timeout=None):
            r = wire(rtype, req, value, intf, data, timeout)
            if dev.gone:
                dev.gone = False
                dev.state, dev.status, dev.busy, dev.pending = dfumod.ST_IDLE, 0, 0, None
            return r
        return stays

    def test_bus_listing_error_during_the_wait(self):
        """A USB bus that can't be listed while the radio may be erasing does not end the install: what an error
        prints starts with switching the radio off. The listing is taken as nothing seen to leave, the wait runs to
        its end, and only then is anything said about switching off"""
        b = self.b
        least, most = cli.unprotect_wait(b)
        for broken_at, mends_at in ((0, None), (3, None), (0, 20)):
            dev, _ = self.stock(protected=True)
            usb = fakedfu.FakeUsb([dev])
            was = mock.Mock(bus=dev.bus, address=dev.address)
            dev.gone = dev.unprotected = True			# it took the command and left the bus
            clock = [1000.0]

            def listing(u, board, clock=clock, broken_at=broken_at, mends_at=mends_at):
                t = clock[0] - 1000.0
                if t >= broken_at and (mends_at is None or t < mends_at):
                    raise OSError("Input/Output Error")
                return usb.core.find()

            with mock.patch.object(cli.time, "monotonic", side_effect=lambda: clock[0]), \
                    mock.patch.object(cli.time, "sleep", side_effect=lambda s: clock.__setitem__(0, clock[0] + s)), \
                    mock.patch.object(dfumod, "find", side_effect=listing):
                left, back = cli.wait_unprotect(usb, b, was, 1000.0)
            what = (broken_at, mends_at)
            took = clock[0] - 1000.0
            # seen to leave before the listing broke, or once it works again; never concluded from a failed listing
            self.assertEqual(left, what != (0, None), what)
            self.assertIsNone(back, what)
            self.assertGreaterEqual(took, most if what == (0, None) else least, what)
        # the whole command: the listing breaks once the Read Unprotect is sent and stays broken
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            with open(bp, "wb") as f:
                f.write(rom_image(9000))
            dev, _ = self.stock(protected=True)
            usb = fakedfu.FakeUsb([dev])
            clock = [0.0]

            def listing(u, board):
                if dev.unprotected:
                    raise OSError("Input/Output Error")
                return usb.core.find()

            with mock.patch.object(dfumod, "import_usb", return_value=usb), \
                    mock.patch.object(cli.time, "monotonic", side_effect=lambda: clock[0]), \
                    mock.patch.object(cli.time, "sleep", side_effect=lambda s: clock.__setitem__(0, clock[0] + s)), \
                    mock.patch.object(dfumod, "find", side_effect=listing):
                rc, out, err = run_cli(["flash", bp, "--board", b.id, "--unprotect", "--yes", "--wait", "0",
                                        "--no-probe", "--no-verify", "--no-reporting"])
            self.assertEqual(rc, 2, out + err)
            self.assertTrue(dev.unprotected)
            # the error comes, with the bootloader's steps, only after the wait and after "Now switch the radio off."
            self.assertIn("Input/Output Error", err)
            self.assertIn("Now switch the radio off.", out)
            self.assertGreaterEqual(clock[0], most)

    def test_radio_back_by_itself_after_the_unprotect(self):
        """A radio that resets into its bootloader by itself after the erase (its key still held) is written without
        being told to switch off first: nothing tells the user to cut the power while it is written"""
        b = self.b
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            img = rom_image(9000)
            with open(bp, "wb") as f:
                f.write(img)
            dev, _ = self.stock(protected=True)

            def left(u, board, was, wait_s, sleep=None):
                dev.power_cycle()					# gone from the old place, back at a new one
                return True

            with mock.patch.object(dfumod, "import_usb", return_value=fakedfu.FakeUsb([dev])), \
                    mock.patch.object(cli.time, "sleep"), mock.patch.object(cli, "wait_gone", side_effect=left):
                rc, out, err = run_cli(["flash", bp, "--board", b.id, "--unprotect", "--yes", "--wait", "0",
                                        "--no-probe", "--no-verify", "--no-reporting"])
            self.assertEqual(rc, 0, out + err)
            self.assertNotIn("switch the radio off", out)
            self.assertLess(out.index("back in its bootloader by itself"), out.index("erase sector"))
            self.assertEqual(dev.read(b.app_base, len(img)), img)

    def test_unprotect_reply_lost(self):
        """The bootloader takes the Read Unprotect and its GETSTATUS reply is lost (it may leave the bus at once, AN3156
        5.4): taken as the erase started, as the web flasher takes it, never as an error that says to switch off"""
        class Lost(fakedfu.FakeRomUsbDevice):
            def ctrl_transfer(self, rtype, req, value, intf, data, timeout=None):
                was = self.pending
                r = super().ctrl_transfer(rtype, req, value, intf, data, timeout)
                if was == "unprotect" and req == dfumod.DFU_GETSTATUS:
                    raise OSError("[Errno 110] Operation timed out")
                return r

        dev = Lost(self.b, fakedfu.rom_flash(self.b, random.Random(5)), protected=True)
        log = []
        dfumod.Dfu(dev, 0, log=log.append, sleep=lambda s: None).read_unprotect(self.b)
        self.assertTrue(dev.unprotected)
        self.assertTrue(any("left the bus" in m and "may be erasing" in m for m in log), log)
        made = []

        class Spy(Lost):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                made.append(self)

        with mock.patch.object(fakedfu, "FakeRomUsbDevice", Spy):
            rc, out, err = run_cli(["flash", "--board", self.b.id, "--simulate", "stock", "--unprotect"])
        self.assertEqual(rc, 0, out + err)
        self.assertIn("leave it on and connected", out)
        self.assertNotIn("USB error while flashing", out + err)
        self.assertNotIn("Turn the radio off", out + err)
        self.assertTrue(made[0].unprotected and made[0].exited)

    def test_read_unprotect_answered_in_another_state(self):
        """AN3156 5.4: a GETSTATUS reply after the command that is not dfuDNBUSY is an error, status 0 or not. A loader
        that answers dfuIDLE or dfuDNLOAD-IDLE did not take the command: nothing is said to be erasing. Nor did one
        that answers dfuDNBUSY with an error status"""
        for status_, state_ in ((0, dfumod.ST_IDLE), (0, dfumod.ST_DNIDLE), (0x0A, dfumod.ST_DNBUSY)):
            with self.subTest(status=status_, state=state_):
                class Idle(fakedfu.FakeRom):
                    def ctrl_transfer(self, rtype, req, value, intf, data, timeout=None, st=state_, status=status_):
                        if req == dfumod.DFU_DNLOAD and bytes(data or b"") == b"\x92":
                            self.state, self.status, self.busy = st, status, 0
                            return 1
                        return super().ctrl_transfer(rtype, req, value, intf, data, timeout)

                dev = Idle(self.b, fakedfu.rom_flash(self.b, random.Random(5)), protected=True)
                before = bytes(dev.flash)
                with self.assertRaises(dfumod.DfuError) as cm:
                    self.dfu(dev).read_unprotect(self.b)
                self.assertIn("did not take", str(cm.exception))
                self.assertIn(dfumod.STATE_NAMES[state_], str(cm.exception))
                self.assertEqual((dev.unprotected, dev.protected, bytes(dev.flash)), (False, True, before))

    def test_vector_table_last(self):
        """The application's first sector is erased first, and its first block (the vector table) is written after
        every other block is written and read back: an install cut anywhere before it leaves no vector table, so
        nothing starts at the next normal power-on. A check of the vector table that fails erases its sector again"""
        b = self.b
        img, p = self.image_plan()
        self.assertGreater(len(p.writes), 1)
        self.assertIn("order: sector %d erased first and written last (a cut install leaves no application to start)"
                      % p.writes[0].sector.index, "\n".join(p.describe()))
        events = []
        dev, _ = self.stock()
        prog, wire = dev._program, dev.ctrl_transfer
        dev._program = lambda a, d: (events.append(("write", a)), prog(a, d))
        dev.ctrl_transfer = lambda rtype, req, value, intf, data, timeout=None: (
            events.append(("read", dev.addr)) if req == dfumod.DFU_UPLOAD else None,
            wire(rtype, req, value, intf, data, timeout))[1]
        self.dfu(dev).run(p)
        self.assertEqual(dev.erased[0], b.app_base)
        writes = [a for k, a in events if k == "write"]
        self.assertEqual(writes[-1], b.app_base)
        self.assertEqual(writes.count(b.app_base), 1)
        at = events.index(("write", b.app_base))
        self.assertEqual(len([e for e in events[:at] if e[0] == "read"]), p.total_blocks - 1,
                         "the rest read back first")
        self.assertEqual(events[at + 1:], [("read", b.app_base)])
        self.assertEqual(dev.read(b.app_base, len(img)), img)
        self.assertTrue(dev.exited)
        # cut after any number of blocks before the last one: no vector table
        for n in (0, 1, p.total_blocks // 2, p.total_blocks - 1):
            dev, _ = self.stock()
            real, count = dev._program, [0]

            def cut(a, d, real=real, count=count, n=n, dev=dev):
                if count[0] == n:
                    dev.gone = True				# the cable: nothing reaches the radio after it
                    raise fakedfu.Gone("No such device")
                count[0] += 1
                real(a, d)
            dev._program = cut
            if n < p.total_blocks - 1:
                with self.assertRaises(fakedfu.Gone):
                    self.dfu(dev).run(p)
            else:
                # cut at the vector table's own block: the erase is tried and can't reach the radio, and the words
                # don't claim a radio that starts nothing
                with self.assertRaises(dfumod.DfuError) as cm:
                    self.dfu(dev).run(p)
                self.assertIn("USB error at the vector table: No such device", str(cm.exception))
                self.assertIn("could not be erased again", str(cm.exception))
                self.assertNotIn("starts nothing", str(cm.exception))
            self.assertEqual(dev.read(b.app_base, plan.BLOCK), b"\xFF" * plan.BLOCK, n)
            self.assertFalse(dev.exited)
        # a block of the rest that doesn't read back: the vector table is never written
        dev, _ = self.stock()
        last = p.writes[-1].sector
        real = dev._program
        dev._program = lambda a, d: None if last.addr <= a < last.end else real(a, d)
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).run(p)
        self.assertIn("vector table is not written yet", str(cm.exception))
        self.assertEqual(dev.read(b.app_base, plan.BLOCK), b"\xFF" * plan.BLOCK)
        # the vector table itself doesn't read back: its sector is erased again
        dev, _ = self.stock()
        real = dev._program
        dev._program = lambda a, d: real(a, b"\x00" * len(d)) if a == b.app_base else real(a, d)
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).run(p)
        self.assertIn("does not read back", str(cm.exception))
        self.assertIn("sector is erased again", str(cm.exception))
        self.assertEqual(dev.erased.count(b.app_base), 2)
        self.assertEqual(dev.read(b.app_base, p.writes[0].sector.size), b"\xFF" * p.writes[0].sector.size)
        self.assertFalse(dev.exited)
        # the vector table's write reported as failed with part of it in flash (the documents describe no such
        # error; the tool is still connected, so nothing unchecked is left to start). The bootloader is in dfuERROR
        # then, where it takes no ABORT: the erase goes after CLRSTATUS
        dev, _ = self.stock()
        real = dev._program

        def half(a, d, real=real, dev=dev):
            real(a, d[:len(d) // 2] if a == b.app_base else d)
            if a == b.app_base:
                dev._error(0x06)				# errPROG
        dev._program = half
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).run(p)
        self.assertIn("block at 0x%08X failed: status 6, state dfuERROR" % b.app_base, str(cm.exception))
        self.assertIn("sector is erased again", str(cm.exception))
        self.assertIn("run the same command", str(cm.exception))
        self.assertEqual(dev.erased.count(b.app_base), 2)
        self.assertEqual(dev.read(b.app_base, p.writes[0].sector.size), b"\xFF" * p.writes[0].sector.size)
        self.assertEqual((dev.state, dev.exited), (dfumod.ST_DNIDLE, False))
        # the vector table's read refused: a stalled UPLOAD leaves dfuERROR too
        dev, _ = self.stock()
        wire = dev.ctrl_transfer

        def no_vectors(rtype, req, value, intf, data, timeout=None, wire=wire, dev=dev):
            if req == dfumod.DFU_UPLOAD and dev.addr == b.app_base:
                dev._error(0x0F)
                raise fakedfu.Stall("Pipe error")
            return wire(rtype, req, value, intf, data, timeout)
        dev.ctrl_transfer = no_vectors
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).run(p)
        self.assertIn("0x%08X could not be read back (Pipe error)" % b.app_base, str(cm.exception))
        self.assertIn("sector is erased again", str(cm.exception))
        self.assertEqual(dev.erased.count(b.app_base), 2)
        self.assertEqual(dev.read(b.app_base, p.writes[0].sector.size), b"\xFF" * p.writes[0].sector.size)
        self.assertFalse(dev.exited)
        # an error that is not rfoundry's own at the vector table's block (pyusb's, when one transfer fails) with the
        # bootloader still there: the sector is erased again all the same, and the words say where it happened
        dev, _ = self.stock()
        wire, sent = dev.ctrl_transfer, [0]

        def once(rtype, req, value, intf, data, timeout=None, wire=wire, sent=sent):
            if req == dfumod.DFU_DNLOAD and value == plan.FIRST_BLOCK:
                sent[0] += 1
                if sent[0] == p.total_blocks:
                    raise OSError("Input/Output Error")
            return wire(rtype, req, value, intf, data, timeout)
        dev.ctrl_transfer = once
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).run(p)
        self.assertIn("USB error at the vector table: Input/Output Error. The vector table's sector is erased again",
                      str(cm.exception))
        self.assertEqual(dev.erased.count(b.app_base), 2)
        self.assertEqual(dev.read(b.app_base, p.writes[0].sector.size), b"\xFF" * p.writes[0].sector.size)
        self.assertFalse(dev.exited)
        # the fake as the standard has it: ABORT in dfuERROR is stalled and clears nothing (DFU 1.1 A.2.11)
        dev, _ = self.stock()
        dev._error(0x06)
        with self.assertRaises(fakedfu.Stall):
            dev.ctrl_transfer(0x21, dfumod.DFU_ABORT, 0, 0, None)
        self.assertEqual((dev.state, dev.status), (dfumod.ST_ERROR, 0x06))
        self.dfu(dev).to_idle()
        self.assertEqual((dev.state, dev.status), (dfumod.ST_IDLE, 0))
        # to_idle from the other states: ABORT out of an idle state of a transfer (CLRSTATUS there is no request of
        # the standard's, and the fake stalls it), a busy bootloader waited out
        with self.assertRaises(fakedfu.Stall):
            dev.state = dfumod.ST_DNIDLE
            dev.ctrl_transfer(0x21, dfumod.DFU_CLRSTATUS, 0, 0, None)
        self.assertEqual(dev.state, dfumod.ST_ERROR)
        for state_ in (dfumod.ST_DNIDLE, dfumod.ST_UPIDLE):
            dev.state, dev.status = state_, 0
            del dev.requests[:]
            self.dfu(dev).to_idle()
            self.assertEqual((dev.state, dev.status), (dfumod.ST_IDLE, 0))
            self.assertEqual([r[1] for r in dev.requests], [dfumod.DFU_GETSTATUS, dfumod.DFU_ABORT,
                                                            dfumod.DFU_GETSTATUS])
        dev.state, dev.busy = dfumod.ST_IDLE, 1
        naps = []
        dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=naps.append).to_idle()
        self.assertEqual((dev.state, len(naps)), (dfumod.ST_IDLE, 1))
        self.assertFalse([r for r in dev.requests[-2:] if r[1] != dfumod.DFU_GETSTATUS], "nothing sent while busy")
        # a plain plan without that order is refused before any request
        dev, before = self.stock()
        q = plan.make_plan(b, [(b.app_base, img)], plan.KEEP)
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).run(q)
        self.assertIn("first and write it last", str(cm.exception))
        self.assertEqual((dev.requests, bytes(dev.flash)), ([], before))
        # nor one whose last step has no block to be the vector table: a refusal, not a crash
        _, q = self.image_plan()
        q.write_order[-1].blocks.clear()
        with self.assertRaises(dfumod.DfuError) as cm:
            self.dfu(dev).run(q)
        self.assertIn("first and write it last", str(cm.exception))
        self.assertEqual((dev.requests, bytes(dev.flash)), ([], before))

    def test_windows_driver_hint_names_the_board_s_bootloader(self):
        """On Windows a ROM bootloader that can't be opened is found in Zadig by its USB ID, not by a keyed
        bootloader's name; a keyed board's hint still names its bootloader"""
        class Denied(fakedfu.FakeUsb):
            def __init__(self, devices, errno_):
                super().__init__(devices)
                err = self.core.USBError("Access denied" if errno_ == 13 else "Resource busy")
                err.errno = errno_

                def claim(dev, intf):
                    raise err
                self.util.claim_interface = staticmethod(claim)

        keyed = fakedfu.FakeUsbDevice(DM1701, KEY)
        with mock.patch.object(dfumod.sys, "platform", "win32"):
            for errno_ in (13, 16):
                dev, _ = self.stock()
                with self.assertRaises(dfumod.DfuError) as cm:
                    dfumod.open_device(Denied([dev], errno_), dev, self.b)
                self.assertIn("the device with USB ID %04X %04X" % tuple(self.b.dfu_ids), str(cm.exception))
                self.assertNotIn("Digital Radio in USB mode", str(cm.exception))
                with self.assertRaises(dfumod.DfuError) as cm:
                    dfumod.open_device(Denied([keyed], errno_), keyed, DM1701)
                self.assertIn('"%s"' % DM1701.dfu_product, str(cm.exception))

        class NoStrings(fakedfu.FakeUsb):			# access denied already reading the strings
            def __init__(self, devices):
                super().__init__(devices)
                err = self.core.USBError("Access denied")
                err.errno = 13

                def get_string(dev, index):
                    raise err
                self.util.get_string = staticmethod(get_string)

        with mock.patch.object(dfumod.sys, "platform", "win32"):
            dev, _ = self.stock()
            with self.assertRaises(dfumod.DfuError) as cm:
                dfumod.fingerprint(NoStrings([dev]), dev, self.b)
            self.assertIn("the device with USB ID %04X %04X" % tuple(self.b.dfu_ids), str(cm.exception))
        # and the install's own device check and claim pass the board on: nothing sent, the right device named
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            with open(bp, "wb") as f:
                f.write(rom_image(9000))
            argv = ["flash", bp, "--board", self.b.id, "--yes", "--wait", "0", "--no-probe", "--no-verify",
                    "--no-reporting"]
            for usb_of in (NoStrings, lambda devs: Denied(devs, 13)):
                dev, before = self.stock()
                with mock.patch.object(dfumod, "import_usb", return_value=usb_of([dev])), \
                        mock.patch.object(dfumod.sys, "platform", "win32"), mock.patch.object(cli.time, "sleep"):
                    rc, out, err = run_cli(argv)
                self.assertNotEqual(rc, 0, out + err)
                self.assertIn("the device with USB ID %04X %04X" % tuple(self.b.dfu_ids), out + err)
                self.assertNotIn("Digital Radio in USB mode", out + err)
                self.assertEqual((dev.requests, bytes(dev.flash)), ([], before))
            # detect knows the device only with --board
            dev, _ = self.stock()
            with mock.patch.object(dfumod, "import_usb", return_value=NoStrings([dev])), \
                    mock.patch.object(dfumod.sys, "platform", "win32"):
                rc, out, err = run_cli(["detect", "--no-probe", "--board", self.b.id])
            self.assertIn("the device with USB ID %04X %04X" % tuple(self.b.dfu_ids), out + err)

    def test_unreadable_layout_is_not_called_another_chip(self):
        dev, _ = self.stock()
        dev.strings = dict(dev.strings, interfaces=[None, None])
        with self.assertRaises(cli.UsageError) as cm:
            cli.check_rom_loader(fakedfu.FakeUsb([dev]), dev, self.b, log=lambda s: None)
        self.assertIn("could not be read", str(cm.exception))
        self.assertNotIn("another chip", str(cm.exception))
        self.assertEqual(dev.requests, [])


class RestartIntoBootloader(unittest.TestCase):
    """A radio that runs RadioFoundry on a board whose firmware restarts into its processor's bootloader when the
    debug link asks (the registry's bootloader_restart): rfoundry asks it instead of asking for the keys, against
    fakedfu.FakeRunning. A refusal changes nothing and leads to the keys; only the radio that was probed is asked."""
    HINT = "Turn the knob one step and try again."

    def setUp(self):
        self.b = rom_board()
        # whatever the first board of the family says for itself: these tests are about a board that can
        for k, v in (("bootloader_restart", True), ("bootloader_restart_hint", self.HINT)):
            patch = mock.patch.object(self.b, k, v, create=True)
            patch.start()
            self.addCleanup(patch.stop)

    def radio(self, **kw):
        """(the running radio, its bootloader's device, the bus)"""
        dev = fakedfu.FakeRomUsbDevice(self.b, fakedfu.rom_flash(self.b, random.Random(5)))
        return fakedfu.FakeRunning(self.b, dev, **kw), dev, fakedfu.FakeUsb([dev])

    def restart(self, running, usb, probe=None, **kw):
        log, naps = [], []
        kw.setdefault("sleep", naps.append)
        got = cli.restart_into_bootloader(usb, self.b, running.probe() if probe is None else probe, log=log.append, **kw)
        return got, "\n".join(log), naps

    def test_registry(self):
        """Only a board installed through its processor's ROM bootloader says its firmware restarts into it, and a
        hint stands only beside that"""
        for b in boards.BOARDS.values():
            with mock.patch.object(b, "bootloader_restart", b.__dict__["bootloader_restart"]), \
                    mock.patch.object(b, "bootloader_restart_hint", b.__dict__["bootloader_restart_hint"]):
                self.assertIsInstance(b.bootloader_restart, bool, b.id)
                if b.bootloader_restart:
                    self.assertEqual(b.family, "dfuse", b.id)
                else:
                    self.assertIsNone(b.bootloader_restart_hint, b.id)
                self.assertTrue(b.bootloader_restart_hint is None or b.bootloader_restart_hint.strip(), b.id)

    def test_what_the_radio_answered(self):
        kinds = {(0, b""): "ok", (7, b"\x05"): "not-now", (7, b"\x03"): "transmitting", (7, b"\x02"): "protected",
                 (7, b"\x01"): "token", (7, b"\x04"): "no-chip", (7, b"\x09"): "other", (7, b""): "other",
                 (1, b""): "unsupported", (4, b""): "unsupported", (2, b""): "other", (3, b""): "other",
                 (5, b"\x0D"): "other"}
        for (status, data), kind in kinds.items():
            self.assertEqual(debuglink.restart_outcome(status, data), kind, (status, data))
        # every kind but the board's own "not now" and the unnamed one has its words
        self.assertEqual(set(debuglink.RESTART_WHY), set(kinds.values()) - {"ok", "not-now", "other"})
        self.assertEqual(debuglink.restart_request(0x12345678), bytes.fromhex("0178563412"))

    def test_link(self):
        running, dev, _ = self.radio()
        link = debuglink.Link(running)
        chip = link.chip()
        self.assertEqual((chip["uid"], chip["token"], chip["family"], chip["rdp"]),
                         (running.uid.hex(), struct.unpack_from("<I", running.uid)[0], debuglink.CHIP_STM32H7, 0))
        self.assertEqual(link.chip_uid(), chip["uid"])
        self.assertTrue(dev.gone)
        for reason, kind in ((5, "not-now"), (3, "transmitting"), (4, "no-chip"), (2, "protected"), (77, "other")):
            running.refuse = reason
            self.assertEqual(link.restart_into_bootloader(chip), kind)
            self.assertTrue(running.running and dev.gone, kind)		# a refusal changes nothing
        self.assertEqual(running.requests, [debuglink.restart_request(chip["token"])] * 5)
        self.assertEqual(link.restart_into_bootloader(dict(chip, token=chip["token"] ^ 1)), "token")
        # the host's half of the gate: a read-protected chip, or a family without a ROM bootloader, is never asked
        del running.requests[:]
        for bad in (dict(chip, rdp=1), dict(chip, rdp=2), dict(chip, family=1, rdp=None), dict(chip, family=9, rdp=None)):
            self.assertEqual(link.restart_into_bootloader(bad), "protected")
        self.assertEqual(running.requests, [])
        # and the radio's own half says the same when the host is wrong about it
        running.refuse, running.rdp = None, 1
        self.assertEqual(link.restart_into_bootloader(chip), "protected")
        self.assertEqual(link.chip()["rdp"], 1)
        running.rdp = 0
        # a refusal's status and reason travel with the error; no reply at all is an error of the link
        with self.assertRaises(debuglink.LinkError) as cm:
            link.request(debuglink.CMD_REBOOT, b"\x01")
        self.assertEqual((cm.exception.status, cm.exception.data), (2, b""))
        self.assertEqual(link.restart_into_bootloader(chip), "ok")
        self.assertFalse(running.running or dev.gone)
        with self.assertRaises(OSError):
            link.version()
        # the simulated radio checks in the firmware's order (core/debuglink/dl_chip.c dl_reboot_check, then the
        # platform's reboot_ok): the chip info, the token, transmitting, the protection, the board's rule last
        running, dev, _ = self.radio(refuse=4)
        link = debuglink.Link(running)
        self.assertEqual(link.restart_into_bootloader(dict(chip, token=chip["token"] ^ 1)), "no-chip")
        running.refuse, running.rdp = 3, 1
        self.assertEqual(link.restart_into_bootloader(dict(chip, token=chip["token"] ^ 1)), "token")
        self.assertEqual(link.restart_into_bootloader(chip), "transmitting")
        running.refuse = 5
        self.assertEqual(link.restart_into_bootloader(chip), "protected")
        running.rdp = 0
        self.assertEqual(link.restart_into_bootloader(chip), "not-now")
        # a mode the firmware doesn't have is a bad argument; the board's rule is about the bootloader mode only, so
        # a normal reset is taken: the status and reason 0, and the firmware starts again, not the bootloader
        with self.assertRaises(debuglink.LinkError) as cm:
            link.request(debuglink.CMD_REBOOT, struct.pack("<BI", 2, chip["token"]))
        self.assertEqual((cm.exception.status, cm.exception.data), (2, b""))
        self.assertTrue(running.running and dev.gone)
        self.assertEqual(link.request(debuglink.CMD_REBOOT, struct.pack("<BI", 0, chip["token"])), b"\x00")
        self.assertFalse(running.running)
        self.assertTrue(dev.gone)
        # a firmware that doesn't know the command, or was built without it
        for status in (debuglink.ST_UNKNOWN, debuglink.ST_UNAVAILABLE):
            old, dev2, _ = self.radio()
            answer = old._answer
            old._answer = lambda cmd, payload: bytes([status]) if cmd == debuglink.CMD_REBOOT else answer(cmd, payload)
            self.assertEqual(debuglink.Link(old).restart_into_bootloader(chip), "unsupported")
            self.assertTrue(old.running and dev2.gone)

    def test_restarts_and_finds_the_bootloader(self):
        running, dev, usb = self.radio()
        got, log, naps = self.restart(running, usb)
        self.assertIs(got, dev)
        self.assertEqual(running.requests, [debuglink.restart_request(struct.unpack_from("<I", running.uid)[0])])
        self.assertIn("restarting it into its bootloader over the debug link", log)
        self.assertIn("in its bootloader: leave it on and connected", log)
        self.assertNotIn(self.HINT, log)
        self.assertEqual(naps, [])						# it was there at the first look

    def test_bootloader_that_takes_a_moment(self):
        running, dev, usb = self.radio()
        real = dev.power_cycle
        late = []

        def slow():
            late.append(real)

        def nap(s):
            if late and len(naps) == 3:
                late.pop()()
            naps.append(s)

        naps = []
        with mock.patch.object(dev, "power_cycle", slow):
            got, log, _ = self.restart(running, usb, sleep=nap)
        self.assertIs(got, dev)
        self.assertEqual(naps, [0.5] * 4)

    def test_refused_by_the_board(self):
        """Reason 5: the board's hint, nothing changed, and the keys"""
        running, dev, usb = self.radio(refuse=5)
        got, log, naps = self.restart(running, usb)
        self.assertIsNone(got)
        self.assertIn("did not restart into its bootloader: it can't right now. Nothing was changed", log)
        self.assertIn(self.HINT, log)
        self.assertIn("run the same command", log)
        self.assertTrue(log.rstrip().endswith("enter the bootloader with its keys:"))
        self.assertTrue(running.running and dev.gone)
        self.assertEqual((len(running.requests), naps), (1, []))
        # a board without a hint of its own says the rest all the same
        with mock.patch.object(self.b, "bootloader_restart_hint", None):
            got, log, _ = self.restart(running, usb)
        self.assertIsNone(got)
        self.assertIn("it can't right now", log)
        self.assertNotIn(self.HINT, log)

    def test_refused_then_tried_again(self):
        """With somebody to ask: the user does what the hint says and the next request is taken; "no" ends it"""
        running, dev, usb = self.radio(refuse=5)
        asked = []

        def turn_the_knob(question, default=False):
            asked.append((question, default))
            if len(asked) == 2:
                running.refuse = None
            return True

        got, log, _ = self.restart(running, usb, ask=turn_the_knob)
        self.assertIs(got, dev)
        self.assertEqual(asked, [("Try the restart again?", True)] * 2)
        self.assertEqual(len(running.requests), 3)
        self.assertEqual(log.count(self.HINT), 2)
        self.assertNotIn("run the same command", log)
        running, dev, usb = self.radio(refuse=5)
        got, log, _ = self.restart(running, usb, ask=lambda q, default=False: False)
        self.assertIsNone(got)
        self.assertEqual(len(running.requests), 1)
        self.assertIn("enter the bootloader with its keys", log)
        self.assertTrue(running.running and dev.gone)

    def test_other_refusals_are_said_as_what_they_are(self):
        for kw, words in (({"refuse": 3}, "the radio is transmitting"), ({"rdp": 1}, "its processor is read protected"),
                          ({"refuse": 4}, "the radio could not read its processor's identity"),
                          ({"refuse": 77}, "it answered the request with an error")):
            running, dev, usb = self.radio(**kw)
            ask = mock.Mock(return_value=True)
            got, log, _ = self.restart(running, usb, ask=ask)
            self.assertIsNone(got, words)
            self.assertIn("did not restart into its bootloader: " + words, log)
            self.assertIn("Nothing was changed: enter the bootloader with its keys", log)
            self.assertNotIn(self.HINT, log)					# the hint is for the board's rule only
            ask.assert_not_called()
            self.assertTrue(running.running and dev.gone, words)

    def test_only_the_radio_that_was_probed(self):
        running, dev, usb = self.radio()
        probe = running.probe()
        other = next(b for b in boards.BOARDS.values() if b.id != self.b.id)
        for what, p, says in (("no probe", None, ""), ("a remembered probe", dict(probe, age_s=60), ""),
                              ("another model", dict(probe, board=other.debuglink_names[0]), "it is not restarted"),
                              ("a name no board has", dict(probe, board="NOSUCH"), "it is not restarted"),
                              ("another radio of the model", dict(probe, uid="00" * 12), "another radio answers")):
            if p is None:
                got, log = cli.restart_into_bootloader(usb, self.b, None, log=self.fail), ""
            else:
                got, log, _ = self.restart(running, usb, probe=p)
            self.assertIsNone(got, what)
            self.assertIn(says, log, what)
            self.assertEqual(running.requests, [], what)
            self.assertTrue(running.running and dev.gone, what)
        # the radio on the port now reports another board than the one probed
        running.name = other.debuglink_names[0]
        got, log, _ = self.restart(running, usb, probe=probe)
        self.assertIsNone(got)
        self.assertIn("another radio answers", log)
        self.assertEqual(running.requests, [])
        # a radio that is another model, or one no board knows, and says so at the probe and on the port: the board
        # asked for is not the one connected (--board names the image's board, not the radio's)
        for name in (other.debuglink_names[0], "NOSUCH"):
            running, dev, usb = self.radio(name=name)
            got, log, _ = self.restart(running, usb)
            self.assertIsNone(got, name)
            self.assertIn("reports board %r, not the %s: it is not restarted" % (name, self.b.name), log)
            self.assertEqual(running.requests, [], name)
            self.assertTrue(running.running and dev.gone, name)
        # a board whose firmware does not do it is never asked, whatever answers
        running, dev, usb = self.radio()
        with mock.patch.object(self.b, "bootloader_restart", False):
            self.assertIsNone(cli.restart_into_bootloader(usb, self.b, running.probe(), log=self.fail))
        self.assertEqual(running.requests, [])
        # nor is any radio while a bootloader is already on USB: the user put one there
        dev.gone = False
        self.assertIsNone(cli.restart_into_bootloader(usb, self.b, running.probe(), log=self.fail))
        self.assertEqual(running.requests, [])
        self.assertTrue(running.running)

    def test_several_radios_on_the_debug_link(self):
        """The probe takes the first radio that answers. With more than one connected that is rfoundry's pick, not
        the user's: none is restarted unless the user named the port"""
        running, dev, usb = self.radio()
        got, log, _ = self.restart(running, usb, links=lambda: ["/dev/ttyA", "udp:12.0.0.2"])
        self.assertIsNone(got)
        self.assertIn("more than one radio that runs RadioFoundry is connected (/dev/ttyA, udp:12.0.0.2): none is "
                      "restarted", log)
        self.assertIn("--port", log)
        self.assertTrue(log.rstrip().endswith("enter the bootloader with its keys:"))
        self.assertEqual(running.requests, [])
        self.assertTrue(running.running and dev.gone)
        # one radio, or a host that lists none (the probe found it all the same): it is the one
        for found in (["/dev/ttyA"], []):
            running, dev, usb = self.radio()
            got, log, _ = self.restart(running, usb, links=lambda found=found: found)
            self.assertIs(got, dev, found)
        # the list is the serial ports and the radios in USB network mode
        with mock.patch.object(debuglink, "find_ports", return_value=["/dev/ttyA"]), \
                mock.patch.object(debuglink, "find_network_radios", return_value=["udp:12.0.0.2"]):
            self.assertEqual(cli.debug_links(), ["/dev/ttyA", "udp:12.0.0.2"])
        with mock.patch.object(debuglink, "find_ports", return_value=None), \
                mock.patch.object(debuglink, "find_network_radios", return_value=[]):
            self.assertEqual(cli.debug_links(), [])		# no pyserial

    def test_real_run_several_radios(self):
        """rfoundry flash with two radios on the debug link: the keys, unless --port names the radio"""
        running, dev, usb = self.radio()

        def user(s):
            if running.running:
                running.enter_by_hand()

        two = [self.PORT, "/dev/ttyFAKE1"]
        rc, out, err, _ = self.real_run(running, usb, "--wait", "5", user=user, links=two)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(running.requests, [])
        self.assertIn("more than one radio that runs RadioFoundry is connected (%s): none is restarted" % ", ".join(two),
                      out)
        self.assertLess(out.index("none is restarted"), out.index("Put the %s in its bootloader now" % self.b.name))
        self.assertEqual(dev.read(self.b.app_base, len(self.img)), self.img)
        running, dev, usb = self.radio()
        with mock.patch.object(cli, "wait_for_dfu", side_effect=AssertionError("the keys were asked for")):
            rc, out, err, _ = self.real_run(running, usb, "--port", self.PORT, links=AssertionError("listed"))
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(len(running.requests), 1)
        self.assertNotIn("more than one", out)

    def test_a_port_that_stopped_answering(self):
        running, dev, usb = self.radio()
        running.running = False							# the cable went since the probe
        got, log, _ = self.restart(running, usb)
        self.assertIsNone(got)
        self.assertIn("could not ask it to restart into its bootloader", log)
        self.assertIn("enter the bootloader with its keys", log)
        self.assertTrue(dev.gone)

    def test_took_the_restart_and_never_showed(self):
        """The wait is bounded, and the words say what is known: the radio stayed up, or it left and nothing came"""
        running, dev, usb = self.radio(stays=True)
        got, log, naps = self.restart(running, usb, wait_s=7)
        self.assertIsNone(got)
        self.assertEqual(naps, [0.5] * 14)
        self.assertIn("took the restart, but no bootloader came onto USB within 7 s", log)
        self.assertIn("it still runs RadioFoundry", log)
        self.assertIn("Now enter the bootloader with its keys:", log)
        self.assertEqual(len(running.requests), 1)
        self.assertEqual(cli.RESTART_WAIT_S, 20)
        running, dev, usb = self.radio()
        with mock.patch.object(dev, "power_cycle"):				# it left, and no device came
            got, log, naps = self.restart(running, usb, wait_s=2)
        self.assertIsNone(got)
        self.assertEqual(len(naps), 4)
        self.assertIn("no bootloader came onto USB within 2 s", log)
        self.assertNotIn("still runs RadioFoundry", log)
        self.assertNotIn("Zadig", log)
        self.assertFalse(running.running)
        # on Windows a bootloader without its driver is not listed: the radio left, so the driver's steps are said,
        # by this board's device; a radio that stayed up needs no driver
        for stays in (False, True):
            running, dev, usb = self.radio(stays=stays)
            with mock.patch.object(dev, "power_cycle"), mock.patch.object(cli.sys, "platform", "win32"):
                got, log, _ = self.restart(running, usb, wait_s=1)
            self.assertIsNone(got)
            self.assertEqual(dfumod.windows_driver_steps(self.b) in log, not stays)

    def cli_run(self, *argv):
        made = []
        real = fakedfu.FakeRunning

        class Spy(real):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                made.append(self)

        with mock.patch.object(fakedfu, "FakeRunning", Spy):
            rc, out, err = run_cli(["flash", "--board", self.b.id] + list(argv))
        return rc, out, err, (made[0] if made else None)

    def test_simulation_shows_both(self):
        rc, out, err, running = self.cli_run("--simulate", "rfoundry-nocodec")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("restarting it into its bootloader over the debug link", out)
        self.assertIn("in its bootloader: leave it on and connected", out)
        self.assertNotIn("with its keys", out)
        self.assertIn("sim      PASS", out)
        self.assertEqual(len(running.requests), 1)
        rc, out, err, running = self.cli_run("--simulate", "rfoundry-refuses")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("it can't right now. Nothing was changed", out)
        self.assertIn(self.HINT, out)
        self.assertIn("(simulated) switched off and put in its bootloader with its keys", out)
        self.assertLess(out.index(self.HINT), out.index("found    bootloader"))
        self.assertIn("sim      PASS", out)
        self.assertEqual(len(running.requests), 1)
        # a radio that doesn't run RadioFoundry is not asked, and a dry run asks nobody
        for argv in (("--simulate", "blank"), ("--simulate", "stock", "--unprotect"),
                     ("--simulate", "rfoundry-nocodec", "--dry-run")):
            rc, out, err, running = self.cli_run(*argv)
            self.assertEqual(rc, 0, out + err)
            self.assertIsNone(running, argv)
            self.assertNotIn("restart", out, argv)

    def test_simulation_without_the_restart(self):
        """A board whose firmware does not restart into its bootloader installs as before, and has no refusal to show"""
        keyed = next(b for _, b in sorted(boards.BOARDS.items()) if b.family == "usb-dfu")
        with mock.patch.object(self.b, "bootloader_restart", False):
            rc, out, err, running = self.cli_run("--simulate", "rfoundry-nocodec")
            self.assertEqual(rc, 0, out + err)
            self.assertIsNone(running)
            self.assertNotIn("restart", out)
            self.assertIn("sim      PASS", out)
            rc, out, err, running = self.cli_run("--simulate", "rfoundry-refuses")
            self.assertEqual(rc, 2, out + err)
            self.assertIn("--simulate rfoundry-refuses is for a radio whose firmware restarts", err)
        rc, out, err = run_cli(["flash", "--board", keyed.id, "--simulate", "rfoundry-refuses", "--yes"])
        self.assertEqual(rc, 2, out + err)
        self.assertIn("--simulate rfoundry-refuses is for", err)

    PORT = "/dev/ttyFAKE0"

    @classmethod
    def PORT_PROBE(cls, running):
        """The running radio's probe as debuglink.probe() gives it: the port by name"""
        return dict(running.probe(), port=cls.PORT)

    def on_port(self, running):
        """The running radio behind the port's name: rfoundry opens the port it probed, no other"""
        link = debuglink.Link

        def open_(port, *a, **kw):
            self.assertEqual(port, self.PORT)
            return link(running, *a, **kw)

        return mock.patch.object(debuglink, "Link", side_effect=open_)

    def real_run(self, running, usb, *extra, probe="live", user=None, links=None):
        """rfoundry flash against the fakes, not simulated: the probe is the running radio's. links: the debug links
        the host lists (the probed port alone unless given; an exception when listing them is a failure)"""
        listed = mock.patch.object(cli, "debug_links", side_effect=links if isinstance(links, Exception) else None,
                                   return_value=[self.PORT] if links is None else links)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            self.img = rom_image(9000)
            with open(bp, "wb") as f:
                f.write(self.img)
            argv = ["flash", bp, "--board", self.b.id, "--yes", "--no-verify", "--no-reporting"] + list(extra)
            with self.on_port(running), mock.patch.object(dfumod, "import_usb", return_value=usb), listed, \
                    mock.patch.object(cli.time, "sleep", side_effect=user), \
                    mock.patch.object(debuglink, "probe", return_value=self.PORT_PROBE(running) if probe == "live"
                                      else probe) as probed:
                rc, out, err = run_cli(argv)
        return rc, out, err, probed

    def test_real_run_restarts_the_radio(self):
        running, dev, usb = self.radio()
        with mock.patch.object(cli, "wait_for_dfu", side_effect=AssertionError("the keys were asked for")):
            rc, out, err, _ = self.real_run(running, usb)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(len(running.requests), 1)
        self.assertLess(out.index("restarting it into its bootloader"), out.index("found    bootloader"))
        self.assertNotIn("Put the", out)
        self.assertEqual(dev.read(self.b.app_base, len(self.img)), self.img)
        self.assertTrue(dev.exited)
        self.assertEqual((dev.unprotected, dev.alt_selected, dev.refused, dev.partial, dev.violations),
                         (False, [], [], [], []))

    def test_real_run_radio_in_usb_network_mode(self):
        """A radio in USB network mode was probed over UDP: the restart is asked there, on the address probed"""
        running, dev, usb = self.radio()
        with mock.patch.object(type(self), "PORT", "udp:12.52.86.121"), \
                mock.patch.object(cli, "wait_for_dfu", side_effect=AssertionError("the keys were asked for")):
            rc, out, err, _ = self.real_run(running, usb)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("RadioFoundry running on udp:12.52.86.121", out)
        self.assertEqual(len(running.requests), 1)
        self.assertEqual(dev.read(self.b.app_base, len(self.img)), self.img)

    def test_real_run_same_requests_as_by_hand(self):
        """Nothing about the install changes: the bootloader gets the same requests after a restart as after the keys"""
        running, dev, usb = self.radio()
        rc, out, err, _ = self.real_run(running, usb)
        self.assertEqual(rc, 0, out + err)
        hand = fakedfu.FakeRomUsbDevice(self.b, fakedfu.rom_flash(self.b, random.Random(5)))
        rc, out, err, probed = self.real_run(running, fakedfu.FakeUsb([hand]), "--no-probe")
        self.assertEqual(rc, 0, out + err)
        probed.assert_not_called()
        self.assertEqual(dev.requests, hand.requests)
        self.assertEqual(bytes(dev.flash), bytes(hand.flash))

    def test_real_run_refused_goes_on_with_the_keys(self):
        running, dev, usb = self.radio(refuse=5)

        def user(s):								# off, the key, on
            if running.running:
                running.enter_by_hand()

        rc, out, err, _ = self.real_run(running, usb, "--wait", "5", user=user)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(len(running.requests), 1)
        self.assertIn(self.HINT, out)
        hint, steps = out.index(self.HINT), out.index("Put the %s in its bootloader now: %s" % (
            self.b.name, cli.bootloader_steps(self.b)))
        self.assertLess(hint, steps)
        self.assertLess(steps, out.index("found    bootloader"))
        self.assertEqual(dev.read(self.b.app_base, len(self.img)), self.img)
        self.assertTrue(dev.exited)

    def test_real_run_without_a_radio_on_the_debug_link(self):
        """--no-probe, no radio found, a remembered probe: exactly what it was, the keys"""
        for extra, probe in ((["--no-probe"], "live"), ([], None)):
            running, dev, usb = self.radio()

            def user(s, running=running):
                if running.running:
                    running.enter_by_hand()

            state.forget_probe()
            rc, out, err, probed = self.real_run(running, usb, "--wait", "5", *extra, probe=probe, user=user)
            self.assertEqual(rc, 0, out + err)
            self.assertEqual(running.requests, [], extra)
            self.assertNotIn("restart", out, extra)
            self.assertIn("Put the %s in its bootloader now" % self.b.name, out)
            self.assertEqual(probed.called, not extra)
            self.assertEqual(dev.read(self.b.app_base, len(self.img)), self.img)

    def test_real_run_no_show_goes_on_with_the_keys(self):
        running, dev, usb = self.radio(stays=True)
        naps = []

        def user(s):
            naps.append(s)
            if len(naps) > 2 * cli.RESTART_WAIT_S and dev.gone:		# only once rfoundry asked for the keys
                running.enter_by_hand()

        rc, out, err, _ = self.real_run(running, usb, "--wait", "5", user=user)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("took the restart, but no bootloader came onto USB within %d s" % cli.RESTART_WAIT_S, out)
        self.assertLess(out.index("took the restart"), out.index("Put the %s in its bootloader now" % self.b.name))
        n = 2 * cli.RESTART_WAIT_S + 1						# the bounded wait, then one for the keys
        self.assertEqual((naps[:n], 0.5 in naps[n:]), ([0.5] * n, False))
        self.assertEqual(dev.read(self.b.app_base, len(self.img)), self.img)

    def test_declined_install_says_where_the_radio_is(self):
        running, dev, usb = self.radio()
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            with open(bp, "wb") as f:
                f.write(rom_image(9000))
            with self.on_port(running), mock.patch.object(dfumod, "import_usb", return_value=usb), \
                    mock.patch.object(cli.time, "sleep"), mock.patch.object(cli, "debug_links", return_value=[self.PORT]), \
                    mock.patch.object(debuglink, "probe", return_value=self.PORT_PROBE(running)), \
                    mock.patch.object(cli, "ask_yes", return_value=False) as ask:
                rc, out, err = run_cli(["flash", bp, "--board", self.b.id, "--no-verify", "--no-reporting"])
        self.assertEqual(rc, 1, out + err)
        ask.assert_called_once()						# the write's question; not a tty: no "again?"
        self.assertIn("nothing done (the radio is in its bootloader: switch it off and on to start it normally)", out)
        self.assertEqual((dev.erased, dev.blocks), ([], 0))


class BootKeyFromRadio(unittest.TestCase):
    """The key read from the bootloader over READ_MEM, found only by its hash."""

    class Radio:
        def __init__(self, flash, base=0x08000000, refuse=False):
            self.flash, self.base, self.refuse, self.reads = flash, base, refuse, 0

        def request(self, cmd, payload=b""):
            if self.refuse:
                raise debuglink.LinkError("command 0x%02X: status 1" % cmd)
            assert cmd == debuglink.CMD_READ_MEM
            addr, n = struct.unpack("<IH", payload)
            assert 1 <= n <= 1000
            self.reads += 1
            return struct.pack("<I", addr) + self.flash[addr - self.base:addr - self.base + n]

        read_mem = debuglink.Link.read_mem

    def setUp(self):
        r = random.Random(5)
        self.key = bytes(r.getrandbits(8) for _ in range(stockfw.KEY_LEN))
        self.board = boards.get("dm1701")
        self.board_key = collections.namedtuple("B", "key_sha256 protected")(
            hashlib.sha256(self.key).hexdigest(), self.board.protected)
        self.boot = bytearray(r.getrandbits(8) for _ in range(0xC000))

    def test_found_anywhere_by_hash(self):
        for off in (0x2340, 0x44B9):
            boot = bytearray(self.boot)
            boot[off:off + len(self.key)] = self.key
            radio = self.Radio(bytes(boot))
            self.assertEqual(bootkey.read_key(radio, self.board_key), self.key)
            self.assertEqual(radio.reads, 50)	# 48 KB in 1000-byte pieces

    def test_nothing_without_a_match_or_read_mem(self):
        self.assertIsNone(bootkey.read_key(self.Radio(bytes(self.boot)), self.board_key))
        self.assertIsNone(bootkey.read_key(self.Radio(b"", refuse=True), self.board_key))
        self.assertIsNone(bootkey.read_key(self.Radio(bytes(self.boot)), boards.get("dm32")))


class CodecFromRadio(unittest.TestCase):
    """The codec read back over READ_MEM (the MD-380's stock windows), checked window by window, saved and loaded again;
    and the decision that uses it."""

    def setUp(self):
        r = random.Random(11)
        self.board = boards.get("md380")
        self.flash = bytearray(r.getrandbits(8) for _ in range(0x100000))
        wins = []
        for a, n in [(0x08050C44, 0x400), (0x0808FC64, 0x200), (0x080D0FC8, 0x24)]:
            o = a - 0x08000000
            if not wins:
                self.flash[o:o + 4] = struct.pack("<I", stockfw.CODEC_SIG)
            wins.append((a, n, hashlib.sha256(self.flash[o:o + n]).hexdigest()))
        prof = {"board": "md380", "codec_profile": self.board.codec.id, "code": wins[0], "tables": wins[1],
                "extra": wins[2:]}
        self.profiles = mock.patch.dict(stockfw.PROFILES, {"f" * 64: prof}, clear=True)
        self.profiles.start()
        self.addCleanup(self.profiles.stop)
        self.tmp = tempfile.mkdtemp()

    def radio(self, flash=None, refuse=False):
        return BootKeyFromRadio.Radio(bytes(flash if flash is not None else self.flash), refuse=refuse)

    def test_read_save_load(self):
        prof, segs = codecsave.read_codec(self.radio(), self.board)
        self.assertEqual([a for a, _ in segs], [0x08050C44, 0x0808FC64, 0x080D0FC8])
        codecsave.save(self.board, segs, self.tmp)
        self.assertEqual(codecsave.load(self.board, self.tmp), (prof, segs))

    def test_a_changed_byte_is_refused(self):
        bad = bytearray(self.flash)
        bad[0x0808FC64 - 0x08000000 + 7] ^= 1
        self.assertIsNone(codecsave.read_codec(self.radio(bad), self.board))
        self.assertIsNone(codecsave.read_codec(self.radio(refuse=True), self.board))
        _, segs = codecsave.read_codec(self.radio(), self.board)
        p = codecsave.save(self.board, segs, self.tmp)
        with open(p, "r+b") as f:
            f.seek(5)
            f.write(b"\x00")
        self.assertIsNone(codecsave.load(self.board, self.tmp))
        self.assertIsNone(codecsave.load(self.board, tempfile.mkdtemp()))

    def with_decoys(self, n):
        """n profiles ahead of the real one whose windows don't match this flash, so the read tries n+1 layouts."""
        real = next(iter(stockfw.PROFILES.values()))
        decoys = {}
        for i in range(n):
            wins = [(a + 0x10 * (i + 1), l, "0" * 64) for a, l, _ in stockfw.windows(real)]
            decoys["%064d" % i] = dict(real, code=wins[0], tables=wins[1], extra=wins[2:])
        return mock.patch.dict(stockfw.PROFILES, {**decoys, **stockfw.PROFILES}, clear=True)

    def test_progress_is_one_call_per_window(self):
        seen = []
        with self.with_decoys(2):
            prof, segs = codecsave.read_codec(self.radio(), self.board, lambda: seen.append(1))
        self.assertEqual(len(seen), 3 * len(segs))	# three layouts tried, the last one matched
        with self.with_decoys(2):	# no callback: the same answer
            self.assertEqual(codecsave.read_codec(self.radio(), self.board), (prof, segs))

    def test_read_progress_output(self):
        def run():
            out = io.StringIO()
            args = collections.namedtuple("A", "no_probe port")(False, None)
            probe = {"board": "MD-380", "codec": self.board.codec.id}
            radio = self.radio()
            with self.with_decoys(7), contextlib.redirect_stdout(out), \
                    mock.patch.object(codecsave, "load", return_value=None), \
                    mock.patch.object(codecsave, "save", return_value="x"), \
                    mock.patch.object(debuglink, "find_ports", return_value=["fake"]), \
                    mock.patch.object(debuglink, "Link", return_value=mock.Mock(read_mem=radio.read_mem, close=lambda: None)):
                self.assertIsNotNone(cli.get_saved_codec(args, self.board, probe))
            return out.getvalue()

        lines = run().splitlines()
        self.assertIn("can take a few minutes", lines[0])
        self.assertIn("a read", lines[0])
        self.assertEqual(lines[1], "reading  " + "." * 24)	# 8 layouts of 3 windows, one dot each, on one line
        self.assertIn("read back from the radio", lines[2])

    def test_decision_uses_the_copy(self):
        man = {"codec": {"profile": self.board.codec.id}}
        d = cli.decide_codec(self.board, man, "auto", False, None, None, None, have_saved=True)
        self.assertEqual((d.mode, d.source), (plan.MERGE, "saved"))
        self.assertEqual(cli.decide_codec(self.board, man, "auto", True, None, None, None).source, "stock")
        with self.assertRaises(cli.UsageError):
            cli.decide_codec(self.board, man, "auto", False, None, None, None)


class CodecRelayout(unittest.TestCase):
    """A radio whose codec is where an earlier release put it (stockfw "former": the MD-380's in sectors 8-11, the
    MD-9600's tables in sector 9), in flash this release's application uses: read back and checked before any erase,
    saved, moved by the install; an install cut at any step is finished by running it again from the saved copy, and
    the bootloader is never written (docs/spec/codec-placement.md, "Moving to a new layout")."""

    BOARDS = (("md380", None), ("md9600", None))

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self._profiles = None
        self.addCleanup(lambda: self._profiles and self._profiles.stop())

    def radio_with_former(self, board_id, sha, seed=5):
        """(board, profile, flash with an old application and the codec in its former layout, current segments)"""
        b = boards.get(board_id)
        if self._profiles:
            self._profiles.stop()
            self._profiles = None
        sha = sha or stockfw.profile_for_board(board_id)
        stock, profiles, _, _ = stockfw.build_synthetic_stock(KEY, random.Random(seed), sha)
        self._profiles = mock.patch.dict(stockfw.PROFILES, profiles, clear=True)
        self._profiles.start()
        prof, segs = stockfw.extract(stock, KEY)
        former = stockfw.forms(prof)[1]
        old = dict(prof, placement=former["placement"], placed=former["placed"])
        del old["former"]
        with mock.patch.dict(stockfw.PROFILES, {stockfw.sha256(stock): old}, clear=True):
            _, osegs = stockfw.extract(stock, KEY)
        flash = fakedfu.stock_flash(b, random.Random(seed + 1), [])
        lo, hi = b.app_region
        flash[lo - 0x08000000:0x08080000 - 0x08000000] = app_image(0x08080000 - lo, seed + 2)	# an old release
        flash[0x08080000 - 0x08000000:] = b"\xFF" * (0x08100000 - 0x08080000)
        for a, d in osegs:
            flash[a - 0x08000000:a - 0x08000000 + len(d)] = d
        return b, prof, flash, segs

    def test_former_layout_overlaps_the_application(self):
        for bid, sha in self.BOARDS:
            b = boards.get(bid)
            self.assertTrue(b.codec_former, bid)
            for h in b.codec_stock:
                prof = stockfw.PROFILES[h]
                former = stockfw.forms(prof)[1]
                cur = stockfw.placed_windows(prof)
                old = stockfw.placed_windows(prof, former)
                self.assertTrue(all(a >= b.app_region[1] for a, _, _ in cur), prof["name"])
                self.assertTrue(any(a < b.app_region[1] for a, _, _ in old), prof["name"])
                self.assertEqual([n for _, n, _ in cur], [n for _, n, _ in old], prof["name"])

    def test_read_back_from_the_former_layout(self):
        for bid, sha in self.BOARDS:
            with self.subTest(bid):
                b, prof, flash, segs = self.radio_with_former(bid, sha)
                radio = BootKeyFromRadio.Radio(bytes(flash))
                got = codecsave.read_codec(radio, b)
                self.assertIsNotNone(got)
                self.assertEqual(got[0], prof)
                self.assertEqual(codecsave.segments(*got), segs)
                # saved under the id the old release reported (a copy from before this release), found by load
                p = codecsave.path(b, self.tmp, b.codec_former[0])
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb") as f:
                    f.write(b"".join(d for _, d in got[1]))
                self.assertEqual(codecsave.load(b, self.tmp), got)
                os.remove(p)
                # one copy per radio: a copy saved from radio A is never what radio B is moved from
                pa = codecsave.save(b, got[1], self.tmp, uid="00aa11bb22cc33dd44ee55ff")
                self.assertTrue(pa.endswith("-00aa11bb22cc33dd44ee55ff.bin"), pa)
                self.assertEqual(codecsave.find(b, self.tmp, uid="00AA11BB22CC33DD44EE55FF").path, pa)
                self.assertIsNone(codecsave.find(b, self.tmp, uid="99aa11bb22cc33dd44ee55ff", strict=True))
                self.assertIsNone(codecsave.find(b, self.tmp, uid="99aa11bb22cc33dd44ee55ff"))
                self.assertEqual(codecsave.find(b, self.tmp).path, pa)		# no UID known: the newest copy
                with open(os.path.join(os.path.dirname(pa), "notes.bin"), "wb") as f:
                    f.write(b"".join(d for _, d in got[1]))		# not a copy's name: never picked up
                self.assertEqual([c.path for c in codecsave.copies(b, self.tmp)], [pa])
                os.remove(pa)
                # a damaged byte anywhere in the old windows: nothing comes back, so nothing is written from it
                for a, d in got[1]:
                    bad = bytearray(flash)
                    bad[a - 0x08000000 + len(d) // 2] ^= 0x10
                    self.assertIsNone(codecsave.read_codec(BootKeyFromRadio.Radio(bytes(bad)), b), hex(a))

    def test_decision(self):
        for bid, sha in self.BOARDS:
            b = boards.get(bid)
            man = {"codec": {"profile": b.codec.id}}
            old = {"board": b.debuglink_names[0], "codec": b.codec_former[0]}
            dc = lambda *a, **k: cli.decide_codec(b, man, *a, **k)	# noqa: E731
            d = dc("auto", False, old, None, None, have_saved=True)
            self.assertEqual((d.mode, d.source), (plan.MERGE, "saved"))
            self.assertIn("moves", d.why)
            self.assertEqual(dc("auto", True, old, None, None).source, "stock")
            with self.assertRaises(cli.UsageError):		# no copy, no stock file: refused before anything is written
                dc("auto", False, old, None, None)
            d = dc("keep", False, old, None, None, have_saved=True)
            self.assertEqual(d.mode, plan.KEEP)
            self.assertFalse(d.expect_codec)
            self.assertTrue(d.risk)
            # the radio already moved: kept
            self.assertEqual(dc("auto", False, dict(old, codec=b.codec.id), None, None).mode, plan.KEEP)

    def test_read_back_before_anything_else(self):
        """A probe naming the former layout reads the codec back and saves it, even where an update keeps the codec,
        and even with a copy saved earlier: the radio's own codec, now, is what the move writes"""
        for bid, _ in self.BOARDS:
            b = boards.get(bid)
            args = collections.namedtuple("A", "no_probe port")(False, None)
            got = ("prof", [])
            old = codecsave.Copy("old.bin", None, "old prof", [])
            for have in (None, old):
                back = codecsave.Copy("x", "00aa11bb22cc33dd44ee55ff", *got)	# the copy as saved, read again
                with mock.patch.object(codecsave, "find", side_effect=[have, back]), \
                        mock.patch.object(codecsave, "from_radio", return_value=got) as rd, \
                        mock.patch.object(codecsave, "save", return_value="x") as sv, \
                        contextlib.redirect_stdout(io.StringIO()):
                    r = cli.get_saved_codec(args, b, {"codec": b.codec_former[0], "uid": "00aa11bb22cc33dd44ee55ff"})
                    self.assertEqual((r, r.origin, r.path), (got, "radio", "x"))
                    self.assertEqual((rd.call_count, sv.call_count), (1, 1))
                    self.assertEqual(rd.call_args.kwargs["uid"], "00aa11bb22cc33dd44ee55ff")	# only that radio is read
                    self.assertEqual(sv.call_args.kwargs["uid"], "00aa11bb22cc33dd44ee55ff")
            # the copy didn't land on disk intact: a move cut short couldn't be finished from it, so nothing is written
            for fail in ([None, None], [None, codecsave.Copy("x", None, "prof", [(1, b"x")])]):
                with mock.patch.object(codecsave, "find", side_effect=fail), \
                        mock.patch.object(codecsave, "from_radio", return_value=got), \
                        mock.patch.object(codecsave, "save", return_value="x"), contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaises(cli.UsageError) as e:
                        cli.get_saved_codec(args, b, {"codec": b.codec_former[0], "uid": "00aa11bb22cc33dd44ee55ff"})
                    self.assertIn("nothing was written", str(e.exception))
            with mock.patch.object(codecsave, "find", return_value=None), \
                    mock.patch.object(codecsave, "from_radio", return_value=got), \
                    mock.patch.object(codecsave, "save", side_effect=OSError("disk full")), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(cli.UsageError):
                    cli.get_saved_codec(args, b, {"codec": b.codec_former[0], "uid": "00aa11bb22cc33dd44ee55ff"})
            # a probe remembered from before the radio went into its bootloader: the copy saved from it
            with mock.patch.object(codecsave, "find", return_value=old) as fd, \
                    mock.patch.object(codecsave, "from_radio") as rd, contextlib.redirect_stdout(io.StringIO()) as out:
                r = cli.get_saved_codec(args, b, {"codec": b.codec_former[0], "age_s": 60})
                self.assertEqual((r, r.origin, r.path), (("old prof", []), "saved", "old.bin"))
                self.assertEqual(rd.call_count, 0)
                self.assertTrue(fd.call_args.kwargs["strict"])
                self.assertIn("old.bin", out.getvalue())
            with mock.patch.object(codecsave, "find", return_value=None), \
                    mock.patch.object(codecsave, "from_radio", return_value=None), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
                self.assertIsNone(cli.get_saved_codec(args, b, {"codec": b.codec_former[0]}))
                self.assertIn("could not be read back", err.getvalue())
            # copies from other radios only: none is used, and the user is told why
            with mock.patch.object(codecsave, "find", return_value=None), \
                    mock.patch.object(codecsave, "copies", return_value=[old]), \
                    mock.patch.object(codecsave, "from_radio", return_value=None), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
                self.assertIsNone(cli.get_saved_codec(args, b, {"codec": b.codec_former[0], "uid": "00aa11bb"}))
                self.assertIn("from another radio", err.getvalue())

    def stub_install(self, b, extra=(), probe=None, read=None, descriptor=None, rc=0):
        """rfoundry flash --route stub, the radio's probe, read-back and descriptor given: (exit code, log, the image
        handed to the stub or None)"""
        img = os.path.join(self.tmp, "app.bin")
        with open(img, "wb") as f:
            f.write(app_image(30000, 3))
        handed = []

        def run(board, image, port=None, log=None):
            handed.append(image)
            return rc
        out = io.StringIO()
        with mock.patch.object(cli, "get_probe", return_value=probe), \
                mock.patch.object(codecsave, "from_radio", return_value=read), \
                mock.patch.object(codecsave, "descriptor_windows", return_value=descriptor), \
                mock.patch.object(cli.stubroute, "run", run), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            try:
                code = cli.main(["flash", img, "--board", b.id, "--route", "stub", "--port", "/dev/x", "--yes"]
                                + list(extra))
            except SystemExit as e:
                code = e.code
        return code, out.getvalue(), handed[0] if handed else None

    def test_stub_route_moves_the_codec(self):
        """The stub route reads the codec back first and hands the stub one image with the codec where it goes now;
        everything that can't work is refused before the radio is handed to the stub"""
        for bid, sha in self.BOARDS:
            with self.subTest(bid):
                b, prof, flash, segs = self.radio_with_former(bid, sha)
                got = codecsave.read_codec(BootKeyFromRadio.Radio(bytes(flash)), b)
                uid = "00aa11bb22cc33dd44ee55ff"
                probe = {"board": b.debuglink_names[0], "codec": b.codec_former[0], "port": "/dev/x", "uid": uid}
                rc, log, image = self.stub_install(b, probe=probe, read=got)
                self.assertEqual(rc, 0, log)
                app, p2, s2 = stockfw.split_merged(image)
                self.assertEqual((app, p2, s2), (app_image(30000, 3), prof, segs))
                saved = codecsave.find(b, uid=uid, strict=True)
                self.assertIsNotNone(saved, log)
                # cut: the note names the copy that finishes it
                rc, log, _ = self.stub_install(b, probe=probe, read=got, rc=1)
                self.assertEqual(rc, 1)
                self.assertIn(saved.path, log)
                # --codec keep can't keep a codec in the application's flash: refused, the stub never entered
                rc, log, image = self.stub_install(b, ["--codec", "keep"], probe=probe, read=got)
                self.assertNotEqual(rc, 0)
                self.assertIsNone(image)
                self.assertIn("can't keep it", log)
                # nothing to write it from: the radio can't be read and no copy of this radio's codec is saved
                os.remove(saved.path)
                codecsave.save(b, got[1], uid="99aa11bb22cc33dd44ee55ff")		# another radio's
                rc, log, image = self.stub_install(b, probe=probe, read=None)
                self.assertNotEqual(rc, 0)
                self.assertIsNone(image)
                self.assertIn("nothing was written", log)
                # the probe reports no codec, but the descriptor in flash names a window in the application's
                # flash (a move that stopped halfway): moved too, from the copy read back
                lost = dict(probe, codec="absent")
                win = [(b.app_region[1] - 0x1000, 0x800)]
                rc, log, image = self.stub_install(b, probe=lost, read=got, descriptor=win)
                self.assertEqual(rc, 0, log)
                self.assertEqual(stockfw.split_merged(image)[2], segs)
                # windows outside it: nothing to move; the image goes as it is
                rc, log, image = self.stub_install(b, probe=lost, read=got, descriptor=[(b.app_region[1], 0x800)])
                self.assertEqual((rc, image), (0, app_image(30000, 3)), log)
                for c in codecsave.copies(b):
                    os.remove(c.path)

    def test_stub_move_notes_name_the_source(self):
        s = cli.Saved("prof", [], "radio", "/x/md380-codec-00aa.bin", "00aa")
        self.assertIn("/x/md380-codec-00aa.bin", cli.stub_move_note(s))
        self.assertIn("--stock", cli.stub_move_note("stock"))
        self.assertIn("same image", cli.stub_move_note("image"))

    class CutDevice(fakedfu.FakeAnyRoad):
        """The bootloader's device losing power after `left` more DNLOAD requests (erases, writes, the exit)"""
        left = None

        def __init__(self, *a, **k):
            super().__init__(*a, **k)
            self.events = []	# one per DNLOAD that went through: ("erase", addr) or ("block", sector addr)

        def ctrl_transfer(self, rtype, req, value, intf, data, timeout=None):
            if rtype == 0x21 and req == dfumod.DFU_DNLOAD and self.left is not None:
                if self.left == 0:
                    raise OSError("the radio lost power")
                self.left -= 1
            r = super().ctrl_transfer(rtype, req, value, intf, data, timeout)
            if rtype == 0x21 and req == dfumod.DFU_DNLOAD:
                d = bytes(data or b"")
                if value:
                    self.events.append(("block", self.addr))
                elif d[:1] == b"\x41":
                    self.events.append(("erase", struct.unpack("<I", d[1:5])[0]))
                else:
                    self.events.append(("command", None))
            return r

    def install(self, b, dev, saved, probe):
        """One run as cmd_flash makes it: the decision, the plan, the bootloader route"""
        man = {"codec": {"profile": b.codec.id}}
        d = cli.decide_codec(b, man, "auto", False, probe, None, None, have_saved=saved is not None)
        self.assertEqual((d.mode, d.source), (plan.MERGE, "saved"))
        prof, parts = saved
        segs = codecsave.segments(prof, parts)
        img = app_image(b.app_region[1] - b.app_base - 0x100, 9)		# the new release fills its region
        p = plan.make_plan(b, [(b.app_base, img)], d.mode, segs, b.merge_windows(prof), vector_last=d.moves)
        dev.vendor, dev.state, dev.busy = [], dfumod.ST_IDLE, 0	# the radio back in its bootloader
        dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None).run(p, KEY)
        return img, segs

    def test_interrupted_at_every_step_then_resumed(self):
        for bid, sha in self.BOARDS:
            b, prof, flash, segs = self.radio_with_former(bid, sha)
            boot = bytes(flash[:0xC000])
            # the first run reads the codec back over the debug link and saves it before anything is erased
            got = codecsave.read_codec(BootKeyFromRadio.Radio(bytes(flash)), b)
            codecsave.save(b, got[1], self.tmp)
            probe = {"board": b.debuglink_names[0], "codec": b.codec_former[0]}
            full = self.CutDevice(b, KEY, flash)
            self.install(b, full, got, probe)
            ev = full.events
            steps = len(ev)		# vendor commands, erases, set address, blocks, the exit
            # the vector table's sector is erased first and written last: until then the bootloader finds no
            # application and stays in its update mode
            erases = [a for k, a in ev if k == "erase"]
            self.assertEqual(erases[0], b.app_base)
            written = [a for k, a in ev if k == "block"]
            first3 = written.index(b.app_base)
            self.assertTrue(len(written) > first3 and set(written[first3:]) == {b.app_base}, bid)
            self.assertTrue(len(set(written)) > 1, bid)
            start3 = [i for i, (k, a) in enumerate(ev) if k == "block" and a == b.app_base][0]
            cuts = sorted(set(list(range(0, 2 + len(full.erased) + 3)) + list(range(0, steps, max(1, steps // 8)))
                              + [start3 - 1, start3, start3 + 1, steps - 2, steps - 1]))
            for cut in cuts:
                with self.subTest(board=bid, cut=cut):
                    dev = self.CutDevice(b, KEY, flash)
                    dev.left = cut
                    with self.assertRaises(OSError):
                        self.install(b, dev, got, probe)
                    self.assertEqual(dev.read(0x08000000, 0xC000), boot, "the bootloader is never written")
                    if b.app_base in dev.erased and cut <= start3:
                        # cut after the first erase and before the last sector: no vector table, nothing to start
                        sec = b.sector_at(b.app_base)
                        self.assertEqual(dev.read(sec.addr, sec.size), b"\xFF" * sec.size, "cut %d" % cut)
                    # the radio comes back in its bootloader (or with a part of the new image, which the owner can
                    # always leave for the bootloader); nothing to probe. The copy saved on this computer finishes it
                    dev.left = None
                    saved = codecsave.load(b, self.tmp)
                    self.assertEqual(saved, got)
                    img, segs2 = self.install(b, dev, saved, None)
                    self.assertEqual(segs2, segs)
                    self.assertEqual(dev.violations, [])
                    self.assertEqual(dev.read(b.app_base, len(img)), img)
                    for a, d in segs:
                        self.assertEqual(dev.read(a, len(d)), d)
                    # what the firmware finds on its next start: the codec in the current layout, every CRC matched
                    self.assertEqual(stockfw.from_flash(bytes(dev.flash), profiles=stockfw.PROFILES), (prof, segs))
                    self.assertEqual(dev.read(0x08000000, 0xC000), boot)


class CodecCopyEverywhere(unittest.TestCase):
    """On a radio that keeps its codec (the DM-1701), a copy is read once, from where the merge placed it or where stock
    keeps it, and written instead of asking for the stock file whenever the codec can't be kept or checked."""

    def setUp(self):
        self.stock, profiles, code, tables = stockfw.build_synthetic_stock(KEY, random.Random(7))
        self.profiles = mock.patch.dict(stockfw.PROFILES, profiles, clear=True)
        self.profiles.start()
        self.addCleanup(self.profiles.stop)
        self.board = boards.get("dm1701")
        self.prof = next(iter(profiles.values()))
        self.placed = stockfw.extract(self.stock, KEY)[1]
        self.stock_windows = (code, tables)

    def flash(self, segs):
        f = bytearray(b"\xff" * 0x100000)
        for a, d in segs:
            f[a - 0x08000000:a - 0x08000000 + len(d)] = d
        return BootKeyFromRadio.Radio(bytes(f))

    def test_read_from_either_layout_and_written_as_the_merge_writes_it(self):
        prof, parts = codecsave.read_codec(self.flash(self.placed), self.board)
        self.assertEqual([(a, len(d)) for a, d in parts], [(a, n) for a, n, _ in stockfw.placed_windows(prof)])
        self.assertEqual(codecsave.segments(prof, parts), self.placed)
        old = list(zip([w[0] for w in stockfw.windows(self.prof)], self.stock_windows))
        prof, parts = codecsave.read_codec(self.flash(old), self.board)
        self.assertEqual(parts[0][0], self.prof["code"][0])
        self.assertEqual(codecsave.segments(prof, parts), self.placed)
        tmp = tempfile.mkdtemp()
        codecsave.save(self.board, parts, tmp)
        self.assertEqual(codecsave.load(self.board, tmp), (prof, parts))
        bad = [(a, bytes([d[0] ^ 1]) + d[1:]) for a, d in self.placed]
        self.assertIsNone(codecsave.read_codec(self.flash(bad), self.board))

    def test_decision(self):
        man = {"codec": {"profile": self.board.codec.id}}
        rfoundry = lambda c: {"board": "DM-1701", "codec": c}	# noqa: E731
        dc = lambda *a, **k: cli.decide_codec(self.board, man, *a, **k)	# noqa: E731
        self.assertEqual(dc("auto", False, rfoundry(self.board.codec.id), None, None, have_saved=True).mode, plan.KEEP)
        self.assertEqual(dc("auto", False, None, None, None, have_saved=True).source, "saved")
        self.assertEqual(dc("auto", False, None, "V02.06 CSV", None, have_saved=True).source, "saved")
        self.assertEqual(dc("auto", False, rfoundry("absent"), None, None, have_saved=True).source, "saved")
        self.assertEqual(dc("keep", False, None, None, None, have_saved=True).mode, plan.KEEP)
        with self.assertRaises(cli.UsageError):
            dc("auto", False, rfoundry("absent"), None, None)

    def test_read_once_on_a_board_that_keeps_it(self):
        args = collections.namedtuple("A", "no_probe port")(False, None)
        got = (self.prof, [])
        probe = {"board": "DM-1701", "codec": self.board.codec.id}
        with mock.patch.object(codecsave, "find", return_value=None), \
                mock.patch.object(codecsave, "from_radio", return_value=got) as rd, \
                mock.patch.object(codecsave, "save", return_value="x"):
            self.assertEqual(cli.get_saved_codec(args, self.board, probe), got)
            self.assertEqual(rd.call_count, 1)
        with mock.patch.object(codecsave, "find", return_value=codecsave.Copy("x", None, *got)), \
                mock.patch.object(codecsave, "from_radio") as rd:
            self.assertEqual(cli.get_saved_codec(args, self.board, probe), got)
            self.assertEqual(rd.call_count, 0)	# a copy already: not read again
            self.assertEqual(cli.get_saved_codec(args, self.board, None), got)


class FlashLog(unittest.TestCase):
    """The start and finish messages of a real flash (simulations send nothing), with urlopen replaced."""

    def setUp(self):
        self.sent = []
        real = cli.urllib.request.urlopen

        def fake(req, timeout=None, context=None):
            self.sent.append((req.get_method(), req.full_url, json.loads(req.data)))
            return io.BytesIO(b"")
        cli.urllib.request.urlopen = fake
        self.addCleanup(setattr, cli.urllib.request, "urlopen", real)
        os.environ.pop("REPORTING", None)

    def args(self, off=False):
        return cli.build_parser().parse_args(["flash", "x.bin"] + (["--no-reporting"] if off else []))

    def test_start_and_finish(self):
        man = {"version": "v0.1.0", "variant": "full", "features": "FM,DMR"}
        started = cli.flash_started(self.args(), DM1701, man)
        cli.flash_finished(started, dfumod.DfuError("no answer from /Users/someone/rfoundry/x.bin"))
        (m1, u1, b1), (m2, u2, b2) = self.sent
        self.assertEqual((m1, b1["task"], b1["source"], b1["radio"], b1["firmware"], b1["variant"], b1["features"]),
                         ("POST", "install", "own", "dm1701", "v0.1.0", "full", "FM,DMR"))
        self.assertEqual((m2, u2), ("PATCH", u1 + "/" + b1["id"]))
        self.assertEqual(b2["result"], "fail")
        self.assertNotIn("someone", b2["reason"])

    def test_opt_out(self):
        self.assertIsNone(cli.flash_started(self.args(off=True), DM1701, {}))
        os.environ["REPORTING"] = "0"
        self.addCleanup(os.environ.pop, "REPORTING", None)
        self.assertIsNone(cli.flash_started(self.args(), DM1701, {}))
        cli.flash_finished(None)
        self.assertEqual(self.sent, [])


class AfterFlash(unittest.TestCase):
    """What rfoundry says about the radio after a flash and in probe, with more than one radio on USB"""

    def run_verify(self, answer):
        out = io.StringIO()
        args = mock.Mock(port=None)
        d = cli.Decision(plan.MERGE, "test")
        with mock.patch.object(debuglink, "find_ports", return_value=["/dev/x"]), \
                mock.patch.object(debuglink, "probe", return_value=answer), \
                mock.patch.object(cli, "hwopt_prompt") as hw, mock.patch.object(state, "remember_probe") as rem, \
                mock.patch.object(cli.time, "sleep"), contextlib.redirect_stdout(out):
            rc = cli.verify_after(args, boards.get("md380"), {}, d)
        return rc, out.getvalue(), hw, rem

    def test_another_radio_answering_is_not_checked(self):
        """An MD-9600 on the same computer answered after an MD-380 was flashed through its bootloader: rfoundry
        reported the MD-9600's codec as the MD-380's, failed, and remembered and prompted on the wrong radio"""
        rc, out, hw, rem = self.run_verify({"board": "MD-9600", "port": "/dev/ttyACM1", "git": "abc", "codec": "md9600-s11"})
        self.assertEqual(rc, 0)
        self.assertIn("not checked", out)
        self.assertNotIn("no usable codec", out)
        hw.assert_not_called()
        rem.assert_not_called()

    def test_the_flashed_radio_is_checked(self):
        b = boards.get("md380")
        rc, out, hw, rem = self.run_verify({"board": "MD-380", "port": "/dev/ttyACM0", "git": "abc", "codec": b.codec.id})
        self.assertEqual(rc, 0)
        self.assertIn("DMR voice is ready", out)
        hw.assert_called_once()
        rem.assert_called_once()

    def test_probe_of_a_former_layout_says_it_moves(self):
        b = boards.get("md380")
        self.assertTrue(b.codec_former)
        out = io.StringIO()
        p = {"board": "MD-380", "port": "/dev/ttyACM0", "git": "abc", "codec": b.codec_former[0]}
        with mock.patch.object(debuglink, "probe", return_value=p), mock.patch.object(state, "remember_probe"), \
                contextlib.redirect_stdout(out):
            cli.cmd_probe(mock.Mock(port=None))
        self.assertIn("moves it", out.getvalue())
        self.assertNotIn("no codec to keep", out.getvalue())


if __name__ == "__main__":
    unittest.main()
