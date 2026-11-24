# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""M17 on the radio (docs/spec/m17.md, the radio core and the voice path), read through the debug link's m17 sections
and driven by RADIO op 44 (debuglink.md 3.7, 3.8). Every test skips without a radio, and on a radio built without M17.

Nothing is sent:

test_m17_view_keys: the five sections (m17, m17_stats, m17_data, m17_voice, heard) answer with the keys the build's
switches give, addresses as text, and no key that could carry a secret (debuglink.md: only the key's state and
fingerprint).

test_m17_mode_switch: the channel on view becomes M17 by a temporary mode change (op 31 mode 4); the m17 section must
show the engine running (`m17.on=1`, no new `m17.c.begin_fails`). Undoing the change brings the channel's own mode
back with the engine stopped.

test_m17_settings: the M17 settings (op 44 sub 0, RAM only) round trip through `m17.set.*` and are put back.

test_m17_time: a host time (op 44 sub 6) becomes the UTC source (`time.src=host`, `time.age_s` small, `time.unix`
within a second or two of this host's), and 0 forgets it again.

test_m17_codec_bench[3200|1600]: M17 running, the codec bench's encode phase (op 44 sub 4) then its decode phase, a
couple of seconds each, on the radio's CPU: the longest block `m17v.enc_us_max` <= ENC_US_MAX and `m17v.dec_us_max` <=
DEC_US_MAX (codec2.md 18.6 and 17.6: 6 and 4 ms of each 20 ms block). A build whose clock can't carry Codec 2 leaves M17
out (boards/db25d at the loader's clock), so it skips as not built.

test_m17_sig_bench (m17_sig): M17 running, the signature bench (op 44 sub 3) signs and verifies m17.md 7.5's vector
(`m17.bench.ok=ok`); `m17.sign_us` and `m17.verify_us` within SIGN_MS for the chip (CK803S at 168 MHz 90 ms, Cortex-M4
80 ms).

test_m17_entropy (m17_aes): the random pool AES nonces come from (crypto.md 8) seeded within ENTROPY_WAIT_S:
`crypto.entropy` pool or trng (not none or weak), a pool with ENTROPY_READY_BITS credited, `crypto.nonce_ok=1` (no AES
key-up refused for want of a new nonce).

test_m17_stored_channel (opt-in: --allow-codeplug-write; nothing is sent): the channel on view is made a stored M17
channel over CPS (m17-codeplug.md 2): its record the safe stand-in (DMR, receive only), a tag of kind 6 with no system
and receive only, and the parameter block: squelch any, Codec 2 1600, CAN STORED_CAN, TX CAN STORED_TX_CAN, the
destination STORED_DST. The radio must run M17 on it (`ch.kind=6`, `mode=m17`, `m17.on=1`) with the channel's
parameters, not the settings' (`m17.run.*`), and drop it once the backup is written back.

The radio transmits (opt-in: --allow-rf --allow-service-stop --m17-mhz MHz, the bench's receiver):

test_m17_tx_off_air: the VFO on --m17-mhz in M17 at the lowest power keys for PTT_S; the bench's 4FSK receiver slices
the over and tools/bench/m17tool.py decodes it: one LSF from the radio's callsign (`m17.call`) to TEST_DST with
TEST_CAN, at least as many data frames as the over's length gives less MISSING_FRAMES, numbered from 0 without a gap,
the end of transmission; on the radio `m17.c.tx_calls` one more, no `m17.c.tx_underruns`. Unsigned the last data
frame has the end bit. Signed (the radio-wide record's Sign M17 calls with a usable key: `m17.tx_sign=1`) the LSF
has TYPE's signed bit and the data frames are followed by the five tail frames 7FFB-7FFE and FFFF, the end bit on
the last (m17.md 7.1; the jump to 7FFB is no gap). Skipped where the radio has no M17 callsign (the codeplug's
radio-wide record, or a radio name that is one), its air interface can't transmit M17, or the first PTT is refused
for signing without a usable key (`m17.deny` SIG_NO_KEY or SIG_KEY_BAD: nothing is sent).

test_m17_audio_tx_60s (opt-in: --allow-rf --m17-mhz; the radio transmits, nothing receives): six PTT_LONG_S overs in
M17 on the VFO; the PCM stream's `audio.late` and `audio.dropped` stay where they were, and the encoder ran
(`m17v.tx_encoded`). Skipped as test_m17_tx_off_air for a refused first PTT.

test_m17_enc_tx_off_air[scrambler|aes] (m17_scrambler, m17_aes; opt-in: --allow-rf --allow-codeplug-write --m17-mhz,
the bench's receiver): the channel on view made a stored M17 channel on --m17-mhz that encrypts, its fresh key in an
M17 system of its own (m17-codeplug.md 2, 6), `m17.tx_enc=1` with that type; two PTT_S overs, each decoded off air
as test_m17_tx_off_air's with TYPE's encryption and subtype the channel's, no `m17.c.nonce_refused`; AES:
`crypto.nonce_ok=1` after both. The codeplug is put back.

The bench transmits, the radio receives (opt-in: --allow-rf --allow-service-stop --m17-mhz MHz, the bench's
transmitter):

test_m17_rx_from_bench: the VFO on --m17-mhz in M17, squelch any, while the bench plays one voice call m17tool writes
(TEST_SRC to TEST_DST, CAN TEST_CAN, a 440 Hz tone in Codec 2 3200): one reception more (`m17.c.rx_calls`) from that
callsign on that CAN, its voice played (`m17.played`) and decoded to sound (`m17v.rx_peak` > 0).

test_m17_heard_list (heard_list): the same call; the heard list's newest entry is TEST_SRC (`heard.0`).

test_m17_sms_rx (m17_packet): an SMS from TEST_SRC to the radio's callsign (to ALL when it has none): `m17.c.sms_rx`
one more, `sms.unread` at least one.

test_m17_bert_rx (m17_bert): the BER test mode on (op 44 sub 5), the receiver's counters started over (sub 1, 2), the
bench plays BERT_FRAMES BERT frames: at least BERT_MIN_FRAMES counted, the receiver locked, `m17b.ber_ppm` <=
BER_PPM_MAX (the engine's host bound, 1e-3). The mode is put back.

test_m17_audio_rx_60s: the bench repeats the voice call for about RX_LONG_S; the PCM stream's `audio.late` and
`audio.dropped` stay where they were, and calls played.

test_m17_enc_rx_no_key[scrambler|aes] (m17_scrambler, m17_aes): the VFO, which has no traffic key, hears a call
m17tool encrypts with a fresh key: received (`m17.c.rx_calls` one more), `m17.why` scr or enc, `m17.enc` its type,
`m17.c.enc_skipped` up, nothing played (`m17v.rx_calls`).

test_m17_enc_rx[scrambler|aes] (m17_scrambler, m17_aes; also --allow-codeplug-write): a stored M17 channel on
--m17-mhz with the key, receive only, hears the call encrypted with it: decrypted and played (`m17.played`,
`m17v.rx_calls`, `m17v.rx_peak` > 0), `m17.enc` its type and subtype, no `enc_skipped`. The codeplug is put back.
"""
import os
import tempfile
import time

import pytest

from rfoundry_debug import CHIP_HRC7000, heard_entry, m17_addr
from rfoundry_hil import as_int, skip
from hil_config import rig

AIR_FIT_RX, AIR_FIT_TX = 0x1, 0x2
SYM_RATE = 4800
FRAME_SYMS = 192
DEV_HZ = 2400
FSK4_HEX_MAX = 8192     # symbols the bench takes in the request itself (fsk4_bench.py)
LEAD_SYMS = 480         # preamble ahead of the bench's calls: a receiver's squelch and the provider's lock
PTT_S = 3.0
PTT_LONG_S = 10.0       # the longest PTT one request holds (rfoundry_debug.py INJECT_MAX_S)
RX_LONG_S = 60.0
MISSING_FRAMES = 4      # the bench receiver finds the carrier a frame or two late; the release frame ends it
ENC_US_MAX = 6000       # Codec 2 encode, one 20 ms block (docs/spec/codec2.md 18.6)
DEC_US_MAX = 4000
SIGN_MS = {CHIP_HRC7000: 90}    # P-256 sign and verify: CK803S at 168 MHz; any other chip, Cortex-M4
SIGN_MS_M4 = 80
BERT_FRAMES = 38        # what fits FSK4_HEX_MAX with the lead
BERT_MIN_FRAMES = 30
BER_PPM_MAX = 1000
TEST_SRC = "Q0TST"      # a made-up callsign (Q is no country's prefix)
ENTROPY_READY_BITS = 256    # core/crypto/entropy.h: credited before the pool counts as seeded
ENTROPY_WAIT_S = 10.0
M17_TYPE_SIGNED = 0x0800
SIG_TAIL_FN = (0x7FFB, 0x7FFC, 0x7FFD, 0x7FFE, 0xFFFF)     # m17.md 7.1, as sent (the last with the end bit)
TXDENY_SIG = {29: "SIG_NO_KEY", 30: "SIG_KEY_BAD"}      # tx_deny_t (core/radio/radio.h): Sign M17 calls without a key
TEST_DST = "ALL"
TEST_CAN = 7
STORED_CAN, STORED_TX_CAN, STORED_DST = 5, 9, "AB1CD"
# Never the last word of a key (m17.c.nonce_refused and crypto.nonce_ok are counts, not a nonce), and no SMS text
SECRET_WORDS = ("d", "k", "key_d", "seed", "nonce", "drbg", "pool", "round_key", "q")


def sections(radio, *names):
    return radio.view(list(names))


def m17_view(radio):
    """The m17 section, skipping where M17 isn't built or the radio's air interface carries none of it"""
    radio.go_home()
    if radio.core().get("hotspot") == "1":
        skip("state", "the radio is in hotspot mode")
    v = radio.view(["m17"])
    if v.get("m17.built") != "1":
        skip("not_built", "built without CONFIG_M17")
    if (as_int(v.get("m17.fit")) or 0) & (AIR_FIT_RX | AIR_FIT_TX) == 0:
        skip("no_hw", "the radio's air interface carries no M17 (m17.fit)")
    return v


def addr_text_ok(text):
    try:
        return m17_addr(text) > 0
    except ValueError:
        return False


def need_feature(radio, switch):
    if switch not in radio.features():
        skip("not_built", f"built without CONFIG_{switch}")


def settings_of(v):
    return {"can": int(v["m17.set.can"]), "tx_can": int(v["m17.set.tx_can"]), "squelch": int(v["m17.set.squelch"]),
            "mode1600": v.get("m17.set.mode1600") == "1", "dst": m17_addr(v["m17.set.dst"])}


class M17On:
    """M17 running on the channel on view for the block (a temporary mode change), the channel's mode back after"""

    def __init__(self, radio):
        self.radio = radio

    def __enter__(self):
        r = self.radio
        self.snap = r.snapshot()
        self.mode0 = r.core().get("mode")
        if self.mode0 != "m17":
            r.link.dmr_fields(mode="m17")
        try:
            return r.wait_for(lambda x: x.get("mode") == "m17" and x.get("m17.on") == "1", 3.0,
                              sections=("core", "m17"), what="M17 running on the channel")
        except AssertionError:
            v = r.view(["core", "m17"])
            if v.get("mode") != "m17":
                self.__exit__(None, None, None)
                skip("state", f"the radio kept mode {v.get('mode')} (a listen-only band, or the VFO without both "
                              "modes)")
            raise

    def __exit__(self, *exc):
        r = self.radio
        if self.mode0 != "m17":
            if self.snap["vfo"] == "1":     # the VFO's mode is its own: restore() only drops a channel's mode for now
                r.link.dmr_fields(mode=self.mode0)
            r.restore(self.snap)
        return False


def vfo_m17(radio, pytestconfig, need):
    """What a test on the VFO needs: --m17-mhz and an air interface that does need (AIR_FIT_RX or _TX) for M17.
    Returns (the frequency in MHz, the settings to put back)"""
    mhz = rig(pytestconfig, "m17-mhz")
    v = m17_view(radio)
    if (as_int(v.get("m17.fit")) or 0) & need == 0:
        skip("no_hw", f"the radio's air interface can't {'transmit' if need == AIR_FIT_TX else 'receive'} M17")
    return mhz, settings_of(v)


def enter_vfo_m17(radio, mhz):
    """Inside radio.vfo_session(): the VFO on mhz in M17 with the test settings (squelch any, TEST_CAN both ways,
    TEST_DST). Returns the core and m17 sections"""
    hz = round(mhz * 1e6)
    L = radio.link
    L.vfo_set(hz)
    radio.wait_for(lambda x: x.get("rx_hz") == str(hz), what="VFO tuned")
    L.m17_settings(can=TEST_CAN, tx_can=TEST_CAN, squelch="any", mode1600=False, dst=TEST_DST)
    if radio.core().get("mode") != "m17":
        L.dmr_fields(mode="m17")
    v = radio.wait_for(lambda x: x.get("mode") == "m17" and x.get("m17.on") == "1" and
                       x.get("m17.set.can") == str(TEST_CAN), 3.0, sections=("core", "m17"), what="M17 on the VFO")
    assert v.get("tx_hz") == str(hz), f"the VFO transmits on {v.get('tx_hz')}, not {hz}"
    return v


def counts(radio, *keys):
    v = radio.view(["m17", "m17_stats", "m17_data", "m17_voice", "heard", "audio"])
    return {k: as_int(v.get(k)) or 0 for k in keys}, v


# --- Nothing sent ---

@pytest.mark.feature("m17")
def test_m17_view_keys(radio):
    m17_view(radio)
    v = radio.view(["m17", "m17_stats", "m17_data", "m17_voice", "heard"])
    feats = radio.features()
    want = ["m17.built", "m17.fit", "m17.on", "m17.rx", "m17.played", "m17.late", "m17.lsf", "m17.packet", "m17.bert",
            "m17.why", "m17.tx", "m17.tx_phase", "m17.src", "m17.dst", "m17.orig", "m17.via", "m17.talk", "m17.call",
            "m17.can", "m17.type", "m17.voice", "m17.enc", "m17.text", "m17.gnss", "m17.ext1", "m17.ext2", "m17.sig",
            "m17.tot_s", "m17.set.can", "m17.set.tx_can", "m17.set.squelch", "m17.set.mode1600", "m17.set.dst",
            "m17.c.begins", "m17.c.rx_calls", "m17.c.tx_syms", "m17.c.sig_tail", "time.src", "time.age_s",
            "m17v.rx_in", "m17v.rx_peak", "m17v.enc_us_max", "m17v.dec_us_max", "m17v.lent", "heard.built"]
    if "M17_SIG" in feats:
        want += ["m17.key", "m17.key_fp", "m17.bench.ok", "m17.sign_us", "m17.verify_us", "m17.c.sig_valid"]
    if "M17_PACKET" in feats:
        want += ["sms.n", "sms.unread", "sms.result", "m17.c.sms_rx"]
    if "M17_BERT" in feats:
        want += ["m17b.state", "m17b.frames", "m17b.bits", "m17b.errs", "m17b.ber_ppm", "m17b.locks", "m17b.losses",
                 "m17b.tx_frames"]
    if "M17_AES" in feats:
        want += ["crypto.entropy", "crypto.ent_bits", "crypto.nonce_ok"]
    if "HEARD_LIST" in feats:
        want += ["heard.n"]
    missing = [k for k in want if k not in v]
    assert not missing, f"keys missing: {missing}"
    assert "truncated" not in v, "a section was cut off"
    secret = [k for k in v if k.split(".")[-1] in SECRET_WORDS or k.startswith("sms.text")]
    assert not secret, f"keys that could carry a secret: {secret}"
    assert v["m17.text"] in ("0", "1") and v["m17.gnss"] in ("0", "1"), "META heard: a flag, never its text"
    if "m17.key_fp" in v:
        assert len(v["m17.key_fp"]) == 16 and all(c in "0123456789ABCDEF" for c in v["m17.key_fp"]), v["m17.key_fp"]
        assert v["m17.key"] in ("none", "unchecked", "ok", "damaged"), v["m17.key"]
    for k in ("m17.src", "m17.dst", "m17.talk", "m17.call", "m17.set.dst"):
        assert v[k] == "-" or addr_text_ok(v[k]), f"{k}={v[k]}: not an address as text"
    print(f"\nm17: fit {v['m17.fit']}, call {v['m17.call']}, talk {v['m17.talk']}, time {v['time.src']}")


@pytest.mark.feature("m17")
def test_m17_mode_switch(radio):
    v = m17_view(radio)
    if radio.core().get("mode") == "m17":
        skip("state", "the channel is M17 already")
    fails0 = as_int(radio.view(["m17_stats"]).get("m17.c.begin_fails")) or 0
    mode0 = radio.core().get("mode")
    with M17On(radio) as v:
        assert (as_int(radio.view(["m17_stats"]).get("m17.c.begin_fails")) or 0) == fails0, "the engine did not start"
        assert radio.core().get("state") in ("idle", "rx")
        print(f"\nm17: CAN {v.get('m17.run.can')}/{v.get('m17.run.tx_can')}, fit {v.get('m17.fit')}")
    radio.wait_for(lambda x: x.get("mode") == mode0 and x.get("m17.on") == "0", 3.0, sections=("core", "m17"),
                   what=f"mode {mode0} back, M17 stopped")
    if mode0 == "dmr" and "DMR" in radio.features():
        radio.wait_for(lambda x: x.get("dmr.active") == "1", 3.0, sections=("dmr",), what="the DMR stack back")


@pytest.mark.feature("m17")
def test_m17_settings(radio):
    old = settings_of(m17_view(radio))
    try:
        radio.link.m17_settings_update(can=11, tx_can=12, squelch="addressed", mode1600=True, dst="AB1CD")
        v = radio.wait_for(lambda x: x.get("m17.set.can") == "11", 2.0, sections=("m17",), what="settings taken")
        assert v.get("m17.set.tx_can") == "12" and v.get("m17.set.squelch") == "2", v
        assert v.get("m17.set.mode1600") == "1" and v.get("m17.set.dst") == "AB1CD", v
        radio.link.m17_settings_update(dst="ALL")
        radio.wait_for(lambda x: x.get("m17.set.dst") == "ALL", 2.0, sections=("m17",), what="broadcast taken")
    finally:
        radio.link.m17_settings(**old)
    radio.wait_for(lambda x: x.get("m17.set.can") == str(old["can"]), 2.0, sections=("m17",), what="settings put back")


@pytest.mark.feature("m17")
def test_m17_time(radio):
    m17_view(radio)
    t0 = radio.view(["m17_stats"])
    try:
        radio.link.m17_time()
        v = radio.wait_for(lambda x: x.get("time.src") == "host", 2.0, sections=("m17_stats",), what="the host time")
        assert (as_int(v.get("time.age_s")) or 0) <= 2, v.get("time.age_s")
        assert abs((as_int(v.get("time.unix")) or 0) - int(time.time())) <= 3, f"time.unix {v.get('time.unix')}"
    finally:
        radio.link.m17_time(0)
    v = radio.view(["m17_stats"])
    assert v.get("time.src") in ("none", "gnss"), \
        f"the host time stayed: {v.get('time.src')} (before: {t0.get('time.src')})"


@pytest.mark.feature("m17")
@pytest.mark.parametrize("mode", ["3200", "1600"])
def test_m17_codec_bench(radio, mode):
    m17_view(radio)
    with M17On(radio):
        L = radio.link
        try:
            res = {}
            for phase in ("enc", "dec"):
                radio.reset_counters()
                L.m17_bench_codec(phase, mode == "1600")
                time.sleep(2.0)
                v = radio.view(["m17_voice"])
                res[phase] = (as_int(v.get(f"m17v.{phase}_us_max")) or 0, as_int(v.get(f"m17v.{phase}_blocks")) or 0)
        finally:
            L.m17_bench_codec("off")
    print(f"\nCodec 2 {mode}: encode {res['enc'][0]} us at most over {res['enc'][1]} blocks, decode {res['dec'][0]} us "
          f"over {res['dec'][1]}")
    assert res["enc"][1] >= 50 and res["dec"][1] >= 50, f"the bench ran too few blocks: {res}"
    if res["enc"][0] == 0 and res["dec"][0] == 0:
        skip("not_built", "the voice path counts blocks but doesn't time them (no microsecond clock given)")
    assert res["enc"][0] <= ENC_US_MAX, f"encode {res['enc'][0]} us a block, budget {ENC_US_MAX}"
    assert res["dec"][0] <= DEC_US_MAX, f"decode {res['dec'][0]} us a block, budget {DEC_US_MAX}"


@pytest.mark.feature("m17")
@pytest.mark.feature("m17_sig")
def test_m17_sig_bench(radio):
    m17_view(radio)
    need_feature(radio, "M17_SIG")
    budget = SIGN_MS.get(radio.link.chip()["family"], SIGN_MS_M4)
    with M17On(radio):
        radio.link.m17_bench_sig()
        v = radio.wait_for(lambda x: x.get("m17.bench.ok") not in ("running", "none"), 5.0, sections=("m17_stats",),
                           what="the signature bench's outcome")
    sign, verify = as_int(v.get("m17.sign_us")) or 0, as_int(v.get("m17.verify_us")) or 0
    print(f"\nP-256: sign {sign / 1000:.1f} ms, verify {verify / 1000:.1f} ms (budget {budget} ms)")
    assert v.get("m17.bench.ok") == "ok", f"the bench: {v.get('m17.bench.ok')}"
    assert 0 < sign <= budget * 1000 and 0 < verify <= budget * 1000


@pytest.mark.feature("m17")
@pytest.mark.feature("m17_aes")
def test_m17_entropy(radio):
    m17_view(radio)
    need_feature(radio, "M17_AES")
    # The pool fills from the board's sources at boot (entropy_boot.c): a little after it at most
    v = radio.wait_for(lambda x: x.get("crypto.entropy") in ("pool", "trng"), ENTROPY_WAIT_S,
                       sections=("m17_stats",), what="the random pool seeded")
    bits = as_int(v.get("crypto.ent_bits")) or 0
    print(f"\nentropy: {v.get('crypto.entropy')}, {bits} bits credited, nonce_ok {v.get('crypto.nonce_ok')}")
    if v.get("crypto.entropy") == "pool":
        assert bits >= ENTROPY_READY_BITS, f"{bits} bits credited, a seeded pool has {ENTROPY_READY_BITS} (crypto.md 8.1)"
    assert v.get("crypto.nonce_ok") == "1", "an AES key-up was refused for want of a new nonce"


# --- A stored M17 channel (m17-codeplug.md, channel-modes.md 2) ---

KIND_M17 = 6
KEY_CH_TAG = 0x0400         # codeplug-settings.md 3: channel tags, 0x0400 + channel - 1


def make_m17(img, number):
    """Channel number of img made a stored M17 channel, receive only: the stand-in record, then its tag"""
    import cps_client
    from test_nxdn import new_area
    from test_p25 import area_with, name_check
    _, a = cps_client.channel_addr(number)
    img[a + 0x18] = 1                   # DMR
    img[a + 0x33] |= 0x04               # receive only: the stand-in, whatever the tag allows
    tag = bytes([KIND_M17, 0xFF]) + name_check(img[a:a + 16]).to_bytes(2, "little") + bytes([0])
    block = bytes([0x01 | 0x04, STORED_CAN, 0x80 | STORED_TX_CAN]) + m17_addr(STORED_DST).to_bytes(6, "big") + \
        bytes([0, 0, 0])                # squelch any, 1600; CAN, TX CAN; destination; in the clear, key 0
    lo, hi = cps_client.SETTINGS_AREA
    area = img[lo:hi]
    if cps_client.area_state(area) != "valid":
        area = new_area(hi - lo)
    img[lo:hi] = area_with(area, KEY_CH_TAG + number - 1, tag + block)


@pytest.mark.feature("m17")
@pytest.mark.codeplug_write
def test_m17_stored_channel(radio, backup_dir):
    m17_view(radio)
    import cps_client
    from test_cps import codeplug_edited, pick_channel, read_backup
    regions = cps_client.REGIONS["radio"]      # with the settings area, where the tag goes
    probe, _ = read_backup(radio, backup_dir, "m17probe", regions)
    lo, hi = cps_client.SETTINGS_AREA
    if cps_client.area_state(probe[lo:hi]) != "valid":
        skip("state", "the codeplug settings area isn't valid: the restore couldn't take the tag back")
    core = radio.core()
    number = pick_channel(radio, probe)
    if core.get("vfo") == "1" or number != as_int(core.get("channel_number"), 0):
        skip("state", "the radio isn't on a stored channel")
    if core.get("am") == "1" or core.get("bcast") == "1":
        skip("state", "the channel is in a listen-only band: no M17 there")
    with codeplug_edited(radio, backup_dir, lambda img: make_m17(img, number), "m17", regions):
        v = radio.wait_for(lambda x: x.get("ch.kind") == str(KIND_M17) and x.get("m17.on") == "1", 5.0,
                           sections=("core", "channel", "m17"), what="the stored M17 channel running")
        assert v.get("mode") == "m17", v
        assert v.get("m17.run.can") == str(STORED_CAN) and v.get("m17.run.tx_can") == str(STORED_TX_CAN), v
        assert v.get("m17.run.squelch") == "1" and v.get("m17.run.mode1600") == "1", v
        assert v.get("m17.run.dst") == STORED_DST and v.get("m17.talk") == STORED_DST, v
    radio.wait_for(lambda x: x.get("ch.kind") == "0" and x.get("m17.on") != "1", 5.0,
                   sections=("core", "channel", "m17"), what="the channel back as it was, M17 stopped")


# --- The radio transmits ---

def decode_over(job, done=None):
    """m17tool's frames and summary of an fsk4 rx job's records; done (its last record) says why there are none"""
    import m17tool
    recs = [r for r in job.records if any(isinstance(r.get(k), (str, dict)) for k in ("dibits", "syms", "symbols"))]
    if not recs:
        what = (done or {}).get("summary")
        if what and what.get("carrier") is False and not what.get("keyings"):
            raise AssertionError(f"the bench board heard no carrier while the radio keyed: {what}")
        skip("bench", f"the bench board sent no symbols (the fsk4 rx op's records): {what}")
    try:
        return m17tool.decode_records(job.records)
    except SystemExit as e:
        pytest.fail(f"m17tool: {e}")


@pytest.mark.feature("m17")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
@pytest.mark.needs_switch("VFO")
def test_m17_tx_off_air(radio, sdr_bench, pytestconfig):
    import m17tool
    mhz, old = vfo_m17(radio, pytestconfig, AIR_FIT_TX)
    if not sdr_bench.can_fsk4_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no 4FSK receive")
    with radio.vfo_session():
        try:
            v0 = enter_vfo_m17(radio, mhz)
            need_callsign(v0)
            c0, _ = counts(radio, "m17.c.tx_calls", "m17.c.tx_underruns", "m17v.tx_frames")
            radio.lowest_power()
            with sdr_bench.lease():
                job = sdr_bench.fsk4_rx_start(mhz, PTT_S + 4, baud=SYM_RATE, shape="rrc", sync=m17tool.SYNC_LSF,
                                              sync_len=8, nominal_dev_hz=DEV_HZ)
                time.sleep(0.5)
                radio.ptt_for(PTT_S)
                radio.wait_for(lambda x: x.get("state") != "tx", timeout=3, what="TX ended")
                done = job.wait(PTT_S + 20)
            c1, v1 = counts(radio, "m17.c.tx_calls", "m17.c.tx_underruns", "m17v.tx_frames")
            refused_skip(v1, c0, c1)
        finally:
            radio.link.m17_settings(**old)
    lines, s = decode_over(job, done)
    check_over(lines, s, v0, c0, c1, PTT_S)


def need_callsign(v):
    """Skip where PTT is refused for want of an M17 callsign (the codeplug's radio-wide record, or a radio name that
    is one)"""
    if v.get("m17.call") in (None, "-") or v.get("m17.no_call") == "1" or v.get("m17.call_bad") == "1":
        skip("state", "no M17 callsign on the radio (the codeplug's radio-wide M17 record, or a radio name that is "
                      "one): PTT is refused")


def refused_skip(v, c0, c1):
    """After the first PTT: skip where the radio refused it for its radio-wide signing (Sign M17 calls on without a
    usable key: m17.deny SIG_NO_KEY or SIG_KEY_BAD), a state the view can't show before a PTT (m17.deny is the last
    refusal; m17.tx_sign is set only with a usable key). Nothing was sent"""
    deny = as_int(v.get("m17.deny")) or 0
    if c1["m17.c.tx_calls"] == c0["m17.c.tx_calls"] and deny in TXDENY_SIG:
        skip("state", f"PTT refused: {TXDENY_SIG[deny]} (the radio-wide record has Sign M17 calls on)")


def stream_fns(lines):
    """The over's stream frame numbers as decoded: (data frames' FN, the signature tail's FN as sent, m17.md 7.1)"""
    fns = [x["fn"] for x in lines if x["ev"] == "stream"]
    data = [f & 0x7FFF for f in fns if (f & 0x7FFF) < SIG_TAIL_FN[0]]
    tail = [f for f in fns if (f & 0x7FFF) >= SIG_TAIL_FN[0]]
    return data, tail


def check_over(lines, s, v0, c0, c1, ptt_s, enc=0, sub=None):
    """The over decoded off air against what the radio sent (v0 its view before the PTT, c0 and c1 its counts): one
    LSF from our callsign to TEST_DST on TEST_CAN, a 3200 voice stream, enc its encryption (TYPE bits 3-4, M17_ENC_*;
    0 clear, where bits 5-6 are META's type) and sub the subtype (bits 5-6),
    data frames numbered from 0 without a gap, then the end: the end bit on the last one, or, signed (m17.tx_sign),
    the five tail frames 7FFB-7FFE and FFFF (the jump to 7FFB is no gap)"""
    signed = v0.get("m17.tx_sign") == "1"
    data, tail = stream_fns(lines)
    gaps = sum(1 for a, b in zip(data, data[1:]) if b != a + 1)
    print(f"\nm17 off air: {s.get('lsf')} LSF {s['lsfs'][:1]}, {s.get('stream')} stream frames "
          f"(data FN {data[:1]}..{data[-1:]}, gaps {gaps}, tail {[f'{f:04X}' for f in tail]}, end {s.get('end')}), "
          f"EoT {s.get('eot')}, lost {s.get('bad')}; signed {signed}; radio {c0} -> {c1}")
    assert s.get("lsf") == 1, f"{s.get('lsf')} LSFs"
    lsf = s["lsfs"][0]
    assert lsf["src"] == v0["m17.call"] and lsf["dst"] == TEST_DST and lsf["can"] == TEST_CAN, lsf
    assert lsf["stream"] == 1 and lsf["data"] == 2, f"not a 3200 voice stream: {lsf}"
    assert (lsf["type"] >> 3) & 3 == enc and (sub is None or (lsf["type"] >> 5) & 3 == sub), \
        f"TYPE {lsf['type']:04X}: encryption {(lsf['type'] >> 3) & 3} subtype {(lsf['type'] >> 5) & 3}, the radio's " \
        f"{enc} {sub}"
    assert bool(lsf["type"] & M17_TYPE_SIGNED) == signed, f"TYPE {lsf['type']:04X}: signed bit, the radio signs {signed}"
    want = ptt_s * 1000 / 40
    assert len(data) >= want - MISSING_FRAMES, f"{len(data)} data frames in a {ptt_s} s over (about {want:.0f})"
    assert data[0] == 0 and gaps == 0 and s["end"] == 1, s
    if signed:
        assert tail == list(SIG_TAIL_FN), f"the signature tail: {[f'{f:04X}' for f in tail]}"
    else:
        assert not tail and s["fn_gaps"] == 0, s
    assert s["eot"] >= 1, "no end of transmission"
    assert c1["m17.c.tx_calls"] == c0["m17.c.tx_calls"] + 1, "the radio counted no M17 transmission"
    assert c1["m17.c.tx_underruns"] == c0["m17.c.tx_underruns"], "the air ran dry during the over"
    assert c1["m17v.tx_frames"] > c0["m17v.tx_frames"], "the voice path handed the engine no frames"


@pytest.mark.feature("m17")
@pytest.mark.radio_tx
@pytest.mark.needs_switch("VFO")
def test_m17_audio_tx_60s(radio, pytestconfig):
    mhz, old = vfo_m17(radio, pytestconfig, AIR_FIT_TX)
    with radio.vfo_session():
        try:
            v0 = enter_vfo_m17(radio, mhz)
            need_callsign(v0)
            keys = ("audio.late", "audio.dropped", "m17v.tx_encoded", "m17.c.tx_calls")
            c0, a = counts(radio, *keys)
            if "audio.late" not in a:
                skip("no_hw", "the board doesn't count its PCM stream's timing (hal_audio_t.stream_stats)")
            radio.lowest_power()
            for n in range(int(RX_LONG_S / PTT_LONG_S)):
                radio.ptt_for(PTT_LONG_S)
                radio.wait_for(lambda x: x.get("state") != "tx", timeout=3, what="TX ended")
                if n == 0:
                    c, v = counts(radio, *keys)
                    refused_skip(v, c0, c)
            c1, _ = counts(radio, *keys)
        finally:
            radio.link.m17_settings(**old)
    print(f"\nm17 TX {RX_LONG_S:.0f} s: {c0} -> {c1}")
    assert c1["m17.c.tx_calls"] - c0["m17.c.tx_calls"] == int(RX_LONG_S / PTT_LONG_S)
    assert c1["m17v.tx_encoded"] - c0["m17v.tx_encoded"] >= RX_LONG_S * 50 * 9 // 10, "the encoder fell behind"
    assert c1["audio.late"] == c0["audio.late"] and c1["audio.dropped"] == c0["audio.dropped"], \
        f"the PCM stream ran late: {c0} -> {c1}"


# --- The bench transmits ---

def bench_play(sdr_bench, mhz, kind, plays=None, **opt):
    """One transmission m17tool writes (kind voice, bert or sms; opt its options) from the bench's 4FSK transmitter on
    mhz, plays times when given (the fsk4 tx op's repeat: plays in all, 0.2 s unkeyed between, fsk4_bench.py);
    returns once the bench is done"""
    import m17tool
    with tempfile.TemporaryDirectory() as tmp:
        try:
            data = m17tool.gen(kind, os.path.join(tmp, "tx.dibits"), lead=LEAD_SYMS, **opt)
        except SystemExit as e:
            pytest.fail(f"m17tool: {e}")
    n = len(data) * 4
    assert n <= FSK4_HEX_MAX, f"the transmission holds {n} symbols, past {FSK4_HEX_MAX}"
    total = (n / SYM_RATE + 0.2) * (plays or 1)
    with sdr_bench.lease():
        tx = sdr_bench.tx_start("fsk4", mhz, None, max_s=total + 2, symbols=n, hex=data.hex(), shape="rrc",
                                baud=SYM_RATE, dev_hz=DEV_HZ, clip_hz=4064, repeat=plays,
                                gap_s=0.2 if plays else None)
        tx.wait(total + 30)
    time.sleep(0.6)


def voice_frames():
    return (FSK4_HEX_MAX - LEAD_SYMS - 3 * FRAME_SYMS) // FRAME_SYMS     # the preamble, the LSF, the EoT


def rx_session(radio, sdr_bench, pytestconfig, play, keys, before=None):
    """The VFO on --m17-mhz in M17 (squelch any) while play(mhz) has the bench transmit; (counts before, after, view)"""
    mhz, old = vfo_m17(radio, pytestconfig, AIR_FIT_RX)
    with radio.vfo_session():
        try:
            enter_vfo_m17(radio, mhz)
            if before is not None:
                before()
            c0, _ = counts(radio, *keys)
            play(mhz)
            c1, v = counts(radio, *keys)
        finally:
            radio.link.m17_settings(**old)
    return c0, c1, v


@pytest.mark.feature("m17")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.needs_switch("VFO")
def test_m17_rx_from_bench(radio, sdr_bench, pytestconfig):
    c0, c1, v = rx_session(radio, sdr_bench, pytestconfig, lambda mhz: bench_play(
        sdr_bench, mhz, "voice", src=TEST_SRC, dst=TEST_DST, can=TEST_CAN, frames=voice_frames(), tone=440),
        ("m17.c.rx_calls", "m17.c.stream", "m17v.rx_frames", "m17v.rx_calls"))
    print(f"\nm17 from the bench: {c0} -> {c1}; src {v.get('m17.src')} can {v.get('m17.can')} "
          f"peak {v.get('m17v.rx_peak')} why {v.get('m17.why')}")
    assert c1["m17.c.rx_calls"] == c0["m17.c.rx_calls"] + 1, "no call received"
    assert v.get("m17.src") == TEST_SRC and v.get("m17.can") == str(TEST_CAN), \
        f"the call from {v.get('m17.src')} on CAN {v.get('m17.can')}, sent {TEST_SRC} on {TEST_CAN}"
    assert c1["m17.c.stream"] - c0["m17.c.stream"] >= voice_frames() - MISSING_FRAMES, "stream frames lost"
    assert v.get("m17.played") == "1" and c1["m17v.rx_calls"] > c0["m17v.rx_calls"], "the call didn't play"
    assert (as_int(v.get("m17v.rx_peak")) or 0) > 0, "the voice decoded to silence"


@pytest.mark.feature("m17")
@pytest.mark.feature("heard_list")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.needs_switch("VFO")
def test_m17_heard_list(radio, sdr_bench, pytestconfig):
    m17_view(radio)
    need_feature(radio, "HEARD_LIST")
    c0, c1, v = rx_session(radio, sdr_bench, pytestconfig, lambda mhz: bench_play(
        sdr_bench, mhz, "voice", src=TEST_SRC, dst=TEST_DST, can=TEST_CAN, frames=12), ("heard.n", "m17.c.rx_calls"))
    print(f"\nheard: {c0['heard.n']} -> {c1['heard.n']}, newest {v.get('heard.0')}")
    assert c1["m17.c.rx_calls"] == c0["m17.c.rx_calls"] + 1, "no call received"
    h = heard_entry(v.get("heard.0"))     # seven words, an address's spaces as '_' (debuglink.md)
    assert h and h["mode"] == "m17" and h["from"] == TEST_SRC and h["to"] == TEST_DST, \
        f"the newest station heard: {v.get('heard.0')}"
    assert h["age_s"] <= 5, f"heard {h['age_s']} s ago"


@pytest.mark.feature("m17")
@pytest.mark.feature("m17_packet")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.needs_switch("VFO")
def test_m17_sms_rx(radio, sdr_bench, pytestconfig):
    m17_view(radio)
    need_feature(radio, "M17_PACKET")
    call = radio.view(["m17"]).get("m17.call")
    dst = call if call not in (None, "-") else "ALL"
    text = f"RadioFoundry HIL {int(time.time()) % 10000}"
    c0, c1, v = rx_session(radio, sdr_bench, pytestconfig, lambda mhz: bench_play(
        sdr_bench, mhz, "sms", src=TEST_SRC, dst=dst, can=TEST_CAN, text=text),
        ("m17.c.sms_rx", "m17.c.pkt_rx", "m17.c.pkt_bad", "sms.unread", "sms.n"))
    print(f"\nm17 SMS to {dst}: {c0} -> {c1}")
    assert c1["m17.c.pkt_rx"] == c0["m17.c.pkt_rx"] + 1, "no packet received"
    assert c1["m17.c.sms_rx"] == c0["m17.c.sms_rx"] + 1, "the packet wasn't taken as an SMS"
    assert c1["sms.unread"] >= 1 and c1["sms.n"] >= 1


@pytest.mark.feature("m17")
@pytest.mark.feature("m17_bert")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.needs_switch("VFO")
def test_m17_bert_rx(radio, sdr_bench, pytestconfig):
    m17_view(radio)
    need_feature(radio, "M17_BERT")
    L = radio.link

    def before():
        L.m17_ber_mode(True)
        L.m17_bert_run("reset")
        radio.wait_for(lambda x: x.get("m17.bert_mode") == "1", 2.0, sections=("m17",), what="the BER test mode")
    try:
        c0, c1, v = rx_session(radio, sdr_bench, pytestconfig,
                               lambda mhz: bench_play(sdr_bench, mhz, "bert", frames=BERT_FRAMES),
                               ("m17b.frames", "m17b.bits", "m17b.errs", "m17b.locks"), before)
    finally:
        L.m17_ber_mode(False)
    print(f"\nm17 BERT: {c0} -> {c1}; state {v.get('m17b.state')}, {v.get('m17b.ber_ppm')} ppm")
    assert c1["m17b.frames"] - c0["m17b.frames"] >= BERT_MIN_FRAMES, "BERT frames lost"
    assert c1["m17b.locks"] >= 1 and c1["m17b.bits"] > 0, "the receiver never locked"
    assert (as_int(v.get("m17b.ber_ppm")) or 0) <= BER_PPM_MAX, f"bit error rate {v.get('m17b.ber_ppm')} ppm"


@pytest.mark.feature("m17")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.needs_switch("VFO")
def test_m17_audio_rx_60s(radio, sdr_bench, pytestconfig):
    frames = voice_frames()
    plays = int(RX_LONG_S / ((LEAD_SYMS + (frames + 3) * FRAME_SYMS) / SYM_RATE + 0.2))
    c0, c1, v = rx_session(radio, sdr_bench, pytestconfig, lambda mhz: bench_play(
        sdr_bench, mhz, "voice", plays=plays, src=TEST_SRC, dst=TEST_DST, can=TEST_CAN, frames=frames),
        ("audio.late", "audio.dropped", "m17v.rx_calls", "m17.c.rx_calls"))
    if "audio.late" not in v:
        skip("no_hw", "the board doesn't count its PCM stream's timing (hal_audio_t.stream_stats)")
    print(f"\nm17 RX {RX_LONG_S:.0f} s ({plays} calls): {c0} -> {c1}")
    assert c1["m17.c.rx_calls"] - c0["m17.c.rx_calls"] >= plays * 9 // 10, "calls lost"
    assert c1["m17v.rx_calls"] > c0["m17v.rx_calls"], "nothing played"
    assert c1["audio.late"] == c0["audio.late"] and c1["audio.dropped"] == c0["audio.dropped"], \
        f"the PCM stream ran late: {c0} -> {c1}"


# --- Encryption: the scrambler and AES (m17.md 6), the key in a stored channel's system (m17-codeplug.md 6) ---

KEY_SYSTEM = 0x0100         # channel-modes.md 3: systems, 0x0100 + n
KEY_TRAFFIC = 0x1900        # ... traffic keys, 0x1900 + entry n
SYSTEM_M17 = 6
# kind: (the block's encryption and the key's algorithm id, M17_ENC_*; the switch; the feature)
ENC = {"scrambler": (1, "M17_SCRAMBLER", "m17_scrambler"), "aes": (2, "M17_AES", "m17_aes")}
ENC_KEY_ID = 1
WHY_ENC = {"scrambler": "scr", "aes": "enc"}


def enc_key(kind):
    """A fresh traffic key: a 16-bit scrambler seed (never zero) or an AES-128 key, and its subtype (m17.md 6)"""
    if kind == "scrambler":
        seed = os.urandom(2)            # two bytes, leading zeros counted: a 16-bit register
        return (seed if any(seed) else b"\x00\x01"), 1                       # M17_SCR_16
    return os.urandom(16), 0                                                # M17_AES_128


def make_m17_enc(img, number, hz, kind, key, sys_n, key_n, tx):
    """Channel number of img made a stored M17 channel on hz (simplex) that encrypts with key: an M17 system sys_n
    with transmit allowed (channel-modes.md 7.1), the key in its traffic key entry key_n (5.4, m17-codeplug.md 6), the
    tag naming the system, the block's encryption and key ID (m17-codeplug.md 2). Receive only unless tx"""
    import cps_client
    from test_p25 import area_with, bcd_hz, name_check
    alg = ENC[kind][0]
    _, a = cps_client.channel_addr(number)
    img[a + 0x18] = 1                   # DMR
    img[a + 0x33] |= 0x04               # receive only: the stand-in, whatever the tag allows
    img[a + 0x10:a + 0x14] = img[a + 0x14:a + 0x18] = bcd_hz(hz)
    lo, hi = cps_client.SETTINGS_AREA
    area = bytes(img[lo:hi])
    system = bytes([SYSTEM_M17, 0x02, 0, 0]) + bytes(8) + b"HIL M17".ljust(16, b"\0")
    area = area_with(area, KEY_SYSTEM + sys_n, system)
    area = area_with(area, KEY_TRAFFIC + key_n, bytes([sys_n, alg]) + ENC_KEY_ID.to_bytes(2, "little") + bytes([0]) +
                     key)
    block = bytes([0x01, TEST_CAN, 0x80 | TEST_CAN]) + bytes(6) + bytes([alg]) + ENC_KEY_ID.to_bytes(2, "little")
    tag = bytes([KIND_M17, sys_n]) + name_check(img[a:a + 16]).to_bytes(2, "little") + bytes([0x01 if tx else 0])
    img[lo:hi] = area_with(area, KEY_CH_TAG + number - 1, tag + block)


class EncChannel:
    """The channel on view a stored M17 channel on mhz that encrypts (kind, key) for the block, over CPS, backed up
    and put back after. Skips where the settings area isn't valid, the radio isn't on a stored channel, or no system
    or key entry number is free. Returns the core, channel and m17 sections once the channel runs with its key"""

    def __init__(self, radio, backup_dir, mhz, kind, key, tx):
        self.radio, self.backup_dir, self.hz, self.kind, self.key, self.tx = radio, backup_dir, round(mhz * 1e6), \
            kind, key, tx

    def __enter__(self):
        import cps_client
        from test_cps import codeplug_edited, pick_channel, read_backup
        from test_p25 import area_keys
        r = self.radio
        regions = cps_client.REGIONS["radio"]
        probe, _ = read_backup(r, self.backup_dir, "m17encprobe", regions)
        lo, hi = cps_client.SETTINGS_AREA
        if cps_client.area_state(probe[lo:hi]) != "valid":
            skip("state", "the codeplug settings area isn't valid: the restore couldn't take the system back")
        core = r.core()
        number = pick_channel(r, probe)
        if core.get("vfo") == "1" or number != as_int(core.get("channel_number"), 0):
            skip("state", "the radio isn't on a stored channel")
        used = set(area_keys(probe[lo:hi]))
        sys_n = next((n for n in range(255) if KEY_SYSTEM + n not in used), None)
        key_n = next((n for n in range(0x400) if KEY_TRAFFIC + n not in used), None)
        if sys_n is None or key_n is None:
            skip("state", "no free system or traffic key number in the codeplug")
        self.cm = codeplug_edited(r, self.backup_dir, lambda img: make_m17_enc(img, number, self.hz, self.kind,
                                  self.key, sys_n, key_n, self.tx), "m17enc", regions)
        self.cm.__enter__()
        try:
            return r.wait_for(lambda x: x.get("ch.kind") == str(KIND_M17) and x.get("m17.on") == "1" and
                              x.get("rx_hz") == str(self.hz), 5.0, sections=("core", "channel", "m17"),
                              what="the stored M17 channel running")
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *exc):
        self.cm.__exit__(*exc)
        self.radio.wait_for(lambda x: x.get("ch.kind") == "0" and x.get("m17.on") != "1", 5.0,
                            sections=("core", "channel", "m17"), what="the channel back as it was, M17 stopped")
        return False


def need_enc(radio, kind):
    m17_view(radio)
    need_feature(radio, ENC[kind][1])


ENC_PARAMS = [pytest.param(k, marks=pytest.mark.feature(ENC[k][2]), id=k) for k in ENC]
RX_COUNTS = ("m17.c.rx_calls", "m17.c.enc_skipped", "m17v.rx_calls", "m17v.rx_frames")


@pytest.mark.feature("m17")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.needs_switch("VFO")
@pytest.mark.parametrize("kind", ENC_PARAMS)
def test_m17_enc_rx_no_key(radio, sdr_bench, pytestconfig, kind):
    """The VFO (no traffic key) hears a call encrypted with a key it doesn't have: received, not played"""
    need_enc(radio, kind)
    key, _ = enc_key(kind)
    c0, c1, v = rx_session(radio, sdr_bench, pytestconfig, lambda mhz: bench_play(
        sdr_bench, mhz, "voice", src=TEST_SRC, dst=TEST_DST, can=TEST_CAN, frames=24, **{kind: key.hex()}), RX_COUNTS)
    print(f"\nm17 {kind} without the key: {c0} -> {c1}; why {v.get('m17.why')} enc {v.get('m17.enc')}")
    assert c1["m17.c.rx_calls"] == c0["m17.c.rx_calls"] + 1, "no call received"
    assert v.get("m17.why") == WHY_ENC[kind] and v.get("m17.played") == "0", \
        f"why {v.get('m17.why')}, played {v.get('m17.played')}: an encrypted call without its key"
    assert (as_int(v.get("m17.enc")) or 0) >> 4 == ENC[kind][0], f"m17.enc {v.get('m17.enc')}"
    assert c1["m17.c.enc_skipped"] > c0["m17.c.enc_skipped"], "not counted as skipped for its encryption"
    assert c1["m17v.rx_calls"] == c0["m17v.rx_calls"], "the voice path played it"


@pytest.mark.feature("m17")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.codeplug_write
@pytest.mark.parametrize("kind", ENC_PARAMS)
def test_m17_enc_rx(radio, sdr_bench, backup_dir, pytestconfig, kind):
    """A stored channel with the key hears a call the bench encrypts with it: decrypted and played"""
    mhz = rig(pytestconfig, "m17-mhz")
    need_enc(radio, kind)
    if (as_int(radio.view(["m17"]).get("m17.fit")) or 0) & AIR_FIT_RX == 0:
        skip("no_hw", "the radio's air interface can't receive M17")
    key, sub = enc_key(kind)
    with EncChannel(radio, backup_dir, mhz, kind, key, tx=False):
        c0, _ = counts(radio, *RX_COUNTS)
        bench_play(sdr_bench, mhz, "voice", src=TEST_SRC, dst=TEST_DST, can=TEST_CAN, frames=voice_frames(), tone=440,
                   **{kind: key.hex()})
        c1, v = counts(radio, *RX_COUNTS, "m17v.rx_peak")
    print(f"\nm17 {kind} with the key: {c0} -> {c1}; why {v.get('m17.why')} enc {v.get('m17.enc')} peak "
          f"{v.get('m17v.rx_peak')}")
    assert c1["m17.c.rx_calls"] == c0["m17.c.rx_calls"] + 1, "no call received"
    assert (as_int(v.get("m17.enc")) or 0) == ENC[kind][0] << 4 | sub, f"m17.enc {v.get('m17.enc')}"
    assert v.get("m17.played") == "1" and c1["m17v.rx_calls"] > c0["m17v.rx_calls"], \
        f"not played (why {v.get('m17.why')})"
    assert c1["m17.c.enc_skipped"] == c0["m17.c.enc_skipped"], "skipped for its encryption with the key on file"
    assert c1["m17v.rx_frames"] - c0["m17v.rx_frames"] >= voice_frames() - MISSING_FRAMES, "voice frames lost"
    assert c1["m17v.rx_peak"] > 0, "the voice decrypted to silence"


@pytest.mark.feature("m17")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
@pytest.mark.codeplug_write
@pytest.mark.parametrize("kind", ENC_PARAMS)
def test_m17_enc_tx_off_air(radio, sdr_bench, backup_dir, pytestconfig, kind):
    """A stored channel with the key keys up twice: each over off air says its encryption (TYPE), its frames as a
    clear one's; the radio found a new nonce for each (AES)"""
    import m17tool
    mhz = rig(pytestconfig, "m17-mhz")
    need_enc(radio, kind)
    if (as_int(radio.view(["m17"]).get("m17.fit")) or 0) & AIR_FIT_TX == 0:
        skip("no_hw", "the radio's air interface can't transmit M17")
    if not sdr_bench.can_fsk4_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no 4FSK receive")
    key, sub = enc_key(kind)
    keys = ("m17.c.tx_calls", "m17.c.tx_underruns", "m17v.tx_frames", "m17.c.nonce_refused")
    overs = []
    with EncChannel(radio, backup_dir, mhz, kind, key, tx=True):
        v0 = radio.view(["m17", "m17_stats"])
        need_callsign(v0)
        assert v0.get("m17.tx_enc") == "1" and (as_int(v0.get("m17.tx_enc_type")) or 0) == ENC[kind][0] << 4 | sub, \
            f"the channel doesn't encrypt with its key: tx_enc {v0.get('m17.tx_enc')} type {v0.get('m17.tx_enc_type')}"
        radio.lowest_power()
        for _ in range(2):
            c0, _ = counts(radio, *keys)
            with sdr_bench.lease():
                job = sdr_bench.fsk4_rx_start(mhz, PTT_S + 4, baud=SYM_RATE, shape="rrc", sync=m17tool.SYNC_LSF,
                                              sync_len=8, nominal_dev_hz=DEV_HZ)
                time.sleep(0.5)
                radio.ptt_for(PTT_S)
                radio.wait_for(lambda x: x.get("state") != "tx", timeout=3, what="TX ended")
                done = job.wait(PTT_S + 20)
            c1, v1 = counts(radio, *keys)
            refused_skip(v1, c0, c1)
            overs.append((decode_over(job, done), c0, c1))
        v2 = radio.view(["m17_stats"])
    for (lines, s), c0, c1 in overs:
        check_over(lines, s, v0, c0, c1, PTT_S, enc=ENC[kind][0], sub=sub)
        assert c1["m17.c.nonce_refused"] == c0["m17.c.nonce_refused"], "a key-up refused for want of a new nonce"
    if kind == "aes":
        assert v2.get("crypto.nonce_ok") == "1", "an AES key-up was refused for want of a new nonce"
