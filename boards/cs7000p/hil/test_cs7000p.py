# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""CS7000 M17 PLUS bring-up checks (docs/testing/cs7000p-bringup.md), radio only, never transmits. They read the debug
link's `board` view section and its second, `board2` (docs/spec/hardware-cs7000p.md 15), which only this radio has:
the clock that started, the chip's revision and ROM bootloader, the knob and the supply hold, the output expander and
the shared DAC, the LCD board check and the panel's ID, the data flash and its reserved sector, the calibration state,
the synthesizer, the modem and its I2S direction probe, the maker's factory table and the tree's copy of it. Nothing
here writes the data flash: the table's check reads it through STORAGE READ; nothing arms the owner's transmit arm,
whose generic tests are tests/hil/test_tx_arm.py. The port was written from the
schematic, so most of these are the first evidence that a line or a polarity is right. The display, the build features and the storage region's generic checks are
tests/hil/test_health.py's (from board.toml [hil]). A board's own test: skipped other_board on any other radio
(tests/hil/hil_plugin.py). Their feature markers are optional: they record bring-up, they gate nothing."""
import time
import zlib

import pytest

from rfoundry_debug import FACTORY_RESULTS, ST_BAD_ARG
from rfoundry_hil import DebugLinkError, skip

KEYS = ["clock", "mcu_rev", "rom_loader", "rdp", "exp", "knob", "hold", "pow_det_mv", "vbat_mv", "vdda_mv", "vol_raw",
        "vox_raw", "lcd_board", "panel", "keys_raw", "chsw_raw", "chsw", "phone_det", "bt_strap", "flash", "store",
        "cal", "tx_refused", "dac", "lo_side", "pll", "ak", "rssi_raw", "noise_raw", "qt_raw", "rx_tune",
        "ref_offset", "pc8_duty", "hrc", "gps_baud", "adc_restarts"]
# board2's (board.view_split), still board.<key>
KEYS2 = ["cal_table", "cal_rec"]

# The maker's factory table (store_cs7000p.h): a factory range, which STORAGE READ may read. The copy's record at
# 0x01B000 is in no area STORAGE READ opens
TABLE_ADDR, TABLE_SIZE = 0x001000, 0x200

# exp_cs7000p.h
EXP_APC_LOOP = 1 << 3
EXP_TX_RAIL = 1 << 9
EXP_POWER_HOLD = 1 << 20
EXP_TX_BITS = EXP_APC_LOOP | EXP_TX_RAIL

# chip_reg 3:5 (hardware-cs7000p.md 14): the reference DAC's coding in receive, 0 the measured hold, 1 the maker's
# state, 2 the same with 4:0x26 = 0x7D. Chip 0 page 4 is the HR_C6000
BOARD_CHIP, REF_CODING = 3, 5
# chip_reg 3:4: the bench's reference offset, 0x100 while none is set; trim A's receive bound (makercal_cs7000p.h)
REF_OFFSET, REF_RELEASE, REF_RX_BOUND = 4, 0x100, 32
HRC_CHIP, HRC_PAGE = 0, 4


def board_view(r):
    # board2 with it, where the firmware has it. Its keys read "busy" in a reply that waited 250 ms for a restore's
    # take to end (hardware-cs7000p.md 15): asked again
    for _ in range(4):
        v = r.view(["board"])
        b = {k[len("board."):]: val for k, val in v.items() if k.startswith("board.")}
        if b.get("cal_table") != "busy":
            return b
        time.sleep(0.5)
    return b


def fields(value):
    """A compound value, "locked r=2 vco_hz=450000000": its first word and the name=value words after it."""
    words = value.split()
    return (words[0] if words else ""), dict(w.split("=", 1) for w in words[1:] if "=" in w)


@pytest.mark.feature("debuglink", optional=True)
def test_board_view_keys(radio):
    """Every key is there: a line that did not fit the section's one frame would be missing from the end."""
    b = board_view(radio)
    assert b.get("name") == "CS7000 M17 PLUS"
    missing = [k for k in KEYS if k not in b]
    assert not missing, f"board view keys missing: {missing}"


@pytest.mark.feature("diag", optional=True)
def test_clock(radio):
    """The 25 MHz crystal started and PLL1 locked on it. hsi-pll: the crystal did not start and the radio runs the
    same rates from its internal oscillator (bring-up question 16)."""
    b = board_view(radio)
    assert b["clock"] in ("hse", "hsi-pll", "hsi"), b["clock"]
    assert b["clock"] == "hse", f"clock {b['clock']}: the 25 MHz crystal or its PLL did not start"


