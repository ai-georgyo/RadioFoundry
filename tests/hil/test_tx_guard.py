# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The TX guard (README.md, "Transmit frequencies"): layer 1 refuses a debug link request that would key outside the
rig's tx_mhz, layer 2 (Radio.tx_gate) sets the firmware's test TX limit: blocked outside radio_tx tests, the run's
transmit frequencies in them. And the factory restore guard: no test sends a FACTORY_RESTORE the radio could accept.
No radio: these run anywhere, and tools/ci.sh runs them."""
import struct
import types
import zlib

import pytest

import hil_config
import rfoundry_hil as H
from rfoundry_debug import STORAGE_OP_FACTORY_INFO


class FakeConfig:
    def __init__(self, allow_rf, tx_mhz, manual=False):
        self.option = types.SimpleNamespace(tx_mhz=tx_mhz)
        self._opts = {"--allow-rf": allow_rf, "--manual": manual}

    def getoption(self, name):
        return self._opts[name]


class FakeLink:
    """The views the guard reads, and the requests that got through"""

    def __init__(self, tx_mhz=461.4625, mode="dmr", aprs_mhz=0.0, via="both"):
        self.views = {"core": {"tx_hz": str(round(tx_mhz * 1e6)), "mode": mode},
                      "aprs": {"aprs.freq_hz": str(round(aprs_mhz * 1e6)), "aprs.block": "ok" if aprs_mhz else "no_freq"},
                      "aprs_set": {"set.aprs_via": via}}
        self.sent = []

    def view(self, sections):
        return dict(self.views[sections[0]])


@pytest.fixture
def rig(monkeypatch):
    sent = []
    monkeypatch.setattr(H, "_request", lambda link, cmd, payload=b"", *a, **k: sent.append((cmd, bytes(payload))))
    monkeypatch.setattr(hil_config, "CONFIG", FakeConfig(True, [461.4625, 446.05]))
    return sent


def ptt(down):
    return H.PTT, struct.pack("<BH", 1 if down else 0, 1000)


@pytest.mark.parametrize("cmd,payload,where", [
    (*ptt(True), "channel"),
    (*ptt(False), None),
    (H.KEY, struct.pack("<BBBb", 0, H.ACTIONS["ptt"], H.PRESS, 0), "channel"),
    (H.KEY, struct.pack("<BBBb", 0, H.ACTIONS["ptt"], H.UP, 0), None),
    (H.KEY, struct.pack("<BBH", 3, H.ACTIONS["ptt"], 500), "channel"),
    (H.KEY, struct.pack("<BBBb", 0, H.ACTIONS["emergency"], H.LONG, 0), "channel"),
    (H.KEY, struct.pack("<BBBb", 0, H.ACTIONS["ok"], H.PRESS, 0), None),
    (H.RADIO, bytes([H.OP_TONE_BURST, 1, 0, 0]), "channel"),
    (H.RADIO, bytes([H.OP_TONE_BURST, 0, 0, 0]), None),
    (H.DMR_DATA, bytes([H.DATA_OP_SMS]) + b"x", "channel"),
    (H.DMR_DATA, bytes([H.DATA_OP_EMERGENCY, 1]), "channel"),
    (H.DMR_DATA, bytes([H.DATA_OP_EMERGENCY, 0]), None),
    (H.APRS, bytes([H.APRS_OP_SEND_NOW]), "position"),
    (H.APRS, bytes([H.APRS_OP_STATUS]) + b"x", "aprs"),
    (H.APRS, bytes([4]) + b"Q0TST", None),       # TEST_CALL: a RAM-only callsign, no transmission
    (9, b"", None),                     # GET_VIEW
])
def test_keys_tx(cmd, payload, where):
    assert H.keys_tx(cmd, payload) == where


def test_allowed_channel_passes(rig):
    H._guarded_request(FakeLink(461.4625), *ptt(True))
    assert len(rig) == 1


def test_split_channel_refused(rig):
    # found by its RX frequency, the channel transmits elsewhere
    with pytest.raises(RuntimeError, match="467.37500 MHz"):
        H._guarded_request(FakeLink(467.375), H.KEY, struct.pack("<BBH", 3, H.ACTIONS["ptt"], 500))
    assert rig == []


def test_requests_that_dont_key_pass_anywhere(rig):
    H._guarded_request(FakeLink(467.375), *ptt(False))
    H._guarded_request(FakeLink(467.375), H.RADIO, bytes([H.OP_TONE_BURST, 0, 0, 0]))
    assert len(rig) == 2


def test_aprs_frequency_and_dmr_channel(rig):
    H._guarded_request(FakeLink(461.4625, "fm", aprs_mhz=446.05), H.APRS, bytes([H.APRS_OP_SEND_NOW]))
    with pytest.raises(RuntimeError, match="144.39000"):
        H._guarded_request(FakeLink(461.4625, "fm", aprs_mhz=144.39), H.APRS, bytes([H.APRS_OP_SEND_NOW]))
    with pytest.raises(RuntimeError, match="467.37500"):     # positions over DMR go on the DMR channel
        H._guarded_request(FakeLink(467.375, "dmr", aprs_mhz=446.05), H.APRS, bytes([H.APRS_OP_SEND_NOW]))
    assert len(rig) == 1


def test_aprs_position_by_its_way_out(rig):
    send = (H.APRS, bytes([H.APRS_OP_SEND_NOW]))
    H._guarded_request(FakeLink(461.4625, "dmr", aprs_mhz=144.39, via="dmr"), *send)    # over DMR: no AFSK
    H._guarded_request(FakeLink(467.375, "dmr", aprs_mhz=446.05, via="rf"), *send)      # by AFSK: the channel idles
    with pytest.raises(RuntimeError, match="467.37500"):
        H._guarded_request(FakeLink(467.375, "dmr", aprs_mhz=446.05, via="dmr"), *send)
    with pytest.raises(RuntimeError, match="144.39000"):
        H._guarded_request(FakeLink(461.4625, "dmr", aprs_mhz=144.39, via="rf"), *send)
    link = FakeLink(461.4625, "dmr", aprs_mhz=144.39)
    del link.views["aprs_set"]["set.aprs_via"]      # a firmware that doesn't report it gets both checked
    with pytest.raises(RuntimeError, match="144.39000"):
        H._guarded_request(link, *send)
    with pytest.raises(RuntimeError, match="144.39000"):      # a message goes by AFSK whatever the via
        H._guarded_request(FakeLink(461.4625, "dmr", aprs_mhz=144.39, via="dmr"), H.APRS, bytes([H.APRS_OP_MESSAGE]) + b"x")
    H._guarded_request(FakeLink(467.375, "dmr", aprs_mhz=446.05), H.APRS, bytes([H.APRS_OP_STATUS]) + b"x")
    assert len(rig) == 3


def test_aprs_frequency_only_when_rf_aprs_can_send(rig):
    link = FakeLink(461.4625, "fm", aprs_mhz=144.39)
    link.views["aprs"]["aprs.block"] = "not_amateur"
    H._guarded_request(link, H.APRS, bytes([H.APRS_OP_SEND_NOW]))
    assert len(rig) == 1


def test_no_check_without_allow_rf(rig, monkeypatch):
    monkeypatch.setattr(hil_config, "CONFIG", FakeConfig(False, []))
    H._guarded_request(FakeLink(467.375), *ptt(True))      # layer 2 keeps a radio without --allow-rf off the air
    assert len(rig) == 1


class GateLink:
    """The core view's txlim key (built False: firmware without the test TX limit) and the limits set"""

    def __init__(self, built=True):
        self.built = built
        self.limits = []

    def view(self, sections):
        assert sections == ["core"]
        return {"txlim": "off"} if self.built else {}

    def tx_limit(self, block=False, mhz=()):
        self.limits.append("block" if block else tuple(mhz))


