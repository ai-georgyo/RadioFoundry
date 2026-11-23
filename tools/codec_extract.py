#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Merge a radio's own stock AMBE+2 codec into a RadioFoundry image, on your machine (the STM32 radios whose
board.toml says [codec] tool = "codec_extract").

Recipes from each radio's codec page (docs/spec/codec-<radio>.md, "Extraction recipe"):
  1. check the stock file (one of rfoundry/stockfw.py PROFILES) by SHA-256 and by what its container's header says;
     the file picks the radio. Two containers are read: "OutSecurityBin", with a segment table, and the one that
     holds its image twice (rfoundry/twinfw.py). Or take a read of the radio's internal flash made while it ran its
     stock firmware (stub/stubtool.py PORT --progmem FILE): the codec is plain there, found by the code window's
     SHA-256, no key;
  2. decrypt every codec window. An "OutSecurityBin" file: plain = file[off] XOR key[(off - 0x100) mod 1024], with the
     radio's 1024-byte bootloader key (a key file checked by SHA-256 as tools/stm32_dfu.py does, or derived from the
     stock file and checked the same way). A file of the other kind has a 256-byte key of its own, derived from the
     file's two copies and checked by its SHA-256; such a radio's bootloader has no key, and --key is refused;
  3. check each window's SHA-256 and the code's first word 0x4FF0E92D;
  4. write a new image: the built application from where the board's image starts, then the codec where its profile
     places it, 0xFF between (docs/spec/codec-placement.md): the code moved as one block and the tables kept at their
     stock address or moved too, as the build's profile says, with the addresses the build's codec map lists
     rewritten, and the descriptor at 0x080FFF80 that tells the firmware where the code is.

The codec is DVSI's and the radio maker's, not ours: it is never stored in this repository or in a release. This tool
reads your stock file and your key file and writes the merged image to a file you choose (by default next to the
built image, in build/, which git ignores). Flash the result with tools/stm32_dfu.py, or with tools/rfoundry for a
radio installed through its processor's bootloader.
"""
import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stm32_dfu  # noqa: E402  (key table and key file check, hardware-dm1701.md 15.3)

# The container, decryption, profiles and merge live in the rfoundry package (one implementation for both tools)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "rfoundry"))
from rfoundry.stockfw import (APP_BASE, CODEC_SIG, DESC_ADDR, FLASH_BASE, FLASH_END, GATES, HEADER_LEN,  # noqa: E402,F401
                              KEY_LEN, MAGIC, OUTSECURITY, PROFILES, SYN_BL, SYN_SELF_REF, SYN_TABLE_REF,
                              StockError as ExtractError, app_file_offset, build_synthetic_stock, container_kind,
                              decrypt, derive_key, descriptor, extract, find_profile, from_flash, gate_crcs,
                              is_container, layout, layout_name, merge, no_codec, parse_container, placed_parts,
                              profile_for_board, profile_key, sha256, stock_segments, windows)
from rfoundry.codecmap import bl_target  # noqa: E402
from rfoundry import boards  # noqa: E402  (the board registry: the boards this tool serves)
import zlib  # noqa: E402

DM1701_V0206 = "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"
MD380_D2032 = "e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43"
# The MD-9600's own placement of its files (stockfw.profile_key); they are DM-1701 codec sources too
MD9600_FILES = [k for k, p in PROFILES.items() if p["board"] == "md9600"]
MDUV380_V1926 = "75e4d470b451cb5d191a647c0836c71801433b826cc4a7fe2c48d750036bd98e"
RT3S_V21960 = "447ed4e09482c45661cd6e4c83d1e9f777463993ab7dd48b0e0597bb14f519ee"
DM1701_V0201 = "cd9ff4bdb7ba1c464299f3e366bfe72f2e948f3505a8fe54ac19047bff4d6713"
DM1701_V0206_NOCSV = "4230c8d0aabf15f3f165c29a118a5f8f9b906a2d9b7d215c1d8a7bc0e3978bd9"
CS7000P_V90096 = "1a4bdf38fed52dc50c738a85f8ce54f6384c070c6a08af21ca919a547b7e5a59"

EPILOG = """\
examples:
  %(prog)s --stock "DM-1701(CSV)-V02.06.bin"                       (the key is derived from the stock file)
  %(prog)s --stock "DM-1701(CSV)-V02.06.bin" --key ~/keys/dm1701.key
  %(prog)s --stock "MD-380-D2.32(AD).bin"                          (MD-380: image build/md380/rfoundry_md380.bin)
  %(prog)s --stock "MD9600-CSV-V06.09.bin" --board md9600          (MD-9600: image build/md9600/rfoundry_md9600.bin; without
                                                                    --board an MD-9600 file is a DM-1701 codec source)
  %(prog)s --stock flash.bin --board mduv380                       (a stub read of a radio on its stock firmware)
  %(prog)s --stock stock.bin --key dm1701.key --image build/dm1701/rfoundry_dm1701.bin -o build/dm1701/rfoundry_dm1701_codec.bin
  %(prog)s --stock "M17_PLUS_HOST_V9.00.96_20260303_1.bin" --board cs7000p
                                                                   (a radio rfoundry installs through its processor's
                                                                    bootloader: no key; image build/cs7000p/rfoundry_cs7000p.bin)
  %(prog)s --self-test