@pytest.mark.feature("diag", optional=True)
def test_chip_identity(radio):
    """Which silicon and which ROM bootloader this unit has (bring-up question 14): printed for the record."""
    b = board_view(radio)
    print(f"MCU revision {b['mcu_rev']}, ROM bootloader {b['rom_loader']}, RDP byte {b['rdp']}")
    assert b["mcu_rev"] in ("Y", "X", "V"), f"an STM32H743 revision the port does not know: {b['mcu_rev']}"
    assert 0 < int(b["rom_loader"], 0) < 0xFF, b["rom_loader"]
    assert int(b["rdp"], 0) != 0xCC, "read protection level 2: the ROM bootloader is gone for good"


@pytest.mark.feature("radio_controls", optional=True)
def test_knob_and_hold(radio):
    """The radio answers, so its knob is on: the ADC scan read it on and the supply is held. noadc: the ADC did not
    start, the supply is never held, and the radio goes off with the knob."""
    b = board_view(radio)
    assert b["knob"] == "on", f"knob {b['knob']} on a running radio"
    assert b["hold"] == "1", "the knob read on but the supply is not held"
    assert int(b["exp"], 0) & EXP_POWER_HOLD, b["exp"]


@pytest.mark.feature("diag", optional=True)
def test_supply_reading(radio):
    """The battery sense line against the model (hardware-cs7000p.md 5.5), and the 3.3 V rail from the chip's own
    reference. The battery's range is test_health.py's."""
    b = board_view(radio)
    pow_det, vbat, vdda = int(b["pow_det_mv"]), int(b["vbat_mv"]), int(b["vdda_mv"])
    print(f"POW_DET {pow_det} mV, battery by the model {vbat} mV, VDDA {vdda} mV")
    assert vdda != 0, "VDDA not measured: the readings use the nominal 3,240 mV"
    assert 3000 <= vdda <= 3500, f"VDDA {vdda} mV (the regulator's divider gives about 3,240)"
    assert 1300 <= pow_det <= 2400, f"POW_DET {pow_det} mV: 1,560 to 2,160 mV is a 6.0 to 8.4 V battery"
    model = (4006 * pow_det - 245000) // 1000
    assert abs(vbat - model) <= 300, f"battery {vbat} mV, the model gives {model} mV from POW_DET (smoothed: retry)"


@pytest.mark.feature("diag", optional=True)
def test_adc_scan(radio):
    """The ADC1 scan runs: every rank in range, and no overrun recovery."""
    b = board_view(radio)
    for k in ("vol_raw", "vox_raw", "rssi_raw", "noise_raw", "qt_raw"):
        assert 0 <= int(b[k]) <= 4095, (k, b[k])
    assert int(b["adc_restarts"]) == 0, f"ADC scan restarted {b['adc_restarts']} times after overruns"


@pytest.mark.feature("fm", optional=True)
def test_transmitter_off_and_interlock(radio):
    """Idle: neither transmit bit of the expander is set, and the shared DAC holds a value above 0 only as the front
    end's tuning voltage. No write was refused by the interlock unless something tried to transmit."""
    b = board_view(radio)
    exp = int(b["exp"], 0)
    dac, f = fields(b["dac"])
    assert exp & EXP_TX_BITS == 0, f"a transmit bit is set on an idle radio: exp {b['exp']}"
    assert 0 <= int(dac) <= 4095, b["dac"]
    print(f"expander {b['exp']}, shared DAC {dac}, interlock refusals {f.get('refused')}, tx refused {b['tx_refused']}")


@pytest.mark.feature("fm", optional=True)
def test_calibration_state(radio):
    """Until the tables are filled from bench measurements the radio refuses every transmission; with the maker's
    table in use (cal=maker) until the owner arms transmission (hardware-cs7000p.md 13.2)."""
    b = board_view(radio)
    assert b["cal"] in ("none", "model", "maker"), b["cal"]
    if b["cal"] == "none":
        print("cal=none: transmit is refused (docs/testing/power-calibration.md fills the tables)")
    elif b["cal"] == "maker":
        print(f"cal=maker: the maker's table; refused until the owner arms transmission (tx_arm={b.get('tx_arm')})")