def gated(built=True):
    r = H.Radio.__new__(H.Radio)
    r.link = GateLink(built)
    r._txlim = None
    return r


def run_config(allow_rf):
    cfg = FakeConfig(allow_rf, [461.4625, 446.05, 467.375])
    cfg.option.dmr_freq = cfg.option.fm_mhz = 461.4625
    cfg.option.aprs_test_mhz = 446.05
    return cfg


def test_run_tx_mhz(monkeypatch):
    monkeypatch.setattr(hil_config, "CONFIG", run_config(True))
    assert hil_config.run_tx_mhz() == [446.05, 461.4625]       # each once, and not all of tx_mhz
    monkeypatch.setattr(hil_config, "CONFIG", run_config(False))
    assert hil_config.run_tx_mhz() == []


def test_run_tx_mhz_wattmeter(monkeypatch):
    """With --manual the wattmeter check's frequencies (default 445 and 146) join the run's, only those in tx_mhz"""
    cfg = run_config(True)
    cfg._opts["--manual"] = True
    cfg.option.tx_mhz = cfg.option.tx_mhz + [146.0]
    monkeypatch.setattr(hil_config, "CONFIG", cfg)
    assert hil_config.wattmeter_mhz() == [445.0, 146.0]
    assert hil_config.run_tx_mhz() == [146.0, 446.05, 461.4625]     # 445 isn't in tx_mhz
    cfg.option.wattmeter_mhz = "446.05, 461.4625"
    assert hil_config.wattmeter_mhz() == [446.05, 461.4625]
    assert hil_config.run_tx_mhz() == [446.05, 461.4625]
    cfg._opts["--manual"] = False
    cfg.option.wattmeter_mhz = None
    assert hil_config.run_tx_mhz() == [446.05, 461.4625]            # no person at the meter: none of them


