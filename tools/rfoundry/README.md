<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# rfoundry

Flashes RadioFoundry release images onto a radio. Release images never contain the AMBE codec; rfoundry either
keeps the codec already in the radio (it writes only the firmware's own flash sectors) or writes the codec from your
radio's stock firmware file. Radios: the boards of the [board registry](../../docs/dev/board-registry.md); a USB DFU
radio flashes over USB, the others over their programming cable (below). A radio installed through the RAM stub
(`[flash] family = "serial-iap"`) uses the web flasher or `stub/stubtool.py`.

| `[flash] family` | Route |
|---|---|
| `usb-dfu` | USB, the radio maker's DFU bootloader: every block encrypted with the bootloader key |
| `dfuse` | USB, the bootloader in the radio's processor (ROM), plain DfuSe: no key, and a read protection to remove on the first install, which erases the codec with everything else. A board with a codec route has a codec step with a rule of its own; a board with `[codec] none` has none ([below](#the-processors-own-bootloader)) |
| `serial-loader` | the programming cable ([below](#the-dm-1801-pro)) |

## Install

- **Executable:** download `rfoundry-<version>-<os>-<arch>` from the release page. One file, nothing else to install
  (libusb is inside). macOS: `xattr -d com.apple.quarantine rfoundry-*` once, then `chmod +x`. Windows: see "USB
  access" below.
- **pipx:** `pipx install "git+https://github.com/radiofoundry/radiofoundry#subdirectory=tools/rfoundry"`, or
  `pipx install ./tools/rfoundry` from a checkout.
- **From a checkout without installing:** `cd tools/rfoundry && python3 -m rfoundry ...` (needs `pip install pyusb
  libusb-package pyserial certifi`).

## Use

```
rfoundry flash --release latest                         newest release, downloaded and checked
rfoundry flash --release v26.11 --dry-run               show exactly which sectors would be erased and written
rfoundry flash rfoundry-dm1701-v26.11.bin                    a downloaded image (its .json manifest next to it)
rfoundry flash rfoundry_dm1701_codec.bin                     an image with the codec already merged in (see below)
rfoundry flash build/dm1701/rfoundry_dm1701.bin --board dm1701   a local build
rfoundry flash --release latest --stock "DM-1701(CSV)-V02.06.bin"
                                                        also write the codec from the stock firmware file
rfoundry probe                                          radio running RadioFoundry: can its codec be kept?
rfoundry detect                                         what's connected: RadioFoundry, and/or the bootloader
rfoundry key --stock "DM-1701(CSV)-V02.06.bin" --save   derive the bootloader key once and keep it
rfoundry download --release v26.11 -o DIR               just fetch and verify the files
rfoundry merge rfoundry-dm1701.bin --stock FILE -o merged.bin  a merged image for other flashing tools
rfoundry flash build/dm1801pro/rfoundry_dm1801pro.bin --board dm1801pro --port /dev/ttyUSB0
                                                        a DM-1801 Pro over its programming cable (below)
rfoundry flash build/cs7000p/rfoundry_cs7000p.bin --board cs7000p --unprotect --stock M17_PLUS_HOST_V9.00.96_20260303_1.bin
                                                        a first install through the processor's own bootloader:
                                                        removes the read protection, which erases the original
                                                        firmware, its bootloader and its codec for good, and
                                                        writes the codec from the maker's firmware file (below)
rfoundry flash build/cs7000p/rfoundry_cs7000p.bin --board cs7000p --unprotect --codec none
                                                        the same without a codec: FM works, DMR voice is off
```

`rfoundry flash` does, in order: load the image and check it against its manifest (SHA-256, the radio model, the
ranges); ask a running RadioFoundry radio for its codec over USB; decide the codec mode; print the plan; on a board with
`[flash.original_version]`, read the version the radio's original firmware reports while it runs (`origversion.py`,
nothing on the radio changes; `--no-version-read` skips it); tell you to put the radio in its bootloader (DM-1701: off,
hold PTT and SK1, on) and wait for it; erase and write; wait for the radio to restart, check what it reports, and store
the hardware option the version settled over the debug link (HWOPT) unless the radio already holds one
([web-flasher.md](../../docs/dev/web-flasher.md#the-original-firmware-version)).

### The codec

| Mode | Erases and writes | When |
|---|---|---|
| keep | the firmware's sectors only (DM-1701: within 3-9; MD-380: within 3-10); the codec sectors (DM-1701: 10, 11; MD-380: 11) are never touched | the radio runs RadioFoundry and reports a matching codec (`rfoundry probe`, or the start of `flash`); or `--codec keep` |
| merge | the firmware's sectors and the codec sectors, with the codec from the copy saved from the radio or from `--stock` | keeping isn't known to be safe and rfoundry has a saved copy or `--stock FILE`; or `--codec merge` |
| blank | the firmware's sectors, and the codec's sector erased and left blank | `--codec none`, only on a radio installed through [its processor's own bootloader](#the-processors-own-bootloader) |

A radio on a RadioFoundry build from before the codec moved reports `dm1701-v0206-csv` (the new layout's profile is
`dm1701-v0206-csv-s11`). Its codec code sits in sectors the update overwrites, so rfoundry writes the codec: from a
copy saved earlier, else from `--stock`. Stock firmware never holds the codec where RadioFoundry keeps it, so `--stock-installed`
does not make keep safe.

A radio whose codec is where an earlier release kept it (MD-380 `md380-d2032-s11`, MD-9600 `md9600-s11`) has it partly
in flash this release's firmware uses, so the install moves it, on both routes. Before anything is written rfoundry
reads the codec back over the debug link (every time, even with a copy saved before), checks it and saves it as
`codecs/<board>-<codec id>-<chip UID>.bin` in its settings folder, one file per radio; the codec is then merged in where
it goes now. Where the radio can't be read, only a copy saved from the same radio is used, else `--stock`. With
neither, or with `--codec keep`, the install is refused and nothing is written. An install that stops halfway is
finished by running it again (through the bootloader route if the radio now starts in its bootloader); rfoundry says
which copy or file finishes it ([codec-placement.md](../../docs/spec/codec-placement.md), "Moving to a new layout").

If rfoundry keeps a codec that turns out not to be there, nothing breaks: the firmware checks the codec before it
uses it, so the radio runs FM only and shows "No codec: voice off". Flash again with `--stock`.

An image with the codec already merged in (`rfoundry merge`, or the `_codec.bin` a build writes when the stock file is
in `codecs/`) needs no manifest or `--board`, and `--stock` only for the bootloader key: rfoundry checks every codec
window against the supported stock files by SHA-256, which also names the radio, requires 0xFF everywhere else past the
application, and flashes it in merge mode with the codec from the image. `--codec keep` writes only its application
part.

An image is one range from 0x0800C000. A release whose manifest lists two ranges (an application and a sector 9 data
part) is from before the codec moved, and rfoundry refuses it.

The stock files rfoundry takes as a codec source are each board's `[codec] stock` (`board.toml`, with the profiles in
`rfoundry/stockfw.py`; `tools/codec_extract.py --help` lists them, and rfoundry names them when it needs one). It checks
each by SHA-256 and refuses other files. A board with several stock files (one codec, each build's tables at its own
address) merges from any of them, each with its own layout.

`--stock` also takes a read of the radio's internal flash made while it ran its stock firmware
(`stub/stubtool.py PORT --progmem FILE`). The codec is plain there: rfoundry finds the build by its code window's SHA-256
at its stock address, checks every window and places the codec as a merge from the stock file would. A read gives no
bootloader key; that comes from a stock file, a key file or the radio.

### The bootloader key

The radio's bootloader needs a 1024-byte key. rfoundry derives it from the stock firmware file (any DM-1701 version)
and checks it against the key's published SHA-256, so `--stock` is enough; `rfoundry key --stock FILE --save` keeps it
for later runs without `--stock`; `--key FILE` takes a key file you already have. A radio that runs RadioFoundry gives the key
itself: rfoundry reads the stock bootloader over the debug link and takes the block with that hash
(`rfoundry/bootkey.py`), so an update needs none of these (`rfoundry key --save` keeps it from the radio). The key is
never part of RadioFoundry.

A radio installed through its processor's own bootloader has no bootloader key: `--key` and `rfoundry key` are
refused for it. Where its stock file is encrypted, the key is the file's own. rfoundry derives it from the file each
time, checks it against its published SHA-256 and never stores it.

### The saved codec

The codec stays in place across an update. rfoundry still reads a copy, the first time it sees the radio running RadioFoundry
with its codec: over the debug link, where stock keeps it or where the merge placed it, checked against the stock
codec's hashes, saved in its config directory. That copy is written instead of asking for `--stock` whenever the
radio's codec can't be kept or checked: the radio doesn't answer over the debug link, is back on stock firmware, or
reports no codec. `--codec keep` still wins. The copy also finishes an update that stopped halfway, when the radio can
no longer start RadioFoundry.

That read takes a few minutes, in a `--dry-run` too: rfoundry tries each layout the codec can have in the radio and stops at
the first that matches. It says so first, then adds a dot for each segment read. There is no percentage, because how many
layouts it needs isn't known in advance.

### The processor's own bootloader

A radio with `[flash] family = "dfuse"` (the CS7000 M17 PLUS) has no bootloader of its maker's that RadioFoundry
keeps: the image starts at the first byte of the program flash and is written through the bootloader in the
processor's ROM, by plain DfuSe. There is no bootloader key (`--key` is refused). `rfoundry flash` does, in order:

1. Load and check the image. On a board with a codec route, decide the codec ([below](#its-codec-step)). Print the
   plan (`--dry-run` stops here). An image that reaches past the application's part of the program flash is refused:
   it is not this radio's, and the bootloader must not erase flash the build keeps unused. Past that part the plan
   touches only the codec's sector, to write the codec there or to erase it.
2. Get the radio into its bootloader. A radio that runs RadioFoundry and answers on its debug link is restarted
   into it by rfoundry, where the board's firmware can do that (`[flash.bootloader] restart` in its `board.toml`):
   no key to hold. The codec step comes before it, while the radio still runs RadioFoundry and can say what codec it
   holds. Otherwise, and when the radio refuses, rfoundry waits for the bootloader (the board's key at power-on;
   rfoundry says which).
3. Check the device before it sends anything: not a radio maker's keyed bootloader (they share the USB IDs), and a
   flash layout that is this radio's chip. Anything else is refused.
4. Probe the read protection: a read of 4 bytes at the application base (on these radios the start of the program
   flash), which changes nothing. The bytes also say whether the radio holds a vector table.
5. On a board with a codec route, settle a keep by what the bootloader said, before any question and before anything
   is erased ([below](#its-codec-step)).
6. Protected, without `--unprotect`: stop and explain. Nothing was changed.
7. Protected, with `--unprotect`: ask (`--yes` answers), then send Read Unprotect. **The processor erases its whole
   program memory: the original firmware, its bootloader and its codec are gone for good**, and going back is the
   maker's own recovery procedure with the maker's files. Nothing shows on the screen. Leave the radio on and
   connected until rfoundry says to switch it off: it waits at least 60 s and until the radio has left USB (at most
   120 s), then asks you to switch it off and put it in its bootloader again. Both waits come from the processor's
   longest mass erase in the registry (`mass_erase_max_s`): at least twice that time, and at most twice the first
   wait; 60 s and 120 s are the least they can be. A radio that is back in its bootloader by itself is written at
   once, and rfoundry says to leave it on.
8. Erase the plan's sectors, the one with the vector table first. Write everything but the vector table's block
   and read it back, then write the vector table and read it back, then start the image. An install cut short
   leaves no vector table, so the radio starts nothing at a normal power-on until the same command has run again; a
   vector table whose write fails or that does not read back has its sector erased again. A block that reads back
   different stops the install with the image not started (the bootloader reports no error for a write it didn't
   carry out): run the same command again. If the radio shows nothing afterwards, switch it off and on.

#### Its codec step

A board with a codec route (`[codec] stock`) keeps its codec in the flash sector after the application's, with the
codec descriptor in that sector's last bytes. A board with `[codec] none` has no codec step, and `--stock`,
`--stock-installed` and `--codec` are refused for it.

The codec is decided as on the other radios: kept when the radio runs RadioFoundry and reports it, else written from
the saved copy or from `--stock FILE`, else refused. The stock file is the maker's firmware file the board lists;
rfoundry names it, and where to download it, when it needs it. Other versions of the maker's file and the maker's
bootloader file are refused with their own reason. A stock radio is read protected and can't be read, so the first
codec always comes from the file.

| Command | Radio | The codec's sector |
|---|---|---|
| `flash IMAGE --unprotect --stock FILE` | on the maker's firmware, read protected | written: the codec from the file |
| the same without `--stock` | the same | nothing is sent: refused, and the refusal names `--stock` and `--codec none` |
| `flash IMAGE --unprotect --codec none` | the same | erased and left blank: FM works, DMR voice is off until an install with `--stock` |
| `flash IMAGE` | runs RadioFoundry and reports its codec | not in the plan: the codec stays. Once per radio rfoundry reads it back and saves a copy |
| `flash IMAGE --stock FILE`, or with a saved copy | runs RadioFoundry and reports no codec | written |
| `flash MERGED_IMAGE` (`rfoundry merge`, or a build with the stock file in `codecs/`) | any | written: the codec from the image |

`--codec none` is this route's only: on every other route a codec sector is kept or written, never erased for
nothing. `--codec keep` never erases a codec sector on any route.

**A keep holds only on a radio that is not read protected and holds a vector table.** The program flash of these
processors has error correction on every flash word. A word that a power cut left half erased or half written can
fault when it is read, and RadioFoundry reads the whole codec at every start. So no image is started over a codec
sector that an unfinished install may have touched: the sector is written whole, erased whole, or left alone only
where the bootloader shows that the last install finished. The vector table shows it, because every install writes
it last, after everything else, the codec's sector included, is written and read back.

| The bootloader says | A keep (from a probe, remembered or not, or `--codec keep`) |
|---|---|
| read protected | void: removing the protection erases the codec with everything else |
| not protected, no vector table | void: the last install did not finish, so the codec's sector may be half written |
| not protected, a vector table | stands: the sector is not in the plan |

A void keep is decided again before anything is asked or erased: a merge from the saved copy, else from `--stock`.
With neither, or with `--codec keep`, the install is refused and nothing on the radio changes; the refusal names
`--stock` and `--codec none`. A merge and `--codec none` are never changed by what the bootloader says. The cost: an
update that was cut writes the codec again although it never touched it, because rfoundry can't tell that cut from
a cut merge.

The order of a merge: erase the sector with the vector table, the image's other sectors and the codec's sector;
write the image's sectors but the first, the codec's sector from its start to its end (the descriptor is in its
last block, after every codec block), then the rest of the first sector; read all of it back; write the vector
table and read it back; start the image. A descriptor is never whole over a half-written codec, and the firmware
checks the codec's bytes against it before it runs the codec.

The remembered probe is forgotten before the first erase of an install that erases a codec sector (on the keyed
route too), and before a Read Unprotect, so an install that erased the codec and was cut is never followed by a keep
on the word of a probe from before it.

What rfoundry can't rule out: another DfuSe tool that leaves the codec's sector half written and then writes a
vector table. RadioFoundry would then fault at every start until an install through the bootloader with `--stock`
or `--codec none`, which stays possible. The codec step has not run on a radio. The design is in
[docs/dev/release.md](../../docs/dev/release.md#the-codec-on-this-route).

`rfoundry detect` with the radio in its bootloader names the radios the device can be and says whether it is read
protected. An update of a radio that already runs RadioFoundry is the same command without `--unprotect` (the
protection is gone after the first install).

The restart in step 2 asks only the radio this run found on the debug link, and only when it reports this board;
`--no-probe` turns it off. With more than one radio that runs RadioFoundry connected, rfoundry restarts none unless
`--port` names the one. A radio may refuse it at that moment (its board's own rule): rfoundry says that nothing
was changed, prints the board's hint for what to do, asks on a terminal whether to try again, and otherwise goes on
with the key. If the radio takes the restart and no bootloader is on USB within 20 s, rfoundry says so and asks for
the key. Not yet tried on a radio.

Use rfoundry or the web flasher for these radios: both fill the image to whole 32-byte flash words, which another
DfuSe tool may not do. Nothing writes the processor's option bytes: rfoundry never selects the bootloader's second
alternate setting, and never sends an address outside the program flash. The design is in
[docs/dev/release.md](../../docs/dev/release.md#the-rom-bootloader-route).

### The DM-1801 Pro

The DM-1801 Pro has no USB data port: rfoundry talks to it over the programming cable (a USB-serial cable, found by
itself when it is the only one; `--port` names it). There are two ways in, and rfoundry wraps the tool that already does
each: the stub, `stub/stubtool.py` ([USER-GUIDE.md](../../stub/USER-GUIDE.md)), and Baofeng's update mode,
`tools/serial_loader.py --model dm1801pro`.

| Route | When (`--route auto`) | Writes | The codec slot (0x030E8000..0x030FFFFF) |
|---|---|---|---|
| stub | RadioFoundry answers on the cable and offers stub entry (built with `CONFIG_STUB_ENTRY=y`) | only the 4 KB sectors that differ, each checked by CRC; then the radio restarts | kept when the image has none (keep); written when it has one or `--stock` is given (merge) |
| bootloader | anything else: rfoundry asks you to switch the radio on with PTT and SK1 held, and waits `--wait` seconds for the loader | the loader erases 0x0300C000..0x030FFFFF, then takes the whole image | erased: it must be in the image (`rfoundry_dm1801pro_codec.bin`) or built from `--stock` (merge); keep is refused |

`--stock` here is a program flash read of the radio (`stubtool.py PORT --progmem FILE`). `tools/hrc7000_codec.py` checks
it by SHA-256 and builds the slot from it. `--route stub` or `--route bootloader` overrides the choice. A route the
radio can't take is refused before anything is written, and so is a radio that reports another board. After the restart
rfoundry reads VERSION over the cable and compares the git hash and the codec with what it wrote. Neither route writes
the bootloader below 0x0300C000.

The serial route loads `tools/serial_loader.py`, `tools/hrc7000_codec.py` and `stub/stubtool.py` from the checkout; the
rfoundry executable carries them. A `pipx` install from the subdirectory doesn't, and says so. Neither route of rfoundry
has flashed a radio yet; the tools it wraps have.

### USB access

| System | Once |
|---|---|
| macOS | nothing |
| Linux | a udev rule for the bootloader (0483:DF11), which rfoundry prints when access is denied; for the radio's serial port, membership of the `dialout` group (`sudo usermod -aG dialout $USER`, then log in again) |
| Windows | bind the bootloader ("Digital Radio in USB mode", 0483:DF11; a processor's own bootloader has the same USB ID and another name) to WinUSB with Zadig, once ([steps](../../docs/user/02-installing.md#windows-the-bootloader-driver)); rfoundry prints them when it can't open the bootloader. The radio's serial port needs no driver |

## For developers

| Module | Does |
|---|---|
| `rfoundry/boards.py` | per radio, read from `registry.json` (generated by `tools/boards.py` from `boards/*/board.toml`): flash sectors, application base, codec windows and profile, bootloader IDs, key hash |
| `rfoundry/plan.py` | which sectors to erase and write for an image and a codec mode: keep, merge, or blank (the codec's sector erased and left blank) |
| `rfoundry/manifest.py` | release manifests: generate (used by `tools/release.py`) and verify |
| `rfoundry/stockfw.py` | the STM32 radios' stock files: container, decryption, codec extraction and merge (also from a flash readout), key derivation (also used by `tools/codec_extract.py`) |
| `rfoundry/twinfw.py` | the stock container that holds its image twice: parse, the file's own key from the two copies, the codec windows |
| `rfoundry/dfu.py` | the USB DFU bootloaders: find, open, run a plan. With a key the AnyRoad bootloader; without one (`Dfu.run(plan)`) plain DfuSe to a processor's ROM bootloader, with `probe_protected` (which also keeps the first 4 bytes: `start_blank`), `read_unprotect` and `check_plain`, the route's limits on a plan |
| `rfoundry/debuglink.py` | VERSION / INFO_CHIP / READ_MEM over the debug link (read only) |
| `rfoundry/codecsave.py` | the codec read back from a radio that runs RadioFoundry (where stock keeps it, or where the merge placed it), checked by hash, saved in the config directory and written back where the radio's codec can't be kept or checked |
| `rfoundry/bootkey.py` | the bootloader key from a radio that runs RadioFoundry, found in its bootloader by the key's hash |
| `rfoundry/fakedfu.py` | fake bootloaders with a flash model, for the tests and `--simulate`: `FakeAnyRoad`, and `FakeRom` with its read protection, its option bytes alternate setting and an install cut at a block of the test's choice (`fail_at`) |
| `rfoundry/serialflash.py` | the HR_C7000 radios over the programming cable: the route choice, the codec slot, and the calls into `stub/stubtool.py` and `tools/serial_loader.py` |
| `rfoundry/release.py`, `state.py`, `cli.py` | GitHub downloads, the remembered probe, the commands |

Tests: `python3 -m unittest discover -s tools/rfoundry/tests -v` (no USB, no serial port; `tools/ci.sh` runs them).
`test_serialflash.py` runs both serial routes against the wrapped tools' own simulated radios. A whole flash
against the fake bootloader: `rfoundry flash IMAGE --simulate rfoundry-codec` (or `stock` with `--stock-installed "V02.06
CSV"`, or `rfoundry-nocodec --stock synthetic`). The processor's own bootloader, without an image file (a simulation
makes one up):

| `rfoundry flash --board cs7000p ...` | The simulated radio | Result |
|---|---|---|
| `--simulate --unprotect --stock synthetic` | on the maker's firmware, read protected | the whole first install: Read Unprotect, the image, the codec in its sector |
| `--simulate --unprotect --codec none` | the same | the same without a codec: its sector erased and left blank |
| `--simulate --unprotect` | the same | refused before anything is sent: no codec source |
| `--simulate --stock synthetic` | the same | stops at the read protection |
| `--simulate rfoundry-codec` | runs RadioFoundry with its codec, takes the restart into its bootloader | an update: the restart, and the codec's sector is not in the plan |
| `--simulate rfoundry-nocodec --stock synthetic` | runs RadioFoundry without one, takes the restart | the restart, and the codec is written |
| `--simulate rfoundry-refuses` | runs RadioFoundry with its codec, refuses the restart (reason 5) | the board's hint, then the keys, then the update |
| `--simulate blank --stock synthetic` | erased: no vector table | a merge; with `--codec keep` instead, refused |

`test_plain_codec.py` runs these, an install cut at every block and each case of the keep. A local build without a
manifest: `rfoundry flash build/dm1701/rfoundry_dm1701.bin --board dm1701`. Design and the sector map:
[docs/dev/release.md](../../docs/dev/release.md).
