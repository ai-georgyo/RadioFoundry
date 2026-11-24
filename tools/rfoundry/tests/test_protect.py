# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Protected flash regions (rfoundry/protect.py): the shared check, the carve-out (byte exact for the writers through the
firmware, whole units for the direct writers), tools/cps_client.py sending no byte of a region, and that every board's
registry entry carries its board.toml [[flash.protect]] list.

    python3 -m unittest discover -s tools/rfoundry/tests -v        (tools/ci.sh runs this)
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from rfoundry import boards, protect  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
CAL = {"mem": "progmem", "start": 0x1FE000, "end": 0x1FF000, "what": "factory calibration (hardware-x.md 6.1)"}
NAND = {"mem": "nand", "start": 0x280000, "end": 0x2E0000, "what": "calibration copy (hardware-x.md 6.5)"}


class Check(unittest.TestCase):
    def test_outside_passes(self):
        protect.check([CAL, NAND], "progmem", 0xC000, 0x1FE000)
        protect.check([CAL, NAND], "progmem", 0x1FF000, 0x200000)
        protect.check([CAL, NAND], "data", 0x1FE000, 0x1FF000)	# another memory

    def test_overlap_names_the_region_and_reason(self):
        with self.assertRaises(protect.ProtectedError) as e:
            protect.check([CAL], "progmem", 0x1FD000, 0x1FE001)
        self.assertIn("0x1FE000-0x1FF000", str(e.exception))
        self.assertIn("factory calibration (hardware-x.md 6.1)", str(e.exception))
        self.assertEqual(e.exception.regions, [CAL])

    def test_absolute_addresses(self):
        protect.check_absolute([CAL], 0x03000000, 0x0300C000, 0x031FE000)
        with self.assertRaises(protect.ProtectedError):
            protect.check_absolute([CAL], 0x03000000, 0x031FE000, 0x031FE100)

    def test_board_or_entry_or_list(self):
        class B:
            protect = [CAL]
        for p in (B(), {"protect": [CAL]}, [CAL]):
            with self.assertRaises(protect.ProtectedError):
                protect.check(p, "progmem", 0x1FE000, 0x1FF000)
        protect.check(None, "progmem", 0, 0x200000)

    def test_whole_chip_erase(self):
        protect.check_whole([CAL], "data")
        with self.assertRaises(protect.ProtectedError):
            protect.check_whole([CAL, NAND], "nand")

    def test_unknown_memory(self):
        with self.assertRaises(ValueError):
            protect.check([CAL], "eeprom", 0, 1)


# A range the firmware keeps inside a sector (an invented table) and one that covers two sectors whole
TABLE = {"mem": "data", "start": 0x1000, "end": 0x1200, "what": "factory table (hardware-x.md 2.1)"}
RECORDS = {"mem": "data", "start": 0x1E000, "end": 0x20000, "what": "maker's records (hardware-x.md 2.4)"}


class Carve(unittest.TestCase):
    def test_pieces_around_regions(self):
        two = [CAL, dict(CAL, start=0x100000, end=0x102000)]
        self.assertEqual(protect.carve_bytes(two, "progmem", 0xC000, 0x200000),
                         [(0xC000, 0x100000), (0x102000, 0x1FE000), (0x1FF000, 0x200000)])
        self.assertEqual(protect.carve_bytes(two, "progmem", 0x1FE000, 0x1FF000), [])
        self.assertEqual(protect.carve_bytes(two, "data", 0, 0x1000), [(0, 0x1000)])

    def test_bytes_exact(self):
        """The writers through the firmware: the pieces outside a range inside a sector, to the byte"""
        both = [TABLE, RECORDS]
        self.assertEqual(protect.carve_bytes(both, "data", 0x1000, 0x2000), [(0x1200, 0x2000)])
        self.assertEqual(protect.carve_bytes(both, "data", 0x0F80, 0x1220), [(0x0F80, 0x1000), (0x1200, 0x1220)])
        self.assertEqual(protect.carve_bytes(both, "data", 0x1100, 0x1180), [])
        self.assertEqual(protect.carve_bytes(both, "data", 0x1D000, 0x21000), [(0x1D000, 0x1E000), (0x20000, 0x21000)])
        self.assertEqual(protect.carve_bytes(both, "data", 0x11FF, 0x1201), [(0x1200, 0x1201)])
        self.assertEqual(protect.carve_bytes(both, "progmem", 0x1000, 0x2000), [(0x1000, 0x2000)])	# another memory

    def test_units(self):
        units = protect.carved_units([CAL], "progmem", 0x1FC000, 0x200000, 0x1000)
        self.assertEqual(units, [0x1FC000, 0x1FD000, 0x1FF000])

    def test_units_widened(self):
        """The direct writers: an entry inside a sector leaves that sector out whole, never cut midway"""
        self.assertEqual(protect.carved_units([TABLE, RECORDS], "data", 0, 0x22000, 0x1000),
                         [a for a in range(0, 0x22000, 0x1000) if a != 0x1000 and not 0x1E000 <= a < 0x20000])
        self.assertEqual(protect.carved_units([dict(TABLE, start=0x1FF0, end=0x2010)], "data", 0, 0x4000, 0x1000),
                         [0, 0x3000])		# across a sector end: both sectors
        self.assertEqual(protect.widen([TABLE, CAL], "data", 0x1000), [dict(TABLE, start=0x1000, end=0x2000)])
        self.assertEqual(protect.widen([RECORDS], "data", 0x1000), [RECORDS])	# on the boundaries: as it is
        self.assertEqual(TABLE["end"], 0x1200, "widen copies, it doesn't change the board's entry")

    def test_check_overlap_of_a_unit(self):
        """A direct writer checks the unit it erases: any overlap with an entry inside it refuses"""
        with self.assertRaises(protect.ProtectedError):
            protect.check([TABLE], "data", 0x1000, 0x2000, "erase")
        protect.check([TABLE], "data", 0x2000, 0x3000, "erase")