def test_gate_blocks_outside_radio_tx(monkeypatch):
    monkeypatch.setattr(hil_config, "CONFIG", run_config(True))
    r = gated()
    assert r.tx_gate(False) and r.tx_gate(True) and r.tx_gate(False)
    r.tx_gate_end()
    assert r.link.limits == ["block", (446.05, 461.4625), "block", ()]


def test_gate_blocks_radio_tx_without_frequencies(monkeypatch):
    monkeypatch.setattr(hil_config, "CONFIG", run_config(False))
    r = gated()
    r.tx_gate(True)
    assert r.link.limits == ["block"]


def test_gate_without_the_limit(monkeypatch):
    monkeypatch.setattr(hil_config, "CONFIG", run_config(True))
    r = gated(False)
    assert not r.tx_gate(False) and not r.tx_gate(True)
    r.tx_gate_end()
    assert r.link.limits == []


def test_run_tx_mhz_t3_payload(monkeypatch):
    """--t3-payload-tx-mhz joins the run's transmit frequencies (the firmware's TX limit lets a granted call key)"""
    cfg = run_config(True)
    cfg.option.t3_payload_tx_mhz = "467.375, 461.4625"
    monkeypatch.setattr(hil_config, "CONFIG", cfg)
    assert hil_config.t3_payload_tx_mhz() == [467.375, 461.4625]
    assert hil_config.run_tx_mhz() == [446.05, 461.4625, 467.375]
    monkeypatch.setattr(hil_config, "CONFIG", run_config(False))
    assert hil_config.run_tx_mhz() == []


def t3_link(on=False, tx_mhz=0.0):
    link = FakeLink()
    link.views["t3"] = {"t3.on": "1" if on else "0", "t3.tx_hz": str(round(tx_mhz * 1e6))}
    return link


def t3_system(opts):
    return H.T3, bytes([H.T3_OP_SYSTEM]) + struct.pack("<IBHBHBIBBhHB", 0, 1, 5, 1, opts, 2, 0, 0, 0, 0, 0, 0)