@pytest.mark.feature("factory_copy")
def test_factory_table_keys(radio):
    """The maker's table and its copy as the firmware took them at start, in board2. The window's CRC the view gives is
    the CRC-32 of the 512 bytes read through STORAGE READ now (the one to compare with the backup's 0x1000-0x11FF),
    and a window in use beside an ok record means the record holds the same bytes. Read only."""
    b = board_view(radio)
    if "cal_table" not in b:
        skip("not_built", "firmware without the table's view keys (older than the factory copy's board2)")
    missing = [k for k in KEYS2 if k not in b]
    assert not missing, f"board2 keys missing: {missing}"
    win, t = fields(b["cal_table"])
    rec, r = fields(b["cal_rec"])
    print(f"cal_table {b['cal_table']}; cal_rec {b['cal_rec']}")
    assert win in ("unread", "blank", "zero", "refused", "ok"), b["cal_table"]
    assert t["src"] in ("window", "record", "none"), b["cal_table"]
    assert rec in ("unread", "blank", "ok", "bad", "newer", "other", "off"), b["cal_rec"]
    # The source order (settings-store.md 4.2): the window when valid, else an ok record, never both when they differ
    if t["src"] == "window":
        assert win == "ok", b["cal_table"]
        # an ok record beside the window in use is its copy: the same bytes (else the source would be none)
        assert rec != "ok" or r["crc"] == t["crc"], b
    elif t["src"] == "record":
        assert win != "ok" and rec == "ok", b
    else:
        assert t["why"] == "conflict" or (win != "ok" and rec != "ok"), b
    if win == "unread":
        skip("state", f"the window was not read at this start ({t['why']}): nothing to compare")
    window = radio.link.storage_read(TABLE_ADDR, TABLE_SIZE)
    assert zlib.crc32(window) == int(t["crc"], 16), \
        f"the window's CRC now 0x{zlib.crc32(window):08x}, the view's {t['crc']}: the window changed since the start"
    if win == "blank":
        print("the window is blank: the table is lost on this radio until it is restored from the backup "
              "(tools/rfoundry_debug.py storage factory --backup FILE --restore)")


@pytest.mark.feature("factory_copy", optional=True)
def test_factory_info_matches_view(radio):
    """STORAGE op 3 FACTORY_INFO says what board2's cal_table and cal_rec say: the source, the window's state and
    CRC, the record's state and CRC, the last restore. The board takes the copy's state at its init and after a
    restore, so the two agree unless a restore's outcome is being taken. Read only."""
    b = board_view(radio)
    if "cal_table" not in b:
        skip("not_built", "firmware without the table's view keys (older than the factory copy's board2)")
    try:
        info = radio.link.factory_info()
    except DebugLinkError as e:
        skip("not_built", f"no FACTORY_INFO in this firmware ({e})")
    if info is None:
        skip("not_built", "FACTORY_INFO reports no factory copy")
    win, t = fields(b["cal_table"])
    rec, r = fields(b["cal_rec"])
    assert (info["addr"], info["size"]) == (TABLE_ADDR, TABLE_SIZE), info
    assert (info["source"], info["window"]) == (t["src"], win), (info, b["cal_table"])
    assert f"0x{info['window_crc']:08x}" == t["crc"], (info, b["cal_table"])
    assert info["record"] == rec, (info, b["cal_rec"])
    want = f"0x{info['record_crc']:08x}" if rec in ("ok", "bad") else "-"
    assert r["crc"] == want, (info, b["cal_rec"])
    assert FACTORY_RESULTS[info["result"]][0] == r["restore"], (info, b["cal_rec"])


