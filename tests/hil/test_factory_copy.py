# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The factory copy (docs/spec/settings-store.md 4.2; debuglink.md 3.13, STORAGE ops 3 to 5) on a radio whose board
keeps its maker's data in the data flash. Read only for the flash:

- FACTORY_INFO answers, its range is a [[flash.protect]] entry of the board in the registry, its window CRC is the
  CRC-32 of the window read now through STORAGE READ (twice, the same), and its source follows the source order.
- FACTORY_LOAD (RAM only) at an offset other than the bytes loaded so far, and past the range's end, is refused.
- FACTORY_RESTORE with a wrong CRC of the loaded bytes is refused with reason 4 (crc), and with a wrong window CRC
  with reason 7 (window_crc); the window's CRC is the same afterwards. Reason 7 needs bytes that pass the board's
  checker: the window's own when it holds a valid table, else the factory range of --data-flash-backup.

No test runs a restore that writes: rfoundry_hil sends a FACTORY_RESTORE only with a window CRC that is not the
window's (factory_restore_guard), so the radio's check 7 refuses it whatever its earlier checks let through.
tools/rfoundry_debug.py storage factory --restore is the owner's command, never a test's."""
import struct
import zlib

import pytest

from rfoundry_hil import DebugLinkError, board_protect, skip
from rfoundry_debug import (FACTORY_CHUNK, FACTORY_PENDING, FACTORY_RESULTS, ST_BAD_ARG, ST_BUSY, ST_REFUSED,
                            ST_UNAVAILABLE, STORAGE, STORAGE_OP_FACTORY_LOAD, factory_backup_bytes)

REASON = {name: n for n, (name, _) in FACTORY_RESULTS.items()}


@pytest.fixture
def factory(radio):
    """FACTORY_INFO as a dict; skips on a radio without a factory copy"""
    try:
        info = radio.link.factory_info()
    except DebugLinkError as e:
        if e.status == ST_UNAVAILABLE:
            skip("not_built", "firmware without the factory copy (CONFIG_FACTORY_COPY), or no flash map")
        if e.status in (ST_BAD_ARG, 1):
            skip("not_built", "firmware older than the factory copy (no FACTORY_INFO)")
        raise
    if info is None:
        skip("no_hw", "the board keeps no maker's data in its data flash (no factory copy)")
    return info


def window(radio, info):
    """The factory range read twice through STORAGE READ, the same both times"""
    a = radio.link.storage_read(info["addr"], info["size"])
    assert radio.link.storage_read(info["addr"], info["size"]) == a, "two reads of the window differ"
    return a


def load(radio, at, data):
    """FACTORY_LOAD of data at offset at: the count the radio holds"""
    _, r = radio.link.request(STORAGE, struct.pack("<BH", STORAGE_OP_FACTORY_LOAD, at) + bytes(data))
    return struct.unpack_from("<H", r)[0]


def refused(radio, n, crc, wcrc):
    """FACTORY_RESTORE that must be refused: its reason (a busy radio skips)"""
    with pytest.raises(DebugLinkError) as e:
        radio.link.factory_restore(n, crc, wcrc)
    if e.value.status == ST_BUSY:
        skip("state", "the factory copy is busy (a restore's job or its outcome pending)")
    assert e.value.status == ST_REFUSED and e.value.data, f"not a refusal: {e.value}"
    if e.value.data[0] == REASON["busy"]:
        skip("state", "the radio refuses as busy: transmitting or armed, a storage job, or a CPS session")
    return e.value.data[0]


@pytest.mark.feature("factory_copy")
def test_factory_info(radio, factory):
    """FACTORY_INFO: the registry's range, the window's CRC as read now, the source order"""
    info = factory
    entries = board_protect(radio.version().get("board"))
    end = info["addr"] + info["size"]
    assert any(p["mem"] == "data" and (p["start"], p["end"]) == (info["addr"], end) for p in entries), \
        f"the factory range 0x{info['addr']:06X}-0x{end:06X} is no [[flash.protect]] entry of the board"
    print(f"source {info['source']}, window {info['window']} 0x{info['window_crc']:08x}, record {info['record']} "
          f"0x{info['record_crc']:08x}, wrote {info['wrote']}, last restore {info['result']}")
    assert info["result"] != FACTORY_PENDING, "a restore's job is pending: no HIL test posts one"
    src, win, rec = info["source"], info["window"], info["record"]
    if src == "window":
        assert win == "ok" and (rec != "ok" or info["record_crc"] == info["window_crc"]), info
    elif src == "record":
        assert win != "ok" and rec == "ok", info
    else:
        assert info["why"] == 0xF2 or (win != "ok" and rec != "ok"), info
    if win == "unread":
        skip("state", "the copy could not read the window at this start: no CRC to compare")
    w = window(radio, info)
    assert zlib.crc32(w) == info["window_crc"], \
        f"the window's CRC now 0x{zlib.crc32(w):08x}, FACTORY_INFO's 0x{info['window_crc']:08x}"


@pytest.mark.feature("factory_copy")
def test_factory_load_offsets(radio, factory):
    """FACTORY_LOAD (RAM only): offset 0 starts over, any other offset must be the count loaded, and the bytes must
    end inside the range"""
    size = factory["size"]
    try:
        assert load(radio, 0, b"\xff" * FACTORY_CHUNK) == FACTORY_CHUNK
    except DebugLinkError as e:
        if e.status == ST_BUSY:
            skip("state", "the factory copy is busy (a restore's job or its outcome pending)")
        raise
    with pytest.raises(DebugLinkError) as e:
        load(radio, FACTORY_CHUNK + 4, b"\xff")
    assert e.value.status == ST_BAD_ARG, e.value
    assert radio.link.factory_info()["loaded"] == FACTORY_CHUNK, "a refused load changed the count"
    radio.link.factory_load(b"\xff" * size)
    with pytest.raises(DebugLinkError) as e:
        load(radio, size, b"\xff")
    assert e.value.status == ST_BAD_ARG, e.value
    assert load(radio, 0, b"\xff") == 1, "offset 0 starts over"


@pytest.mark.feature("factory_copy")
def test_factory_restore_refusals(radio, factory, pytestconfig):
    """FACTORY_RESTORE with a wrong CRC (reason 4) and with a wrong window CRC (reason 7): refused, the window the same
    afterwards. Every request carries a window CRC that is not the window's (factory_restore_guard)"""
    info, size = factory, factory["size"]
    before = window(radio, info)
    wrong_wcrc = zlib.crc32(before) ^ 1
    region = radio.link.storage_info()["region"]
    radio.link.factory_load(before)
    reason = refused(radio, size, zlib.crc32(before) ^ 1, wrong_wcrc)
    if region != "ours":
        assert reason == REASON["foreign"], reason
        skip("state", f"the storage region is {region}: the restore stops at its first check (foreign)")
    assert reason == REASON["crc"], f"a wrong CRC of the loaded bytes: reason {reason}, 4 expected"
    # Bytes that pass the checker: the window's own when it holds a valid table, else the owner's backup's
    if info["window"] == "ok":
        good, whence = before, "the window's own bytes"
    else:
        path = pytestconfig.getoption("--data-flash-backup")
        if not path:
            skip("opt_in", "the window holds no valid table: give --data-flash-backup FILE (this radio's backup) for "
                           "the window CRC refusal")
        good, whence = factory_backup_bytes(path, info["addr"], size)[1], f"the factory range of {path}"
    radio.link.factory_load(good)
    reason = refused(radio, size, zlib.crc32(good), wrong_wcrc)
    assert reason == REASON["window_crc"], f"{whence} with a wrong window CRC: reason {reason}, 7 expected " \
                                           f"({FACTORY_RESULTS.get(reason, ('?',))[0]})"
    after = window(radio, info)
    assert after == before, "the window changed"
    now = radio.link.factory_info()
    assert now["window_crc"] == info["window_crc"] and now["result"] == REASON["window_crc"], now

