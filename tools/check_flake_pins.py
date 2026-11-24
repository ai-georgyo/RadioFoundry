#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Check that flake.nix and flake.lock pin every submodule of third_party/ as its link in HEAD does (docs/dev/nix.md).

    tools/check_flake_pins.py
    tools/check_flake_pins.py --self-test

The Nix builds (flake.nix) take third_party/<name> from a flake input of that name: a flake's source holds no
submodule content and cannot read a link's commit. That makes two records of one fact, and nothing in Nix can compare
them. This does, for every link:

- flake.lock has an input named as the link's directory,
- the input's commit is the link's, both the one the lock was written for (original) and the one fetched (locked),
- the input's repository is the one .gitmodules names for that path,
- flake.nix asks for that commit of that repository, on one line `<name>.url = "<reference>";`. The lock alone is
  not enough: with another commit in flake.nix, Nix writes a new lock by itself at the next build and builds that
  commit.

Exit status 0 when they agree, 1 otherwise. tools/ci.sh runs it in its licenses step.
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = "set the commit and the repository in flake.nix inputs to the link's, then run: nix flake lock"


def repo_url(part):
    """the repository a flake.lock reference names, as a URL without the .git ending"""
    if part.get("type") == "github":
        return "https://github.com/%s/%s" % (part.get("owner"), part.get("repo"))
    url = part.get("url") or ""
    return url[:-4] if url.endswith(".git") else url


def asked(reference):
    """(repository URL without the .git ending, commit) a flake reference in flake.nix names; None for a part it
    doesn't give, as with a branch in place of a commit"""
    m = re.fullmatch(r"github:([^/?]+)/([^/?]+)/([0-9a-f]{40})", reference)
    if m:
        return "https://github.com/%s/%s" % (m.group(1), m.group(2)), m.group(3)
    m = re.fullmatch(r"git\+([^?]+)\?(.*)", reference)
    if m:
        rev = dict(kv.partition("=")[::2] for kv in m.group(2).split("&")).get("rev")
        url = m.group(1)
        return url[:-4] if url.endswith(".git") else url, rev
    return None, None


def problems(links, urls, lock, flake_nix):
    """links: {name: commit} from HEAD; urls: {name: url} from .gitmodules; lock: flake.lock as a dict; flake_nix:
    the text of flake.nix"""
    out = []
    root = lock["nodes"][lock["root"]].get("inputs", {})
    for name, commit in sorted(links.items()):
        # an input that follows another is a list here, never a submodule's
        key = root.get(name)
        node = lock["nodes"].get(key) if isinstance(key, str) else None
        if node is None:
            out.append("third_party/%s: flake.nix has no input named %s" % (name, name))
            continue
        want = urls.get(name)
        if want is None:
            out.append("third_party/%s: .gitmodules has no url for that path" % name)
        elif want.endswith(".git"):
            want = want[:-4]
        for part in ("locked", "original"):
            ref = node.get(part, {})
            if ref.get("rev") != commit:
                out.append("third_party/%s: the link is %s, flake.lock %s.rev is %s"
                           % (name, commit, part, ref.get("rev")))
            if want is not None and repo_url(ref) != want:
                out.append("third_party/%s: .gitmodules names %s, flake.lock %s is %s"
                           % (name, want, part, repo_url(ref) or "no repository"))
        refs = re.findall(r'^[ \t]*%s\.url[ \t]*=[ \t]*"([^"]*)"[ \t]*;' % re.escape(name), flake_nix, re.M)
        if len(refs) != 1:
            out.append('third_party/%s: flake.nix has %d lines `%s.url = "<reference>";`, and this reads the input '
                       "from exactly one" % (name, len(refs), name))
            continue
        url, rev = asked(refs[0])
        if rev != commit:
            out.append("third_party/%s: the link is %s, flake.nix asks for %s" % (name, commit, rev or refs[0]))
        if want is not None and url != want:
            out.append("third_party/%s: .gitmodules names %s, flake.nix asks for %s" % (name, want, url or refs[0]))
    return out


def git(*args):
    return subprocess.run(("git",) + args, cwd=ROOT, capture_output=True, text=True, check=True).stdout


