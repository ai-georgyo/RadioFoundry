# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The processor state the STM32 stock codec needs (docs/spec/codec-placement.md, "Processor state"): the core's
CCR.UNALIGN_TRP (bit 3) and CCR.DIV_0_TRP (bit 4) are 0. The platform clears both at start-up where the image can be
entered without a reset, the host clears them once before its first codec call, and nothing may set them later
(docs/spec/gotchas.md). Read only: one READ_MEM of the system control block (docs/spec/debuglink.md 3.10), nothing
transmitted.

Run it once after an install's own start (a ROM bootloader's jump into the image) and once after a power cycle: the
two starts reach the image with different core state.
"""
import os
import sys

import pytest

from rfoundry_hil import DebugLinkError, skip
from rfoundry_debug import CHIP_NAMES, CHIP_STM32F4, CHIP_STM32H7, ST_REFUSED, ST_UNAVAILABLE

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "rfoundry"))
from rfoundry import boards  # noqa: E402

ST_UNKNOWN_CMD = 1
SCB_CCR = 0xE000ED14            # the configuration and control register, in both STM32 families' READ_MEM windows
CCR_UNALIGN_TRP = 1 << 3
CCR_DIV_0_TRP = 1 << 4


@pytest.mark.feature("codec_check")
def test_codec_trap_bits_clear(radio):
    family = radio.link.chip()["family"]
    if family not in (CHIP_STM32F4, CHIP_STM32H7):
        skip("no_hw", f"no STM32 stock codec host on chip family {CHIP_NAMES.get(family, family)}")
    board = boards.from_debuglink_name(radio.version(fresh=True).get("board", ""))
    if board is None:
        skip("not_built", "the radio reports a board the registry does not know")
    if not board.has_codec:
        skip("not_built", f"{board.id} has no codec route ([codec] none), so its firmware has no codec host")
    try:
        ccr = int.from_bytes(radio.link.read_mem(SCB_CCR, 4), "little")
    except DebugLinkError as e:
        if e.status in (ST_UNKNOWN_CMD, ST_UNAVAILABLE, ST_REFUSED):
            skip("not_built", f"firmware whose READ_MEM windows hold no system control block (status {e.status})")
        raise
    print(f"CCR 0x{ccr:08X}")
    assert not ccr & CCR_UNALIGN_TRP, f"CCR.UNALIGN_TRP is set (CCR 0x{ccr:08X}): the stock codec needs it off"
    assert not ccr & CCR_DIV_0_TRP, f"CCR.DIV_0_TRP is set (CCR 0x{ccr:08X}): the stock codec needs it off"
