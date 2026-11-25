# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The owner's transmit arm (docs/spec/radio-features.md 6.3; debuglink.md 3.8, RADIO op 44) on a radio whose board
has one. No test here keys the radio:

- The info: a stage no higher than the board's highest, the calibration's range, its CRC-32 0 when none is in use,
  and, on a radio with a factory copy of that range (STORAGE op 3), the copy's table in use and its CRC.
- Refusals that change nothing: an id that is not the calibration's CRC-32 (reason 2, or 1 with none in use), a
  stage over the highest (3), a request of another length (bad argument); off is always taken.
- With --allow-reboot: the stage is off after a restart.
- With --allow-rf (marker radio_tx) and this radio's own backup (--data-flash-backup): the test TX limit set to block
  first, so nothing can key, then stage 1 with the CRC-32 of the backup's bytes (never the one the info gave), read
  back, a factory restore refused as busy while armed, then off and the limit back to the run's.

A radio without the arm skips other_board. rfoundry_hil.tx_arm_guard sends a raising op 44 with the calibration's
CRC only with --allow-rf and the limit at block, so no test arms by itself. What an armed radio transmits is the TX
tests' business: they skip (state) where the arm doesn't cover them (rfoundry_hil.tx_arm_skip). Arming for those is
the owner's command, tools/rfoundry_debug.py radio tx-arm, never a test's."""
import struct
import time
import zlib

import pytest

from rfoundry_hil import DebugLinkError, as_int, skip
from rfoundry_debug import (FACTORY_RESULTS, OP_TX_ARM, RADIO, ST_BAD_ARG, ST_REFUSED, ST_UNAVAILABLE,
                            decode_tx_arm_info, factory_backup_bytes, tx_arm_compare)

REASON = {name: n for n, (name, _) in FACTORY_RESULTS.items()}


@pytest.fixture
def arm(radio):
    """The arm's info (RADIO op 44) as a dict; skips on a radio without it"""
    try:
        _, r = radio.link.request(RADIO, bytes([OP_TX_ARM]))
    except DebugLinkError as e:
        if e.status == ST_UNAVAILABLE:
            skip("other_board", "this radio has no transmit arm (hal_radio_t.tx_arm): its transmissions need none")
        if e.status == ST_BAD_ARG:
            skip("not_built", "firmware older than the transmit arm (no RADIO op 44)")
        raise
    return decode_tx_arm_info(r)


def refused(radio, payload):
    """A RADIO op 44 request that must be refused: its reason"""
    with pytest.raises(DebugLinkError) as e:
        radio.link.request(RADIO, bytes([OP_TX_ARM]) + payload)
    assert e.value.status == ST_REFUSED and e.value.data, f"not a refusal: {e.value}"
    return e.value.data[0]


@pytest.mark.feature("tx_arm")
def test_tx_arm_info(radio, arm):
    """The info: the stage within the board's stages, the range, a CRC only with a calibration in use, and the same
    table and CRC as the factory copy reports where it keeps that range"""
    info = arm
    print(f"stage {info['stage']}, highest {info['max']}, range 0x{info['addr']:06X}+0x{info['len']:X}, "
          f"crc 0x{info['crc']:08x}, cal_ok {info['cal_ok']}")
    assert 1 <= info["max"] <= 3 and info["stage"] <= info["max"], info
    assert info["len"] > 0, info
    assert info["cal_ok"] or info["crc"] == 0, info
    if info["stage"]:
        print("armed: by the owner's tool since the start (RAM only, off at every start)")
    try:
        fi = radio.link.factory_info()
    except DebugLinkError as e:
        if e.status not in (ST_UNAVAILABLE, ST_BAD_ARG, 1):
            raise
        fi = None
    if fi is None or (fi["addr"], fi["size"]) != (info["addr"], info["len"]):
        return
    assert info["cal_ok"] == (fi["source"] != "none"), (info, fi)
    if fi["source"] == "window":
        assert info["crc"] == fi["window_crc"], (info, fi)
    elif fi["source"] == "record":
        assert info["crc"] == fi["record_crc"], (info, fi)


