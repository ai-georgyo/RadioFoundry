#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Fixtures for the web flasher's tests, made with rfoundry itself, so the JavaScript port is checked against the
Python it was ported from (site/tests/flasher.test.mjs):

  - a synthetic DM-1701 stock file and its key (rfoundry's stockfw.build_synthetic_stock), with its profile
  - one synthetic stock file per codec layout of every placed profile, with what stockfw.extract and plan.py make of
    it (site/tests/stocklayouts.test.mjs), and every profile's readout name (stockfw.layout_name)
  - for the stock container that holds its image twice (rfoundry's twinfw.py): the key it derives from a synthetic
    file, what its header test says of a row of files, and its refusal for each damaged or mismatched file
    (site/tests/twinfw.test.mjs)
  - for every board installed through its processor's ROM bootloader: its install plans in keep, merge and blank mode
    as plan.describe() prints them, and the same board without a codec route (site/tests/dfuse.test.mjs)
  - a plain image standing in for a file known to hold no codec, and rfoundry's message for it
  - a synthetic MD-UV380 stock file and a read of the internal flash of a radio running it, with the codec rfoundry's
    stockfw.from_flash finds in the read
  - plans for a synthetic image in keep and merge mode, as plan.describe() prints them
  - a release manifest for that image (manifest.generate)
  - the site's data files (radios, features, variants, flash boards, the C7000 stub), as site/build.py writes them
  - for the HR_C7000 radios: an LZ4 block from stubtool's encoder, and a synthetic program flash with a codec at
    made-up addresses, its profile, what hrc7000_codec.locate() finds and the slot hrc7000_codec.merge() builds from it
  - for the Baofeng updater: the bytes tools/serial_loader.py's Flasher sends to its FakeBootloader for a small image
    (block numbers wrapping, three blocks damaged on the line), so bfloader.js is checked byte for byte against it
  - for the STM32F4 loader: each radio's board record from stub/f4stub.py config_record(), and the HELLO replies its
    simulated stub (FakeF4) gives before and after CONFIG, so f4stub.js sends and parses the Python tool's bytes; and
    f4stub's test profile, a board with other SPI wiring
  - for the wait after a Read Unprotect: rfoundry's own waits (cli.unprotect_wait) for a row of mass erase times and
    for every board written through a processor's ROM bootloader, so dfuse.js waits what rfoundry waits
  - for the restart into a processor's ROM bootloader: rfoundry's request, the kind it reads from every reply and its
    words for them (debuglink.py), and which chips it asks at all, so link.js asks and reads the same
  - for the codeplug editor: the radio's own codeplug code on the host (tests/host/cpsSim), a codeplug it stored
    and what it reads back, so site/tests/codeplug.test.mjs checks the JavaScript decoder and encoder against it

    python3 site/tests/fixtures.py OUT_DIR
"""
import collections
import json
import os
import random
import struct
import subprocess
import sys
import types

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "rfoundry"))

sys.path.insert(0, os.path.join(ROOT, "site"))

from rfoundry import boards, codecmap, dfu, manifest, plan, stockfw, twinfw  # noqa: E402
from rfoundrysite import repo  # noqa: E402
from rfoundry.fakedfu import synthetic_key  # noqa: E402


def codec_map_vectors(out):
	"""rfoundry's shared codec map vectors with the map normalized, as data/flash-boards.json carries maps"""
	with open(os.path.join(ROOT, "tools", "rfoundry", "tests", "codecmap_vectors.json")) as f:
		v = json.load(f)
	v["map"] = codecmap.normalize(v["map"])
	v["new_run"] = int(v["new_run"], 0)
	v["table_moves"] = {int(k, 0): int(x, 0) for k, x in v["table_moves"].items()}
	t = v["thumb"]
	t["map"] = codecmap.normalize(t["map"])
	t["new_run"], t["window_addr"] = int(t["new_run"], 0), int(t["window_addr"], 0)
	w = v["window"]
	w["map"] = codecmap.normalize(w["map"])
	w["code_run"], w["tables_run"] = int(w["code_run"], 0), int(w["tables_run"], 0)
	j = v["joined"]
	j["map"] = codecmap.normalize(j["map"])
	j["windows"] = [[int(a, 0), d] for a, d in j["windows"]]
	j["layout"] = [int(a, 0) for a in j["layout"]]
	j["block_at"] = int(j["block_at"], 0)
	with open(os.path.join(out, "codecmap.json"), "w") as f:
		json.dump(v, f, indent=1)


def f4_vectors(out):
	"""stub/f4stub.py's board records, and HELLO from its FakeF4 before and after each radio's CONFIG"""
	f4 = repo._tool("stub", "f4stub.py")
	st = f4.stubtool

	def ask(fake, op, payload=b""):
		fake.write(bytes([0x02, op, len(payload) >> 8, len(payload) & 0xFF, st.sum8(payload), 0x03]) + payload)
		r = bytes(fake.out)
		fake.out.clear()
		return {"status": r[1], "body": r[6:].hex()}

	doc = {"protocol": f4.VERSION, "records": {}, "hello": {}}
	for bid in f4.BOARDS:
		rec = f4.config_record(bid)
		doc["records"][bid] = rec.hex()
		fake = f4.FakeF4(b"\xff" * 0x100000, spi=b"\xff" * 0x10000)
		doc["hello"][bid] = {"before": ask(fake, f4.OP_HELLO), "config": ask(fake, f4.OP_CONFIG, rec),
				     "after": ask(fake, f4.OP_HELLO)}
	# a profile with other wiring than any board's (f4stub.test_board), in data/flash-boards.json's shape
	tb = f4.test_board()
	doc["test_board"] = {"id": f4.TEST_BOARD, "name": tb["name"], "stub_id": tb["id"], "record": f4.record(tb).hex(),
			     "owned": [list(r) for r in tb["owned"]], "codec": list(tb["codec"]),
			     "v2_wiring": f4.record(tb)[1:] == f4.V2_WIRING}
	with open(os.path.join(out, "f4-vectors.json"), "w") as f:
		json.dump(doc, f, indent=1)


def md380_like(out):
	"""A stock file whose plain text is like Thumb code, 0x00 common but not dominant, as in the MD-380 image: the column
	vote gets hundreds of key columns wrong and only the byte-pair refinement finds the key (rfoundry's
	test_derive_key_refines_a_bad_vote). Returns its fixture entry."""
	rng = random.Random(1)
	key = synthetic_key(380)
	vocab = [rng.randrange(1, 65536) for _ in range(48)] + [0]
	weights = [1.0 / (i + 1) for i in range(48)] + [1.0]
	n = 0xF2C00
	nxt = {w: rng.choices(vocab, weights, k=3) for w in vocab}
	pool = rng.choices(vocab, weights, k=n // 2)
	hw, cur = [], 0
	for i in range(n // 2):
		cur = nxt[cur][i % 3] if rng.random() < 0.6 else pool[i]
		hw.append(cur)
	plain = struct.pack("<%dH" % (n // 2), *hw)
	hdr = bytearray(stockfw.HEADER_LEN)
	hdr[:len(stockfw.MAGIC)] = stockfw.MAGIC
	struct.pack_into("<I", hdr, 0x7C, 0xFFFFFFFF)
	struct.pack_into("<II", hdr, 0x80, 0x0800C000, n)
	stream = (key * (n // len(key) + 1))[:n]
	stock = bytes(hdr) + bytes(a ^ b for a, b in zip(plain, stream)) + bytes(0x100)
	with open(os.path.join(out, "stock-md380like.bin"), "wb") as f:
		f.write(stock)
	payload = stock[stockfw.HEADER_LEN:stockfw.HEADER_LEN + n]
	vote = bytes(collections.Counter(payload[j::1024]).most_common(1)[0][0] for j in range(1024))
	return {"key": key.hex(), "derived_key": stockfw.derive_key(stock, stockfw.sha256(key)).hex(),
		"vote_wrong": sum(a != b for a, b in zip(vote, key))}


def js_profile(prof):
	"""A profile as data/flash-boards.json carries it (site/rfoundrysite/repo.py flash_boards)"""
	q = dict(prof, segments=[list(s) for s in prof["segments"]], windows=[list(w) for w in stockfw.windows(prof)])
	for k in ("code", "tables", "extra"):
		q.pop(k, None)
	return q


def stock_layouts(out, image):
	"""One synthetic stock file per codec layout of every placed profile (stockfw.same_layout: the files that put the
	same codec at the same addresses count once), and what rfoundry makes of it: the codec placed, the descriptor's
	fields and the merge plan, for stocklayouts.test.mjs. Also every real profile's readout name (layout_name).
	Returns the fixture entry, and {file name: (stock bytes, its profile, key)} for the fixtures built on these files."""
	seen, docs, made = set(), [], {}
	for i, (sha, real) in enumerate(stockfw.PROFILES.items()):
		group = stockfw.same_layout(real)
		if not real.get("placement") or id(group[0]) in seen:
			continue
		seen.add(id(group[0]))
		key = synthetic_key(1000 + i)
		stock, profiles, _, _ = stockfw.build_synthetic_stock(key, random.Random(i), sha)
		(ssha, prof), = profiles.items()
		got, segs = stockfw.extract(stock, key, profiles)
		board = boards.get(prof["board"])
		merge = plan.make_plan(board, [(board.app_base, image)], plan.MERGE, stockfw.codec_segments(prof, segs),
				       board.merge_windows(prof))
		desc = segs[-1][1]
		w = struct.unpack_from("<%dI" % (7 + 3 * stockfw.DESC_WINDOWS), desc)
		name = "layout-%s%s.bin" % (sha[:8], sha[64:].replace("@", "-"))	# "<sha>@<board>" too
		with open(os.path.join(out, name), "wb") as f:
			f.write(stock)
		made[name] = (stock, prof, key)
		docs.append({"file": name, "real_sha256": sha, "stock_sha256": ssha, "key": key.hex(), "profile": js_profile(prof),
			     "versions": [p["version"] for p in group],
			     "placed": [[a, stockfw.sha256(d), len(d)] for a, d in segs],
			     "descriptor": {"tag": w[2], "code": list(w[3:6]),
					    "tables": [list(w[7 + 3 * k:10 + 3 * k]) for k in range(w[6])]},
			     "merge_windows": [list(x) for x in board.merge_windows(prof)],
			     "merge": merge.describe()})
	names = {sha: stockfw.layout_name(p) for sha, p in stockfw.PROFILES.items()}
	return {"files": docs, "names": names}, made


def message(fn, *args):
	"""The text of the StockError a call raises"""
	try:
		fn(*args)
	except stockfw.StockError as e:
		return str(e)
	raise AssertionError("no StockError")


def changed(data, cut=None, append=0, flips=(), words=()):
	"""A file with a change the JavaScript test makes the same way: cut to a length, zero bytes appended, bytes XORed
	([offset, mask]) and 32-bit little-endian words set ([offset, value])"""
	b = bytearray(data[:cut] if cut is not None else data) + bytes(append)
	for at, mask in flips:
		b[at] ^= mask
	for at, value in words:
		struct.pack_into("<I", b, at, value)
	return bytes(b)


def tied_vote(rng):
	"""A file of the second container whose key vote ties, and the key twinfw.derive_key gives for it: an image of the
	bytes 0 and 1 only, with as many of each in every chain as the length words (of 0x10000: bytes 0 and 1 too) and the
	zero words leave even, so in every chain the file's key and that key with bit 0 of each byte flipped score the
	same. The lower first byte wins: where that is the flipped one, the key the vote gives is not the file's, and the
	page must give that same key. Returns (file, that key)"""
	n, chains = 0x10000, 4		# step (n + 4) mod 256 = 4: four chains, a payload position's chain its offset mod 4
	key = bytearray(stockfw.random_bytes(rng, twinfw.KEY_LEN))
	key[1] |= 1			# one chain at least whose flipped first byte is the lower
	word = struct.pack("<I", n)
	fixed = list(enumerate(word)) + [(4 + n + p, b) for p, b in enumerate(word)] + [(8 + 2 * n + p, 0) for p in range(twinfw.TAIL)]
	image = bytearray(n)
	for j in range(chains):
		at = [i for i in range(n) if (4 + i) % chains == j]	# each image byte is in the payload twice, in chain j both times
		mine = [b for p, b in fixed if p % chains == j]
		zero = (2 * len(at) + mine.count(1) - mine.count(0)) // 4
		bits = [0] * zero + [1] * (len(at) - zero)
		rng.shuffle(bits)
		for i, b in zip(at, bits):
			image[i] = b
	tied = bytearray(key)
	for j in range(chains):
		if key[j] & 1:
			for c in range(j, twinfw.KEY_LEN, chains):
				tied[c] ^= 1
	assert tied != key
	return twinfw.pack(bytes(key), bytes(image), b"\x01\x02"), bytes(tied)


def twin(out, layouts, made):
	"""The stock container that holds its image twice (rfoundry twinfw.py), for twinfw.test.mjs, on the synthetic file
	stock_layouts made for the first profile in it: the key twinfw.derive_key finds, three small files whose two
	copies lie other distances apart with theirs and one whose key vote ties (tied_vote), what header_kind and
	stockfw.container_kind say of a row of files, and rfoundry's refusal for every change the test makes to the file
	or to its profile. None where no profile names the container. Returns the fixture entry"""
	doc = next((d for d in layouts["files"] if d["profile"].get("container") == stockfw.TWIN), None)
	if doc is None:
		return None
	stock, prof, key = made[doc["file"]]
	key = key[:twinfw.KEY_LEN]
	sha = doc["stock_sha256"]
	real = stockfw.PROFILES[doc["real_sha256"]]
	m, wins = prof["map"], stockfw.windows(prof)
	link, n = prof["image"]
	want = prof["key_sha256"]

	def write(name, data):
		with open(os.path.join(out, name), "wb") as f:
			f.write(data)
		return name

	rng = random.Random(11)
	zeros = bytes(b if b < 100 else 0 for b in range(256))
	keys = []
	for what, size in (("an odd step, one chain", 70001), ("step 0, every column its own chain", 70140),
			   ("step 128, 128 chains of two", 70012)):
		k = stockfw.random_bytes(rng, twinfw.KEY_LEN)
		data = twinfw.pack(k, stockfw.random_bytes(rng, size).translate(zeros), b"\x01\x02")
		keys.append({"what": what, "file": write("twin-key-%d.bin" % size, data), "step": (size + 4) % twinfw.KEY_LEN,
			     "key_sha256": stockfw.sha256(k), "derived_key": twinfw.derive_key(data, stockfw.sha256(k)).hex()})
	data, tied = tied_vote(rng)
	keys.append({"what": "a vote that ties in every chain: the lower first byte", "file": write("twin-key-tie.bin", data),
		     "step": (0x10000 + 4) % twinfw.KEY_LEN, "key_sha256": stockfw.sha256(tied),
		     "derived_key": twinfw.derive_key(data, stockfw.sha256(tied)).hex()})

	# (above the size the page's opener sets a file aside at as too small to be firmware, so that it reads the header)
	boot = write("twin-boot.bin", twinfw.build_synthetic_boot(random.Random(3), 0x12000))
	small = write("twin-small.bin", twinfw.pack(key, bytes(1000)))
	vectors = write("twin-vectors.bin", struct.pack("<I", 0x24080000) + b"".join(struct.pack("<I", 0x08000401 + 4 * i) for i in range(63))
			+ stockfw.random_bytes(random.Random(4), 4096))
	blank = write("twin-blank.bin", b"\xFF" * 65536)
	kinds = []
	for what, name, change in (("the firmware file", doc["file"], {}), ("cut to 0x100 bytes", doc["file"], {"cut": 0x100}),
				   ("with 4 KB appended", doc["file"], {"append": 4096}), ("a bootloader file", boot, {}),
				   ("the offset in both places", boot, {"words": [[twinfw.OFF_AT, twinfw.HEADER_LEN]]}),
				   ("a file of the first kind", "stock.bin", {}), ("an application image", vectors, {}),
				   ("64 KB of 0xFF", blank, {}), ("0x40 bytes", doc["file"], {"cut": 0x40})):
		with open(os.path.join(out, name), "rb") as f:
			data = changed(f.read(), **change)
		kinds.append(dict(change, what=what, file=name, kind=twinfw.header_kind(data), container=stockfw.container_kind(data),
				  boot=stockfw.boot_file(data)))

	length = 2 * n + 16
	t5 = wins[7][0]
	both = [[twinfw.offset(prof, t5 + 3), 1], [twinfw.offset(prof, t5 + 3, 1), 1]]
	refusals = []

	def refuse(what, call, change=None, **over):
		"""call: "parse", "derive_key" (want: the SHA-256 asked for), "carve" (profile: keys of the profile replaced;
		wins, key and map: the arguments replaced). The message is rfoundry's for the same change"""
		data = changed(stock if "file" not in over else open(os.path.join(out, over["file"]), "rb").read(), **(change or {}))
		p = dict(prof, **over.get("profile", {}))
		if call == "parse":
			text = message(twinfw.parse, data)
		elif call == "derive_key":
			text = message(twinfw.derive_key, data, over.get("want", want))
		else:
			k = bytes.fromhex(over["key"]) if "key" in over else key
			text = message(twinfw.carve, data, p, over.get("wins", wins), k, over.get("map", m))
		refusals.append(dict(over, what=what, call=call, change=change or {}, message=text))

	refuse("short", "parse", {"cut": 0x8F})
	refuse("the offset is not 0x80", "parse", {"words": [[twinfw.OFF_AT, 0x100]]})
	refuse("an odd image length", "parse", {"words": [[twinfw.LEN_AT, length + 1]]})
	refuse("no room for the two length words", "parse", {"words": [[twinfw.LEN_AT, 14]]})
	refuse("a length past the file", "parse", {"words": [[twinfw.LEN_AT, length + 4]]})
	refuse("17 bytes after the payload", "parse", {"append": 15})
	refuse("a byte of the second copy", "derive_key", {"flips": [[twinfw.offset(prof, link + 0x1234, 1), 0x40]]})
	refuse("another key's hash", "derive_key", want="00" * 32)
	refuse("an image too short for the vote", "derive_key", file=small)
	refuse("a cut file", "derive_key", {"cut": 0x100})
	refuse("a byte of a window in the second copy", "carve", {"flips": both[1:]})
	refuse("a byte of a window in both copies", "carve", {"flips": both})
	refuse("the first length word", "carve", {"flips": [[0x80, 1]]})
	refuse("the second length word", "carve", {"flips": [[0x80 + 4 + n + 1, 1]]})
	refuse("a nonzero byte in the tail words", "carve", {"flips": [[0x80 + 8 + 2 * n + 5, 1]]})
	refuse("another size", "carve", {"append": 1})
	refuse("another image length in the profile", "carve", profile={"image": [link, n - 2]})
	refuse("another payload in the profile", "carve", profile={"payload": [0x80, 2 * n]})
	refuse("a window outside the image", "carve", wins=[[link + n - 8, 16, "00" * 32]])
	refuse("another key", "carve", key=(bytes([key[0] ^ 1]) + key[1:]).hex())
	for what, change in (("a range's SHA-256", {"sha256": "00" * 32}), ("a range's first word", {"first_word": 0x12345678})):
		m2 = json.loads(json.dumps(m))
		m2["code"]["ranges"][1].update(change)
		refuse(what, "carve", map=m2)

	# the join: rfoundry's refusal for a placement or a map that does not hold together (stockfw.placed_parts on the
	# file's own windows). "placement": keys of the placement replaced; "map": the map in the profile's place
	segs = twinfw.carve(stock, prof, wins, key, m)
	pl = prof["placement"]
	extra = list(pl["extra"])
	up = stockfw.DESC_ADDR - (pl["tables"] + pl["tables_len"]) + 16
	up -= up % 16
	no_holes = json.loads(json.dumps(m))
	no_holes["code"].pop("holes")
	no_holes["code"]["ranges"] = [{"off": 0, "len": no_holes["code"]["len"]}]
	for w in no_holes["table_windows"]:
		w.pop("holes", None)
	stray = json.loads(json.dumps(m))
	stray["table_windows"].append({"flash": wins[-1][0] + 0x1000, "run": wins[-1][0] + 0x1000, "len": 16,
				       "holes": [{"off": 4, "len": 2}]})
	joins = []
	for what, change, cmap in (
			("the code overlaps the block", {"code": pl["code"] + 16}, None),
			("a piece inside the one before", {"extra": [extra[0] - 16] + extra[1:]}, None),
			("a piece below the block", {"tables": pl["tables"] + 16}, None),
			("the last piece ends before the block does", {"tables_len": pl["tables_len"] + 1}, None),
			("the last piece ends past the block", {"tables_len": pl["tables_len"] - 1}, None),
			("the block reaches the descriptor", {"tables": pl["tables"] + up, "extra": [a + up for a in extra]}, None),
			("nothing to blank", {}, no_holes), ("a hole in a window the profile does not carve", {}, stray)):
		p = dict(prof, placement=dict(pl, gates=None, **change), **({"map": cmap} if cmap else {}))
		joins.append({"what": what, "placement": change, "map": cmap, "message": message(stockfw.placed_parts, p, segs)})

	# the two files of this container that are named and refused: a version nobody has mapped (a stand-in registered
	# under its own SHA-256 with the first entry's text) and the bootloader stand-in
	from unittest import mock
	entry = next((e for e in stockfw.UNMAPPED_FILES.values() if e["board"] == real["board"]), None)
	other = None
	if entry:
		data = changed(stock, flips=[[len(stock) - 1, 1]])
		with mock.patch.dict(stockfw.UNMAPPED_FILES, {stockfw.sha256(data): entry}):
			other = {"change": {"flips": [[len(stock) - 1, 1]]}, "sha256": stockfw.sha256(data), "entry": entry,
				 "message": message(stockfw.find_profile, data)}
	with open(os.path.join(out, boot), "rb") as f:
		boot_message = message(stockfw.find_profile, f.read())
	return {"file": doc["file"], "stock_sha256": sha, "key": key.hex(), "derived_key": twinfw.derive_key(stock, want).hex(),
		"parse": list(twinfw.parse(stock)), "keys": keys, "kinds": kinds, "refusals": refusals, "joins": joins,
		"unmapped": other, "boot": {"file": boot, "message": boot_message}}


def rom_board_without_codec(real):
	"""A board of the ROM bootloader route as one without a codec route, as rfoundry's own tests build it
	(tools/rfoundry/tests/test_rfoundry.py rom_board_without_codec): [codec] none with its reason, no codec tool, no
	stock file, nothing to keep, and the application region reaching to the end of the flash a codec route keeps free.
	So the tests of the route itself don't depend on which radios have a codec route"""
	with open(boards.REGISTRY, encoding="utf-8") as f:
		e = json.load(f)["boards"][real.id]
	end = max([real.app_region[1]] + [s.end for s in real.codec_sectors()])
	return boards.Board(dict(e, codec_none="No codec route in this test: DMR voice is off.", codec_tool=None, codec_stock=[],
				 keep_codec_supported=False, stock_download=None, app_region=[e["app_region"][0], end],
				 codec={"id": None, "windows": None, "installed": [], "sources": None}))


def rom_boards(image, layouts, made):
	"""Every board installed through its processor's ROM bootloader (family "dfuse"), for dfuse.test.mjs: its install
	plans as plan.py makes them with the vector table last and dfu.check_plain takes them: keep, and with a codec
	route merge (the codec of the synthetic stock file stock_layouts made for its first profile) and blank. Under
	"plain" the same board without a codec route, as data/flash-boards.json would carry it, with its keep plan, the
	reason it gives for having no codec and rfoundry's refusal of the blank mode. Returns {board id: entry}"""
	from unittest import mock
	docs = {}
	for b in boards.BOARDS.values():
		if b.family != "dfuse":
			continue
		segs = [(b.app_base, image)]

		def lines(p):
			dfu.check_plain(p)
			return p.describe()

		doc = {"codec": b.has_codec, "keep": lines(plan.make_plan(b, segs, plan.KEEP, vector_last=True))}
		lay = next((d for d in layouts["files"] if d["profile"]["board"] == b.id), None)
		if b.has_codec and lay:
			stock, prof, key = made[lay["file"]]
			_, codec = stockfw.extract(stock, key, {lay["stock_sha256"]: prof})
			doc.update(layout=lay["file"],
				   merge=lines(plan.make_plan(b, segs, plan.MERGE, stockfw.codec_segments(prof, codec), b.merge_windows(prof), vector_last=True)),
				   blank=lines(plan.make_plan(b, segs, plan.BLANK, vector_last=True)))
		nc = rom_board_without_codec(b)
		with mock.patch.dict(boards.BOARDS, {nc.id: nc}):
			js = repo.flash_boards()["boards"][nc.id]
		try:
			plan.make_plan(nc, segs, plan.BLANK, vector_last=True)
			raise AssertionError("a board without a codec route took the blank mode")
		except plan.PlanError as e:
			refusal = str(e)
		doc["plain"] = {"board": js, "codec_none": nc.codec_none, "keep": lines(plan.make_plan(nc, segs, plan.KEEP, vector_last=True)),
				"blank_refusal": refusal}
		docs[b.id] = doc
	return docs


def flash_readout(out, rng, sha=None, name="mduv380"):
	"""A synthetic MD-UV380 stock file (default the first build, V19.26), and a read of the internal flash of a radio
	running it (the application decrypted at 0x0800C000, as stockfw.from_flash takes it): what stockfw.js fromFlash must
	find, and the codec it must give, as rfoundry places it. Returns the fixture entry."""
	key = synthetic_key(380)
	sha = sha or next(h for h, p in stockfw.PROFILES.items() if p["board"] == "mduv380")
	stock, profiles, _, _ = stockfw.build_synthetic_stock(key, rng, sha)
	(ssha, prof), = profiles.items()
	segs = stockfw.parse_container(stock)
	flash = bytearray(b"\xFF" * (stockfw.FLASH_END - stockfw.FLASH_BASE))
	app = segs[-1]
	flash[app[0] - stockfw.FLASH_BASE:app[0] - stockfw.FLASH_BASE + app[1]] = stockfw.decrypt(
		stock, stockfw.app_file_offset(segs, app[0]), app[1], key)
	got, codec = stockfw.from_flash(bytes(flash), profiles=profiles)
	assert got is prof
	with open(os.path.join(out, "flash-%s.bin" % name), "wb") as f:
		f.write(flash)
	with open(os.path.join(out, "stock-%s.bin" % name), "wb") as f:
		f.write(stock)
	q = dict(prof, segments=[list(x) for x in prof["segments"]], windows=[list(w) for w in stockfw.windows(prof)])
	for k in ("code", "tables", "extra"):
		q.pop(k, None)
	return {"key": key.hex(), "stock_sha256": ssha, "profile": q,
		"placed": [[a, stockfw.sha256(d), len(d)] for a, d in codec]}


def no_codec(out, rng):
	"""A plain application image standing in for a file known to hold no codec (stockfw.NO_CODEC_FILES; the real one
	is never in the repository): registered under its own SHA-256 with the first entry's text, and the message
	rfoundry gives for it, which the flasher must give too. Returns the fixture entry."""
	from unittest import mock
	data = bytes(rng.getrandbits(8) for _ in range(96 * 1024))
	sha = stockfw.sha256(data)
	real = next(iter(stockfw.NO_CODEC_FILES))
	with mock.patch.dict(stockfw.NO_CODEC_FILES, {sha: stockfw.NO_CODEC_FILES[real]}):
		msg = stockfw.no_codec(data)
		try:
			stockfw.find_profile(data)
			raise AssertionError("a file without a codec was taken")
		except stockfw.StockError as e:
			assert str(e) == msg
	assert stockfw.from_flash(data) is None
	with open(os.path.join(out, "nocodec.bin"), "wb") as f:
		f.write(data)
	return {"sha256": sha, "real_sha256": real, "message": msg}


def unprotect_waits(out):
	"""rfoundry's waits after a Read Unprotect: the two floors, [mass erase time, least, most] for a row of times on
	both sides of the floors, and {board: [least, most]} for every board of the plain DfuSe family"""
	from rfoundry import cli
	row = [[n, *cli.unprotect_wait(types.SimpleNamespace(name="x", mass_erase_max_s=n))] for n in (1, 13, 26, 30, 31, 45, 120)]
	doc = {"min_s": cli.UNPROTECT_MIN_S, "wait_s": cli.UNPROTECT_WAIT_S, "bounds": row,
	       "boards": {b.id: list(cli.unprotect_wait(b)) for b in boards.BOARDS.values() if b.family == "dfuse"}}
	with open(os.path.join(out, "unprotect-wait.json"), "w") as f:
		json.dump(doc, f, indent=1)


def restart_vectors(out):
	"""rfoundry's side of the restart into the bootloader over the debug link (debuglink.md 3.11): the frame it sends
	for a token, [status, bytes after it, kind] for every reply the spec names and some it doesn't, its words for the
	kinds, and [family, RDP level, asked] from its Link against the simulated running radio: the host's half of the
	gate"""
	from rfoundry import debuglink as dl, fakedfu
	token = 0x89ABCDEF
	replies = [(0, b"")] + [(7, bytes([r])) for r in range(8)] + [(7, b""), (7, b"\x05\x00")] + \
		[(st, b"") for st in (1, 2, 3, 4, 6, 9)] + [(5, bytes([dl.CMD_REBOOT]))]
	board = next(b for _, b in sorted(boards.BOARDS.items()) if b.family == "dfuse")
	gate = []
	for family in (0, 1, 2, 3, 9):
		for rdp in (0, 1, 2):
			radio = fakedfu.FakeRunning(board, fakedfu.FakeRom(board), family=family, rdp=rdp, stays=True)
			link = dl.Link(radio)
			link.restart_into_bootloader(link.chip())
			gate.append([family, rdp, bool(radio.requests)])
	doc = {"cmd": dl.CMD_REBOOT, "token": token, "seq": 0x41, "request": dl.restart_request(token).hex(),
	       "frame": dl.frame(dl.CMD_REBOOT, 0x41, dl.restart_request(token)).hex(),
	       "replies": [[st, d.hex(), dl.restart_outcome(st, d)] for st, d in replies], "why": dl.RESTART_WHY, "gate": gate}
	with open(os.path.join(out, "restart-vectors.json"), "w") as f:
		json.dump(doc, f, indent=1)


def main(out):
	os.makedirs(out, exist_ok=True)
	codec_map_vectors(out)
	f4_vectors(out)
	unprotect_waits(out)
	restart_vectors(out)
	rng = random.Random(42)
	key = synthetic_key(1701)
	stock, profiles, code, tables = stockfw.build_synthetic_stock(key, rng)
	(sha, prof), = profiles.items()
	board = boards.get("dm1701")
	image = bytes(rng.getrandbits(8) for _ in range(150 * 1024 + 123))
	_, codec = stockfw.extract(stock, key, profiles)
	keep = plan.make_plan(board, [(board.app_base, image)], plan.KEEP)
	merge = plan.make_plan(board, [(board.app_base, image)], plan.MERGE, stockfw.codec_segments(prof, codec),
			       board.merge_windows(prof))
	m = manifest.generate(board, image, "v0.0.0-test", "ab" * 20, features="FM,DMR")
	with open(os.path.join(out, "stock.bin"), "wb") as f:
		f.write(stock)
	with open(os.path.join(out, "image.bin"), "wb") as f:
		f.write(image)
	doc = {
		"key": key.hex(),
		"stock_sha256": sha,
		"profile": dict(prof, segments=[list(s) for s in prof["segments"]],
				windows=[list(w) for w in stockfw.windows(prof)]),
		"derived_key": stockfw.derive_key(stock, stockfw.sha256(key)).hex(),
		"keep": keep.describe(),
		"merge": merge.describe(),
		"manifest": m,
		# the codec as the merge places it (code moved, tables cut, descriptor): what stockfw.js must give
		"placed": [[a, stockfw.sha256(d), len(d)] for a, d in codec],
		"md380like": md380_like(out),
		"readout": flash_readout(out, random.Random(380)),
		# RT3S V219.60: the code doesn't fit after its tables, so it goes to the start of sector 10, below them
		"readout_v21960": flash_readout(out, random.Random(21960),
						"447ed4e09482c45661cd6e4c83d1e9f777463993ab7dd48b0e0597bb14f519ee", "rt3s-v21960"),
		"no_codec": no_codec(out, random.Random(1702)),
	}
	doc["layouts"], made = stock_layouts(out, image)
	doc["twin"] = twin(out, doc["layouts"], made)
	doc["rom"] = rom_boards(image, doc["layouts"], made)
	for k in ("code", "tables", "extra"):
		doc["profile"].pop(k, None)
	with open(os.path.join(out, "fixtures.json"), "w") as f:
		json.dump(doc, f, indent=1)
	# the site's data files the flasher reads (what site/build.py writes to data/)
	feats = repo.features()
	variants = repo.variants()
	radios, _ = repo.boards([f["name"] for f in feats], [v["id"] for v in variants])
	# the synthetic codecs' maps go into the flash-boards data, as a real build's would (docs/spec/codec-placement.md)
	test_maps = {**c7000(out, rng), **db25d(out, rng)}
	bfloader(out)
	codeplug(out)
	fb = repo.flash_boards()
	fb["codec_maps"].update(test_maps)
	fb["c7000"]["codec_maps"].update(test_maps)
	for name, obj in (("c7000-stub.json", repo.c7000_stub()), ("f4-stub.json", repo.f4_stub()), ("flash-boards.json", fb), ("features.json", {"features": feats}),
			  ("variants.json", {"variants": variants}), ("radios.json", {"radios": radios})):
		with open(os.path.join(out, name), "w") as f:
			json.dump(obj, f)


def c7000(out, rng):
	import struct
	import zlib
	st, dc = repo._tool("stub", "stubtool.py"), repo._tool("tools", "hrc7000_codec.py")
	sector = bytes(rng.choice(b"RadioFoundry flash  \x00\xff") for _ in range(st.SECTOR))
	real = next(p for p in dc.PROFILES.values() if p["board"] == "dm1801pro")
	dump = bytearray(rng.getrandbits(8) for _ in range(dc.PROGMEM_SIZE))
	code = bytearray(rng.getrandbits(8) for _ in range(real["code"][1]))
	struct.pack_into("<2I", code, 0, *dc.CODEC_SIG)
	tables = bytes(rng.getrandbits(8) for _ in range(real["tables"][1]))
	dump[0xB1234:0xB1234 + len(code)] = code
	dump[0x60010:0x60010 + len(tables)] = tables
	prof = dict(real, code_sha256=dc.sha256(bytes(code)), tables_sha256=dc.sha256(tables),
		    tables_head_crc=zlib.crc32(tables[:64]))
	found = dc.locate(bytes(dump), {prof["code_sha256"]: prof}, "dm1801pro")
	image = bytes(rng.getrandbits(8) for _ in range(300 * 1024 + 5))
	maps = dc.synthetic_map(found, bytes(code))
	merged = dc.merge(image, found, *dc.carve(bytes(dump), found), maps)
	for name, data in (("lz4_plain.bin", sector), ("lz4_block.bin", st.lz4_encode(sector)), ("c7000_progmem.bin", dump),
			   ("c7000_image.bin", image), ("c7000_merged.bin", merged)):
		with open(os.path.join(out, name), "wb") as f:
			f.write(data)
	doc = {"profile": {"name": "synthetic", "board": "dm1801pro", "tag": prof["tag"], "code_len": len(code), "run": prof["code"][2],
			   "code_sha256": prof["code_sha256"], "tables_len": len(tables), "tables_ram": prof["tables"][2],
			   "tables_sha256": prof["tables_sha256"], "tables_head_crc": prof["tables_head_crc"],
			   "entries": [prof["entries"][k] for k in dc.ENTRIES], "stock_file": None},
	       "found": {"code": found["code"][0] - dc.PROGMEM_BASE, "tables": found["tables"][0] - dc.PROGMEM_BASE}}
	# a slot (hrc7000_codec.test_slot: a synthetic DB25-D codec with a map), the code window it was made for
	slot2, windows, moved_crc, m = dc.test_slot()
	with open(os.path.join(out, "c7000_slot_v2.bin"), "wb") as f:
		f.write(slot2)
	run_lo, run_hi = struct.unpack_from("<2I", windows, 0)
	doc["v2"] = {"map": m, "run": [run_lo, run_hi], "moved_crc": moved_crc}
	with open(os.path.join(out, "c7000.json"), "w") as f:
		json.dump(doc, f, indent=1)
	return maps


def db25d(out, rng):
	"""The DB25-D's plain stock image (no header, no container), a synthetic one with the real profile's addresses, and
	the merged image hrc7000_codec.merge() builds from it. With RFOUNDRY_DB25D_STOCK=<one of the real 018 files> the expected
	result for that file is made as well, so the JavaScript can be checked on the real bytes (never kept in the repo)."""
	import struct
	dc = repo._tool("tools", "hrc7000_codec.py")
	real = dc.PROFILES["341536cbac0c1b2471f8198ebc456946e5fe026d96d153df14d6aa5566304f8b"]
	plain = bytearray(rng.getrandbits(8) for _ in range(real["size"]))
	caddr, clen, _ = real["code"]
	taddr, tlen, _ = real["tables"]
	struct.pack_into("<2I", plain, dc.file_offset(caddr, 0), *dc.CODEC_SIG)
	plain = bytes(plain)
	prof = dict(real)
	found, code, tables = dc.extract(plain, {dc.sha256(plain): prof}, board="db25d")
	image = bytes(rng.getrandbits(8) for _ in range(0x1000 + 20000))
	maps = dc.synthetic_map(found, code)
	merged = dc.merge(image, found, code, tables, maps)
	files = {"db25d_stock.bin": plain, "db25d_image.bin": image, "db25d_merged.bin": merged}
	doc = {"sha256": dc.sha256(plain), "size": len(plain), "code": caddr, "tables": taddr, "real": None}
	path = os.environ.get("RFOUNDRY_DB25D_STOCK")
	if path:
		with open(path, "rb") as f:
			data = f.read()
		p, c, t = dc.extract(data, board="db25d")
		files["db25d_real_slot.bin"] = dc.slot_bytes(p, c, t)
		doc["real"] = {"path": os.path.abspath(path), "name": p["name"]}
	for name, data in files.items():
		with open(os.path.join(out, name), "wb") as f:
			f.write(data)
	with open(os.path.join(out, "db25d.json"), "w") as f:
		json.dump(doc, f, indent=1)
	return maps


def bfloader(out):
	"""serial_loader's Flasher writing a 261-block image to its FakeBootloader, every byte it sent recorded"""
	sl = repo._tool("tools", "serial_loader.py")

	class Recorded(sl.FakeBootloader):
		def __init__(self, *a, **k):
			super().__init__(*a, **k)
			self.sent = bytearray()

		def write(self, b):
			self.sent += b
			super().write(b)

	model = next(m for m in sl.MODELS.values() if m.codec_slot)	# any radio on this loader: the framing is the same
	raw = bytes((i * 31 + (i >> 10)) & 0xFF for i in range(260 * 1024 + 517))
	corrupt = [3, 255, 0]
	fake = Recorded(model, corrupt=set(corrupt))
	name = "rfoundry_%s_codec.bin" % model.id
	sl.Flasher(fake, log=lambda *a: None, step_timeout=0.2, model=model).flash(raw, name)
	assert not fake.errors and fake.image() == raw and fake.naks == len(corrupt)
	for n, data in (("bfloader_image.bin", raw), ("bfloader_host.bin", bytes(fake.sent))):
		with open(os.path.join(out, n), "wb") as f:
			f.write(data)
	with open(os.path.join(out, "bfloader.json"), "w") as f:
		json.dump({"board": model.id, "id": model.tag, "name": name, "corrupt": corrupt, "naks": fake.naks,
			   "blocks": len(fake.blocks), "header": list(fake.header)}, f, indent=1)


def codeplug(out):
	host = os.path.join(ROOT, "tests", "host")
	subprocess.run(["make", "-s", "-C", host, "cpsSim"], check=True)
	sim = os.path.join(host, "cpsSim")
	image = os.path.join(out, "codeplug_sim.bin")
	subprocess.run([sim, "--populate", "--dump", image], stdin=subprocess.DEVNULL, check=True)
	detail = subprocess.run([sim, "--image", image, "--detail"], capture_output=True, text=True, check=True).stdout
	with open(os.path.join(out, "codeplug.json"), "w") as f:
		json.dump({"cpssim": sim, "detail": detail}, f, indent=1)


if __name__ == "__main__":
	main(sys.argv[1])