@pytest.mark.feature("tx_arm", optional=True)
def test_tx_arm_key(radio):
    """board2's tx_arm says what RADIO op 44 says (hardware-cs7000p.md 13.2, 15): the stage, the id it was armed with
    (0 when off), list=1 exactly while the core view's test TX limit is a list; the info's range is the maker's table
    and its CRC the table in use's (cal_table's for the window, cal_rec's for the record), and the top stage is dmr
    in a DMR build, fm without. Read only: nothing is armed"""
    b = board_view(radio)
    if "tx_arm" not in b:
        skip("not_built", "firmware without board2's tx_arm key (older than the owner's transmit arm)")
    info = radio.link.tx_arm_info()
    assert info is not None, "board2 has tx_arm but RADIO op 44 answers no arm"
    stage, a = fields(b["tx_arm"])
    _, t = fields(b["cal_table"])
    rec, r = fields(b["cal_rec"])
    print(f"tx_arm {b['tx_arm']}; op 44 stage {info['stage']} max {info['max']} crc 0x{info['crc']:08x}")
    assert stage == ("off", "fm-low", "fm", "dmr")[info["stage"]], (b["tx_arm"], info)
    assert stage != "off" or a["id"] == "0x00000000", b["tx_arm"]
    assert a["list"] == ("1" if radio.view(["core"]).get("txlim") == "list" else "0"), b["tx_arm"]
    assert (info["addr"], info["len"]) == (TABLE_ADDR, TABLE_SIZE), info
    assert info["max"] == (3 if "DMR" in radio.features() else 2), info
    assert info["cal_ok"] == (t["src"] != "none"), (info, b["cal_table"])
    if t["src"] == "window":
        assert f"0x{info['crc']:08x}" == t["crc"], (info, b["cal_table"])
    elif t["src"] == "record":
        assert rec == "ok" and f"0x{info['crc']:08x}" == r["crc"], (info, b["cal_rec"])
    if stage != "off":
        assert a["id"] == f"0x{info['crc']:08x}", (b["tx_arm"], info)


@pytest.mark.feature("ui_navigation", optional=True)
def test_lcd_board(radio):
    """The LCD board is not in the maker's schematic. The check reads five of its lines before anything is driven:
    on the assumed board only one of them reads low. unexpected: the display and the key matrix stay off by design."""
    b = board_view(radio)
    state, f = fields(b["lcd_board"])
    print(f"LCD board {state}, levels {f.get('levels')}")
    assert state in ("assumed", "unexpected", "forced"), b["lcd_board"]
    assert state != "unexpected", \
        f"the LCD connector does not read as the assumed board (levels {f.get('levels')}): trace it before forcing"


@pytest.mark.feature("ui_navigation", optional=True)
def test_panel_id(radio):
    """The panel's ID reads back the same through both commands (a working read path), and the driver knows it."""
    b = board_view(radio)
    if fields(b["lcd_board"])[0] == "unexpected":
        skip("no_hw", "the LCD board is not the assumed one: no bus, no ID read")
    controller, f = fields(b["panel"])
    print(f"panel {controller}, source {f.get('source')}, id {f.get('id')}, id1-3 {f.get('reg')}")
    assert f.get("id") not in (None, "000000", "ffffff"), f"panel ID not read: {b['panel']}"
    assert f.get("id") == f.get("reg"), f"RDDID and ID1-ID3 differ: {b['panel']}"
    assert f.get("source") in ("id", "build"), f"the ID is not one the driver knows: {b['panel']}"
    assert controller not in ("unknown", "none"), b["panel"]


@pytest.mark.feature("radio_controls", optional=True)
def test_keys_and_selector_read(radio):
    """The raw scan and the selector's four lines, printed for the bring-up record (the matrix positions of the
    function keys and the selector's code are measured from these)."""
    b = board_view(radio)
    print(f"keys_raw {b['keys_raw']}, chsw_raw {b['chsw_raw']}, chsw {b['chsw']}")
    assert len(b["keys_raw"]) == 10 and int(b["keys_raw"], 16) >= 0, b["keys_raw"]
    assert 0 <= int(b["chsw_raw"], 0) <= 15, b["chsw_raw"]
    assert b["chsw"] == "unverified" or 0 <= int(b["chsw"]) <= 15, b["chsw"]
    assert b["phone_det"] in ("0", "1") and b["bt_strap"] in ("0", "1")


@pytest.mark.feature("settings_store", optional=True)
def test_data_flash(radio):
    """The 32 MB part answers on SPI4 with 4-byte addresses, the driver refused no write of the maker's bytes, and the
    board sector was read."""
    b = board_view(radio)
    jedec, f = fields(b["flash"])
    assert jedec == "EF4019", f"data flash ID {jedec}: not the W25Q256JV (000000: no answer, or left busy / suspended)"
    assert int(f["size"], 0) == 0x2000000 and f["addr4"] == "1", b["flash"]
    # the driver's backstop: no erase or program reached a sector holding the maker's bytes outside storage's grant
    assert f.get("refused", "0") == "0", f"a writer missed the kept-bytes rule: {b['flash']}"
    assert b["store"] in ("blank", "ours", "foreign"), f"board sector {b['store']}"
    if b["store"] == "foreign":
        print("the board sector at 0x01B000 holds someone else's bytes: note it, it is not usable for the table's copy")
    s = radio.view(["storage"])
    assert s["storage.region"] in ("ours", "foreign"), s["storage.region"]


