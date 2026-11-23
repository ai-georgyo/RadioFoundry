# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Host tests for the codec step of an install through a processor's ROM bootloader (family dfuse, plain DfuSe): the
merge, keep and blank plans, the route's limits on them, the keep settled by what the loader says, an install cut at
every block, the remembered probe, and a maker's file that is no codec source or no image, on every way in.

Against fakedfu.FakeRom: no USB, no serial port, no radio. No real key or stock file: every stock file is synthetic,
built in code with the layout of a board's own profile (stockfw.build_synthetic_stock).

Nothing here names a radio. The boards are the registry's boards of this route that have a codec route, a profile is
found by its board, a container by its profile, and a codec id is the board's. The route without a codec route, and
the route's own protocol, are in test_rfoundry.py (RomBootloader, NoCodecBoard), on a board built there.

    python3 -m unittest discover -s tools/rfoundry/tests -v        (tools/ci.sh runs this)
"""
import contextlib
import io
import json
import os
import random
import re
import struct
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import home  # noqa: E402,F401 (the settings folder in a temporary folder)
import test_rfoundry as base  # noqa: E402 (its helpers; the module, so that its tests are not collected here again)
from rfoundry import boards, cli, codecsave, debuglink, fakedfu, plan, state, stockfw, twinfw  # noqa: E402
from rfoundry import dfu as dfumod  # noqa: E402

KEY = fakedfu.synthetic_key()
FF = b"\xFF"
END = stockfw.DESC_ADDR + stockfw.DESC_LEN	# where every stock-file layout ends: its descriptor's last byte


def codec_boards():
    """The boards installed through a ROM bootloader that have a codec from a stock file"""
    return [b for _, b in sorted(boards.BOARDS.items()) if b.family == "dfuse" and b.codec_stock]


_SYNTHETIC = {}


def synthetic(b):
    """What --stock synthetic gives a simulation of board b (cli.sim_codec): (stock file, {its SHA-256: profile},
    profile, the codec as a merge writes it)"""
    if b.id not in _SYNTHETIC:
        stock, profiles, _, _ = stockfw.build_synthetic_stock(KEY, random.Random(7), stockfw.profile_for_board(b.id))
        prof, segs = stockfw.extract(stock, KEY, profiles)
        _SYNTHETIC[b.id] = (stock, profiles, prof, segs)
    return _SYNTHETIC[b.id]


def sim_codec(b, key):
    """cli.sim_codec without building the synthetic file again in every run"""
    return synthetic(b)[2], list(synthetic(b)[3])


def sector_image(s, segs):
    """Sector s as a merge leaves it: the codec's segments in erased flash"""
    out = bytearray(FF * s.size)
    for a, d in segs:
        lo, hi = max(a, s.addr), min(a + len(d), s.end)
        if lo < hi:
            out[lo - s.addr:hi - s.addr] = d[lo - a:hi - a]
    return bytes(out)


class Watched(fakedfu.FakeRomUsbDevice):
    """A fake loader that notes, when the vector table's block comes, what its codec sectors hold and whether this
    run erased them. stop_erase: it leaves the bus when the erase with this number (from 0) comes, so that sector
    and every later one keep what they hold, as a cable or the power cut during the erases leaves them"""
    at_vectors = None
    stop_erase = None

    def ctrl_transfer(self, rtype, req, value, intf, data, timeout=None):
        if (self.stop_erase is not None and not self.gone and rtype == 0x21 and req == dfumod.DFU_DNLOAD
                and value == 0 and data and bytes(data)[0] == dfumod.CMD_ERASE and len(self.erased) == self.stop_erase):
            self.gone = True
        return super().ctrl_transfer(rtype, req, value, intf, data, timeout)

    def _program(self, addr, data):
        if addr == self.board.app_base:
            self.at_vectors = [(s, s.addr in self.erased, self.read(s.addr, s.size))
                               for s in self.board.codec_sectors()]
        super()._program(addr, data)


class PlainCodec(unittest.TestCase):
    def setUp(self):
        self.boards = codec_boards()
        if not self.boards:
            self.skipTest("no board installed through a ROM bootloader has a codec route")
        self.b = self.boards[0]
        state.forget_probe()
        self.addCleanup(state.forget_probe)
        fast = mock.patch.object(cli, "sim_codec", sim_codec)
        fast.start()
        self.addCleanup(fast.stop)

    def merge_plan(self, b, n=150020):
        _, _, prof, segs = synthetic(b)
        return plan.make_plan(b, [(b.app_base, base.rom_image(n))], plan.MERGE, segs, b.merge_windows(prof),
                              vector_last=True)

    def sim(self, b, *argv, **force):
        """A simulated install of board b: (exit code, stdout, stderr, the fake loader it ran against or None when none
        was made). force: what the fake radio is, whatever the simulation's state says: flash (its program flash),
        protected, fail_at, torn, stop_erase"""
        made = []
        self.assertIn("--simulate", argv)

        class Spy(Watched):
            stop_erase = force.get("stop_erase")

            def __init__(self, board, flash=None, **kw):
                kw.update((k, v) for k, v in force.items() if k not in ("flash", "stop_erase"))
                super().__init__(board, force.get("flash", flash), **kw)
                self.before = bytes(self.flash)
                made.append(self)

        with mock.patch.object(fakedfu, "FakeRomUsbDevice", Spy):
            rc, out, err = base.run_cli(["flash", "--board", b.id] + list(argv))
        return rc, out, err, (made[0] if made else None)

    def holds_codec(self, b, dev, segs=None):
        """The fake radio's codec sectors hold the codec as a merge writes it, and nothing else"""
        segs = synthetic(b)[3] if segs is None else segs
        for s in b.codec_sectors():
            self.assertTrue(dev.read(s.addr, s.size) == sector_image(s, segs), "sector %d" % s.index)

    def blank(self, b, dev):
        for s in b.codec_sectors():
            self.assertTrue(dev.read(s.addr, s.size) == FF * s.size, "sector %d is not blank" % s.index)


