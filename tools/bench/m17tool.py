#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
m17tool: M17 on the bench, through the firmware's own code (core/m17, core/codec2, core/fec, built for the host from
tools/bench/m17: `make -C tools/bench/m17`, done here when the program is missing or older than its sources).

    m17tool.py voice OUT.dibits [--src CALL] [--dst CALL|ALL] [--can N] [--frames N] [--1600] [--tone HZ] [--lead N]
                                [--scrambler SEED | --aes KEY]
    m17tool.py bert OUT.dibits [--frames N] [--lead N]
    m17tool.py sms OUT.dibits [--src CALL] [--dst CALL|ALL] [--can N] --text TEXT [--lead N]
    m17tool.py decode IN [--unpacked | --records]
    m17tool.py --self-test

voice, bert and sms write a transmission as the radio's transmit path lays it out (docs/spec/m17.md 1.1): the preamble
(--lead more of it first), the LSF (not for BERT), the frames, the end of transmission. Voice frames carry Codec 2 of
a tone (--tone, default 440 Hz; 0 silence), encrypted with --scrambler (a seed of 1-3 bytes) or --aes (a key of 16, 24
or 32 bytes), both in hex, as the radio encrypts (m17.md 6). The dibit file is the air interface's packing (four a byte, the first in
bits 7-6; 01 +3, 00 +1, 10 -1, 11 -3), what the bench's fsk4 transmitter takes as hex.

decode reads symbols (that packing; one a byte with --unpacked; the bench receiver's or `rfoundry_debug.py air rx`
records with --records, as nxdntool.py reads them) and prints a line per frame, then the summary: the LSF's source,
destination and CAN, stream frames and their numbers (gaps, the end bit), the decoded voice's peak, packets (an SMS's
text), BERT frames, bits and errors. Exit 0 when a transmission was found.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(HERE, "m17")
BIN = os.path.join(ROOT, "build", "bench", "m17tool")
SYM_RATE = 4800
DEV_HZ = 2400           # the outer symbol's deviation (m17.md 1.3)
SYNC_LSF = "55F7"       # the LSF's sync word, 8 symbols (m17.md 1.2): what a bench receiver locks to


def build():
    srcs = [os.path.join(SRC, "m17tool.c"), os.path.join(SRC, "Makefile")]
    for d in ("core/m17", "core/codec2", "core/dsp", "core/fec", "core/air", "core/crypto"):
        p = os.path.join(ROOT, d)
        srcs += [os.path.join(p, f) for f in os.listdir(p) if f.endswith((".c", ".h"))]
    if not os.path.exists(BIN) or os.path.getmtime(BIN) < max(os.path.getmtime(s) for s in srcs):
        r = subprocess.run(["make", "-s", "-C", SRC], capture_output=True, text=True)
        if r.returncode:
            sys.exit("m17tool: build failed\n" + r.stdout + r.stderr)
    return BIN


def run(args, check=True):
    """The program's JSON lines; SystemExit when it fails (check) or finds no transmission (decode)"""
    r = subprocess.run([build()] + [str(a) for a in args], capture_output=True, text=True)
    if r.returncode and (check or r.returncode != 1):
        sys.exit(r.stderr.strip() or "m17tool: failed")
    return [json.loads(line) for line in r.stdout.splitlines() if line.startswith("{")]


def gen(kind, path, **opt):
    """voice, bert or sms into path; opt: src, dst, can, frames, tone, lead, text, m1600, scrambler, aes (hex).
    Returns the dibits"""
    args = [kind, path]
    for k, v in opt.items():
        if k == "m1600":
            args += ["--1600"] if v else []
        elif v is not None:
            args += ["--" + k, v]
    run(args)
    with open(path, "rb") as f:
        return f.read()


def decode(path, unpacked=False):
    return run(["decode", path] + (["--unpacked"] if unpacked else []), check=False)


def decode_records(records):
    """The bench receiver's records (or `air rx` chunks) through decode: (frame lines, the summary)"""
    sys.path.insert(0, HERE)
    import nxdntool
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "rx.jsonl")
        with open(path, "w") as f:
            for r in records:
                f.write(json.dumps(r if "hard" in r or "rec" in r else {"rec": r}) + "\n")
        lines = decode(nxdntool.records_to_syms(path, tmp), unpacked=True)
    return lines, summary(lines)


def summary(lines):
    s = next((x for x in lines if x["ev"] == "summary"), {})
    lsf = [x for x in lines if x["ev"] == "lsf"]
    s = dict(s)
    s["lsfs"] = lsf
    s["sms"] = [x["text"] for x in lines if x["ev"] == "packet" and x["ok"] and x["type"] == 5]
    return s


def put_sym(d, at, v):
    """Symbol at in packed dibits d (a bytearray) set to v"""
    sh = 6 - 2 * (at & 3)
    d[at >> 2] = (d[at >> 2] & ~(3 << sh) & 0xFF) | (v << sh)