class RegistryCarriesTheList(unittest.TestCase):
    """Every board's registry entry has its board.toml [[flash.protect]] list, and the regions are inside memories the
    tools know."""

    def test_every_board(self):
        try:
            import tomllib
        except ImportError:
            self.skipTest("board.toml needs Python 3.11 (tomllib); tools/boards.py check covers it there")
        for bid, b in boards.BOARDS.items():
            with open(os.path.join(ROOT, "boards", bid, "board.toml"), "rb") as f:
                want = tomllib.load(f)["flash"].get("protect", [])
            key = lambda p: (p["mem"], p["start"])
            self.assertEqual(sorted(b.protect, key=key), sorted(want, key=key), bid)
            for p in b.protect:
                self.assertIn(p["mem"], protect.MEMS, bid)

    def test_each_entry_is_refused(self):
        listed = [b for b in boards.BOARDS.values() if b.protect]
        self.assertTrue(listed, "some board has a protected region")
        for b in listed:
            for p in b.protect:
                with self.assertRaises(protect.ProtectedError):
                    protect.check(b, p["mem"], p["end"] - 1, p["end"])
                self.assertEqual(protect.carve_bytes(b, p["mem"], p["start"], p["end"]), [])


class Writers(unittest.TestCase):
    """The rfoundry DFU route: a plan that would erase a protected sector is refused, and Dfu.run checks again before
    it sends anything."""

    def setUp(self):
        import copy
        import random
        import struct
        from rfoundry import fakedfu, stockfw
        # a board on the keyed DFU route: the fake below is its bootloader, and the image reaches its third sector
        self.base = next(b for b in boards.BOARDS.values() if b.family == "usb-dfu")
        self.b = copy.copy(self.base)
        sec = self.b.sectors[2]		# the third sector the bootloader erases
        self.b.protect = [{"mem": "progmem", "start": sec.addr - self.b.rom_base, "end": sec.end - self.b.rom_base,
                           "what": "test (x.md 1)"}]
        rng = random.Random(3)
        self.img = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + stockfw.random_bytes(rng, 150000)
        self.key = fakedfu.synthetic_key()
        self.dev = fakedfu.FakeAnyRoad(self.b, self.key, fakedfu.stock_flash(self.b, rng, []))

    def test_plan_refused(self):
        from rfoundry import plan
        with self.assertRaises(plan.PlanError) as e:
            plan.make_plan(self.b, [(self.b.app_base, self.img)], plan.KEEP)
        self.assertIn("protected region", str(e.exception))
        p = plan.make_plan(self.b, [(self.b.app_base, self.img[:5000])], plan.KEEP)	# the first sector only: fine
        self.assertEqual(p.erase, [self.b.sectors[0]])

    def test_dfu_refuses_before_anything_is_sent(self):
        from rfoundry import dfu, plan
        p = plan.make_plan(self.base, [(self.b.app_base, self.img)], plan.KEEP)
        p.board = self.b
        with self.assertRaises(protect.ProtectedError):
            dfu.Dfu(self.dev, 0, log=lambda *a: None, sleep=lambda s: None).run(p, self.key)
        self.assertEqual((self.dev.erased, self.dev.vendor), ([], []))