class Plans(PlainCodec):
    def test_merge(self):
        for b in self.boards:
            _, _, prof, segs = synthetic(b)
            p = self.merge_plan(b)
            by = {st.sector.index: st for st in p.steps}
            image = b.sectors_for(b.app_base, b.app_base + 150020)
            self.assertGreater(len(image), 1)
            for s in image:
                self.assertEqual((by[s.index].action, by[s.index].content), ("write", "app"))
            for s in b.codec_sectors():
                st = by[s.index]
                self.assertEqual((st.action, st.content), ("write", "codec"))
                self.assertEqual(b"".join(blk for _, blk in st.blocks).ljust(s.size, FF), sector_image(s, segs))
            self.assertEqual(p.erase, image + b.codec_sectors())
            # the sector with the vector table is erased first and written last, after the codec's
            self.assertEqual(p.erase_order[0].addr, b.app_base)
            order = [st.sector for st in p.write_order]
            self.assertEqual(order[-1].addr, b.app_base)
            for s in b.codec_sectors():
                self.assertLess(order.index(s), len(order) - 1)
            # the descriptor is in the last block of its sector's step: every codec block is sent before it, and it
            # holds no codec byte. Between the codec's last byte and it, erased flash
            st = by[b.sector_at(stockfw.DESC_ADDR).index]
            self.assertEqual(len(st.blocks), st.sector.size // plan.BLOCK)
            num, blk = st.blocks[-1]
            at = st.sector.addr + (num - plan.FIRST_BLOCK) * plan.BLOCK
            self.assertEqual(at + plan.BLOCK, END)
            self.assertEqual(segs[-1][0], stockfw.DESC_ADDR)
            self.assertEqual(blk, FF * (plan.BLOCK - stockfw.DESC_LEN) + segs[-1][1])
            end = max(a + len(d) for a, d in segs[:-1])
            last = (end - 1 - st.sector.addr) // plan.BLOCK
            self.assertLess(last, len(st.blocks) - 1)
            self.assertNotEqual(st.blocks[last][1], FF * plan.BLOCK)
            for _, blk in st.blocks[last + 1:-1]:
                self.assertEqual(blk, FF * plan.BLOCK)
            pl = prof["placement"]
            if (pl["code"], pl["tables"], pl.get("tables_len")) == (0x080E0004, 0x080F5CA0, 0x3E83):
                # the numbers of that layout: 128 blocks, the codec's last byte in block 102, the descriptor in 127
                self.assertEqual((len(st.blocks), last, (stockfw.DESC_ADDR - st.sector.addr) // plan.BLOCK),
                                 (128, 102, 127))
            dfumod.check_plain(p)
            self.assertIn("codec mode: merge", p.describe()[0])

    def test_keep(self):
        for b in self.boards:
            lo, hi = b.app_region
            p = plan.make_plan(b, [(lo, base.rom_image(150020))], plan.KEEP, vector_last=True)
            by = {st.sector.index: st for st in p.steps}
            for s in b.codec_sectors():
                self.assertEqual(by[s.index].action, "keep")
                self.assertNotIn(s, p.erase)
            self.assertEqual(p.kept, b.codec_sectors())
            dfumod.check_plain(p)
            # the application region to its last byte is this radio's image; one byte more is not, in any mode
            dfumod.check_plain(plan.make_plan(b, [(lo, base.rom_image(hi - lo))], plan.KEEP, vector_last=True))
            _, _, prof, segs = synthetic(b)
            for mode, codec in ((plan.KEEP, ()), (plan.BLANK, ()), (plan.MERGE, segs)):
                with self.assertRaises(plan.PlanError, msg=mode):
                    plan.make_plan(b, [(lo, base.rom_image(hi - lo + 1))], mode, codec,
                                   b.merge_windows(prof) if codec else None, vector_last=True)

    def test_blank(self):
        for b in self.boards:
            img = base.rom_image(150020)
            p = plan.make_plan(b, [(b.app_base, img)], plan.BLANK, vector_last=True)
            by = {st.sector.index: st for st in p.steps}
            for s in b.codec_sectors():
                st = by[s.index]
                self.assertEqual((st.action, st.blocks, st.content), ("erase", [], ""))
                self.assertIn(s, p.erase)
                self.assertNotIn(st, p.writes)
            self.assertEqual((p.kept, p.mode), ([], plan.BLANK))
            keep = plan.make_plan(b, [(b.app_base, img)], plan.KEEP, vector_last=True)
            self.assertEqual(p.erase, keep.erase + b.codec_sectors())
            self.assertEqual(([st.sector for st in p.writes], p.total_blocks),
                             ([st.sector for st in keep.writes], keep.total_blocks))
            self.assertEqual(p.erase_order[0].addr, b.app_base)
            self.assertEqual(p.write_order[-1].sector.addr, b.app_base)
            text = "\n".join(p.describe())
            self.assertIn("board %s, codec mode: blank" % b.name, text)
            self.assertEqual(text.count("ERASE, left blank (codec window "), len(b.codec_sectors()))
            self.assertIn("erase %d sector(s)" % len(p.erase), text)
            self.assertNotIn("never erased or written: codec", text)
            dfumod.check_plain(p)
            _, _, prof, segs = synthetic(b)
            for wins in (None, b.merge_windows(prof)):		# codec data in this mode, as in keep mode
                with self.assertRaises(plan.PlanError) as cm:
                    plan.make_plan(b, [(b.app_base, img)], plan.BLANK, segs, wins)
                self.assertIn("codec data given in blank mode", str(cm.exception))
            with self.assertRaises(plan.PlanError) as cm:
                plan.make_plan(b, [(b.app_base, img)], plan.KEEP, segs)
            self.assertIn("codec data given in keep mode", str(cm.exception))
            with self.assertRaises(plan.PlanError):
                plan.make_plan(b, [(b.app_base, img)], "none")

    def test_blank_is_refused_where_no_codec_sector_stands_alone(self):
        """A board whose image shares a sector with its codec, and a board without a codec route"""
        shared = [b for _, b in sorted(boards.BOARDS.items()) if b.has_codec and not b.keep_codec_supported]
        for b in shared + [base.rom_board_without_codec()]:
            with self.assertRaises(plan.PlanError, msg=b.id) as cm:
                plan.make_plan(b, [(b.app_base, base.app_image(5000))], plan.BLANK)
            self.assertIn("no codec sector that can be erased and left blank", str(cm.exception))

    def test_the_other_modes_erase_what_they_write(self):
        """No keep or merge plan of any board has a sector that is erased and not written"""
        for _, b in sorted(boards.BOARDS.items()):
            plans = [plan.make_plan(b, [(b.app_base, base.app_image(5000))], plan.KEEP)]
            for h in b.codec_stock[:1] if b.codec_layouts else ():
                stock, profiles, _, _ = stockfw.build_synthetic_stock(KEY, random.Random(7), h)
                prof, segs = stockfw.extract(stock, KEY, profiles)
                plans.append(plan.make_plan(b, [(b.app_base, base.app_image(5000))], plan.MERGE, segs,
                                            b.merge_windows(prof)))
            for p in plans:
                self.assertEqual(p.erase, [st.sector for st in p.writes], b.id)
                self.assertEqual({st.action for st in p.steps} - {"write", "keep", "skip"}, set(), b.id)
                self.assertNotIn("blank", "\n".join(p.describe()), b.id)


class Registry(PlainCodec):
    def test_the_codec_sectors_follow_the_region_and_end_at_the_descriptor(self):
        """What lets the route erase a sector past the application region (dfu.check_plain): the codec sectors are
        one run from the region's end to the end of the descriptor, and every window of every layout is in them"""
        for b in [b for b in boards.by(family="dfuse") if b.has_codec]:
            lo, hi = b.app_region
            secs = b.codec_sectors()
            self.assertTrue(secs, b.id)
            self.assertEqual(secs, b.sectors_for(hi, END), b.id)
            self.assertEqual((secs[0].addr, secs[-1].end), (hi, END), b.id)
            for s, t in zip(secs, secs[1:]):
                self.assertEqual(s.end, t.addr)
            wins = list(b.codec.windows)
            for h in b.codec_stock:
                self.assertEqual(b.codec_layouts[h], [(a, n) for a, n, _ in stockfw.layout(stockfw.PROFILES[h])])
                wins += b.codec_layouts[h]
            for a, n in wins:
                self.assertTrue(hi <= a and a + n <= END, "%s: window 0x%08X+0x%X" % (b.id, a, n))
            self.assertTrue(b.keep_codec_supported, b.id)

    def test_the_firmware_calls_the_entries_of_the_map(self):
        """boards/<board>/vocoder_<board>.c enters the codec at the offsets its codec map gives, name by name, in the
        profile with the tag the merge's descriptor carries (the windows and CRC-32s: test_rfoundry.py,
        FirmwareProfiles). Every board with a codec from a stock file, whatever its route: the map and the firmware
        are written apart, and nothing else compares the two"""
        every = [b for _, b in sorted(boards.BOARDS.items()) if b.codec_stock]
        self.assertTrue(set(x.id for x in self.boards) <= set(x.id for x in every))
        for b in every:
            with open(os.path.join(HERE, "..", "..", "..", "boards", b.id, "vocoder_%s.c" % b.id)) as f:
                src = f.read()
            defs = dict(re.findall(r"#define (\w+)\s+(\w+)u\b", src))
            entries = {}
            for m in re.finditer(r"stm32_codec_profile_t (\w+) = \{(.*?)\n\};", src, re.S):
                tag = re.search(r"\.tag = (\w+)", m.group(2)).group(1)
                at = re.search(r"\.at = (\{[^}]*\}|\w+)", m.group(2)).group(1)
                if not at.startswith("{"):		# a name several profiles share
                    at = re.search(r"#define %s\s+(\{[^}]*\})" % at, src).group(1)
                entries[int(defs.get(tag, tag).rstrip("u"), 0)] = {
                    k: int(v, 16) for k, v in re.findall(r"\.(\w+) = (0x[0-9A-Fa-f]+)", at)}
            for h in b.codec_stock:
                prof = stockfw.PROFILES[h]
                called = entries.get(prof["placement"]["tag"])
                self.assertTrue(called, "%s: no profile with tag 0x%X in the firmware" % (b.id, prof["placement"]["tag"]))
                in_map = stockfw.code_map(prof)["entries"]
                for name, off in called.items():
                    self.assertEqual(in_map.get(name), off, "%s %s: entry %s" % (b.id, prof["version"], name))
                    self.assertLess(off, prof["code"][1])


class CheckPlain(PlainCodec):
    def refused(self, p, text):
        """check_plain refuses p, and so does a run, before any request"""
        with self.assertRaises(dfumod.DfuError) as cm:
            dfumod.check_plain(p)
        self.assertIn(text, str(cm.exception))
        dev = fakedfu.FakeRom(p.board, fakedfu.rom_installed(p.board, random.Random(2)))
        before = bytes(dev.flash)
        with self.assertRaises(dfumod.DfuError):
            dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None).run(p)
        self.assertEqual((dev.requests, bytes(dev.flash) == before), ([], True))

    def test_limits(self):
        for b in self.boards:
            img = base.rom_image(150020)
            for p in (self.merge_plan(b), plan.make_plan(b, [(b.app_base, img)], plan.KEEP, vector_last=True),
                      plan.make_plan(b, [(b.app_base, img)], plan.BLANK, vector_last=True)):
                dfumod.check_plain(p)
            other = "outside the %s's application flash" % b.name
            # a sector past the codec's (the second bank of a chip that has one): never, whatever the step says
            past = [s for s in b.sectors if s.addr >= END]
            for s in past[:1] + past[-1:]:
                for action, content in (("write", "codec"), ("write", "app"), ("erase", "")):
                    p = self.merge_plan(b)
                    i = next(i for i, st in enumerate(p.steps) if st.sector is s)
                    blocks = [(plan.FIRST_BLOCK, FF * plan.BLOCK)] if action == "write" else []
                    p.steps[i] = plan.Step(s, action, blocks, content)
                    self.refused(p, other)
            # a codec sector takes the codec and nothing else
            for content in ("app", "app+codec", ""):
                p = self.merge_plan(b)
                next(st for st in p.steps if st.content == "codec").content = content
                self.refused(p, other)
            # no sector of the application region is erased and left unwritten
            p = plan.make_plan(b, [(b.app_base, img)], plan.BLANK, vector_last=True)
            free = next(i for i, st in enumerate(p.steps) if st.action == "skip" and st.sector.addr < b.app_region[1])
            p.steps[free] = plan.Step(p.steps[free].sector, "erase")
            self.refused(p, "writes nothing there")
            # the order still holds with the codec's sector in the plan
            p = plan.make_plan(b, [(b.app_base, img)], plan.MERGE, synthetic(b)[3],
                               b.merge_windows(synthetic(b)[2]))
            self.refused(p, "first and write it last")
            # and the erase order by itself: the sector with the vector table is erased before any other, so no
            # vector table stands while the codec's sector is erased or written (what a keep later rests on)

            class Late(plan.Plan):
                @property
                def erase_order(self):
                    return list(self.erase)[::-1]

            good = self.merge_plan(b)
            p = Late(b, good.mode, good.steps, vector_last=True)
            self.assertEqual(([st.sector for st in p.write_order], p.erase_order[-1].addr),
                             ([st.sector for st in good.write_order], b.app_base))
            self.assertNotEqual(p.erase_order[0].addr, b.app_base)
            self.refused(p, "first and write it last")

    def test_a_board_without_a_codec_route(self):
        """Nothing past its application region, as before: no step of any kind"""
        b = base.rom_board_without_codec()
        img = base.rom_image(150020)
        p = plan.make_plan(b, [(b.app_base, img)], plan.KEEP, vector_last=True)
        dfumod.check_plain(p)
        self.assertEqual(b.codec_sectors(), [])
        past = next(i for i, st in enumerate(p.steps) if st.sector.addr >= b.app_region[1])
        for action, content in (("write", "codec"), ("write", "app"), ("erase", "")):
            q = plan.make_plan(b, [(b.app_base, img)], plan.KEEP, vector_last=True)
            q.steps[past] = plan.Step(q.steps[past].sector, action,
                                      [(plan.FIRST_BLOCK, FF * plan.BLOCK)] if action == "write" else [], content)
            self.refused(q, "outside the %s's application flash" % b.name)


class Decision(PlainCodec):
    def test_stock_hint_names_every_boards_own_files(self):
        every = cli.stock_hint()
        for b in [b for b in boards.BOARDS.values() if b.codec_stock]:
            hint = cli.stock_hint(b)
            own = [stockfw.PROFILES[h] for h in b.codec_stock if not stockfw.codec_only(stockfw.PROFILES[h])]
            for p in own:
                self.assertIn(p["name"], hint, b.id)
                self.assertIn(p["name"], every, b.id)
            if own and b.stock_download:
                self.assertIn(b.stock_download, hint, b.id)
            self.assertTrue(cli.stock_hint(b, key=True))
        for b in self.boards:		# the radio's own file: listed as its stock firmware file, never as another radio's
            self.assertIn("the %s stock firmware file " % b.name, cli.stock_hint(b))
            self.assertNotIn("for the codec only", cli.stock_hint(b))

    def test_before_the_loader_is_asked(self):
        """The rows of the route's cases that need no loader"""
        for b in self.boards:
            self.assertTrue(b.keep_codec_supported)
            self.assertEqual(b.codec.installed, [])	# no stock firmware leaves the codec where RadioFoundry keeps it
            name = b.debuglink_names[0]
            probe = lambda c: {"board": name, "codec": c}	# noqa: E731
            dc = lambda *a, **k: cli.decide_codec(b, {}, *a, **k)	# noqa: E731
            # on the maker's firmware: with the stock file a merge from it, without it refused
            d = dc("auto", True, None, None, None)
            self.assertEqual((d.mode, d.source, d.moves), (plan.MERGE, "stock", False))
            with self.assertRaises(cli.UsageError) as cm:
                dc("auto", False, None, None, None)
            self.assertIn("can't tell whether the radio holds the codec", str(cm.exception))
            self.assertIn("--stock FILE", str(cm.exception))
            for p in [stockfw.PROFILES[h] for h in b.codec_stock]:
                self.assertIn(p["name"], str(cm.exception))
            with self.assertRaises(cli.UsageError):		# what the radio runs doesn't help: no version holds it there
                dc("auto", False, None, "V1.00", None)
            # RadioFoundry with its codec: kept, also with a file at hand
            for have in (False, True):
                d = dc("auto", have, probe(b.codec.id), None, None, have_saved=have)
                self.assertEqual((d.mode, d.expect_codec), (plan.KEEP, True))
            # RadioFoundry without one: the saved copy before the stock file, else refused
            self.assertEqual(dc("auto", True, probe("absent"), None, None).source, "stock")
            self.assertEqual(dc("auto", True, probe("absent"), None, None, have_saved=True).source, "saved")
            self.assertEqual(dc("auto", False, None, None, None, have_saved=True).source, "saved")
            with self.assertRaises(cli.UsageError):
                dc("auto", False, probe("absent"), None, None)
            # asked for
            self.assertEqual(dc("keep", True, None, None, None, have_saved=True).mode, plan.KEEP)
            self.assertEqual(dc("merge", True, probe(b.codec.id), None, None).mode, plan.MERGE)
            with self.assertRaises(cli.UsageError):
                dc("merge", False, None, None, None)

    def test_the_keep_settled_by_the_loader(self):
        b = self.b
        args = lambda codec="auto": mock.Mock(codec=codec)	# noqa: E731
        keep = cli.Decision(plan.KEEP, "kept")
        merge = cli.Decision(plan.MERGE, "merged")
        none = cli.Decision(plan.BLANK, "none", expect_codec=False)
        sk = cli.settle_keep
        # a merge and an install without a codec stand whatever the loader says; so does a keep on a radio that is
        # not read protected and holds a vector table
        for protected, blank in ((True, False), (False, True), (False, False)):
            for d in (merge, none):
                self.assertIs(sk(args(), b, d, protected, blank, None, False), d)
        for saved, stock in ((None, False), ("copy", True)):
            self.assertIs(sk(args(), b, keep, False, False, saved, stock), keep)
            self.assertIs(sk(args("keep"), b, keep, False, False, saved, stock), keep)
        for protected, blank, why in ((True, False, "read protected, and removing the protection erases the codec"),
                                      (False, True, "holds no vector table: its last install did not finish")):
            # void: the saved copy before the stock file
            d = sk(args(), b, keep, protected, blank, "copy", True)
            self.assertEqual((d.mode, d.source), (plan.MERGE, "saved"))
            self.assertIn(why, d.why)
            d = sk(args(), b, keep, protected, blank, None, True)
            self.assertEqual((d.mode, d.source), (plan.MERGE, "stock"))
            self.assertIn(why, d.why)
            # no source: refused, with both ways on
            with self.assertRaises(cli.UsageError) as cm:
                sk(args(), b, keep, protected, blank, None, False)
            msg = str(cm.exception)
            self.assertIn(why, msg)
            self.assertIn("Flash with --stock FILE to write the codec (%s)" % cli.stock_hint(b), msg)
            self.assertIn("--codec none to install without one (FM works, no DMR voice)", msg)
            # a keep that was asked for: refused, and the option named as what stops the merge, with the source
            # that would be used without it (adding --stock to the same command would only repeat the refusal)
            for saved, stock, how in (
                    ("copy", True, "run the same command without it, and the codec is written from the copy saved "
                                   "from the radio earlier"),
                    (None, True, "run the same command without it, and the codec is written from your stock file"),
                    (None, False, "leave it out and flash with --stock FILE to write the codec (%s)"
                                  % cli.stock_hint(b))):
                with self.assertRaises(cli.UsageError) as cm:
                    sk(args("keep"), b, keep, protected, blank, saved, stock)
                msg = str(cm.exception)
                self.assertIn(why, msg)
                self.assertIn(". --codec keep can't be honored here: " + how + "; ", msg)
                self.assertIn("; or flash with --codec none to install without one (FM works, no DMR voice)", msg)
                self.assertNotIn("Flash with --stock FILE", msg)


class SimulatedInstalls(PlainCodec):
    def test_first_install_with_the_stock_file(self):
        for b in self.boards:
            rc, out, err, dev = self.sim(b, "--simulate", "stock", "--unprotect", "--stock", "synthetic")
            self.assertEqual(rc, 0, out + err)
            self.assertIn("codec    MERGE: the radio's codec is unknown, so it is written from your stock file", out)
            self.assertIn("board %s, codec mode: merge" % b.name, out)
            self.assertIn("sim      PASS", out)
            self.assertTrue(dev.unprotected and dev.exited)
            self.holds_codec(b, dev)
            self.assertEqual((dev.alt_selected, dev.refused, dev.partial, dev.violations), ([], [], [], []))
            # what the firmware and every tool find there afterwards: this codec, every window checked
            _, profiles, prof, segs = synthetic(b)
            self.assertEqual(stockfw.from_flash(bytes(dev.flash), b.rom_base, profiles), (prof, segs))
            # the order the loader saw: the sector with the vector table erased first; the image's other sectors
            # written, then the codec's, then the rest of the first sector, and the vector table's block last
            image = stockfw.random_bytes(random.Random(1), cli.SIM_IMAGE_SIZE)
            p = plan.make_plan(b, [(b.app_base, image)], plan.MERGE, segs, b.merge_windows(prof), vector_last=True)
            self.assertEqual(dev.erased, [s.addr for s in p.erase_order])
            self.assertEqual(dev.erased[0], b.app_base)
            want = [st.sector.addr + (num - plan.FIRST_BLOCK) * plan.BLOCK for st in p.write_order for num, _ in st.blocks]
            want.remove(b.app_base)
            self.assertEqual(dev.placed, want + [b.app_base])
            first = b.sector_at(b.app_base)
            codec = [i for i, a in enumerate(dev.placed) if b.sector_at(a) in b.codec_sectors()]
            head = [i for i, a in enumerate(dev.placed) if b.sector_at(a) is first]
            self.assertTrue(codec and head and max(codec) < min(head))

    def test_first_install_without_a_source(self):
        """Refused before a device is looked for, and the way to install without a codec is named"""
        for b in self.boards:
            for state_ in ("stock", "blank", "rfoundry-nocodec"):
                rc, out, err, dev = self.sim(b, "--simulate", state_, "--unprotect")
                self.assertEqual(rc, 2, out + err)
                self.assertIsNone(dev)
                self.assertIn("--stock FILE", err)
                self.assertTrue(err.rstrip().endswith(
                    "To install without a codec (FM works, no DMR voice): --codec none"), err)
            self.assertIn("can't tell whether the radio holds the codec", self.sim(b, "--simulate", "stock")[2])

    def test_install_without_a_codec(self):
        for b in self.boards:
            for extra, state_ in ((["--unprotect"], "stock"), ([], "blank"), ([], "rfoundry-codec"),
                                  ([], "rfoundry-nocodec")):
                rc, out, err, dev = self.sim(b, "--simulate", state_, "--codec", "none", *extra)
                self.assertEqual(rc, 0, state_ + out + err)
                self.assertIn("codec    BLANK: asked for: the radio is installed without a codec", out)
                self.assertIn("DMR voice will stay off (FM works) until you flash with --stock", err)
                self.assertIn("board %s, codec mode: blank" % b.name, out)
                self.assertIn("sim      PASS", out)
                self.blank(b, dev)
                for s in b.codec_sectors():		# erased by the plan, also right after a Read Unprotect
                    self.assertIn(s.addr, dev.erased)
                    self.assertIn("sim      codec sector %d blank" % s.index, out)
                self.assertEqual(dev.read(b.app_base, cli.SIM_IMAGE_SIZE),
                                 stockfw.random_bytes(random.Random(1), cli.SIM_IMAGE_SIZE))
            # a stock file given with it is not used, and says so
            rc, out, err, dev = self.sim(b, "--simulate", "blank", "--codec", "none", "--stock", "synthetic")
            self.assertEqual(rc, 0, out + err)
            self.assertIn("--codec none: the codec in the stock file is not written", out)
            self.blank(b, dev)

    def test_update_keeps_the_codec(self):
        """A radio that runs RadioFoundry with its codec, not read protected, with a vector table: the codec's
        sector is not in the plan and is byte for byte as before"""
        for b in self.boards:
            rc, out, err, dev = self.sim(b, "--simulate", "rfoundry-codec")
            self.assertEqual(rc, 0, out + err)
            self.assertIn("codec    KEEP: RadioFoundry on the radio checked its codec (%s" % b.codec.id, out)
            self.assertIn("sim      PASS", out)
            self.holds_codec(b, dev)
            for s in b.codec_sectors():
                self.assertNotIn(s.addr, dev.erased)
                self.assertTrue(dev.read(s.addr, s.size) == dev.before[s.addr - b.rom_base:s.end - b.rom_base])
                self.assertIn("never erased or written: codec sector(s) %d" % s.index, out)
                self.assertIn("sim      codec sector %d unchanged" % s.index, out)
                self.assertFalse([a for a in dev.placed if s.addr <= a < s.end])
            self.assertFalse(dev.unprotected)
            # --unprotect on it removes nothing, and the keep stands
            rc, out, err, dev = self.sim(b, "--simulate", "rfoundry-codec", "--unprotect", "--stock", "synthetic")
            self.assertEqual(rc, 0, out + err)
            self.assertIn("not read protected: nothing to remove", out)
            self.assertIn("codec mode: keep", out)
            self.holds_codec(b, dev)
            self.assertFalse(dev.unprotected)

    def test_update_writes_a_missing_codec(self):
        for b in self.boards:
            rc, out, err, dev = self.sim(b, "--simulate", "rfoundry-nocodec", "--stock", "synthetic")
            self.assertEqual(rc, 0, out + err)
            self.assertIn("codec    MERGE: the radio has no codec to keep (codec=absent)", out)
            self.assertIn("sim      PASS", out)
            self.holds_codec(b, dev)
            self.assertFalse(dev.unprotected)
            rc, out, err, dev = self.sim(b, "--simulate", "blank", "--stock", "synthetic")
            self.assertEqual(rc, 0, out + err)
            self.holds_codec(b, dev)

    def test_what_does_not_apply(self):
        b = self.b
        with tempfile.TemporaryDirectory() as d:
            kp = os.path.join(d, "key.bin")
            with open(kp, "wb") as f:
                f.write(KEY)
            for extra in ([], ["--stock", "synthetic"], ["--codec", "none"]):
                rc, out, err, dev = self.sim(b, "--simulate", "stock", "--unprotect", "--key", kp, *extra)
                self.assertEqual(rc, 2, out + err)
                self.assertIn("the %s's bootloader has no key" % b.name, err)
                self.assertIsNone(dev)
        # --codec none is this route's: a keyed bootloader's codec sectors are kept or written, never left erased
        keyed = next(x for x in boards.by(family="usb-dfu") if x.has_codec)
        rc, out, err = base.run_cli(["flash", "--board", keyed.id, "--simulate", "rfoundry-codec", "--codec", "none"])
        self.assertEqual(rc, 2, out + err)
        self.assertIn("--codec none is for a radio installed through its processor's own bootloader", err)

    def test_an_image_past_the_application_region(self):
        """Refused before a device is looked for, whatever the codec mode: nothing is unprotected or erased for an
        image that can't be written"""
        b = self.b
        lo, hi = b.app_region
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            with open(bp, "wb") as f:
                f.write(base.rom_image(hi - lo + 4))
            for extra in (["--stock", "synthetic"], ["--codec", "none"], ["--codec", "keep"],
                          ["--stock", "synthetic", "--dry-run"]):
                rc, out, err, dev = self.sim(b, bp, "--simulate", "stock", "--unprotect", *extra)
                self.assertEqual(rc, 2, out + err)
                self.assertIn("reaches into codec sector(s)", err)
                self.assertIsNone(dev)

    def test_dry_run_says_the_loader_is_asked_first(self):
        b = self.b
        for extra in (["--stock", "synthetic"], ["--codec", "none"], ["--codec", "keep"]):
            rc, out, err, dev = self.sim(b, "--simulate", "stock", "--dry-run", *extra)
            self.assertEqual(rc, 0, out + err)
            self.assertIsNone(dev)
            self.assertIn("dry run: nothing was erased or written", out)
            self.assertIn("asks the radio's bootloader first, before anything is erased", out)
            self.assertIn("only on a radio that is not read protected and holds a vector table", out)

    def test_an_image_with_the_codec_merged_in(self):
        """rfoundry merge's file, or a build's with the stock file at hand: the codec comes from the image, with no
        --board and no --stock, and the application is the image's own from the board's base"""
        for b in self.boards:
            _, profiles, prof, segs = synthetic(b)
            app = base.rom_image(70004)
            whole = stockfw.merge(app, prof, segs)
            self.assertEqual(len(whole), END - b.app_base)
            with tempfile.TemporaryDirectory() as d, mock.patch.dict(stockfw.PROFILES, profiles):
                bp = os.path.join(d, "merged.bin")
                with open(bp, "wb") as f:
                    f.write(whole)
                made = []

                class Spy(fakedfu.FakeRomUsbDevice):
                    def __init__(self, *a, **kw):
                        super().__init__(*a, **kw)
                        made.append(self)

                for extra in ([], ["--board", b.id], ["--stock", "synthetic"]):
                    del made[:]
                    with mock.patch.object(fakedfu, "FakeRomUsbDevice", Spy):
                        rc, out, err = base.run_cli(["flash", bp, "--simulate", "blank"] + extra)
                    self.assertEqual(rc, 0, out + err)
                    self.assertIn("with the codec merged in (%s, every window verified by SHA-256)" % b.codec.id, out)
                    self.assertIn("codec    MERGE: the image carries the codec", out)
                    self.assertIn("sim      PASS", out)
                    self.assertEqual(made[0].read(b.app_base, len(app)), app)
                    self.holds_codec(b, made[0], segs)
                    self.assertEqual("stock file is not used" in out, bool(extra[:1] == ["--stock"]))
                # --codec keep writes its application part only; on a radio without a vector table that is refused
                with mock.patch.object(fakedfu, "FakeRomUsbDevice", Spy):
                    rc, out, err = base.run_cli(["flash", bp, "--simulate", "rfoundry-codec", "--codec", "keep"])
                    self.assertEqual(rc, 0, out + err)
                    self.assertIn("codec mode: keep", out)
                    rc, out, err = base.run_cli(["flash", bp, "--simulate", "blank", "--codec", "keep"])
                    self.assertEqual(rc, 2, out + err)
                    self.assertIn("holds no vector table", err)
                # another radio's --board is refused by the codec's name
                other = next(x for x in boards.by(family="usb-dfu") if x.has_codec)
                rc, out, err = base.run_cli(["flash", bp, "--board", other.id, "--simulate", "blank"])
                self.assertEqual(rc, 2, out + err)
                self.assertIn("holds the %s codec, but --board says %s" % (b.name, other.name), err)


class KeepOnAProtectedRadio(PlainCodec):
    """A keep rests on what a probe said, or on --codec keep. A read-protected radio voids it: removing the protection
    erases the codec with everything else"""

    def test_simulated(self):
        for b in self.boards:
            for yes in ([], ["--yes"]):
                rc, out, err, dev = self.sim(b, "--simulate", "rfoundry-codec", "--unprotect", *yes, protected=True)
                self.assertEqual(rc, 2, out + err)
                self.assertIn("codec    KEEP", out)
                self.assertIn("the radio's program memory is read protected, and removing the protection erases the "
                              "codec with everything else. Flash with --stock FILE", err)
                self.assertIn("--codec none", err)
                # nothing was sent but the probe: no Read Unprotect, no erase, no block
                self.assertEqual((dev.unprotected, dev.protected, dev.erased, dev.blocks), (False, True, [], 0))
                self.assertTrue(bytes(dev.flash) == dev.before)
                self.assertNotIn(bytes([dfumod.CMD_READ_UNPROTECT]),
                                 [r[3] for r in dev.requests if r[1] == dfumod.DFU_DNLOAD])
            # with a source it becomes a merge, said before the protection is removed
            rc, out, err, dev = self.sim(b, "--simulate", "rfoundry-codec", "--unprotect", "--stock", "synthetic",
                                         protected=True)
            self.assertEqual(rc, 0, out + err)
            self.assertIn("codec    MERGE: the radio's program memory is read protected, and removing the protection "
                          "erases the codec with everything else; the codec is written from your stock file", out)
            self.assertLess(out.index("codec    MERGE"), out.index("erasing  the radio removes its read protection"))
            self.assertLess(out.index("codec mode: keep"), out.index("codec mode: merge"))
            self.assertIn("sim      PASS", out)
            self.assertTrue(dev.unprotected)
            self.holds_codec(b, dev)
            # a keep that was asked for is not turned into a merge
            for extra in ([], ["--stock", "synthetic"]):
                rc, out, err, dev = self.sim(b, "--simulate", "rfoundry-codec", "--unprotect", "--codec", "keep",
                                             *extra, protected=True)
                self.assertEqual(rc, 2, out + err)
                self.assertIn("removing the protection erases the codec with everything else", err)
                self.assertEqual((dev.unprotected, dev.erased, dev.blocks), (False, [], 0))
            # without --unprotect and with a source: the merge is said, then the read protection stops the install
            rc, out, err, dev = self.sim(b, "--simulate", "rfoundry-codec", "--stock", "synthetic", protected=True)
            self.assertEqual(rc, 2, out + err)
            self.assertIn("codec mode: merge", out)
            self.assertIn("run the same command with --unprotect", err)
            self.assertEqual((dev.unprotected, dev.erased, dev.blocks), (False, [], 0))


class RestartBesideTheCodecStep(PlainCodec):
    """A board with a codec route whose firmware restarts into its processor's bootloader when the debug link asks
    (the restart itself: test_rfoundry.py RestartIntoBootloader). The codec step comes first, while the radio still
    runs RadioFoundry and can be asked about its codec; the restart comes after it, in place of the keys, and the keep
    is still settled by what the loader then says."""
    HINT = "Do what the board says and try again."

    def setUp(self):
        super().setUp()
        self.made = []
        made, real = self.made, fakedfu.FakeRunning

        class Spy(real):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                made.append(self)

        for p in [mock.patch.object(fakedfu, "FakeRunning", Spy)] + [
                mock.patch.object(b, k, v, create=True) for b in self.boards
                for k, v in (("bootloader_restart", True), ("bootloader_restart_hint", self.HINT))]:
            p.start()
            self.addCleanup(p.stop)

    def run_sim(self, b, *argv, **force):
        del self.made[:]
        rc, out, err, dev = self.sim(b, *argv, **force)
        return rc, out, err, dev, (self.made[0] if self.made else None)

    def test_simulated(self):
        for b in self.boards:
            # an update that keeps the codec, and one that writes a missing one: the decision, then the restart
            for argv, mode in ((("--simulate", "rfoundry-codec"), "KEEP"),
                               (("--simulate", "rfoundry-nocodec", "--stock", "synthetic"), "MERGE")):
                rc, out, err, dev, running = self.run_sim(b, *argv)
                self.assertEqual(rc, 0, out + err)
                self.assertLess(out.index("codec    " + mode), out.index("restarting it into its bootloader"))
                self.assertLess(out.index("restarting it into its bootloader"), out.index("found    bootloader"))
                self.assertNotIn("with its keys", out)
                self.assertEqual(len(running.requests), 1)
                self.assertEqual(running.codec, b.codec.id if mode == "KEEP" else "absent")
                self.assertIn("sim      PASS", out)
                self.holds_codec(b, dev)
                self.assertFalse(dev.unprotected)
            # a radio that refuses: its codec is kept, the board's hint is said, and it is put there with its keys
            rc, out, err, dev, running = self.run_sim(b, "--simulate", "rfoundry-refuses")
            self.assertEqual(rc, 0, out + err)
            self.assertIn("codec    KEEP: RadioFoundry on the radio checked its codec (%s" % b.codec.id, out)
            self.assertIn("it can't right now. Nothing was changed", out)
            self.assertLess(out.index(self.HINT), out.index("found    bootloader"))
            self.assertIn("(simulated) switched off and put in its bootloader with its keys", out)
            self.assertEqual(len(running.requests), 1)
            self.assertIn("sim      PASS", out)
            self.holds_codec(b, dev)
            # a codec decision that is refused, and a dry run, restart nothing
            for argv, code in ((("--simulate", "rfoundry-nocodec"), 2),
                               (("--simulate", "rfoundry-codec", "--dry-run"), 0)):
                rc, out, err, dev, running = self.run_sim(b, *argv)
                self.assertEqual(rc, code, out + err)
                self.assertIsNone(running, argv)
                self.assertIsNone(dev, argv)
                self.assertNotIn("restart", out, argv)
            # --codec none: no codec step to come first, and the restart all the same
            rc, out, err, dev, running = self.run_sim(b, "--simulate", "rfoundry-codec", "--codec", "none")
            self.assertEqual(rc, 0, out + err)
            self.assertLess(out.index("codec    BLANK"), out.index("restarting it into its bootloader"))
            self.assertEqual(len(running.requests), 1)
            self.blank(b, dev)

    def test_the_keep_is_settled_after_the_restart(self):
        """The loader a restart reached answers as one the keys reached: a radio without a vector table voids the
        keep, and it becomes a merge from the stock file"""
        for b in self.boards:
            flash = fakedfu.rom_installed(b, random.Random(4), synthetic(b)[3], vectors=False)
            rc, out, err, dev, running = self.run_sim(b, "--simulate", "rfoundry-codec", "--stock", "synthetic",
                                                      flash=flash)
            self.assertEqual(rc, 0, out + err)
            self.assertEqual(len(running.requests), 1)
            self.assertLess(out.index("restarting it into its bootloader"), out.index("codec    MERGE: the radio holds "
                                                                                      "no vector table"))
            self.assertIn("sim      PASS", out)
            self.holds_codec(b, dev)

    def test_real_run(self):
        """Not simulated: the live probe says the codec, the codec step runs while the radio runs RadioFoundry, then
        the radio is restarted, not the keys asked for, and the codec's sector is left as it was"""
        for b in self.boards:
            segs = synthetic(b)[3]
            dev = fakedfu.FakeRomUsbDevice(b, fakedfu.rom_installed(b, random.Random(3), segs))
            before = bytes(dev.flash)
            running = fakedfu.FakeRunning(b, dev, codec=b.codec.id)
            link = debuglink.Link
            with tempfile.TemporaryDirectory() as d:
                bp = os.path.join(d, "image.bin")
                with open(bp, "wb") as f:
                    f.write(base.rom_image(9000))
                with mock.patch.object(debuglink, "Link", side_effect=lambda port, *a, **kw: link(running, *a, **kw)), \
                        mock.patch.object(debuglink, "probe", return_value=dict(running.probe(), port="/dev/ttyFAKE0")), \
                        mock.patch.object(cli, "debug_links", return_value=["/dev/ttyFAKE0"]), \
                        mock.patch.object(cli, "get_saved_codec", return_value=None), \
                        mock.patch.object(dfumod, "import_usb", return_value=fakedfu.FakeUsb([dev])), \
                        mock.patch.object(cli, "wait_for_dfu", side_effect=AssertionError("the keys were asked for")), \
                        mock.patch.object(cli.time, "sleep"):
                    rc, out, err = base.run_cli(["flash", bp, "--board", b.id, "--yes", "--no-verify",
                                                 "--no-reporting"])
            self.assertEqual(rc, 0, out + err)
            self.assertLess(out.index("codec    KEEP"), out.index("restarting it into its bootloader"))
            self.assertEqual(len(running.requests), 1)
            for s in b.codec_sectors():
                self.assertNotIn(s.addr, dev.erased)
                self.assertTrue(dev.read(s.addr, s.size) == before[s.addr - b.rom_base:s.end - b.rom_base])
            self.assertTrue(dev.exited)


def remembered_probe(args, log=None):
    """What cli.get_probe gives when no radio answers on the debug link: the remembered probe, if any. The runs that
    use it carry --no-probe, so nothing is asked over a serial port or the network whatever this stand-in does"""
    return state.recent_probe()


class RealRun(PlainCodec):
    """Runs that are not simulations, against the fake loader: the probe is the remembered one (state.py, in the
    tests' own settings folder), the stock file a synthetic one on disk. Every run carries --no-probe, and
    cli.get_probe is replaced by the half of it that needs no radio (remembered_probe)."""

    def setUp(self):
        super().setUp()
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.image = base.rom_image(9004)
        self.bp = self.path("image.bin", self.image)

    def path(self, name, data):
        p = os.path.join(self.dir.name, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def remembered(self, b, codec):
        state.remember_probe({"board": b.debuglink_names[0], "git": "00000000", "codec": codec, "port": "gone",
                              "uid": None})

    def run_real(self, b, dev, *argv, answer=None, comes_back=True, verify=None):
        """(exit code, stdout, stderr, the questions asked). answer: what every question gets; None fails the test at
        a question. comes_back: after a Read Unprotect the radio is put in its bootloader again. verify: a stand-in
        for cli.verify_after, which the run then calls after the install (without one the run carries --no-verify)"""
        usb = fakedfu.FakeUsb([dev])
        asked = []

        def ask(q, default=False):
            asked.append(q)
            self.assertIsNotNone(answer, "asked: %s" % q)
            return answer

        def left(u, board, was, sent):		# after a Read Unprotect: the radio leaves USB, then the user's part
            self.assertTrue(dev.gone and dev.unprotected)
            if comes_back:
                dev.power_cycle()
            return True, None

        with mock.patch.dict(stockfw.PROFILES, synthetic(b)[1]), \
                mock.patch.object(cli, "get_probe", remembered_probe), \
                mock.patch.object(debuglink, "probe", return_value=None), \
                mock.patch.object(debuglink, "find_ports", return_value=None), \
                mock.patch.object(dfumod, "import_usb", return_value=usb), \
                mock.patch.object(cli, "wait_unprotect", side_effect=left), \
                mock.patch.object(cli, "verify_after", verify or mock.Mock(side_effect=AssertionError)), \
                mock.patch.object(cli.time, "sleep"), mock.patch.object(cli, "ask_yes", ask):
            rc, out, err = base.run_cli(["flash", self.bp, "--board", b.id, "--wait", "0", "--no-probe",
                                         "--no-reporting"] + ([] if verify else ["--no-verify"]) + list(argv))
        return rc, out, err, asked

    def test_a_remembered_probe_and_a_protected_radio(self):
        b = self.b
        stock, _, prof, segs = synthetic(b)
        sp = self.path("stock.bin", stock)
        for yes in ([], ["--yes"]):
            self.remembered(b, b.codec.id)
            dev = fakedfu.FakeRomUsbDevice(b, fakedfu.rom_flash(b, random.Random(5)), protected=True)
            before = bytes(dev.flash)
            rc, out, err, asked = self.run_real(b, dev, "--unprotect", *yes)
            self.assertEqual(rc, 2, out + err)
            self.assertIn("codec    KEEP: RadioFoundry on the radio checked its codec", out)
            self.assertIn("removing the protection erases the codec with everything else", err)
            self.assertEqual(asked, [])				# refused before the Read Unprotect question
            self.assertEqual((dev.unprotected, dev.protected, bytes(dev.flash) == before), (False, True, True))
            self.assertIsNotNone(state.recent_probe())		# nothing was erased: what it says still holds
        # with the stock file: a merge, one question (the Read Unprotect), and the probe forgotten before the erase
        self.remembered(b, b.codec.id)
        dev = fakedfu.FakeRomUsbDevice(b, fakedfu.rom_flash(b, random.Random(5)), protected=True)
        rc, out, err, asked = self.run_real(b, dev, "--unprotect", "--stock", sp, answer=True)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(len(asked), 1)
        self.assertIn("Remove the read protection now?", asked[0])
        self.assertIn("stock    %s verified (SHA-256), codec windows decrypted and checked" % prof["name"], out)
        self.assertLess(out.index("the codec is written from your stock file"), out.index("erasing  "))
        self.assertTrue(dev.unprotected and dev.exited)
        self.assertEqual(dev.read(b.app_base, len(self.image)), self.image)
        self.holds_codec(b, dev, segs)
        self.assertIsNone(state.recent_probe())
        # the Read Unprotect is sent and the radio never comes back: the codec the probe spoke of is erased, so the
        # probe is gone before the command went out, not only once the install has finished
        self.remembered(b, b.codec.id)
        dev = fakedfu.FakeRomUsbDevice(b, fakedfu.rom_flash(b, random.Random(5)), protected=True)
        rc, out, err, asked = self.run_real(b, dev, "--unprotect", "--yes", "--stock", sp, comes_back=False)
        self.assertEqual(rc, 2, out + err)
        self.assertIn("The read protection is removed and the program memory is empty", err)
        self.assertEqual((dev.unprotected, dev.erased, dev.blocks), (True, [], 0))
        self.assertIsNone(state.recent_probe())
        # "no" to that question: nothing done, and the probe is still there
        self.remembered(b, b.codec.id)
        dev = fakedfu.FakeRomUsbDevice(b, fakedfu.rom_flash(b, random.Random(5)), protected=True)
        rc, out, err, asked = self.run_real(b, dev, "--unprotect", "--stock", sp, answer=False)
        self.assertEqual((rc, dev.unprotected, len(asked)), (1, False, 1), out + err)
        self.assertIsNotNone(state.recent_probe())

    def test_a_saved_copy_comes_before_the_stock_file(self):
        b = self.b
        stock, profiles, prof, segs = synthetic(b)
        with mock.patch.dict(stockfw.PROFILES, profiles):
            copy = codecsave.save(b, segs[:len(stockfw.placed_windows(prof))])
        self.addCleanup(os.remove, copy)
        self.remembered(b, b.codec.id)
        dev = fakedfu.FakeRomUsbDevice(b, fakedfu.rom_flash(b, random.Random(5)), protected=True)
        rc, out, err, _ = self.run_real(b, dev, "--unprotect", "--yes", "--stock", self.path("stock.bin", stock))
        self.assertEqual(rc, 0, out + err)
        self.assertIn("erases the codec with everything else; the copy saved from the radio earlier is written", out)
        self.assertNotIn("stock    %s verified" % prof["name"], out)
        self.holds_codec(b, dev, segs)
        # and without any file: a cut update (no vector table) of a radio whose codec was saved is finished from it
        self.remembered(b, b.codec.id)
        dev = fakedfu.FakeRomUsbDevice(b, fakedfu.rom_installed(b, random.Random(4), segs, vectors=False))
        rc, out, err, _ = self.run_real(b, dev, "--yes")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("holds no vector table: its last install did not finish, so its codec sector may be half "
                      "written; the copy saved from the radio earlier is written", out)
        self.holds_codec(b, dev, segs)
        for s in b.codec_sectors():
            self.assertIn(s.addr, dev.erased)

    def test_a_keep_with_a_vector_table(self):
        """The loader shows a radio that is not read protected and holds a vector table: the remembered probe's keep
        stands, and the codec's sector is not touched"""
        b = self.b
        stock, _, prof, segs = synthetic(b)
        self.remembered(b, b.codec.id)
        dev = fakedfu.FakeRomUsbDevice(b, fakedfu.rom_installed(b, random.Random(4), segs))
        before = bytes(dev.flash)
        rc, out, err, asked = self.run_real(b, dev, "--stock", self.path("stock.bin", stock), answer=True)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(len(asked), 1)
        self.assertIn("Erase and write 1 sector(s) now?", asked[0])
        self.assertIn("codec mode: keep", out)
        self.assertNotIn("codec mode: merge", out)
        for s in b.codec_sectors():
            self.assertNotIn(s.addr, dev.erased)
            self.assertTrue(dev.read(s.addr, s.size) == before[s.addr - b.rom_base:s.end - b.rom_base])
        self.assertEqual(dev.read(b.app_base, len(self.image)), self.image)
        self.assertIsNone(state.recent_probe())		# after a finished install, as on every route

    def test_the_check_after_the_install_gets_the_decision(self):
        """What the radio must report after the restart depends on the decision: a codec after a merge or a keep,
        none after an install without one"""
        b = self.b
        stock, _, prof, segs = synthetic(b)
        sp = self.path("stock.bin", stock)
        for argv, codec, mode, expect in ((["--stock", sp], None, plan.MERGE, True),
                                          (["--codec", "none"], None, plan.BLANK, False),
                                          ([], b.codec.id, plan.KEEP, True)):
            state.forget_probe()
            if codec:
                self.remembered(b, codec)
            dev = fakedfu.FakeRomUsbDevice(b, fakedfu.rom_installed(b, random.Random(4), segs))
            verify = mock.Mock(return_value=0)
            rc, out, err, _ = self.run_real(b, dev, "--yes", *argv, verify=verify)
            self.assertEqual(rc, 0, out + err)
            self.assertTrue(dev.exited)
            (args, board, man, d), = [c.args for c in verify.call_args_list]
            self.assertEqual((board.id, d.mode, d.expect_codec), (b.id, mode, expect))

    def test_the_probe_is_forgotten_before_the_first_erase(self):
        """A merge that fails at its first write: the probe from before it says nothing true about the codec sector
        any more"""
        b = self.b
        stock, _, prof, segs = synthetic(b)
        sp = self.path("stock.bin", stock)
        self.remembered(b, "absent")
        dev = fakedfu.FakeRomUsbDevice(b, fakedfu.rom_installed(b, random.Random(4)), fail_at=0)
        rc, out, err, _ = self.run_real(b, dev, "--yes", "--stock", sp)
        self.assertEqual(rc, 2, out + err)
        self.assertIn("USB error while flashing", err)
        self.assertEqual(dev.blocks, 0)
        for s in b.codec_sectors():
            self.assertIn(s.addr, dev.erased)
        self.assertIsNone(state.recent_probe())
        # an update that keeps the codec erases no codec sector: cut, its probe is still there, and the next run's
        # keep is settled by the loader, which finds no vector table
        self.remembered(b, b.codec.id)
        dev = fakedfu.FakeRomUsbDevice(b, fakedfu.rom_installed(b, random.Random(4), segs), fail_at=0)
        rc, out, err, _ = self.run_real(b, dev, "--yes")
        self.assertEqual(rc, 2, out + err)
        self.assertIsNotNone(state.recent_probe())
        again = fakedfu.FakeRomUsbDevice(b, bytes(dev.flash))
        rc, out, err, _ = self.run_real(b, again, "--yes")
        self.assertEqual(rc, 2, out + err)
        self.assertIn("holds no vector table", err)
        self.assertEqual((again.erased, again.blocks), ([], 0))
        rc, out, err, _ = self.run_real(b, again, "--yes", "--stock", sp)
        self.assertEqual(rc, 0, out + err)
        self.holds_codec(b, again, segs)

    def test_the_keyed_route_forgets_it_too(self):
        b = next(x for x in boards.by(family="usb-dfu") if x.keep_codec_supported and x.codec_stock)
        stock, profiles, _, _ = stockfw.build_synthetic_stock(KEY, random.Random(7), stockfw.profile_for_board(b.id))

        class Cut(fakedfu.FakeUsbDevice):		# the bus fails at the first block
            def ctrl_transfer(self, rtype, req, value, intf, data, timeout=None):
                if rtype == 0x21 and req == dfumod.DFU_DNLOAD and value >= plan.FIRST_BLOCK:
                    raise OSError("Input/Output Error")
                return super().ctrl_transfer(rtype, req, value, intf, data, timeout)

        def run(dev, *argv):
            with mock.patch.dict(stockfw.PROFILES, profiles), mock.patch.object(cli, "get_key", return_value=KEY), \
                    mock.patch.object(cli, "get_probe", remembered_probe), \
                    mock.patch.object(debuglink, "probe", return_value=None), \
                    mock.patch.object(debuglink, "find_ports", return_value=None), \
                    mock.patch.object(dfumod, "import_usb", return_value=fakedfu.FakeUsb([dev])), \
                    mock.patch.object(cli.time, "sleep"):
                return base.run_cli(["flash", self.path("keyed.bin", base.app_image(9000)), "--board", b.id, "--yes",
                                     "--wait", "0", "--no-probe", "--no-verify", "--no-reporting"] + list(argv))

        name = b.debuglink_names[0]
        state.remember_probe({"board": name, "git": "00000000", "codec": "absent", "port": "gone", "uid": None})
        dev = Cut(b, KEY, fakedfu.stock_flash(b, random.Random(3), []))
        rc, out, err = run(dev, "--stock", self.path("keyed-stock.bin", stock))
        self.assertEqual(rc, 2, out + err)
        self.assertIn("codec mode: merge", out)
        self.assertIn("USB error while flashing", err)
        self.assertTrue({s.addr for s in b.codec_sectors()} <= set(dev.erased))
        self.assertIsNone(state.recent_probe())
        # a keep on that route erases no codec sector, and its probe stays until the install has finished
        state.remember_probe({"board": name, "git": "00000000", "codec": b.codec.id, "port": "gone", "uid": None})
        dev = Cut(b, KEY, fakedfu.stock_flash(b, random.Random(3), []))
        rc, out, err = run(dev)
        self.assertEqual(rc, 2, out + err)
        self.assertIn("codec mode: keep", out)
        self.assertIsNotNone(state.recent_probe())


class CutInstall(PlainCodec):
    """An install cut at every block: no image is ever started over a codec sector that an unfinished install may
    have touched. The fake loader leaves the bus at block k; the radio then holds no vector table, and whatever runs
    next writes the codec's sector whole or erases it whole, or is refused."""

    def check_vectors(self, b, dev, segs, what, whole=True):
        """When the vector table's block came, every codec sector had been erased in that run and was blank or held
        the whole codec. And the order the loader saw: the sector with the vector table erased before any other,
        and (whole: the block was written whole) its first block written after every other block"""
        self.assertIsNotNone(dev.at_vectors, what)
        for s, erased, held in dev.at_vectors:
            self.assertTrue(erased, "%s: sector %d not erased in this run" % (what, s.index))
            self.assertTrue(held in (FF * s.size, sector_image(s, segs)), "%s: sector %d half written" % (what, s.index))
        self.assertEqual(dev.erased[0], b.app_base, what)
        if whole:
            self.assertEqual((dev.placed.count(b.app_base), dev.placed[-1]), (1, b.app_base), what)

    def test_cut_at_every_block(self):
        b = self.b
        _, _, prof, segs = synthetic(b)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            image = base.rom_image(5004)
            with open(bp, "wb") as f:
                f.write(image)
            p = plan.make_plan(b, [(b.app_base, image)], plan.MERGE, segs, b.merge_windows(prof), vector_last=True)
            total = p.total_blocks
            codec_blocks = sum(len(st.blocks) for st in p.writes if st.content == "codec")
            self.assertGreaterEqual(codec_blocks, 100)
            self.assertEqual(p.write_order[0].content, "codec")	# its blocks come first: k below codec_blocks is in it
            merge = ("--simulate", "rfoundry-nocodec", "--stock", "synthetic")
            for k in range(total):
                torn = k % 5 == 2 and k < total - 1	# a torn vector table: test_cut_inside_the_vector_table
                rc, out, err, dev = self.sim(b, bp, *merge, fail_at=k, torn=torn)
                what = "cut at block %d of %d" % (k, total)
                self.assertEqual(rc, 2, what + out + err)
                self.assertEqual(dev.blocks, k, what)
                self.assertFalse(dev.exited, what)
                self.assertIsNone(dev.at_vectors, what)		# nothing of the vector table's block reached the flash
                if k < total - 1:
                    self.assertIn("USB error while flashing", err)
                    self.assertIn("vector table is written last", err)
                else:		# the vector table's own block: everything else was written and read back before it
                    self.assertIn("USB error at the vector table", err)
                    self.holds_codec(b, dev, segs)
                self.assertEqual(dev.read(b.app_base, plan.BLOCK), FF * plan.BLOCK, what)
                if k < codec_blocks:
                    # the descriptor's block is the sector's last: until then its place is blank
                    self.assertEqual(dev.read(stockfw.DESC_ADDR, stockfw.DESC_LEN), FF * stockfw.DESC_LEN, what)
                cut = bytes(dev.flash)
                # the same command again
                rc, out, err, dev = self.sim(b, bp, *merge, flash=cut)
                self.assertEqual(rc, 0, what + out + err)
                self.assertIn("sim      PASS", out)
                self.check_vectors(b, dev, segs, what)
                self.holds_codec(b, dev, segs)
                self.assertEqual(dev.read(b.app_base, len(image)), image)
                self.assertEqual(dev.violations, [], what)
                # an install whose decision is keep (the radio's own report, from before the cut): refused without
                # a source, with nothing sent but the probe
                rc, out, err, dev = self.sim(b, bp, "--simulate", "rfoundry-codec", flash=cut)
                self.assertEqual(rc, 2, what + out + err)
                self.assertIn("codec    KEEP", out)
                self.assertIn("the radio holds no vector table: its last install did not finish, so its codec sector "
                              "may be half written. Flash with --stock FILE", err)
                self.assertEqual((dev.erased, dev.blocks, bytes(dev.flash) == cut), ([], 0, True), what)
                self.assertIsNone(dev.at_vectors)
                # with the stock file it becomes a merge
                rc, out, err, dev = self.sim(b, bp, "--simulate", "rfoundry-codec", "--stock", "synthetic", flash=cut)
                self.assertEqual(rc, 0, what + out + err)
                self.assertIn("codec    MERGE: the radio holds no vector table", out)
                self.check_vectors(b, dev, segs, what)
                self.holds_codec(b, dev, segs)
                # without a codec: the sector is erased whole
                rc, out, err, dev = self.sim(b, bp, "--simulate", "rfoundry-codec", "--codec", "none", flash=cut)
                self.assertEqual(rc, 0, what + out + err)
                self.check_vectors(b, dev, segs, what)
                self.blank(b, dev)
                self.assertEqual(dev.read(b.app_base, len(image)), image)

    def test_cut_inside_the_vector_table(self):
        """The one cut that can leave a part of a vector table (where the loader can't be reached to erase it again):
        by then every other block, the codec's included, was written and read back, so a keep over it is sound"""
        b = self.b
        _, _, prof, segs = synthetic(b)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            image = base.rom_image(5004)
            with open(bp, "wb") as f:
                f.write(image)
            p = plan.make_plan(b, [(b.app_base, image)], plan.MERGE, segs, b.merge_windows(prof), vector_last=True)
            rc, out, err, dev = self.sim(b, bp, "--simulate", "rfoundry-nocodec", "--stock", "synthetic",
                                         fail_at=p.total_blocks - 1, torn=True)
            self.assertEqual(rc, 2, out + err)
            self.assertIn("could not be erased again", err)
            self.assertNotEqual(dev.read(b.app_base, 4), FF * 4)
            self.check_vectors(b, dev, segs, "torn vector table", whole=False)
            self.assertNotIn(b.app_base, dev.placed)
            self.holds_codec(b, dev, segs)
            cut = bytes(dev.flash)
            rc, out, err, dev = self.sim(b, bp, "--simulate", "rfoundry-codec", flash=cut)
            self.assertEqual(rc, 0, out + err)
            self.assertIn("codec mode: keep", out)
            self.holds_codec(b, dev, segs)
            self.assertEqual(dev.read(b.app_base, len(image)), image)

    def test_cut_during_the_erases(self):
        """The sector with the vector table is erased first. A cut before that erase leaves the radio as it was, and
        a keep stands. A cut after it leaves no vector table and a codec sector that this run did not reach, or
        reached and erased: the next run can't tell which, and never keeps it"""
        b = self.b
        _, _, prof, segs = synthetic(b)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "image.bin")
            image = base.rom_image(5004)
            with open(bp, "wb") as f:
                f.write(image)
            p = plan.make_plan(b, [(b.app_base, image)], plan.MERGE, segs, b.merge_windows(prof), vector_last=True)
            had = fakedfu.rom_installed(b, random.Random(6), segs)		# a finished install with the codec
            merge = ("--simulate", "rfoundry-codec", "--codec", "merge", "--stock", "synthetic")
            for n in range(len(p.erase_order) + 1):
                what = "cut at erase %d of %d" % (n, len(p.erase_order))
                rc, out, err, dev = self.sim(b, bp, *merge, flash=had, stop_erase=n)
                if n == len(p.erase_order):		# every erase ran: the install goes through
                    self.assertEqual(rc, 0, what + out + err)
                    continue
                self.assertEqual(rc, 2, what + out + err)
                self.assertIn("USB error while flashing", err)
                self.assertEqual((dev.erased, dev.blocks), ([s.addr for s in p.erase_order[:n]], 0), what)
                cut = bytes(dev.flash)
                if n == 0:
                    # nothing was erased: the vector table and the codec are as the last finished install left them
                    self.assertTrue(cut == bytes(had))
                    rc, out, err, dev = self.sim(b, bp, "--simulate", "rfoundry-codec", flash=cut)
                    self.assertEqual(rc, 0, what + out + err)
                    self.assertIn("codec mode: keep", out)
                    self.holds_codec(b, dev, segs)
                    continue
                self.assertEqual(dev.read(b.app_base, 4), FF * 4, what)
                # a keep (the radio's own report, from before the cut): refused without a source, nothing sent
                rc, out, err, dev = self.sim(b, bp, "--simulate", "rfoundry-codec", flash=cut)
                self.assertEqual(rc, 2, what + out + err)
                self.assertIn("the radio holds no vector table", err)
                self.assertEqual((dev.erased, dev.blocks, bytes(dev.flash) == cut), ([], 0, True), what)
                # with the stock file it is a merge, and without a codec the sector is erased
                rc, out, err, dev = self.sim(b, bp, "--simulate", "rfoundry-codec", "--stock", "synthetic", flash=cut)
                self.assertEqual(rc, 0, what + out + err)
                self.check_vectors(b, dev, segs, what)
                self.holds_codec(b, dev, segs)
                rc, out, err, dev = self.sim(b, bp, "--simulate", "rfoundry-codec", "--codec", "none", flash=cut)
                self.assertEqual(rc, 0, what + out + err)
                self.check_vectors(b, dev, segs, what)
                self.blank(b, dev)

    def test_a_codec_block_the_loader_did_not_write(self):
        """A ROM bootloader reports no error for a write it did not carry out. The codec's sector is read back with
        the rest before the vector table is written: a block that is missing there leaves no vector table"""
        b = self.b
        _, _, prof, segs = synthetic(b)
        p = plan.make_plan(b, [(b.app_base, base.rom_image(5004))], plan.MERGE, segs, b.merge_windows(prof),
                           vector_last=True)
        st = next(st for st in p.writes if st.sector is b.sector_at(stockfw.DESC_ADDR))
        held = [(st.sector.addr + (num - plan.FIRST_BLOCK) * plan.BLOCK, blk) for num, blk in st.blocks
                if blk != FF * plan.BLOCK]		# a block of erased flash reads back the same, written or not
        self.assertGreater(len(held), 50)
        for skip, blk in (held[0], held[len(held) // 2], held[-1]):		# the first, one inside, the descriptor's
            self.assertEqual(skip + plan.BLOCK == END, blk is held[-1][1])
            dev = fakedfu.FakeRom(b, fakedfu.rom_installed(b, random.Random(6), segs))
            real = dev._program
            dev._program = lambda a, d, skip=skip, real=real: None if a == skip else real(a, d)
            with self.assertRaises(dfumod.DfuError) as cm:
                dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None).run(p)
            at = skip + next(i for i, x in enumerate(blk) if x != 0xFF)
            self.assertIn("0x%08X does not read back" % at, str(cm.exception))
            self.assertIn("vector table is not written yet", str(cm.exception))
            self.assertEqual(dev.read(b.app_base, plan.BLOCK), FF * plan.BLOCK)
            self.assertNotIn(b.app_base, dev.placed)
            self.assertFalse(dev.exited)

    def test_cut_after_a_read_unprotect(self):
        """The first install, cut in the codec's sector and at its descriptor: the next run finds a radio that is
        not protected any more and holds no vector table"""
        b = self.b
        _, _, prof, segs = synthetic(b)
        first = ("--simulate", "stock", "--unprotect", "--stock", "synthetic")
        image = stockfw.random_bytes(random.Random(1), cli.SIM_IMAGE_SIZE)	# what a simulation without a file writes
        p = plan.make_plan(b, [(b.app_base, image)], plan.MERGE, segs, b.merge_windows(prof), vector_last=True)
        sec = b.sector_at(stockfw.DESC_ADDR)
        at = 0					# the number of the first block of the descriptor's sector
        for st in p.write_order:
            if st.sector is sec:
                break
            at += len(st.blocks)
        desc = (stockfw.DESC_ADDR - sec.addr) // plan.BLOCK
        for k, torn in ((0, False), (desc // 2, True), (desc, False), (desc, True), (desc + 1, False)):
            rc, out, err, dev = self.sim(b, *first, fail_at=at + k, torn=torn)
            self.assertEqual(rc, 2, out + err)
            self.assertTrue(dev.unprotected)
            self.assertEqual(dev.read(b.app_base, 4), FF * 4)
            if k <= desc:				# no whole descriptor before its sector's last block is through
                self.assertNotEqual(dev.read(stockfw.DESC_ADDR, stockfw.DESC_LEN), segs[-1][1])
            cut = bytes(dev.flash)
            rc, out, err, dev = self.sim(b, "--simulate", "blank", "--unprotect", "--stock", "synthetic", flash=cut)
            self.assertEqual(rc, 0, out + err)
            self.assertIn("not read protected: nothing to remove", out)
            self.holds_codec(b, dev, segs)
            rc, out, err, dev = self.sim(b, "--simulate", "blank", "--codec", "keep", flash=cut)
            self.assertEqual(rc, 2, out + err)
            self.assertEqual((dev.erased, dev.blocks), ([], 0))

    def test_the_fake_loader_cuts_where_it_is_told(self):
        b = self.b
        dev = fakedfu.FakeRom(b, fail_at=1, torn=True)
        d = dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None)
        p = plan.make_plan(b, [(b.app_base, base.rom_image(3 * plan.BLOCK))], plan.BLANK, vector_last=True)
        with self.assertRaises(fakedfu.Gone):
            d.run(p)
        self.assertEqual((dev.blocks, dev.gone, dev.placed), (1, True, [b.app_base + plan.BLOCK]))
        second = dev.read(b.app_base + 2 * plan.BLOCK, plan.BLOCK)
        self.assertEqual(second[plan.BLOCK // 2:], FF * (plan.BLOCK // 2))	# torn: its first half only
        self.assertNotEqual(second[:plan.BLOCK // 2], FF * (plan.BLOCK // 2))
        self.assertEqual(dev.partial, [])
        with self.assertRaises(fakedfu.Gone):
            d.get_status()
        dev.power_cycle()
        self.assertFalse(d.probe_protected(b))
        self.assertTrue(d.start_blank)
        # what the loader's first bytes say: read protected is not "blank", nor is a vector table
        prot = fakedfu.FakeRom(b, protected=True)
        d = dfumod.Dfu(prot, 0, log=lambda *a: None, sleep=lambda s: None)
        self.assertEqual((d.start, d.start_blank), (None, False))
        self.assertTrue(d.probe_protected(b))
        self.assertEqual((d.start, d.start_blank), (None, False))
        # and one answer never stands for the next: the same loader object, the radio protected since
        dev = fakedfu.FakeRom(b)
        d = dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None)
        self.assertFalse(d.probe_protected(b))
        self.assertTrue(d.start_blank)
        dev.protected = True
        self.assertTrue(d.probe_protected(b))
        self.assertEqual((d.start, d.start_blank), (None, False))
        for vectors in (True, False):
            dev = fakedfu.FakeRom(b, fakedfu.rom_installed(b, random.Random(1), vectors=vectors))
            d = dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None)
            self.assertFalse(d.probe_protected(b))
            self.assertEqual(d.start_blank, not vectors)
            self.assertEqual(d.start, dev.read(b.app_base, 4))
        # a block over flash that is not erased: the bits that can't be set again are recorded, and erased flash takes
        # the bytes as they come
        dev = fakedfu.FakeRom(b)
        dev._program(b.app_base, b"\x0F" * 64)
        self.assertEqual((dev.read(b.app_base, 64), dev.violations), (b"\x0F" * 64, []))
        dev._program(b.app_base, b"\xF0" * 32)
        self.assertEqual(dev.read(b.app_base, 64), bytes(32) + b"\x0F" * 32)
        self.assertEqual(dev.violations, [b.app_base + i for i in range(32)])
        # the 4 bytes are the vector table's, read where the image starts: on a board whose image starts above the
        # start of the program flash, what lies below it says nothing about the last install
        with open(boards.REGISTRY, encoding="utf-8") as f:
            e = json.load(f)["boards"][b.id]
        up = b.sectors[1].addr
        self.assertLess(b.rom_base, up)
        above = boards.Board(dict(e, app_base=up, app_region=[up, e["app_region"][1]],
                                  protected=[[e["rom_base"], up]]))
        self.assertEqual((above.rom_base, above.app_base), (b.rom_base, up))
        for vectors in (True, False):
            flash = fakedfu.rom_installed(above, random.Random(1), vectors=vectors)
            for below in (FF * 4, b"\x00\x00\x02\x20"):		# erased flash, or another program's vector table
                flash[0:4] = below
                dev = fakedfu.FakeRom(above, flash)
                d = dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None)
                self.assertFalse(d.probe_protected(above))
                self.assertEqual((d.start, d.start_blank), (dev.read(up, 4), not vectors))
                cmds = [r[3] for r in dev.requests if r[1] == dfumod.DFU_DNLOAD]
                self.assertEqual(cmds, [bytes([dfumod.CMD_SET_ADDRESS]) + struct.pack("<I", up)])
            self.assertTrue(dfumod.Dfu(fakedfu.FakeRom(above, flash, protected=True), 0, log=lambda *a: None,
                                       sleep=lambda s: None).probe_protected(above))
        # rom_installed: the image over the application region, the codec's sectors blank but for the codec, the rest
        # of the program flash erased
        segs = synthetic(b)[3]
        flash = fakedfu.rom_installed(b, random.Random(1), segs)
        lo, hi = b.app_region
        self.assertEqual(len(flash), b.flash_end - b.rom_base)
        self.assertNotIn(FF * 64, bytes(flash[lo - b.rom_base:hi - b.rom_base]))
        for s in b.codec_sectors():
            self.assertTrue(bytes(flash[s.addr - b.rom_base:s.end - b.rom_base]) == sector_image(s, segs))
        self.assertTrue(bytes(flash[END - b.rom_base:]) == FF * (b.flash_end - END))


class NamedFiles(PlainCodec):
    """A maker's file that is no codec source says why, in its own sentence, on every way in: never the first
    container's "not an OutSecurityBin file", and never the question which radio it is for"""

    def setUp(self):
        super().setUp()
        b = self.b
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        stock, profiles, prof, _ = synthetic(b)
        if prof.get("container") != stockfw.TWIN:
            self.skipTest("the board's stock file is not in the container these files are in")
        # a version whose codec is not mapped: a file of the container with another image, known by its SHA-256
        old = twinfw.pack(KEY[:twinfw.KEY_LEN], base.rom_image(0x3000, 5), b"\0\0")
        entry = {"board": b.id, "name": "OLD_VERSION.bin", "version": "V0.00.01"}
        patch = mock.patch.dict(stockfw.UNMAPPED_FILES, {stockfw.sha256(old): entry})
        patch.start()
        self.addCleanup(patch.stop)
        boot = twinfw.build_synthetic_boot(random.Random(5))
        self.files = [(self.path("old.bin", old), stockfw.unmapped(old)),
                      (self.path("boot.bin", boot), stockfw.boot_file(boot))]
        self.assertIn("OLD_VERSION.bin (V0.00.01)", self.files[0][1])
        self.assertIn("Use %s" % stockfw.PROFILES[b.codec_stock[0]]["name"], self.files[0][1])
        self.assertIn("bootloader file in the maker's container", self.files[1][1])
        self.stock, self.old, self.boot = stock, old, boot
        self.image = self.path("image.bin", base.rom_image(9004))

    def path(self, name, data):
        p = os.path.join(self.dir.name, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def says(self, argv, sentence):
        with mock.patch("sys.stdin.isatty", return_value=False):
            rc, out, err = base.run_cli(argv)
        what = " ".join(argv[:1] + argv[-3:]) + ": " + out + err
        self.assertEqual(rc, 2, what)
        self.assertIn(sentence, err, what)
        self.assertNotIn("OutSecurityBin", out + err, what)
        self.assertNotIn("which radio", out + err, what)
        self.assertNotIn("Traceback", out + err, what)

    def test_merge(self):
        out = os.path.join(self.dir.name, "merged.bin")
        keyed = next(x for x in boards.by(family="usb-dfu") if x.key_sha256)
        for f, sentence in self.files:
            for extra in ([], ["--board", self.b.id], ["--board", keyed.id]):
                self.says(["merge", self.image, "--stock", f, "-o", out] + extra, sentence)
            self.assertFalse(os.path.exists(out))

    def test_key(self):
        keyed = next(x for x in boards.by(family="usb-dfu") if x.key_sha256)
        for f, sentence in self.files:
            for extra in ([], ["--board", self.b.id], ["--board", keyed.id]):
                self.says(["key", "--stock", f] + extra, sentence)
                rc, out, err = base.run_cli(["key", "--stock", f] + extra)
                self.assertNotIn("trying anyway", out)		# the sentence before anything is tried

    def test_an_unknown_file_of_that_container(self):
        """A file in the container with a key of its own that no profile knows: an unknown stock file, for a keyed
        radio too, and no bootloader key is looked for on its account"""
        b = self.b
        keyed = next(x for x in boards.by(family="usb-dfu") if x.key_sha256 and x.keep_codec_supported)
        unknown = twinfw.pack(KEY[:twinfw.KEY_LEN], base.rom_image(0x2000, 9), b"\0\0")
        up = self.path("unknown.bin", unknown)
        out = os.path.join(self.dir.name, "merged.bin")
        for x in (b, keyed):
            self.says(["merge", self.image, "--stock", up, "-o", out, "--board", x.id], "unknown stock file")
            self.says(["flash", "--board", x.id, "--simulate", "blank", "--yes", "--stock", up], "unknown stock file")
            args = mock.Mock(key=None, simulate=None, port=None)
            with mock.patch.object(cli, "get_key", side_effect=AssertionError("a bootloader key was looked for")), \
                    self.assertRaises(cli.UsageError) as cm:
                cli.codec_from_source(args, x, cli.Decision(plan.MERGE, "x"), None, unknown)
            self.assertIn("unknown stock file", str(cm.exception))
        self.assertFalse(os.path.exists(out))

    def test_flash(self):
        keyed = next(x for x in boards.by(family="usb-dfu") if x.key_sha256 and x.keep_codec_supported)
        for f, sentence in self.files:
            for sim in ("stock", "blank", "rfoundry-nocodec"):
                self.says(["flash", "--board", self.b.id, "--simulate", sim, "--unprotect", "--stock", f], sentence)
            # the keyed route: no key is derived from a file that can give none
            for sim in ("blank", "rfoundry-nocodec"):
                self.says(["flash", "--board", keyed.id, "--simulate", sim, "--yes", "--stock", f], sentence)
        # on this route the refusal ends with the way to install without a codec
        with mock.patch("sys.stdin.isatty", return_value=False):
            rc, out, err = base.run_cli(["flash", "--board", self.b.id, "--simulate", "blank", "--stock",
                                         self.files[0][0]])
        self.assertTrue(err.rstrip().endswith("To install without a codec (FM works, no DMR voice): --codec none"), err)

    def test_the_key_of_such_a_file_is_no_bootloaders(self):
        """A keyed radio's key never comes from a file in the container with a key of its own, and a radio without
        a bootloader key has none to derive or to give"""
        b = self.b
        sp = self.path("stock.bin", self.stock)
        with mock.patch.dict(stockfw.PROFILES, synthetic(b)[1]):
            rc, out, err = base.run_cli(["key", "--stock", sp])
            self.assertEqual(rc, 2, out + err)
            self.assertIn("the %s's bootloader has no key" % b.name, err)
            out_path = os.path.join(self.dir.name, "merged.bin")
            rc, out, err = base.run_cli(["merge", self.image, "--stock", sp, "-o", out_path, "--key", self.image])
            self.assertEqual(rc, 2, out + err)
            self.assertIn("the %s's bootloader has no key" % b.name, err)
            # without --key the merge needs none, with or without --board
            for extra in ([], ["--board", b.id]):
                rc, out, err = base.run_cli(["merge", self.image, "--stock", sp, "-o", out_path] + extra)
                self.assertEqual(rc, 0, out + err)
                with open(out_path, "rb") as f:
                    whole = f.read()
                self.assertEqual(len(whole), END - b.app_base)
                self.assertTrue(whole.startswith(base.rom_image(9004)))
                app, prof, segs = stockfw.split_merged(whole)
                self.assertEqual((app, segs), (base.rom_image(9004), synthetic(b)[3]))
            # a keyed radio given this file: another radio's, by name, before any key is tried
            keyed = next(x for x in boards.by(family="usb-dfu") if x.key_sha256 and x.keep_codec_supported)
            rc, out, err = base.run_cli(["flash", "--board", keyed.id, "--simulate", "blank", "--yes", "--stock", sp])
            self.assertEqual(rc, 2, out + err)
            self.assertIn("is a firmware file for the %s, not the %s" % (b.id, keyed.id), err)
            self.assertNotIn("OutSecurityBin", err)
            args = mock.Mock(key=None, simulate=None, port=None)
            with self.assertRaises(cli.UsageError) as cm, contextlib.redirect_stdout(io.StringIO()):
                cli.get_key(args, keyed, self.stock)
            self.assertIn("is a firmware file for the", str(cm.exception))
            # a named file that is no codec source gives its sentence there too
            for data, sentence in ((self.old, self.files[0][1]), (self.boot, self.files[1][1])):
                with mock.patch.object(cli.bootkey, "from_radio", return_value=None), \
                        self.assertRaises(cli.UsageError) as cm, contextlib.redirect_stdout(io.StringIO()):
                    cli.get_key(args, keyed, data)
                self.assertEqual(str(cm.exception), sentence)
            # and an unknown file of that container is no key source: the key comes from where it comes without one
            unknown = twinfw.pack(KEY[:twinfw.KEY_LEN], base.rom_image(0x2000, 9), b"\0\0")
            with mock.patch.object(cli.bootkey, "from_radio", return_value=None), \
                    self.assertRaises(cli.UsageError) as cm, contextlib.redirect_stdout(io.StringIO()):
                cli.get_key(args, keyed, unknown)
            self.assertIn("bootloader needs its 1024-byte key", str(cm.exception))

    def test_a_makers_file_is_not_an_image(self):
        b = self.b
        first = stockfw.MAGIC + bytes(300)
        cases = [(first, "is an official Baofeng/TYT firmware file, not a RadioFoundry image"),
                 (self.stock, "is a maker's firmware file, not a RadioFoundry image. Use it with --stock"),
                 (self.stock[:0x4000], "is a maker's firmware file, not a RadioFoundry image"),	# cut short
                 (self.stock + bytes(64), "is a maker's firmware file, not a RadioFoundry image"),	# padded
                 (self.old, "is a maker's firmware file, not a RadioFoundry image"),
                 (self.boot, "is a maker's bootloader file, not a RadioFoundry image")]
        for i, (data, sentence) in enumerate(cases):
            fp = self.path("as-image-%d.bin" % i, data)
            for extra in (["--codec", "none"], ["--stock", "synthetic"]):
                rc, out, err, dev = self.sim(b, fp, "--simulate", "blank", *extra)
                self.assertEqual(rc, 2, out + err)
                self.assertIn(sentence, err)
                self.assertIsNone(dev)
                if "bootloader file" in sentence:		# nothing to use it for
                    self.assertNotIn("--stock", err)
            # and without --board, where a file of that container was never taken for a local build
            rc, out, err = base.run_cli(["flash", fp, "--simulate", "blank"])
            self.assertEqual(rc, 2, out + err)
            self.assertIn(sentence, err)
        # a RadioFoundry image is none of them: its words there are vector table entries
        rc, out, err, dev = self.sim(b, self.image, "--simulate", "blank", "--codec", "none")
        self.assertEqual(rc, 0, out + err)


class LocalBuild(PlainCodec):
    def test_the_signature_word_at_another_radios_address(self):
        """A local build with --board is tried against that radio's profiles only: the word a codec starts with is an
        ordinary function prologue, and a plain image that holds it where another radio's layout puts its code is
        not that radio's merged image"""
        b = self.b
        lo, hi = b.app_region
        offs = sorted({stockfw.layout(p)[0][0] - p.get("app_base", stockfw.APP_BASE)
                       for p in stockfw.PROFILES.values() if p["board"] != b.id})
        offs = [o for o in offs if 0 < o < hi - lo - 0x1000]
        if not offs:
            self.skipTest("no other radio's codec lies where this board's application can")
        image = bytearray(base.rom_image(offs[0] + 0x1000))
        struct.pack_into("<I", image, offs[0], stockfw.CODEC_SIG)
        image = bytes(image)
        with self.assertRaises(stockfw.StockError):		# for every radio at once it looks merged, and damaged
            stockfw.split_merged(image)
        self.assertIsNone(cli.merged_codec(image, b))
        with self.assertRaises(stockfw.StockError):
            cli.merged_codec(image)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "local.bin")
            with open(bp, "wb") as f:
                f.write(image)
            rc, out, err, dev = self.sim(b, bp, "--simulate", "blank", "--codec", "none")
            self.assertEqual(rc, 0, out + err)
            self.assertIn("treating %s as a local %s build" % (bp, b.name), err)
            self.assertEqual(dev.read(b.app_base, len(image)), image)
            rc, out, err = base.run_cli(["flash", bp, "--simulate", "blank"])	# without --board: as before
            self.assertEqual(rc, 2, out + err)
            self.assertIn("the image has a codec at", err)
        # the board's own merged image, damaged, is still refused with --board
        _, profiles, prof, segs = synthetic(b)
        whole = bytearray(stockfw.merge(base.rom_image(9004), prof, segs))
        whole[segs[0][0] - b.app_base + 100] ^= 1
        with mock.patch.dict(stockfw.PROFILES, profiles), self.assertRaises(stockfw.StockError):
            cli.merged_codec(bytes(whole), b)

    def test_another_radios_merged_image_is_refused_by_name(self):
        """With --board, a whole merged image of another radio is still that radio's, found by its windows' SHA-256s
        and refused by name: also where the two radios' layouts put their code at one address, so that this board's
        profiles alone would call it a damaged image of this board"""
        every = [x for _, x in sorted(boards.BOARDS.items()) if x.codec_stock and x.family in ("usb-dfu", "dfuse")]
        self.assertGreater(len(every), 1)
        app = base.rom_image(9004)
        for x in every:
            for h in sorted({x.codec_stock[0], x.codec_stock[len(x.codec_stock) // 2], x.codec_stock[-1]}):
                stock, profiles, _, _ = stockfw.build_synthetic_stock(KEY, random.Random(7), h)
                prof, segs = stockfw.extract(stock, KEY, profiles)
                whole = stockfw.merge(app, prof, segs)
                with mock.patch.dict(stockfw.PROFILES, profiles):
                    for y in every:
                        got = cli.merged_codec(whole, y)
                        self.assertIsNotNone(got, "%s as %s" % (x.id, y.id))
                        self.assertEqual((got[0], got[1]["board"], got[2]), (app, x.id, segs),
                                         "%s as %s" % (x.id, y.id))
                    self.assertEqual(cli.merged_codec(whole)[1]["board"], x.id)
                    y = next(y for y in every if y.id != x.id)
                    with tempfile.TemporaryDirectory() as d:
                        bp = os.path.join(d, "merged.bin")
                        with open(bp, "wb") as f:
                            f.write(whole)
                        rc, out, err = base.run_cli(["flash", bp, "--board", y.id, "--simulate", "blank", "--dry-run"])
                    self.assertEqual(rc, 2, out + err)
                    self.assertIn("holds the %s codec, but --board says %s: refusing" % (x.name, y.name), err)


class AfterTheInstall(PlainCodec):
    def verify(self, b, codec, d):
        p = {"board": b.debuglink_names[0], "port": "/dev/ttyACM0", "git": "abc", "codec": codec}
        out = io.StringIO()
        with mock.patch.object(debuglink, "find_ports", return_value=["/dev/x"]), \
                mock.patch.object(debuglink, "probe", return_value=p), mock.patch.object(cli, "hwopt_prompt"), \
                mock.patch.object(state, "remember_probe"), mock.patch.object(cli.time, "sleep"), \
                contextlib.redirect_stdout(out):
            rc = cli.verify_after(mock.Mock(port=None), b, {}, d)
        return rc, out.getvalue()

    def test_what_the_radio_reports(self):
        for b in self.boards:
            merge = cli.Decision(plan.MERGE, "x")
            none = cli.Decision(plan.BLANK, "x", expect_codec=False)
            rc, out = self.verify(b, b.codec.id, merge)
            self.assertEqual(rc, 0)
            self.assertIn("ok       DMR voice is ready", out)
            rc, out = self.verify(b, "absent", merge)	# a merge that left no codec: an error
            self.assertEqual(rc, 1)
            self.assertIn("no usable codec (codec=absent)", out)
            self.assertIn(stockfw.PROFILES[b.codec_stock[0]]["name"], out)
            rc, out = self.verify(b, "absent", none)	# an install without one: as asked
            self.assertEqual(rc, 0)
            self.assertIn("no usable codec (codec=absent)", out)
            self.assertEqual(self.verify(b, b.codec.id, cli.Decision(plan.KEEP, "x"))[0], 0)


if __name__ == "__main__":
    unittest.main()