then flash:  tools/stm32_dfu.py --model dm1701 --key ~/keys/dm1701.key build/dm1701/rfoundry_dm1701_codec.bin
        or:  rfoundry flash build/cs7000p/rfoundry_cs7000p_codec.bin --board cs7000p   (tools/rfoundry: a radio
                                                                    installed through its processor's bootloader)

Supported stock files: rfoundry/stockfw.py PROFILES (--help lists them below). The key file is the radio's
1024-byte firmware key (its SHA-256 is in the board's board.toml [flash.dfu]); without --key it is derived from the
stock file and checked by that hash. A radio whose bootloader has no key takes none: its stock file is decrypted with
the file's own key, derived from the file. Neither is in this repository. For flashing release images, tools/rfoundry
does all of this in one step.

supported stock files:
""" + "".join("  %-34s %-14s %s (sha256 %s...)\n" % (p["name"], p["board"], p["version"], h[:8])
              for h, p in PROFILES.items())


def repo_root():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(here)


def check_output_path(path):
    """Refuse to write the merged image anywhere git would pick it up."""
    root = repo_root()
    full = os.path.abspath(path)
    if os.path.commonpath([full, root]) != root:
        return
    try:
        r = subprocess.run(["git", "-C", root, "check-ignore", "-q", full], capture_output=True)
        ignored = r.returncode == 0
    except OSError:
        rel = os.path.relpath(full, root).split(os.sep)
        ignored = rel[0] in ("build", "codec") or full.endswith(".bin")
    if not ignored:
        raise ExtractError("%s is inside the repository and not ignored by git: the merged image contains the "
                           "proprietary codec, write it under build/ or outside the tree" % path)


def self_test():
    import random
    from unittest import mock
    rng = random.Random(1701)
    fails = []

    def check(cond, msg):
        print(("ok    " if cond else "FAIL  ") + msg)
        if not cond:
            fails.append(msg)

    def expect_error(fn, msg):
        try:
            fn()
            check(False, msg)
        except ExtractError:
            check(True, msg)

    # The real layouts with a random key and random windows (synthetic files: no codec, no key, no stock bytes)
    key = bytes(rng.randrange(256) for _ in range(KEY_LEN))
    stock, profiles, code, tables = build_synthetic_stock(key, rng, DM1701_V0206)
    segs = PROFILES[DM1701_V0206]["segments"]

    check(parse_container(stock) == segs, "container segment table parsed")
    check(app_file_offset(segs, 0x08074714) == 0xC5C14, "code window file offset 0xC5C14 (spec)")
    check(app_file_offset(segs, 0x080D19F0) == 0x122EF0, "table window file offset 0x122EF0 (spec)")
    check((0xC5C14 - HEADER_LEN) % KEY_LEN == 0x314 and (0x122EF0 - HEADER_LEN) % KEY_LEN == 0x1F0,
          "cipher phases 0x314 and 0x1F0 (spec)")
    p, wins = extract(stock, key, profiles)
    CODE, TABN = 0x080E3A74, 0x12082
    delta = CODE - 0x08074714
    check([(a, len(b)) for a, b in wins] == [(CODE, len(code)), (0x080D19F0, TABN), (DESC_ADDR, 0x80)],
          "windows placed: the code right after the codec's data, the tables cut there, then the descriptor")
    mc, mt = wins[0][1], wins[1][1]
    check(struct.unpack_from("<I", mc, SYN_SELF_REF)[0] == 0x08074714 + 0x201 + delta, "self reference moved")
    check(mc[:SYN_SELF_REF] + mc[SYN_SELF_REF + 4:] == code[:SYN_SELF_REF] + code[SYN_SELF_REF + 4:],
          "nothing else in the code changed")
    check(bl_target(mt, SYN_BL, 0x080D19F0 + SYN_BL) == 0x08074714 + 0x200 + delta, "island call re-encoded")
    check(mt[:SYN_BL] + mt[SYN_BL + 4:] == tables[:SYN_BL] + tables[SYN_BL + 4:TABN], "nothing else in the tables changed")
    d = wins[2][1]
    f = struct.unpack_from("<7I", d, 0)
    check(f == (0x43534544, 1, 0x0206, CODE, len(code), zlib.crc32(mc), 1), "descriptor header and code window")
    check(struct.unpack_from("<3I", d, 28) == (0x080D19F0, TABN, zlib.crc32(mt)), "descriptor table window")
    check(struct.unpack_from("<I", d, 124)[0] == zlib.crc32(d[:124]) & 0xFFFFFFFF, "descriptor CRC")

    image = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(rng.randrange(256) for _ in range(5000))
    merged = merge(image, p, wins)
    check(len(merged) == FLASH_END - APP_BASE, "merged image ends with the descriptor at the end of flash")
    check(merged[:len(image)] == image, "application unchanged")
    check(merged[len(image):0x080D19F0 - APP_BASE] == b"\xFF" * (0x080D19F0 - APP_BASE - len(image)),
          "0xFF up to the tables")
    check(merged[0x080D19F0 - APP_BASE:0x080D19F0 - APP_BASE + TABN] == mt, "tables at 0x080D19F0")
    check(merged[0x080D19F0 + TABN - APP_BASE:CODE - APP_BASE] == b"\xFF\xFF", "0xFF between tables and code")
    check(merged[CODE - APP_BASE:CODE - APP_BASE + len(code)] == mc, "code at 0x080E3A74")
    check(merged[DESC_ADDR - APP_BASE:] == d, "descriptor at 0x080FFF80")
    check(not stm32_dfu.check_image(merged, "dm1701"), "stm32_dfu accepts the merged image without warnings")
    big = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(0x080C0000 - APP_BASE - 8)
    check(len(merge(big, p, wins)) == FLASH_END - APP_BASE, "an application filling sectors 3-9 merges")

    bad_key = bytes([key[0] ^ 1]) + key[1:]
    expect_error(lambda: extract(stock, bad_key, profiles), "wrong key: window hash mismatch")
    expect_error(lambda: extract(stock[:-1] + b"\0", key, profiles), "other file: unsupported sha256")
    expect_error(lambda: extract(stock, key), "synthetic file is not the real stock file")
    expect_error(lambda: merge(b"\0" * (0x080D19F0 - APP_BASE + 1), p, wins), "image overlapping the codec refused")
    expect_error(lambda: merge(stock, p, wins), "official file as --image refused")
    expect_error(lambda: merge(big + b"\0" * 0x119F4, p, wins), "an application reaching the tables refused")
    expect_error(lambda: parse_container(b"x" * 300), "non-container refused")

    # MD-380 D002.032: single-segment container, code window in sectors 6 and 7, the main table window in sector 8,
    # four small ones in sectors 10 and 11; the merge moves every window as one block into sector 11 (code, main
    # tables, small ones, each at its stock address modulo 16) with every address between them rewritten
    import rfoundry.stockfw as sf
    mstock, mprofiles, mcode, mtables = build_synthetic_stock(key, rng, MD380_D2032)
    mreal = PROFILES[MD380_D2032]
    check(parse_container(mstock) == [(0x0800C000, 0xF2C00)], "MD-380 single-segment container parsed")
    check(app_file_offset(mreal["segments"], 0x08050C44) == 0x44D44, "MD-380 code window file offset 0x44D44 (spec)")
    check(app_file_offset(mreal["segments"], 0x0808FC64) == 0x83D64, "MD-380 table window file offset 0x83D64 (spec)")
    mp, mwins = extract(mstock, key, mprofiles)
    MCODE, MTAB = 0x080E0004, 0x080F0894
    check([a for a, _ in mwins] == [MCODE, MTAB, 0x080FB898, 0x080FB8C8, 0x080FB8E4, 0x080FB900, DESC_ADDR],
          "MD-380: every window moved into sector 11 in order, then the descriptor")
    mc = mwins[0][1]
    check(mc[SYN_SELF_REF:SYN_SELF_REF + 4] == struct.pack("<I", MCODE + 0x201) and
          mc[SYN_TABLE_REF:SYN_TABLE_REF + 4] == struct.pack("<I", MTAB + 0x20) and
          mc[SYN_TABLE_REF + 16:SYN_TABLE_REF + 20] == struct.pack("<I", 0x080FB898 + 4),
          "MD-380: the code's references to itself, the tables and a small window rewritten")
    check(bl_target(mwins[1][1], SYN_BL, MTAB + SYN_BL) == MCODE + 0x200, "MD-380: the island's call follows the code")
    md = mwins[6][1]
    check(struct.unpack_from("<7I", md, 0) == (0x43534544, 1, 0x2032, MCODE, len(mcode), zlib.crc32(mc), 5),
          "MD-380 descriptor header, code window and five table windows")
    check(struct.unpack_from("<3I", md, 28) == (MTAB, len(mtables), zlib.crc32(mwins[1][1])),
          "MD-380 descriptor main table window")
    check(struct.unpack_from("<3I", md, 40) == (0x080FB898, 0x24, zlib.crc32(mwins[2][1])),
          "MD-380 descriptor first small window")
    mimage = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(rng.randrange(256) for _ in range(0xD4000 - 8))
    mmerged = merge(mimage, mp, mwins)
    check(len(mmerged) == FLASH_END - APP_BASE, "MD-380 merged image ends with the descriptor at the end of flash")
    check(mmerged[:len(mimage)] == mimage, "MD-380: an application filling sectors 3-10 unchanged")
    check(all(mmerged[a - APP_BASE:a - APP_BASE + len(d)] == d for a, d in mwins), "MD-380 windows in place")
    check(not stm32_dfu.check_image(mmerged, "md380"), "stm32_dfu accepts the MD-380 merged image without warnings")
    expect_error(lambda: merge(mimage + b"\0" * 0x4005, mp, mwins), "MD-380: application reaching the codec refused")
    expect_error(lambda: extract(mstock, bad_key, mprofiles), "MD-380 wrong key refused")
    check(find_profile(mstock, mprofiles)["board"] == "md380", "MD-380 profile found by SHA-256")
    # the same codec where releases before put it (the former layout: code in sector 11, the tables in sectors 8, 10
    # and 11 at their stock addresses), as read back from a radio: moved to the current layout, byte for byte
    former = sf.forms(mp)[1]
    oflash = bytearray(b"\xFF" * (FLASH_END - FLASH_BASE))
    old = dict(mp, placement=former["placement"], placed=former["placed"])
    del old["former"]
    _, owins_ = extract(mstock, key, {sha256(mstock): old})
    for a, d in owins_:
        oflash[a - FLASH_BASE:a - FLASH_BASE + len(d)] = d
    check(owins_[0][0] == 0x080E0000 and owins_[1][0] == 0x0808FC64, "MD-380 former layout: tables in sector 8")
    check(from_flash(bytes(oflash), profiles=mprofiles) == (mp, mwins),
          "MD-380: a codec in the former layout read back and moved to the current one")
    oflash[0x0808FC64 - FLASH_BASE + 0x30] ^= 1
    check(from_flash(bytes(oflash), profiles=mprofiles) is None, "MD-380: a damaged former-layout table gives nothing")

    # MD-UV380 V19.26 CSV: two-segment container, the DM-1701's placement at its own addresses; and the same codec from
    # a read of the internal flash of a radio on its stock firmware (the application plain at 0x0800C000)
    ustock, uprofiles, ucode, _ = build_synthetic_stock(key, rng, MDUV380_V1926)
    up, uwins = extract(ustock, key, uprofiles)
    check([(a, len(d)) for a, d in uwins] == [(0x080EB300, len(ucode)), (0x080D8C94, 0x1266A), (DESC_ADDR, 0x80)],
          "MD-UV380: code at 0x080EB300 after the tables cut at 0x080EB2FE, then the descriptor")
    check(struct.unpack_from("<I", uwins[2][1], 8)[0] == 0x00561926, "MD-UV380 V19.26 descriptor tag")
    usegs = parse_container(ustock)
    flash = bytearray(b"\xFF" * (FLASH_END - FLASH_BASE))
    flash[APP_BASE - FLASH_BASE:APP_BASE - FLASH_BASE + usegs[1][1]] = decrypt(
        ustock, app_file_offset(usegs, APP_BASE), usegs[1][1], key)
    check(from_flash(bytes(flash), profiles=uprofiles) == (up, uwins), "MD-UV380: the codec from a flash readout")
    flash[0x0807B830 - FLASH_BASE + 8] ^= 1
    check(from_flash(bytes(flash), profiles=uprofiles) is None, "a readout with a changed code byte gives nothing")
    umerged = merge(struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(0x080C0000 - APP_BASE - 8), up, uwins)
    check(not stm32_dfu.check_image(umerged, "mduv380"), "stm32_dfu accepts the MD-UV380 merged image")
    # RT3S V219.60: the code doesn't fit after the tables, so it goes to the start of sector 10, below them
    rstock, rprofiles, rcode, _ = build_synthetic_stock(key, rng, RT3S_V21960)
    rp, rwins = extract(rstock, key, rprofiles)
    check([a for a, _ in rwins] == [0x080C0000, 0x080E322C, DESC_ADDR] and 0x080C0000 + len(rcode) <= 0x080E322C,
          "RT3S 219.60: code at the start of sector 10, below the tables")
    rmerged = merge(struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(0x080C0000 - APP_BASE - 8), rp, rwins)
    check(not stm32_dfu.check_image(rmerged, "mduv380"), "stm32_dfu accepts the RT3S 219.60 merged image")
    # DM-1701 V02.01 CSV (V02.02 and V02.03 alike): the V02.06 placement 0x40 lower, its own tag
    ostock, oprofiles, ocode, _ = build_synthetic_stock(key, rng, DM1701_V0201)
    op, owins = extract(ostock, key, oprofiles)
    check([(a, len(d)) for a, d in owins] == [(0x080E3A34, len(ocode)), (0x080D19B0, 0x12082), (DESC_ADDR, 0x80)],
          "DM-1701 V02.01: code at 0x080E3A34 after the tables cut at 0x080E3A32, then the descriptor")
    check(struct.unpack_from("<I", owins[2][1], 8)[0] == 0x0203, "DM-1701 V02.01 descriptor tag")
    omerged = merge(struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(0x080C0000 - APP_BASE - 8), op, owins)
    check(not stm32_dfu.check_image(omerged, "dm1701"), "stm32_dfu accepts the DM-1701 V02.01 merged image")
    # DM-1701 V02.06 noCSV (V02.03 noCSV alike, 0x40 lower): a larger application segment, the codec higher up, the
    # code 0x1C bytes shorter; still after the tables in sector 11, below the descriptor
    nstock, nprofiles, ncode, _ = build_synthetic_stock(key, rng, DM1701_V0206_NOCSV)
    np_, nwins = extract(nstock, key, nprofiles)
    check([(a, len(d)) for a, d in nwins] == [(0x080E6828, 0x10200), (0x080D4794, 0x12092), (DESC_ADDR, 0x80)]
          and len(ncode) == 0x10200 and 0x080E6828 + 0x10200 <= DESC_ADDR,
          "DM-1701 V02.06 noCSV: code at 0x080E6828 after the tables cut at 0x080E6826, then the descriptor")
    check(struct.unpack_from("<I", nwins[2][1], 8)[0] == 0x10206, "DM-1701 V02.06 noCSV descriptor tag")
    nmerged = merge(struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(0x080C0000 - APP_BASE - 8), np_, nwins)
    check(not stm32_dfu.check_image(nmerged, "dm1701"), "stm32_dfu accepts the DM-1701 V02.06 noCSV merged image")

    # Every MD-9600 original firmware file: a two-segment container, the code in sectors 7 and 8 moved to the start of
    # sector 11, the table window moved as one block to the start of sector 10 (its stock address modulo 16)
    for sha in MD9600_FILES:
        tab_at, tag = PROFILES[sha]["placement"]["tables"], PROFILES[sha]["placement"]["tag"]
        gstock, gprofiles, gcode, gtables = build_synthetic_stock(key, rng, sha)
        greal = PROFILES[sha]
        check(find_profile(gstock, gprofiles, "md9600")["board"] == "md9600", "MD-9600 %05X profile found for the board" % tag)
        check(parse_container(gstock) == greal["segments"], "MD-9600 %05X container parsed" % tag)
        gp, gwins = extract(gstock, key, gprofiles)
        check([a for a, _ in gwins] == [0x080E0000, tab_at, DESC_ADDR] and 0x080C0000 <= tab_at < 0x080C0010,
              "MD-9600 %05X: the code in sector 11, the tables in sector 10, the descriptor" % tag)
        check(gwins[0][1] != gcode and len(gwins[0][1]) == len(gcode), "MD-9600 %05X: the code moved" % tag)
        gd = gwins[2][1]
        check(struct.unpack_from("<7I", gd, 0) == (0x43534544, 1, tag, 0x080E0000, len(gcode), zlib.crc32(gwins[0][1]), 1),
              "MD-9600 %05X descriptor header and code window" % tag)
        gimage = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(rng.randrange(256) for _ in range(0xB4000 - 8))
        gmerged = merge(gimage, gp, gwins)
        check(len(gmerged) == FLASH_END - APP_BASE, "MD-9600 %05X merged image ends with the descriptor" % tag)
        check(not stm32_dfu.check_image(gmerged, "md9600"), "stm32_dfu accepts the MD-9600 %05X merged image" % tag)
        expect_error(lambda: merge(gimage + b"\0" * (tab_at - 0x080C0000 + 1), gp, gwins),
                     "MD-9600 %05X: application reaching the tables refused" % tag)
        expect_error(lambda: extract(gstock, bad_key, gprofiles), "MD-9600 %05X wrong key refused" % tag)

    # CS7000 M17 PLUS V9.00.96 (docs/spec/codec-cs7000p.md): the container that holds its image twice, under a
    # 256-byte key of its own (the radio's bootloader has none). 23 windows: the code (three ranges with the maker's
    # code between) to the start of sector 7, the two islands and the twenty tables joined into one block after it,
    # every span that is not the codec's 0xFF, the descriptor at the end of the sector
    import rfoundry.twinfw as tw
    from rfoundry import codecmap as cm
    creal = PROFILES[CS7000P_V90096]
    cstock, cprofiles, ccode, _ = build_synthetic_stock(key, rng, CS7000P_V90096)
    cprof = cprofiles[sha256(cstock)]
    cmap = sf.code_map(cprof)
    check(tw.parse(cstock) == (0x80, 0x1AD4BC, 0xD6A56) and len(cstock) == 0x1AD53E,
          "CS7000: container parsed (payload offset, payload length, image length)")
    check(tw.offset(creal, 0x080A4F24) == 0x84FA8 and tw.phase(creal, 0x84FA8) == 0x28,
          "CS7000: code window file offset 0x84FA8, key phase 0x28 (spec)")
    check(tw.offset(creal, 0x080C469C) == 0xA4720 and tw.phase(creal, 0xA4720) == 0xA0,
          "CS7000: first table window file offset 0xA4720, key phase 0xA0 (spec)")
    check(tw.offset(creal, 0x080A4F24, 1) == 0x84FA8 + 0xD6A5A, "CS7000: the second copy 0xD6A5A further on (spec)")
    check(tw.derive_key(cstock, cprof["key_sha256"]) == key[:tw.KEY_LEN], "CS7000: the key from the file's two copies")
    cp, cwins = extract(cstock, None, cprofiles)
    CCODE, CBLOCK = 0x080E0004, 0x080F5CA0
    check([(a, len(d)) for a, d in cwins] == [(CCODE, 0x15C98), (CBLOCK, 0x3E83), (DESC_ADDR, 0x80)],
          "CS7000: the code at the start of sector 7, the other windows as one block after it, the descriptor")
    cc, cb = cwins[0][1], cwins[1][1]
    cstockw = windows(cp)
    csrc = [a for a, _, _ in cstockw]
    cdst = [CCODE, CBLOCK] + cp["placement"]["extra"]
    check(sf.pack_after(CCODE + 0x15C98, cstockw[1:]) == cdst[1:] and cdst[-1] + cstockw[-1][1] == 0x080F9B23,
          "CS7000: each window after the code at the lowest address with its stock address modulo 16, to 0x080F9B23")
    csites = cm._layout_sites(cmap, [w[:2] for w in cstockw], csrc, cdst)
    check(len(csites) == 96 and sum(1 for x in csites if x[2]["kind"] == "bl") == 12,
          "CS7000: 96 sites are rewritten, 12 of them calls from island 1 into the code")
    T17 = cdst[csrc.index(0x080F4674)]
    check(T17 == 0x080F9AA4 and [struct.unpack_from("<I", cc, o)[0] for o in (0x5C4, 0xC9C)] == [T17 - 2] * 2,
          "CS7000: the two words that point two bytes below a table moved with it")
    b0 = cmap["branches"][0]
    check(bl_target(cb, b0["at"] - csrc[1], CBLOCK + b0["at"] - csrc[1]) == b0["target"] + CCODE - csrc[0],
          "CS7000: an island's call into the code re-encoded")
    choles = cm.holes(cmap)
    check(len(choles) == 7 and sum(n for _, n in choles) == 33368 and all(cc[o:o + n] == b"\xFF" * n for o, n in choles),
          "CS7000: the code's seven spans that are not the codec's are 0xFF (33368 bytes)")
    skip = {o + i for o, n in choles for i in range(n)}
    for w, off, r, _, _ in csites:
        if w == 0:
            skip.update(range(off, off + 4))
            skip.update(range(off + r.get("off2", 0) - r.get("off", 0), off + r.get("off2", 0) - r.get("off", 0) + 4))
    check(all(cc[i] == ccode[i] for i in range(len(cc)) if i not in skip), "CS7000: nothing else in the code changed")
    _, cstocksegs = stock_segments(cstock, None, cprofiles)
    tholes = cm.table_holes(cmap)
    same, blank, used = True, 0, set()
    for (a, d), at in zip(cstocksegs[1:], cdst[1:]):
        o = at - CBLOCK
        used.update(range(o, o + len(d)))
        hole = {x + i for x, n in tholes.get(a, []) for i in range(n)}
        site = {off + i for w, off, _, _, _ in csites if csrc[w] == a for i in range(4)}
        blank += sum(1 for i in hole if cb[o + i] == 0xFF)
        same = same and all(cb[o + i] == d[i] for i in range(len(d)) if i not in hole and i not in site)
    check(blank == 14 and len(tholes) == 3, "CS7000: three table windows' spans that are not the codec's are 0xFF (14 bytes)")
    check(same and all(cb[i] == 0xFF for i in range(len(cb)) if i not in used) and len(cb) - len(used) == 128,
          "CS7000: nothing else in the block changed, and the 128 bytes between its windows are 0xFF")
    cd = cwins[2][1]
    check(struct.unpack_from("<10I", cd, 0) == (0x43534544, 1, 0x00090096, CCODE, 0x15C98, zlib.crc32(cc), 1,
                                                 CBLOCK, 0x3E83, zlib.crc32(cb))
          and cd[40:124] == bytes(84) and struct.unpack_from("<I", cd, 124)[0] == zlib.crc32(cd[:124]),
          "CS7000: descriptor tag, code window, one table window, its CRC")
    changed = [(a, bytearray(d)) for a, d in cstocksegs]
    changed[0][1][0x7200] ^= 1		# a byte of the second range
    try:
        placed_parts(cp, changed)
        check(False, "CS7000: a changed byte in a range fails the gate")
    except ExtractError as e:
        check("rewritten code's CRC-32" in str(e), "CS7000: a changed byte in a range fails the gate")
    cmerged = merge(image, cp, cwins)
    check(len(cmerged) == 0x100000 and cmerged[:len(image)] == image
          and cmerged[len(image):CCODE - 0x08000000] == b"\xFF" * (CCODE - 0x08000000 - len(image)),
          "CS7000: merged image 1048576 bytes from 0x08000000, the application unchanged, 0xFF up to the codec")
    check(sf.split_merged(cmerged, cprofiles) == (image, cp, cwins), "CS7000: the merged image splits into its parts again")
    expect_error(lambda: merge(cstock, cp, cwins), "CS7000: the maker's file as --image refused")
    expect_error(lambda: merge(bytes(CCODE - 0x08000000 + 1), cp, cwins), "CS7000: an application reaching the codec refused")
    expect_error(lambda: extract(cstock, None, cprofiles, board="dm1701"), "CS7000: the file refused for another radio")
    expect_error(lambda: extract(cstock[:-1] + bytes([cstock[-1] ^ 1]), None, cprofiles), "CS7000: another file refused")

    def says(fn, *words):
        try:
            fn()
        except ExtractError as e:
            return all(w in str(e) for w in words)
        return False

    older = cstock[:-1] + bytes([cstock[-1] ^ 1])	# a stand-in for a version whose codec has not been mapped
    with mock.patch.dict(sf.UNMAPPED_FILES, {sha256(older): {"board": "cs7000p", "name": "older.bin", "version": "V0.1"}}):
        check(says(lambda: find_profile(older), "older.bin (V0.1)", "has not been mapped", creal["name"]),
              "CS7000: a version whose codec is not mapped is named, with the file to use")
    cboot = tw.build_synthetic_boot(rng)
    check(says(lambda: find_profile(cboot), "bootloader file", "no codec") and is_container(cboot),
          "CS7000: a bootloader file in the maker's container is named and refused")
    check(is_container(cstock[:0x100]) and says(lambda: find_profile(cstock[:0x100]), "unknown stock file"),
          "CS7000: a cut file is still a stock file, and an unknown one")

    # main() end to end on the synthetic file
    import contextlib
    import io
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        sp, ip, op = (os.path.join(d, n) for n in ("stock.bin", "rfoundry_dm1701.bin", "out.bin"))
        for path, blob in ((sp, stock), (ip, image)):
            with open(path, "wb") as f:
                f.write(blob)

        def run(*extra):
            argv = ["codec_extract.py", "--stock", sp, "--image", ip, "--output", op, "--key", "unused"] + list(extra)
            err = io.StringIO()
            with mock.patch.object(sys, "argv", argv), mock.patch.object(sys.modules[__name__], "find_profile",
                                                                          lambda s, **kw: p), \
                    mock.patch.object(stm32_dfu, "load_key", lambda path, board: key), \
                    mock.patch.object(sys.modules[__name__], "extract", lambda s, k, **kw: (p, wins)), \
                    mock.patch.object(sys.modules[__name__], "check_output_path", lambda path: None), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                return main(), err.getvalue()

        rc, err = run()
        with open(op, "rb") as f:
            check(rc == 0 and f.read() == merged, "main() writes the merged image")

    expect_error(lambda: check_output_path(os.path.join(repo_root(), "tools", "x_codec.img")),
                 "output inside the tree and not ignored refused")
    try:
        check_output_path(os.path.join(repo_root(), "build", "dm1701", "rfoundry_dm1701_codec.bin"))
        check(True, "output under build/ allowed")
    except ExtractError:
        check(False, "output under build/ allowed")
    check(sha256(key) not in [h for h, _ in stm32_dfu.KEYS.values()], "synthetic key is not a real key")

    print("%d failures" % len(fails))
    return 1 if fails else 0


def placed_report(stock, key, board=None, profiles=None):
    """--placed: (lines, ok) for a stock file whose profile places its codec. One line per gate of the placement (the
    CRC-32 of the bytes, the gate, "ok" or "MISMATCH"), then, only when every gate holds, one line per window as the
    merge writes it: address, length, CRC-32, SHA-256. These are a profile's "placed" values, so it works for a profile
    that has none yet, and nothing is compared with them"""
    prof, segs = stock_segments(stock, key, profiles, board)
    if not prof.get("placement"):
        raise ExtractError("%s has no placement: its codec stays at its stock addresses" % prof["name"])
    lines, ok = [], True
    want = prof["placement"].get("gates")
    if want:
        got = gate_crcs(prof, segs)
        for k, what in GATES:
            ok = ok and got[k] == want[k]
            lines.append("gate   %-24s 0x%08X  expected 0x%08X  %s"
                         % (what, got[k], want[k], "ok" if got[k] == want[k] else "MISMATCH"))
    if ok:
        for a, d in placed_parts(prof, segs):
            lines.append("placed 0x%08X+0x%X  crc32 0x%08X  sha256 %s" % (a, len(d), zlib.crc32(d) & 0xFFFFFFFF, sha256(d)))
    return lines, ok


def test_desc(path, board="dm1701", profile=None):
    """Write the codec sectors as the merge leaves them for a synthetic codec of the board's first stock file (or of
    the stock file whose SHA-256 profile is: an MD-9600 file, whose tables the merge moves), for
    tests/host/codecDescTest.c (the firmware's descriptor checks and this writer must agree): from the end of the
    board's application region (the DM-1701's and the MD-9600's sectors 10 and 11, 0x080C0000..0x08100000; the
    MD-380's sector 11 and the CS7000 M17 PLUS's sector 7, 0x080E0000..0x08100000) to the end of flash"""
    import random
    rng = random.Random(1701)
    key = bytes(rng.randrange(256) for _ in range(KEY_LEN))
    stock, profiles, _, _ = build_synthetic_stock(key, rng, profile or profile_for_board(board))
    p, wins = extract(stock, key, profiles)
    lo = boards.get(board).app_region[1]
    out = bytearray(b"\xFF" * (FLASH_END - lo))
    for a, d in wins:
        out[a - lo:a - lo + len(d)] = d
    with open(path, "wb") as f:
        f.write(out)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], epilog=EPILOG,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stock", help="the radio's stock firmware file (one listed below), or a read of its internal "
                    "flash while it ran its stock firmware (stubtool.py PORT --progmem FILE)")
    ap.add_argument("--key", help="the radio's firmware key file (1024 bytes); derived from the stock file when left out")
    ap.add_argument("--board", choices=sorted(b for b, t in boards.BOARDS.items() if t.codec_tool == "codec_extract"),
                    help="refuse a stock file for another radio (the stock file picks the board otherwise)")
    ap.add_argument("--image", help="built RadioFoundry image (default: build/<board>/rfoundry_<board>.bin)")
    ap.add_argument("-o", "--output", help="merged image to write (default: <image>_codec.bin next to the image)")
    ap.add_argument("--test-profile", metavar="SHA256", help="with --test-desc: the stock file whose layout the synthetic "
                    "codec takes (default: the board's first)")
    ap.add_argument("--placed", action="store_true", help="for maintainers, with --stock: print the codec's windows as "
                    "the file's profile places them (address, length, CRC-32, SHA-256), after one line per gate of "
                    "the placement; writes no file. How a new profile's \"placed\" values are made (docs/dev/codec.md)")
    ap.add_argument("--self-test", action="store_true", help="check the tool's logic on a synthetic file and key")
    ap.add_argument("--test-desc", metavar="FILE", help="write the codec sectors for a synthetic codec, as merged "
                    "(tests/host/codecDescTest.c), for --board")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if args.test_desc:
        if not args.board:
            ap.error("--test-desc needs --board")
        return test_desc(args.test_desc, args.board, args.test_profile)
    if not args.stock:
        ap.error("--stock is required (or --self-test)")
    try:
        with open(os.path.expanduser(args.stock), "rb") as f:
            stock = f.read()
        if no_codec(stock):
            raise ExtractError(no_codec(stock))
        readout = None if is_container(stock) else from_flash(stock)
        if not is_container(stock) and readout is None:
            raise ExtractError("%s is neither a supported stock file nor a flash readout (from 0x%08X) holding a "
                               "supported codec" % (args.stock, FLASH_BASE))
        prof = readout[0] if readout else find_profile(stock, board=args.board)
        board = prof["board"]
        if args.board and args.board != board:
            raise ExtractError("%s is a %s stock file, not a %s one" % (args.stock, board, args.board))
        # A bootloader key only for a board whose bootloader has one, and only for a file it encrypts: a file in a
        # container with a key of its own is decrypted with that key, derived inside (stockfw.stock_segments)
        has_key = boards.get(board).key_sha256 is not None
        if args.key and not has_key:
            raise ExtractError("the %s's bootloader has no key: it takes the image as it is" % boards.get(board).name)
        key = None
        if readout is None and has_key and container_kind(stock) == OUTSECURITY:
            key = stm32_dfu.load_key(args.key, board) if args.key else derive_key(stock, prof["key_sha256"])
        if args.placed:
            if readout is not None:
                raise ExtractError("--placed takes the stock file, not a flash readout")
            lines, ok = placed_report(stock, key, args.board)
            print("\n".join(lines))
            return 0 if ok else 1
        image_path = args.image or os.path.join(repo_root(), "build", board, "rfoundry_%s.bin" % board)
        out = args.output or os.path.splitext(image_path)[0] + "_codec.bin"
        with open(os.path.expanduser(image_path), "rb") as f:
            image = f.read()
        check_output_path(out)
        prof, wins = readout or extract(stock, key, board=args.board)
        merged = merge(image, prof, wins)
        for w in stm32_dfu.check_image(merged, board) if has_key else ():	# the radios tools/stm32_dfu.py flashes
            print("warning: %s" % w)
        with open(out, "wb") as f:
            f.write(merged)
    except (OSError, ExtractError, stm32_dfu.DfuError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    print("stock  %s (%s), %s codec: %s verified"
          % (prof["name"] if readout is None else "flash readout with " + layout_name(prof), sha256(stock)[:8], board,
             ", ".join("0x%08X+0x%X" % (a, n) for a, n, _ in layout(prof))))
    print("wrote  %s (%d bytes; contains the proprietary codec: keep it to yourself)" % (out, len(merged)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