class FakeShim:
    """The CPS shim's write path as cps-protocol.md 2.3 gives it, enough for write_image: C answers '-', R 1 reads the
    flash, X 1 loads the stored sector into a buffer, X 2 merges the bytes it is sent, X 3 stores the buffer. Every
    X 2 is recorded as (wire address, bytes). flash: the data flash (wire addresses)"""

    def __init__(self, flash):
        self.flash = flash
        self.out = b""
        self.data, self.prepared, self.buf = [], [], None

    def write(self, b):
        b = bytes(b)
        if b[0] == 0x43:
            self.out += b"-"
        elif b[0] == 0x52:
            addr, n = int.from_bytes(b[2:6], "big"), int.from_bytes(b[6:8], "big")
            self.out += b"R" + n.to_bytes(2, "big") + bytes(self.flash[addr:addr + n])
        elif b[:2] == b"X\x01":
            self.at = int.from_bytes(b[2:5], "big") * 4096
            self.prepared.append(self.at)
            self.buf = bytearray(self.flash[self.at:self.at + 4096])
            self.out += b"X\x01"
        elif b[:2] == b"X\x02":
            addr, n = int.from_bytes(b[2:6], "big"), int.from_bytes(b[6:8], "big")
            self.data.append((addr, b[8:8 + n]))
            self.buf[addr - self.at:addr - self.at + n] = b[8:8 + n]
            self.out += b"X\x02"
        elif b[:2] == b"X\x03":
            self.flash[self.at:self.at + 4096] = self.buf
            self.out += b"X\x03"
        else:
            raise AssertionError("unexpected request %r" % b[:8])

    def read(self, n, timeout):
        r, self.out = self.out[:n], self.out[n:]
        return r

    def sent(self):
        """The wire addresses X 2 carried bytes for"""
        return {a + i for a, d in self.data for i in range(len(d))}