def t3_ctrl(tx_mhz):
    return H.T3, bytes([H.T3_OP_CTRL]) + struct.pack("<HIIB", 1, 450012500, round(tx_mhz * 1e6), 0xFF)


@pytest.mark.parametrize("op,where", [(H.T3_OP_START, "t3"), (12, "t3"), (13, "t3"), (19, "t3"), (9, None),
                                      (H.T3_OP_CTRL, None)])
def test_keys_tx_t3(op, where):
    assert H.keys_tx(H.T3, bytes([op])) == where


def test_t3_start_checks_the_staged_control_channel(rig):
    """Not started yet: the guard checks the control channel staged through the link"""
    link = t3_link()
    H._guarded_request(link, *t3_system(0x20))
    H._guarded_request(link, *t3_ctrl(467.375))
    with pytest.raises(RuntimeError, match="467.37500 MHz"):
        H._guarded_request(link, H.T3, bytes([H.T3_OP_START]))
    link = t3_link()
    H._guarded_request(link, *t3_system(0x20))
    H._guarded_request(link, *t3_ctrl(461.4625))
    H._guarded_request(link, H.T3, bytes([H.T3_OP_START]))
    assert rig[-1] == (H.T3, bytes([H.T3_OP_START]))


def test_t3_listen_only_start_passes(rig):
    link = t3_link()
    H._guarded_request(link, *t3_system(0x21))
    H._guarded_request(link, *t3_ctrl(467.375))
    H._guarded_request(link, H.T3, bytes([H.T3_OP_START]))
    assert rig[-1] == (H.T3, bytes([H.T3_OP_START]))


def test_t3_camped_checks_its_control_channel(rig):
    with pytest.raises(RuntimeError, match="467.37500 MHz"):
        H._guarded_request(t3_link(True, 467.375), H.T3, bytes([12]) + struct.pack("<IBB", 1, 1, 37))
    H._guarded_request(t3_link(True, 461.4625), H.T3, bytes([12]) + struct.pack("<IBB", 1, 1, 37))


class FactoryLink:
    """The factory range a FACTORY_RESTORE guard reads (op 3's range, the window through STORAGE READ)"""

    def __init__(self, window=b"\x01\x02\x03\x04", info=True):
        self.window, self.info = window, info

    def factory_info(self):
        return {"addr": 0x1000, "size": len(self.window)} if self.info else None

    def storage_read(self, addr, n):
        assert (addr, n) == (0x1000, len(self.window))
        return self.window


def restore(window_crc):
    return H.STORAGE, struct.pack("<BHII", H.STORAGE_OP_FACTORY_RESTORE, 4, 0, window_crc)


@pytest.mark.parametrize("allow_rf", [False, True])
def test_factory_restore_guard(rig, monkeypatch, allow_rf):
    """No HIL test runs a factory restore that writes: a FACTORY_RESTORE whose window CRC is the window's is never
    sent, with or without --allow-rf; one with another CRC (the radio's check 7 refuses it) goes through"""
    monkeypatch.setattr(hil_config, "CONFIG", FakeConfig(allow_rf, []))
    live = zlib.crc32(b"\x01\x02\x03\x04")
    with pytest.raises(RuntimeError, match="FACTORY_RESTORE not sent"):
        H._guarded_request(FactoryLink(), *restore(live))
    assert rig == []
    H._guarded_request(FactoryLink(), *restore(live ^ 1))
    H._guarded_request(FactoryLink(info=False), *restore(live))        # no factory copy: the radio refuses it
    H._guarded_request(FactoryLink(), H.STORAGE, bytes([STORAGE_OP_FACTORY_INFO]))   # the other ops pass
    assert [p[0] for _, p in rig] == [H.STORAGE_OP_FACTORY_RESTORE, H.STORAGE_OP_FACTORY_RESTORE,
                                      STORAGE_OP_FACTORY_INFO]