def head_links():
    """{name: commit} of the submodule links (mode 160000) under third_party/ in HEAD"""
    links = {}
    for line in git("ls-tree", "HEAD", "third_party/").splitlines():
        meta, path = line.split("\t", 1)
        mode, _kind, commit = meta.split()
        if mode == "160000":
            links[os.path.basename(path)] = commit
    return links


def module_urls():
    """{name: url} from .gitmodules, by the directory name of each submodule's path"""
    values = {}
    for line in git("config", "-f", ".gitmodules", "--get-regexp", r"^submodule\..*\.(path|url)$").splitlines():
        key, value = line.split(" ", 1)
        section, field = key.rsplit(".", 1)
        values.setdefault(section, {})[field] = value
    return {os.path.basename(v["path"]): v["url"] for v in values.values() if "path" in v and "url" in v}


def self_test():
    a, b = "1" * 40, "2" * 40
    ref = {"type": "github", "owner": "o", "repo": "r", "rev": a}
    lock = {"root": "root", "nodes": {"root": {"inputs": {"x": "x", "f": ["x"]}},
                                      "x": {"flake": False, "locked": dict(ref), "original": dict(ref)}}}
    url = "https://github.com/o/r.git"
    nix = '  inputs = {\n    x.url = "github:o/r/%s";\n    x.flake = false;\n    # y.url = "github:o/r/%s";\n  };\n'
    same = nix % (a, a)
    checks = (
        ("the same commit and repository", {"x": a}, {"x": url}, same, 0),
        ("a url without .git", {"x": a}, {"x": url[:-4]}, same, 0),
        ("another commit: locked, original and flake.nix", {"x": b}, {"x": url}, same, 3),
        ("another repository: locked, original and flake.nix", {"x": a}, {"x": "https://github.com/o/fork.git"},
         same, 3),
        ("another commit and repository", {"x": b}, {"x": "https://github.com/p/r.git"}, same, 6),
        ("a link without an input", {"y": a}, {"y": url}, same, 1),
        ("a link whose input follows another", {"f": a}, {"f": url}, same, 1),
        ("a link .gitmodules doesn't name", {"x": a}, {}, same, 1),
        ("no links", {}, {}, same, 0),
        # the lock still holds the link's commit: Nix would write a new one by itself and build the other commit
        ("another commit in flake.nix alone", {"x": a}, {"x": url}, nix % (b, a), 1),
        ("another repository in flake.nix alone", {"x": a}, {"x": url}, same.replace("o/r", "o/fork"), 1),
        ("a branch in flake.nix, not a commit", {"x": a}, {"x": url}, same.replace("/" + a, "/main"), 2),
        ("no url line in flake.nix", {"x": a}, {"x": url}, same.replace("x.url", "x.uri"), 1),
        ("two url lines in flake.nix", {"x": a}, {"x": url}, same + same, 1),
    )
    for what, links, urls, text, count in checks:
        got = problems(links, urls, lock, text)
        assert len(got) == count, "%s: expected %d problems, got %r" % (what, count, got)
    # a locked commit that differs from the one the lock was written for is one problem
    stale = json.loads(json.dumps(lock))
    stale["nodes"]["x"]["locked"]["rev"] = b
    assert len(problems({"x": a}, {"x": url}, stale, same)) == 1
    # a git input names its repository by url, in the lock and in flake.nix
    gitref = {"type": "git", "url": url, "rev": a}
    plain = {"root": "root", "nodes": {"root": {"inputs": {"x": "x"}}, "x": {"locked": gitref, "original": gitref}}}
    gitnix = '    x.url = "git+%s?ref=main&rev=%s";\n'
    assert problems({"x": a}, {"x": url}, plain, gitnix % (url, a)) == []
    assert len(problems({"x": a}, {"x": url}, plain, gitnix % (url, b))) == 1
    print("check_flake_pins self-test: %d checks ok" % (len(checks) + 3))


def main():
    if "--self-test" in sys.argv[1:]:
        self_test()
        return 0
    links = head_links()
    with open(os.path.join(ROOT, "flake.lock"), encoding="utf-8") as f:
        lock = json.load(f)
    with open(os.path.join(ROOT, "flake.nix"), encoding="utf-8") as f:
        bad = problems(links, module_urls(), lock, f.read())
    for line in bad:
        print(line)
    if bad:
        print(FIX)
        return 1
    print("%d submodules" % len(links))
    return 0


if __name__ == "__main__":
    sys.exit(main())