@pytest.mark.feature("fm", optional=True)
def test_receiver_chips(radio):
    """The modem answered after its set-up, the IF receiver took its set-up, and the synthesizer is locked on the
    receive frequency: the three buses, the sleep and power-down polarities and the VCO select (question 32)."""
    b = board_view(radio)
    hrc, hf = fields(b["hrc"])
    pll, pf = fields(b["pll"])
    print(f"hrc {b['hrc']}, ak {b['ak']}, pll {b['pll']}, lo_side {b['lo_side']}, rx_tune {b['rx_tune']}, "
          f"ref_offset {b['ref_offset']}, pc8_duty {b['pc8_duty']}")
    assert hrc == "1", f"the HR_C6000 did not answer: {b['hrc']}"
    assert b["ak"] == "ok", "the AK2365A's set-up failed"
    assert pll == "locked", f"synthesizer not locked: {b['pll']}"
    assert int(pf["vco_hz"]) > 0 and int(pf["r"]) > 0, b["pll"]
    assert b["lo_side"] in ("low", "high")


@pytest.mark.feature("fm", optional=True)
def test_reference_coding_knob(radio):
    """chip_reg 3:5 reaches the modem at the next receive set-up, and back (hardware-cs7000p.md 6.5): 1 leaves the
    set-up's 4:0x25 = 0x0E with the driver's 4:0x26 = 0xFD, 2 adds 4:0x26 = 0x7D, 0 gives the measured hold 0x0C and
    0xFD again. It writes that RAM-only knob and sets it back, whatever happens; it keys nothing. Whether the receiver
    stays on channel in the maker's state is bring-up stage 9.1's test, with a carrier."""
    try:
        start = radio.link.chip_reg(BOARD_CHIP, REF_CODING)
    except DebugLinkError as e:
        if e.status == ST_BAD_ARG:
            skip("not_built", "firmware without chip_reg 3:5 (older than the reference coding knob)")
        raise
    if fields(board_view(radio)["hrc"])[0] != "1":
        skip("no_hw", "the HR_C6000 did not answer: no receive set-up to apply the knob")

    def r25():
        return fields(radio.view(["board"]).get("board.hrc", ""))[1].get("r25")

    def adc():
        return radio.link.chip_reg(HRC_CHIP, 0x26, page=HRC_PAGE)

    def until(cond, what, timeout=10.0):
        # taken at the radio task's next status poll; a receiver asleep (FM battery saving) takes it when it wakes
        end = time.monotonic() + timeout
        while not cond():
            assert time.monotonic() < end, f"timed out after {timeout} s waiting for {what}"
            time.sleep(0.2)

    try:
        assert radio.link.chip_reg(BOARD_CHIP, REF_CODING, 1) == 1
        until(lambda: r25() == "0x0e", "board.hrc r25=0x0e under 3:5 = 1")
        assert adc() == 0xFD, f"4:0x26 0x{adc():02x} under 3:5 = 1: the driver's 0xFD expected"
        assert radio.link.chip_reg(BOARD_CHIP, REF_CODING, 2) == 2
        until(lambda: adc() == 0x7D, "4:0x26 = 0x7D under 3:5 = 2")
        assert r25() == "0x0e"
        assert radio.link.chip_reg(BOARD_CHIP, REF_CODING, 0) == 0
        until(lambda: r25() == "0x0c", "board.hrc r25=0x0c with 3:5 back at 0")
        until(lambda: adc() == 0xFD, "4:0x26 = 0xFD with 3:5 back at 0")
    finally:
        radio.link.chip_reg(BOARD_CHIP, REF_CODING, start)
    if start != 0:
        print(f"chip_reg 3:5 was {start} before the test (a bench test in progress?) and is set back to it")