def arm_goes(*request):
    """A guarded request that must go: a skip it raised fails the test (pytest would count it a skip, not a failure)"""
    try:
        H._guarded_request(*request)
    except BaseException as e:
        if H.skip_kind(e) is not None:
            pytest.fail(f"skipped where it must go: {e}")
        raise


def arm_skipped(*request):
    """The skip a guarded request raised (rfoundry_hil.skip, kind state: pytest's skip is no Exception), else None"""
    try:
        H._guarded_request(*request)
    except BaseException as e:
        if H.skip_kind(e) is None:
            raise
        assert H.skip_kind(e) == "state", e
        return e
    return None


class ArmLink(FakeLink):
    """A radio with the owner's transmit arm (RADIO op 44's info, None: none), the core view's power level and test
    TX limit, in a test that may transmit (may: Radio.tx_gate gave it the run's frequencies)"""

    def __init__(self, stage=0, cal_ok=True, crc=0x1234ABCD, mode="fm", power=0, txlim="list", may=True, arm=True):
        super().__init__(461.4625, mode)
        self.views["core"].update({"power": str(power), "txlim": txlim})
        self.info = {"stage": stage, "max": 3, "addr": 0x1000, "len": 512, "crc": crc, "cal_ok": cal_ok} if arm else None
        self._tx_may = may

    def tx_arm_info(self):
        return dict(self.info) if self.info else None


def arm_req(stage, ident):
    return H.RADIO, struct.pack("<BBI", H.OP_TX_ARM, stage, ident)


@pytest.mark.parametrize("link,dmr,level,armed", [
    (ArmLink(arm=False), True, 1, True),                # no arm: nothing the arm holds back
    (ArmLink(cal_ok=False), True, 1, True),             # no calibration the arm covers: the board's own rules
    (ArmLink(stage=0), False, 0, False),
    (ArmLink(stage=1), False, 0, True),
    (ArmLink(stage=1), False, 1, False),
    (ArmLink(stage=1), False, None, False),             # an unknown level needs FM at every level
    (ArmLink(stage=2), False, 3, True),
    (ArmLink(stage=2), True, 0, False),
    (ArmLink(stage=3), True, 1, True),
])
def test_tx_armed(link, dmr, level, armed):
    assert H.tx_armed(link, dmr, level) == armed


@pytest.mark.parametrize("stage,mode,power,keys", [(0, "fm", 0, False), (1, "fm", 0, True), (1, "fm", 1, False),
                                                   (2, "fm", 1, True), (2, "dmr", 0, False), (3, "dmr", 0, True)])
def test_arm_skip_by_mode_and_level(rig, stage, mode, power, keys):
    """A key-up the arm doesn't cover skips the test (state) before anything is sent; one it covers goes"""
    link = ArmLink(stage=stage, mode=mode, power=power)
    if keys:
        arm_goes(link, *ptt(True))
        assert len(rig) == 1
        return
    e = arm_skipped(link, *ptt(True))
    assert e is not None and "radio tx-arm" in str(e) and rig == []


def test_arm_skip_only_where_it_applies(rig, monkeypatch):
    """No skip: in a test that may not transmit (its key-up meets the blocking limit, which is what it checks), on a
    radio without the arm, with no calibration the arm covers, for a request that doesn't key, without --allow-rf"""
    for link in (ArmLink(may=False), ArmLink(arm=False), ArmLink(cal_ok=False)):
        arm_goes(link, *ptt(True))
    arm_goes(ArmLink(), *ptt(False))
    monkeypatch.setattr(hil_config, "CONFIG", FakeConfig(False, []))
    arm_goes(ArmLink(), *ptt(True))
    assert len(rig) == 5


