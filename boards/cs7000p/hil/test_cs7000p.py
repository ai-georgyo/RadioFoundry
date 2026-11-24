# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""CS7000 M17 PLUS bring-up checks (docs/testing/cs7000p-bringup.md), radio only, never transmits. They read the debug
link's `board` view section (docs/spec/hardware-cs7000p.md 15), which only this radio has: the clock that started,
the chip's revision and ROM bootloader, the knob and the supply hold, the output expander and the shared DAC, the LCD
board check and the panel's ID, the data flash and its reserved sector, the calibration state, the synthesizer, the
modem and its I2S direction probe. The port was written from the schematic, so most of these are the first evidence
that a line or a polarity is right. The display, the build features and the storage region's generic checks are
tests/hil/test_health.py's (from board.toml [hil]). A board's own test: skipped other_board on any other radio
(tests/hil/hil_plugin.py). Their feature markers are optional: they record bring-up, they gate nothing."""
import pytest

from rfoundry_hil import skip

KEYS = ["clock", "mcu_rev", "rom_loader", "rdp", "exp", "knob", "hold", "pow_det_mv", "vbat_mv", "vdda_mv", "vol_raw",
        "vox_raw", "lcd_board", "panel", "keys_raw", "chsw_raw", "chsw", "phone_det", "bt_strap", "flash", "store",
        "cal", "tx_refused", "dac", "lo_side", "pll", "ak", "rssi_raw", "noise_raw", "qt_raw", "rx_tune",
        "ref_offset", "pc8_duty", "hrc", "gps_baud", "adc_restarts"]

# exp_cs7000p.h
EXP_APC_LOOP = 1 << 3
EXP_TX_RAIL = 1 << 9
EXP_POWER_HOLD = 1 << 20
EXP_TX_BITS = EXP_APC_LOOP | EXP_TX_RAIL


def board_view(r):
    v = r.view(["board"])
    return {k[len("board."):]: val for k, val in v.items() if k.startswith("board.")}


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
    """Until the tables are filled from bench measurements the radio refuses every transmission."""
    b = board_view(radio)
    assert b["cal"] in ("none", "model"), b["cal"]
    if b["cal"] == "none":
        print("cal=none: transmit is refused (docs/testing/power-calibration.md fills the tables)")


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