@pytest.mark.feature("tx_arm")
def test_tx_arm_refusals(radio, arm):
    """Refusals change nothing: an id that is not the calibration's CRC-32, a stage over the highest, a request of
    another length. Off is always taken. Every id sent differs from the CRC the info gave"""
    info = arm
    if info["stage"]:
        skip("state", f"armed (stage {info['stage']}, by the owner's tool): the test leaves an armed radio alone")
    wrong = info["crc"] ^ 0xFFFFFFFF
    try:
        why = refused(radio, struct.pack("<BI", 1, wrong))
        if why == 4:
            skip("state", "the radio refuses every arm now (reason 4: a bench state, a stored reference trim, "
                          "the power's off state, a restore pending): no id check to see")
        assert why == (2 if info["cal_ok"] else 1), f"an arm with another id: reason {why}"
        assert refused(radio, struct.pack("<BI", info["max"] + 1, wrong)) == 3, "a stage over the highest"
        with pytest.raises(DebugLinkError) as e:
            radio.link.request(RADIO, bytes([OP_TX_ARM, 1]))
        assert e.value.status == ST_BAD_ARG, e.value
        now = radio.link.tx_arm_info()
        assert now["stage"] == 0 and now["crc"] == info["crc"], f"after the refusals: {now}"
    finally:
        radio.link.tx_arm(0)        # a refusal the radio took after all must not outlive the test
    now = radio.link.tx_arm_info()
    assert now["stage"] == 0 and now["crc"] == info["crc"], now


@pytest.mark.feature("tx_arm")
@pytest.mark.reboot
def test_tx_arm_off_after_restart(radio, arm):
    """RAM only: off after every start, whatever it was before"""
    up_before = as_int(radio.view(["link"])["uptime_ms"])
    radio.link.reboot(0, radio.link.chip()["token"])
    radio.link.close()
    time.sleep(2.0)
    radio.reconnect(30, "the restart")
    radio.tx_gate(False)            # a restart cleared the test TX limit: blocked again, as the test began
    assert as_int(radio.view(["link"])["uptime_ms"]) < up_before + 2000, "the radio didn't restart"
    now = radio.link.tx_arm_info()
    assert now is not None and now["stage"] == 0, now


@pytest.mark.feature("tx_arm")
@pytest.mark.radio_tx
def test_tx_arm_under_block(radio, arm, pytestconfig):
    """The handshake with the owner's backup, under a test TX limit that blocks every transmission: stage 1 with the
    CRC-32 of the backup's range is taken and read back, a factory restore is refused as busy while armed, off is
    taken. No PTT; with the block nothing could key, an automatic transmission included"""
    info = arm
    path = pytestconfig.getoption("--data-flash-backup")
    if not path:
        skip("opt_in", "give --data-flash-backup FILE (this radio's own backup) for the arm's handshake")
    if not info["cal_ok"]:
        skip("state", "no calibration in use that the arm covers (a blank or refused table): nothing to arm")
    if info["stage"]:
        skip("state", f"armed (stage {info['stage']}, by the owner's tool): the test leaves an armed radio alone")
    backup = factory_backup_bytes(path, info["addr"], info["len"])[1]
    same, lines = tx_arm_compare(radio.link, info, backup)
    print("\n".join(lines))
    assert same, f"{path}: its range is not the calibration in use, so it is not this radio's backup of that table"
    radio.link.tx_limit(block=True)
    try:
        assert radio.view(["core"]).get("txlim") == "block", "the test TX limit doesn't block"
        try:
            radio.link.tx_arm(1, backup)
        except DebugLinkError as e:
            if e.status == ST_REFUSED and e.data and e.data[0] == 4:
                skip("state", "the radio refuses every arm now (reason 4: a bench state, a stored reference trim, "
                              "the power's off state, a restore pending)")
            raise
        now = radio.link.tx_arm_info()
        assert now["stage"] == 1 and now["crc"] == zlib.crc32(backup), now
        try:
            fi = radio.link.factory_info()
        except DebugLinkError:
            fi = None
        if fi is not None and radio.link.storage_info()["region"] == "ours":
            # the window CRC sent is not the window's (rfoundry_hil.factory_restore_guard): never one that could write
            wrong = zlib.crc32(radio.link.storage_read(fi["addr"], fi["size"])) ^ 1
            with pytest.raises(DebugLinkError) as e:
                radio.link.factory_restore(fi["size"], 0, wrong)
            assert e.value.status == ST_REFUSED and e.value.data[:1] == bytes([REASON["busy"]]), \
                f"a factory restore while armed: {e.value} {e.value.data.hex()}"
    finally:
        try:
            radio.link.tx_arm(0)
            off = radio.link.tx_arm_info()
        except (DebugLinkError, OSError):
            off = None
        # Not off: the limit stays at block and the run ends here, before a later test's gate sets the run's
        # frequencies (with the limit off at the end, the armed radio refuses again); a restart clears the arm
        if off is None or off["stage"] != 0:
            pytest.exit(f"the transmit arm is still on after off ({off}): switch the radio off and on", returncode=1)
        radio.tx_gate(True)         # the run's limit for a radio_tx test; the test's teardown blocks it again