def test_arm_skip_other_ways_out(rig):
    """An APRS message is FM at any level; a position by its way out; a Tier III request digital"""
    assert arm_skipped(ArmLink(stage=1), H.APRS, bytes([H.APRS_OP_STATUS]) + b"x")
    link = ArmLink(stage=2)
    link.views["aprs"] = {"aprs.freq_hz": "461462500", "aprs.block": "ok"}
    link.views["aprs_set"]["set.aprs_via"] = "rf"
    arm_goes(link, H.APRS, bytes([H.APRS_OP_SEND_NOW]))
    link.views["aprs_set"]["set.aprs_via"] = "both"
    arm_goes(link, H.APRS, bytes([H.APRS_OP_SEND_NOW]))      # from an FM channel: AFSK alone
    link.views["core"]["mode"] = "dmr"
    assert arm_skipped(link, H.APRS, bytes([H.APRS_OP_SEND_NOW]))       # from a DMR channel: over DMR too
    link.views["aprs_set"]["set.aprs_via"] = "dmr"
    assert arm_skipped(link, H.APRS, bytes([H.APRS_OP_SEND_NOW]))
    link.views["t3"] = {"t3.on": "1", "t3.tx_hz": "461462500"}
    assert arm_skipped(link, H.T3, bytes([H.T3_OP_START]))
    assert len(rig) == 2


def test_arm_skip_not_for_a_bench_drive(rig):
    """The one key-up after a bench drive (RADIO op 36) is the bench's, which the arm doesn't concern; the next is not"""
    link = ArmLink(stage=0)
    arm_goes(link, H.RADIO, struct.pack("<BH", H.OP_TX_DRIVE, 300))
    arm_goes(link, *ptt(True))
    assert arm_skipped(link, *ptt(True))
    arm_goes(link, H.RADIO, struct.pack("<BH", H.OP_TX_DRIVE, 300))
    arm_goes(link, H.RADIO, struct.pack("<BH", H.OP_TX_DRIVE, 0))      # dropped again
    assert arm_skipped(link, *ptt(True))
    assert [p[0] for _, p in rig] == [H.OP_TX_DRIVE, 1, H.OP_TX_DRIVE, H.OP_TX_DRIVE]


@pytest.mark.parametrize("allow_rf", [False, True])
def test_tx_arm_guard(rig, monkeypatch, allow_rf):
    """No HIL test arms by itself: an op 44 raising the stage with the calibration's CRC goes only with --allow-rf and
    the test TX limit at block; another id (the radio refuses it), off, a lower stage and the info always go"""
    monkeypatch.setattr(hil_config, "CONFIG", FakeConfig(allow_rf, []))
    crc = 0x1234ABCD
    for txlim in ("list", "off"):
        with pytest.raises(RuntimeError, match="RADIO op 44 not sent"):
            arm_goes(ArmLink(txlim=txlim), *arm_req(1, crc))
        with pytest.raises(RuntimeError, match="RADIO op 44 not sent"):
            arm_goes(ArmLink(stage=1, txlim=txlim), *arm_req(1, crc))    # the same stage again: guarded
        with pytest.raises(RuntimeError, match="RADIO op 44 not sent"):
            arm_goes(ArmLink(stage=1, txlim=txlim), *arm_req(3, crc))
    assert rig == []
    if allow_rf:
        arm_goes(ArmLink(txlim="block"), *arm_req(1, crc))
    else:
        with pytest.raises(RuntimeError, match="--allow-rf"):
            arm_goes(ArmLink(txlim="block"), *arm_req(1, crc))
    arm_goes(ArmLink(), *arm_req(1, crc ^ 1))             # another id: refused by the radio, reason 2
    arm_goes(ArmLink(stage=2), *arm_req(1, crc))          # lower: always taken, arms nothing new
    arm_goes(ArmLink(stage=2), *arm_req(0, 0))            # off
    arm_goes(ArmLink(arm=False), *arm_req(1, crc))        # no arm: the radio answers unavailable
    arm_goes(ArmLink(cal_ok=False), *arm_req(1, crc))     # none in use: refused, reason 1
    arm_goes(ArmLink(), H.RADIO, bytes([H.OP_TX_ARM]))    # the info
    assert len(rig) == (7 if allow_rf else 6)
