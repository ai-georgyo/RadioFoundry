#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Merge the codec into a freshly built image when the radio's stock firmware is in codecs/ (run by make).

Looks through codecs/ for a stock firmware file the board's codec tool supports (checked by SHA-256, so file names
don't matter) and runs that tool to write <image>_codec.bin next to the image. With no supported file it says so and
does nothing: the plain image is still the build's output. A file of the radio's maker that is known but whose codec
has not been mapped (stockfw.UNMAPPED_FILES) is named with the file to use instead, then the same. A board without a
codec route (board.toml [codec] none) has no tool: it says the board's reason and does nothing. See codecs/README.md.
"""
import argparse
import contextlib
import copy
import hashlib
import io
import os
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOOLS, "rfoundry"))
from rfoundry import boards  # noqa: E402  (the board registry)
from rfoundry import stockfw  # noqa: E402  (stock file profiles)

# Board -> codec tool, the registry's [codec] tool (each takes --stock, --image, -o and keeps its supported stock files
# in PROFILES); None for a board without a codec route
CODEC_TOOLS = {b.id: b.codec_tool for b in boards.BOARDS.values()}
SKIP = ("README.md", ".gitignore")


def no_codec(board):
    """Why the board gets no codec merge: the registry's reason ([codec] none), or None for a board with a codec route
    (and for an id the registry doesn't know)."""
    b = boards.BOARDS.get(board)
    return b.codec_none if b is not None else None


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def candidates(folder):
    for dirpath, dirnames, filenames in os.walk(folder):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            if name not in SKIP and not name.startswith(".") and os.path.isfile(path):	# not a dangling link
                yield path


def find_key(folder, board):
    """The board's key file in the folder (optional: the codec tool derives the key from the stock file without one;
    the boards with an encrypted stock container, whose registry entry has the key's hash)."""
    want = boards.BOARDS[board].key_sha256 if board in boards.BOARDS else None
    if want is None:
        return None
    for path in candidates(folder):
        if os.path.getsize(path) == 1024 and sha256_file(path) == want:
            return path
    return None


def board_profiles(profiles, board):
    """The profiles that count for a board: its own and those that name no board"""
    return {h: p for h, p in profiles.items() if p.get("board", board) == board}


def profile_shas(profiles):
    """The stock files' SHA-256 values of PROFILES keys. A file with its own placement on a board is keyed
    "<sha>@<board>" (stockfw.profile_key), so the key itself never equals a file's hash"""
    return {stockfw.key_sha(h) for h in profiles}


def unmapped(folder, board):
    """[(path, why)] for the files in the folder that are known files of this board's maker whose codec has not been
    mapped (stockfw.UNMAPPED_FILES): each with the sentence that names the file to use instead"""
    out = []
    for path in candidates(folder):
        f = stockfw.UNMAPPED_FILES.get(sha256_file(path))
        if f is not None and f["board"] == board:
            with open(path, "rb") as fh:
                out.append((path, stockfw.unmapped(fh.read())))
    return out


def self_test():
    from unittest import mock
    sha = "ab" * 32
    fake = {sha: {"board": "one"}, stockfw.profile_key(sha, "two"): {"board": "two"}}
    fails = []
    for board in ("one", "two"):
        if profile_shas(board_profiles(fake, board)) != {sha}:
            fails.append("a file keyed for %s is not found by its SHA-256" % board)
    for b, t in CODEC_TOOLS.items():	# every board with a codec tool finds each of its stock files
        if t is None:
            continue
        profs = board_profiles(__import__(t).PROFILES, b)
        if profs and not all(stockfw.key_sha(h) in profile_shas(profs) for h in profs):
            fails.append("%s: a stock file is not found" % b)
    for b, t in CODEC_TOOLS.items():	# a board has a codec tool or the reason it has none, never both or neither
        why = no_codec(b)
        if (t is None) != bool(why):
            fails.append("%s: codec tool %r with the reason %r" % (b, t, why))
        if not why:
            continue
        # the build's call for such a board: the reason and exit 0, before any file in the folder is looked at (the
        # tools folder stands in for codecs/: it exists and holds large files)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = main(["--board", b, "--image", os.path.join(TOOLS, "no-such-image.bin"), "--dir", TOOLS])
        if rc != 0 or "no codec for this radio" not in out.getvalue() or why not in out.getvalue():
            fails.append("%s: a board without a codec route gave %r, exit %r" % (b, out.getvalue(), rc))
    # The same call for a board made here, so the case runs whatever the registry holds: the first registry entry
    # with its codec route taken away (a reason, no tool)
    bid, why = "no-route-test", "this test's board has no codec route"
    nb = copy.copy(next(iter(boards.BOARDS.values())))
    nb.id, nb.codec_none, nb.codec_tool, nb.has_codec = bid, why, None, False
    out = io.StringIO()
    with mock.patch.dict(boards.BOARDS, {bid: nb}), mock.patch.dict(CODEC_TOOLS, {bid: None}), \
            contextlib.redirect_stdout(out):
        rc = main(["--board", bid, "--image", os.path.join(TOOLS, "no-such-image.bin"), "--dir", TOOLS])
    if rc != 0 or "no codec for this radio" not in out.getvalue() or why not in out.getvalue():
        fails.append("a board without a codec route gave %r, exit %r" % (out.getvalue(), rc))
    print("ran   a board without a codec route: its reason, exit 0, no file looked at")
    # A maker's file whose codec has not been mapped, in the folder of a board with a codec tool: the sentence that
    # names the file to use, then as with no supported file (exit 0, nothing written). The file is random bytes
    # under its own hash
    with_tool = next((b for b, t in CODEC_TOOLS.items() if t and not hasattr(__import__(t), "identify")), None)
    if with_tool is None:
        fails.append("no board with a codec tool that knows its files by SHA-256")
    else:
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "old-version.bin")
            with open(path, "wb") as f:
                f.write(os.urandom(8192))
            entry = {"board": with_tool, "name": "old-version.bin", "version": "V0.1"}
            out = io.StringIO()
            with mock.patch.dict(stockfw.UNMAPPED_FILES, {sha256_file(path): entry}), contextlib.redirect_stdout(out):
                rc = main(["--board", with_tool, "--image", os.path.join(d, "image.bin"), "--dir", d])
            text = out.getvalue()
            if rc != 0 or "has not been mapped" not in text or "V0.1" not in text or "no supported" not in text \
                    or os.listdir(d) != ["old-version.bin"]:
                fails.append("a file whose codec is not mapped gave %r, exit %r" % (text, rc))
        print("ran   a known file whose codec is not mapped: the sentence, exit 0, built without the codec")
    for f in fails:
        print("FAIL:", f)
    print("codec_auto self-test %s" % ("PASSED" if not fails else "FAILED"))
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--board")
    ap.add_argument("--image", help="the built image")
    ap.add_argument("--self-test", action="store_true", help="check the profile matching, no files needed")
    ap.add_argument("--dir", default=os.path.join(ROOT, "codecs"), help="where to look (default: codecs/)")
    ap.add_argument("-o", "--output", help="merged image (default: <image>_codec.bin)")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.board or not args.image:
        ap.error("--board and --image are required")

    why = no_codec(args.board)
    if why:
        print("codec: no codec for this radio (%s): %s" % (args.board, why))
        return 0
    tool = CODEC_TOOLS.get(args.board)
    if tool is None or not os.path.isdir(args.dir):
        return 0
    # one tool may serve several boards: only this board's stock files count
    mod = __import__(tool)
    profiles = board_profiles(mod.PROFILES, args.board)
    if hasattr(mod, "identify"):	# a tool that knows a file by more than its SHA-256 (a program flash dump)
        def known(path):
            with open(path, "rb") as f:
                return mod.identify(f.read(), board=args.board) is not None
    else:
        shas = profile_shas(profiles)

        def known(path):
            return sha256_file(path) in shas
    stock = next((p for p in candidates(args.dir) if os.path.getsize(p) > 4096 and known(p)), None)
    out = args.output or os.path.splitext(args.image)[0] + "_codec.bin"
    if stock is None:
        for path, why in unmapped(args.dir, args.board):
            print("codec: %s: %s" % (os.path.relpath(path), why))
        print("codec: no supported %s stock firmware in %s, built without the codec (codecs/README.md)"
              % (args.board, os.path.relpath(args.dir)))
        if os.path.exists(out) and os.path.getmtime(out) < os.path.getmtime(args.image):
            os.remove(out)	# a merged image from an older build would be flashed by mistake
        return 0
    cmd = [sys.executable, os.path.join(TOOLS, tool + ".py"), "--stock", stock, "--image", args.image, "-o", out]
    cmd += ["--board", args.board]
    key = find_key(args.dir, args.board)
    if key:
        cmd += ["--key", key]
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