def self_test():
    ok = True

    def check(cond, what):
        nonlocal ok
        print("%s %s" % ("ok  " if cond else "FAIL", what))
        ok = ok and cond
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "t.dibits")
        d = gen("voice", p, src="N0CALL", dst="ALL", can=3, frames=12, lead=40)
        s = summary(decode(p))
        check(len(d) * 4 >= (40 + 192 * 15) and s.get("lsf") == 1 and s["lsfs"][0]["src"] == "N0CALL" and
              s["lsfs"][0]["dst"] == "ALL" and s["lsfs"][0]["can"] == 3, "voice: the LSF's fields")
        check(s.get("stream") == 12 and s.get("fn_gaps") == 0 and s.get("end") == 1 and s.get("eot") == 1 and
              s.get("voice_peak", 0) > 1000, "voice: 12 frames, the end bit, EoT, a tone decoded")
        gen("voice", p, src="AB1CD", dst="EE6B28000001", frames=4, m1600=True, tone=0)
        s = summary(decode(p))
        check(s.get("stream") == 4 and s["lsfs"][0]["dst"] == "EE6B28000001" and s["lsfs"][0]["data"] == 3,
              "voice 1600 to an extended address")
        for enc, key, sub in ((1, "A5C3", 1), (2, "000102030405060708090A0B0C0D0E0F", 0)):
            gen("voice", p, src="N0CALL", frames=6, lead=40, **{"scrambler" if enc == 1 else "aes": key})
            s = summary(decode(p))
            t = s["lsfs"][0]["type"] if s.get("lsf") == 1 else 0
            check(s.get("stream") == 6 and (t >> 3) & 3 == enc and (t >> 5) & 3 == sub and s.get("fn_gaps") == 0,
                  f"voice {'scrambled' if enc == 1 else 'AES'}: TYPE says it, the frames as in the clear")
        gen("bert", p, frames=30)
        s = summary(decode(p))
        check(s.get("bert_frames") == 30 and s.get("bert_bits", 0) > 4000 and s.get("bert_errs") == 0 and
              s.get("lsf") == 0, "bert: 30 frames locked, no errors")
        gen("sms", p, src="N0CALL", dst="AB1CD", text="hello M17", can=1)
        s = summary(decode(p))
        check(s["sms"] == ["hello M17"] and s["lsfs"][0]["stream"] == 0, "sms: the text back")
        # A sync one symbol from its twin's (m17.md 1.2: LSF and packet, stream and BERT differ in symbols 1 and 6):
        # each frame is still its transmission's kind, as the firmware decodes it (m17.md 14.2)
        text = "x" * 80     # four packet frames
        d = bytearray(gen("sms", p, src="N0CALL", dst="AB1CD", text=text))
        for k in range(4):
            put_sym(d, 384 + 192 * k + (1 if k % 2 else 6), 1)    # -3 to +3: the LSF word's symbol
        with open(p, "wb") as f:
            f.write(d)
        s = summary(decode(p))
        check(s["sms"] == [text] and s.get("bad") == 0, "sms: a packet sync one symbol from the LSF's")
        d = bytearray(gen("bert", p, frames=12))
        for k in range(1, 12, 2):
            put_sym(d, 192 + 192 * k + (1 if k % 4 == 1 else 6), 3)   # +3 to -3: the stream word's symbol
        with open(p, "wb") as f:
            f.write(d)
        s = summary(decode(p))
        check(s.get("bert_frames") == 12 and s.get("bad") == 0, "bert: a sync one symbol from the stream's")
        # A packet whose last frame and end were lost, then the next transmission: its LSF starts the packet over
        a = bytearray(gen("sms", p, src="N0CALL", dst="AB1CD", text=text))
        n = len(a) * 4
        for at in range(n - 384, n):
            put_sym(a, at, 0)
        b = gen("sms", p, src="N0CALL", dst="AB1CD", text="second")
        with open(p, "wb") as f:
            f.write(bytes(a) + b)
        s = summary(decode(p))
        check(s["sms"] == ["second"] and s.get("lsf") == 2, "sms: a packet cut short, the next one whole")
        with open(p, "wb") as f:
            f.write(bytes(range(256)) * 8)
        check(decode(p)[-1].get("lsf") == 0, "noise: no transmission")
    print("m17tool self-test:", "ok" if ok else "FAILED")
    return 0 if ok else 1


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv == ["--self-test"]:
        return self_test()
    if not argv:
        print(__doc__)
        return 2
    if argv[0] == "decode" and "--records" in argv:
        with open(argv[1]) as f:
            lines, s = decode_records([json.loads(x) for x in f if x.strip()])
    else:
        lines = run(argv, check=argv[0] != "decode")
        s = summary(lines)
    for x in lines:
        print(json.dumps(x))
    return 0 if argv[0] != "decode" or s.get("lsf") or s.get("bert_frames") else 1


if __name__ == "__main__":
    sys.exit(main())