@pytest.mark.feature("fm", optional=True)
def test_receive_calibration_sources(radio):
    """What receive takes from the maker's table (hardware-cs7000p.md 13.1, 15): with no table in use every value is
    today's, a trim A past the receive bound leaves the level at today's, and the modem holds the level and the 4:0x04
    that board.ref gives. Read only: it writes no knob and keys nothing. Whether the table's values receive better is
    bring-up stage 9.1's A/B with chip_reg 3:0 and 3:4."""
    b = board_view(radio)
    if "ref" not in b or "rx_cal" not in b:
        skip("not_built", "firmware without the receive calibration keys (older than the table's receive use)")
    _, t = fields(b["cal_table"])
    coding, r = fields(b["ref"])
    tune, c = fields(b["rx_cal"])
    print(f"ref {b['ref']}; rx_cal {b['rx_cal']}; cal_table src={t['src']}")
    assert coding in ("hold", "maker", "maker7d"), b["ref"]
    assert r["src"] in ("maker", "default", "bench") and tune in ("maker", "default", "bench"), b
    assert c["rssi"] in ("maker", "sheet", "bench") and c["vco"] in ("low", "high"), b["rx_cal"]
    if t["src"] == "none":
        assert r["src"] != "maker" and tune != "maker" and c["rssi"] != "maker", b
    if r["src"] == "maker":
        assert r["why"] == "none" and abs(int(r["d_a"])) <= REF_RX_BOUND, b["ref"]
    if r["why"] == "rx_bound":
        assert r["src"] == "default" and abs(int(r["d_a"])) > REF_RX_BOUND, b["ref"]
    # The IF chip's words (chip_reg 3:7) and the maker's squelch thresholds, shown only. The reader checked their
    # order at each point; an extension past the end points need not keep it, so only the codes' range is checked
    if "ifw" in c:
        assert c["ifw"] in ("port", "maker"), b["rx_cal"]
    if "sq_maker" in b:
        state, q = fields(b["sq_maker"])
        assert state in ("ok", "none"), b["sq_maker"]
        if t["src"] == "none":
            assert state == "none" and q["why"] == "table", b["sq_maker"]
        if state == "ok":
            assert all(0 <= int(q[k]) <= 255 for k in ("o1", "c1", "o9", "c9")), b["sq_maker"]
        else:
            assert q["why"] in ("table", "group", "band", "switch", "math"), b["sq_maker"]
    # chip_reg 3:4 reads 0x100 while no bench offset is set, and the level is the bench's while one is
    assert (radio.link.chip_reg(BOARD_CHIP, REF_OFFSET) == REF_RELEASE) == (r["src"] != "bench"), b["ref"]
    if coding != "hold" or "ask" in r or fields(b["hrc"])[0] != "1":
        skip("state", "the modem is not up, or chip_reg 3:5 holds or asks for the maker's coding: "
             "no held level to compare")
    lo = radio.link.chip_reg(HRC_CHIP, 0x47, page=HRC_PAGE)
    hi = radio.link.chip_reg(HRC_CHIP, 0x48, page=HRC_PAGE)
    r04 = radio.link.chip_reg(HRC_CHIP, 0x04, page=HRC_PAGE)
    assert ((hi & 0x03) << 8 | lo) == int(r["bias"], 16), f"4:0x47/0x48 0x{hi:02x}{lo:02x}, the view's {b['ref']}"
    assert r04 == int(r["r04"], 16), f"4:0x04 0x{r04:02x}, the view's {b['ref']}"

@pytest.mark.feature("beep", optional=True)
def test_i2s_direction(radio):
    """The I2S block's data direction as probed (bring-up question 8): ok or swapped, and then the stream may run.
    busy: something else drives the pad. none: neither sense drove it. unknown: the chip's I2S clock was not seen."""
    b = board_view(radio)
    if "i2s" not in b:
        skip("not_built", "firmware without DMR: no PCM stream, no probe")
    running, f = fields(b["i2s"])
    print(f"i2s {b['i2s']}")
    if f.get("dir") == "unknown" and running == "0":
        skip("state", "no audio has started since the reset: the probe has not run (play a beep first)")
    assert f.get("dir") in ("ok", "swapped"), f"I2S direction {f.get('dir')}: no beeps and no PCM path ({b['i2s']})"


@pytest.mark.feature("gps", optional=True)
def test_gps_rate(radio):
    """The rate the module answered at, of the two the port listens at. 0: not heard."""
    b = board_view(radio)
    assert int(b["gps_baud"]) in (0, 9600, 115200), b["gps_baud"]
    if int(b["gps_baud"]) == 0:
        skip("state", "the GPS module has not been heard (yet): no rate to record")
    print(f"GPS at {b['gps_baud']} baud")