class CpsWrites(unittest.TestCase):
    """tools/cps_client.py: the wire addresses are the data flash's. A write whose sectors hold a protected region is
    written without a byte of it (the shim's X 1 loaded the radio's own), not refused; a sector the regions cover
    whole isn't written; one line on stderr names a region the image holds bytes for."""

    def setUp(self):
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import cps_client
        self.cps_client = cps_client

    def write(self, img, regions, protect_, offset=0, base=None, flash=None):
        import contextlib
        import io
        cc = self.cps_client
        flash = flash if flash is not None else bytearray(b"\xa5" * (cc.IMAGE_SIZE + offset))
        shim = FakeShim(flash)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            cc.Cps(shim, 0.01, offset).write_image(img, regions, reboot=False, base=base, protect=protect_)
        return shim, err.getvalue()

    def test_range_inside_a_sector(self):
        cc = self.cps_client
        img = bytearray(b"\x5a" * cc.IMAGE_SIZE)
        regions = [(0x80, 0x6060)]
        for offset in (0, 0x700000):
            prot = [dict(TABLE, start=TABLE["start"] + offset, end=TABLE["end"] + offset)]
            shim, err = self.write(img, regions, prot, offset)
            sent = shim.sent()
            lo, hi = 0x1000 + offset, 0x1200 + offset
            self.assertFalse(any(lo <= a < hi for a in sent), "no byte of the range in X 2")
            self.assertTrue(set(range(0x1200 + offset, 0x2000 + offset)) <= sent and 0x1000 + offset in shim.prepared,
                            "the rest of the sector sent, the sector written")
            self.assertEqual(sent, set(range(0x80 + offset, 0x6060 + offset)) - set(range(lo, hi)), "every other byte")
            self.assertEqual(bytes(shim.flash[lo:hi]), b"\xa5" * 0x200, "the radio's bytes stay")
            self.assertEqual(bytes(shim.flash[hi:0x2000 + offset]), b"\x5a" * (0x2000 - 0x1200), "the rest written")
            self.assertEqual(len(err.splitlines()), 1, err)
            self.assertIn("0x%X-0x%X" % (lo, hi), err)
            self.assertIn("kept", err)
            self.assertIn("rfoundry_debug.py storage factory --backup", err, "the way to put a backup's bytes back")

    def test_blank_range_no_message(self):
        cc = self.cps_client
        img = bytearray(b"\x5a" * cc.IMAGE_SIZE)
        img[0x1000:0x1200] = b"\xff" * 0x200
        shim, err = self.write(img, [(0x80, 0x6060)], [TABLE])
        self.assertEqual(err, "")
        self.assertFalse(any(0x1000 <= a < 0x1200 for a in shim.sent()))

    def test_whole_sector_range_not_written(self):
        cc = self.cps_client
        img = bytearray(b"\x5a" * cc.IMAGE_SIZE)
        rec = dict(RECORDS, start=0x2000, end=0x4000)
        shim, err = self.write(img, [(0x0, 0x6000)], [rec])
        self.assertNotIn(0x2000, shim.prepared)
        self.assertNotIn(0x3000, shim.prepared)
        self.assertEqual(shim.prepared, [0, 0x1000, 0x4000, 0x5000])
        self.assertEqual(bytes(shim.flash[0x2000:0x4000]), b"\xa5" * 0x2000)
        self.assertIn("0x2000-0x4000", err)

    def test_base_differs_only_inside_the_range(self):
        """With base (the radio's bytes now), a sector that differs only inside the range isn't written; its bytes
        aren't the radio's, so the line still says they were kept"""
        cc = self.cps_client
        base = bytearray(b"\xa5" * cc.IMAGE_SIZE)
        img = bytearray(base)
        img[0x1000:0x1200] = b"\x5a" * 0x200
        shim, err = self.write(img, [(0x80, 0x6060)], [TABLE], base=bytes(base), flash=bytearray(base))
        self.assertEqual(shim.prepared, [])
        self.assertIn("0x1000-0x1200", err)
        shim, err = self.write(base, [(0x80, 0x6060)], [TABLE], base=bytes(base), flash=bytearray(base))
        self.assertEqual((shim.prepared, err), ([], ""), "the radio's own bytes in the image: nothing to say")

    def test_plan(self):
        cc = self.cps_client
        keep = cc.kept_ranges([TABLE, RECORDS, CAL], 0)
        self.assertEqual(keep, [(0x1000, 0x1200), (0x1E000, 0x20000)])
        self.assertEqual(cc.kept_ranges([dict(TABLE, start=0x700000, end=0x701000), TABLE], 0x700000), [(0, 0x1000)])
        base = bytearray(b"\xa5" * 0x8000)
        img = bytearray(base)
        img[0x1100] = 0
        self.assertEqual(cc.plan_write(img, [(0, 0x8000)], base, keep), [])
        img[0x1300] = 0
        jobs = cc.plan_write(img, [(0, 0x8000)], base, keep)
        self.assertEqual(jobs, [(1, 0, 0x8000)])
        after = cc.apply_plan(bytearray(base), img, jobs, keep)
        self.assertEqual((after[0x1100], after[0x1300]), (0xA5, 0), "the dry run keeps the range too")
        self.assertEqual(cc.outside(0x0F00, 0x2000, keep), [(0x0F00, 0x1000), (0x1200, 0x2000)])
        self.assertFalse(cc.job_sends((0x1E, 0x1E000, 0x20000), keep))
        self.assertTrue(cc.same_but_vfo(b"\x00" * 0x10, b"\x01" * 0x10, 0x1100, keep))
        # several boards' entries (protect_for): overlapping or touching ranges joined, so outside() stays exact
        self.assertEqual(cc.kept_ranges([dict(TABLE, start=0x1100, end=0x1400), TABLE,
                                         dict(TABLE, start=0x1400, end=0x1500)]), [(0x1000, 0x1500)])
        self.assertEqual(cc.outside(0x0F00, 0x2000, cc.kept_ranges([TABLE, dict(TABLE, start=0x1100, end=0x1300)])),
                         [(0x0F00, 0x1000), (0x1300, 0x2000)])

    def test_protect_for(self):
        """The board --board names; else every board reporting the CPS radio type (radios of different makers share
        one), their entries joined and a note when their data entries differ; a type no board lists: the board the
        model name means"""
        cc = self.cps_client

        class B:
            def __init__(self, bid, types, prot, names=()):
                self.id, self.cps_radio_types, self.protect, self.debuglink_names = bid, types, prot, list(names)
        sec = dict(TABLE, mem="secreg", start=0, end=0x200)
        fake = {"a": B("a", [7], [TABLE, RECORDS]), "b": B("b", [7], [sec]), "c": B("c", [], [RECORDS], ["C-1"]),
                "d": B("d", [5, 6], [TABLE]), "e": B("e", [6], [TABLE, sec])}
        saved = cc.rfoundryboards.BOARDS
        cc.rfoundryboards.BOARDS = fake
        try:
            got, note = cc.protect_for({"radio_type": 7, "model": "X-1"})
            self.assertEqual(got, [TABLE, RECORDS, sec])
            self.assertIn("a and b", note)
            self.assertIn("--board", note)
            self.assertEqual(cc.protect_for({"radio_type": 7}, "b"), ([sec], None), "--board: that board only")
            self.assertEqual(cc.protect_for({}, "a"), ([TABLE, RECORDS], None))
            self.assertEqual(cc.protect_for({"radio_type": 5}), ([TABLE], None), "one board")
            self.assertEqual(cc.protect_for({"radio_type": 6}), ([TABLE, sec], None),
                             "two boards with the same data entries: no note")
            self.assertEqual(cc.protect_for({"radio_type": 11, "model": "C-1"}), ([RECORDS], None), "by model name")
            self.assertEqual(cc.protect_for({"radio_type": 11, "model": "?"}), ([], None), "none")
            # The board VERSION names: that board's alone, no note; one the registry doesn't know: the radio type
            fake["b"].debuglink_names = ["B-1"]
            self.assertEqual(cc.protect_for({"radio_type": 7, "board": "B-1"}), ([sec], None), "VERSION: b only")
            self.assertEqual(cc.protect_for({"radio_type": 7, "board": "Z-9"})[0], [TABLE, RECORDS, sec])
            self.assertEqual(cc.protect_for({"radio_type": 7, "board": None})[0], [TABLE, RECORDS, sec])
        finally:
            cc.rfoundryboards.BOARDS = saved
        # The registry: an MD-380 and a CS7000 M17 PLUS both report radio type 7; the MD-380's VERSION board gives
        # no data range (the CS7000's maker's table is not an MD-380's)
        md380 = boards.get("md380")
        got, note = cc.protect_for({"radio_type": 7, "model": "MD-380", "board": md380.debuglink_names[0]})
        self.assertEqual(([p for p in got if p["mem"] == "data"], note), ([], None))
        self.assertEqual(cc.kept_ranges(got), [])
        # The registry: each board reporting a radio type has its entries kept by a write to that type
        for t in {t for b in saved.values() for t in b.cps_radio_types}:
            got, _ = cc.protect_for({"radio_type": t})
            for b in saved.values():
                if t in b.cps_radio_types:
                    self.assertTrue(all(p in got for p in b.protect), (t, b.id))

    def test_version_board(self):
        """Cps.version_board: a debug link VERSION frame on the CPS port, its board= value; None from a radio that
        answers no debug link frame (another firmware), the stray bytes it sent thrown away"""
        cc = self.cps_client
        dbg = cc.rfoundry_debug_module()

        class Port:
            def __init__(self, answer):
                self.answer, self.out, self.sent = answer, b"", b""

            def write(self, b):
                self.sent += b
                if b[:2] == bytes([dbg.SOF1, dbg.SOF2]):
                    self.out += self.answer(b[3])
                else:
                    self.out += b"-"

            def read(self, n, timeout):
                r, self.out = self.out[:n], self.out[n:]
                return r

        def version(seq):
            return b"junk" + dbg.frame(dbg.VERSION | 0x80, seq, b"\0proto=1\nboard=MD-380\nfeatures=x\n")
        port = Port(version)
        self.assertEqual(cc.Cps(port).version_board(), "MD-380")
        self.assertEqual(port.out, b"")
        self.assertEqual(cc.Cps(Port(lambda seq: version(seq ^ 1))).version_board(0.1), None, "another request's")
        port = Port(lambda seq: b"-")
        self.assertEqual(cc.Cps(port).version_board(0.1), None)
        self.assertEqual(port.out, b"", "what came is thrown away")

    def test_registry_boards_as_before(self):
        """Every board's data entries that the CPS wire can reach are on 4 KB boundaries, unless its flash map keeps
        them: a sector is then in or out whole, as before (the aligned entries of the boards that list no range)"""
        try:
            import tomllib  # noqa: F401  (tools/boards.py reads board.toml)
        except ImportError:
            self.skipTest("board.toml needs Python 3.11 (tomllib)")
        import boards as registry
        cc = self.cps_client
        for bid, b in registry.load().items():
            base = b.flash_map.get("openfw_eeprom")
            if base is None or any(n for _, n in b.factory):
                continue		# no CPS image, or a board whose firmware keeps its ranges inside sectors
            for p in b.protect:
                if p["mem"] == "data":
                    self.assertTrue(p["start"] % cc.SECTOR == 0 and p["end"] % cc.SECTOR == 0, (bid, p))
            keep = cc.kept_ranges(b, base)
            for name, regions in cc.REGIONS.items():
                for sector, start, end in cc.plan_write(bytearray(cc.IMAGE_SIZE), regions):
                    lo, hi = max(start, sector * cc.SECTOR), min(end, (sector + 1) * cc.SECTOR)
                    self.assertIn(cc.outside(lo, hi, keep), ([], [(lo, hi)]), (bid, name, hex(sector)))


if __name__ == "__main__":
    unittest.main()
