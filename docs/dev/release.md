<!-- SPDX-License-Identifier: MIT -->

# Releases

A release is a git tag on main named for its year and month (`v26.11`, below) and a GitHub release that carries,
for each releasable radio, the firmware image **without the AMBE codec**, its manifest, `SHA256SUMS`, the `rfoundry`
flashing tool as single-file executables for Linux, macOS and Windows, and the user manual as a PDF. Users never
build firmware: `rfoundry` downloads the image, checks it, and either keeps the codec already in the radio or writes
the codec from the user's own stock firmware file ([tools/rfoundry/README.md](../../tools/rfoundry/README.md)).

## Release names

A release is named for the year and month it comes out, so a user sees at a glance how old the firmware is.
`tools/release_version.py` holds the rule; the Makefile, `tools/release_build.sh`, the release workflow and the site
all ask it.

| Name | When |
|---|---|
| `vYY.M` | The first release of a month: `v26.11` |
| `vYY.M.N` | Another release that month: `v26.11.1`, `v26.11.2` |
| `vYY.MbN` | A pre-release (beta) of that name: `v26.12b1`. The final keeps the name even if it ships the next month |

The year has two digits (`26` is 2026) and a beta is a bare `b` and its count, to save room on the radios' small
displays. The month has no leading zero and the count starts at 1, so `v26.11.3` never reads as March. Main is the
alpha; there is no release candidate yet. `tools/release_build.sh` and the release workflow refuse any other tag.

**On the radio.** A clean build of a commit that carries a release tag gets `RFOUNDRY_RELEASE` (the Makefile reads the
tag; `make RELEASE=<tag>` sets it, as `tools/release_build.sh` does, and `RELEASE=` leaves it out). The splash screen
shows the release name in place of the commit, About shows both on their own lines (Release, then Build), and the debug link's VERSION adds `release=`
([debuglink.md](../spec/debuglink.md) 3.1). `RFOUNDRY_GIT` stays the commit everywhere else: the crash record's build
id, the manifests and the CPS id need it. The build service fetches tags, so a custom build of a tagged commit shows
the release name too.

**Betas.** A tag ending `bN` becomes a GitHub pre-release. GitHub's latest release skips pre-releases, and so does
the site: the flasher installs the newest release that is not a pre-release by default, and offers a newer beta only as
an opt-in version link (`?version=<tag>`, [website.md](website.md#flasher-notices)). `?version=main` still builds main.

**On the site.** The footer shows the firmware release the flasher installs, linked to its GitHub release, and a
Changelog link to the GitHub releases. The release notes start from GitHub's generated notes (`--generate-notes`):
edit them into a short list of what changed for users.

## Checklist

1. Main is green: `JOBS=8 tools/ci.sh` ends with "all checks passed" ([ci.md](ci.md)), on a machine with both compilers and `reuse`.
2. The project lead runs the similarity scan against the reference trees and reviews every hit
   ([docs/testing/README.md](../testing/README.md) 1).
3. A comprehensive legal scan of what the release ships, reviewed by the project lead: the license and notices of
   every file and dependency (`reuse lint`, `third_party/`, the rfoundry executables' bundled packages), the names
   and marks used in the docs, the site and the images, and nothing in the tree, the release or the site that the
   clean-room rules ([CLEANROOM.md](../CLEANROOM.md)) or "Never commit" ([AGENTS.md](../../AGENTS.md)) forbid.
4. The HIL tests pass on each radio the release claims, with the flags its features need.
5. rfoundry has flashed the release candidate on each radio the release claims, in both codec modes (below, "Testing
   rfoundry on a radio").
6. The user manual matches the release: the status of each setting in [Settings](../user/17-settings.md), the
   supported radios in the [Introduction](../user/01-introduction.md), the screenshots (`make -C tests/host ui-gallery`).
7. The README's radio status table is current.
8. Build the manual locally and read it through ([manual-pdf.md](manual-pdf.md)).
9. Update the contributors list and commit it if it changed: `python3 tools/gen_contributors.py` writes
   `CONTRIBUTORS` from the git history. CI never checks that the file is current, so a new contributor's pull
   request can't fail over it.
10. Tag and push the tag:

   ```
   git tag -a v26.11 -m "RadioFoundry 26.11"
   git push origin v26.11
   ```

11. The release workflow builds the firmware and rfoundry and attaches them; the manual workflow attaches the PDF
    ([manual-pdf.md](manual-pdf.md)). Edit the release notes on GitHub (they are the changelog the site links).
12. The next push to main redeploys the site, which then installs the new release (or offers the new beta).

## Release artifacts

| File | What |
|---|---|
| `rfoundry-<board>-<version>.bin` | The raw image `make BOARD=<board>` built, linked for the board's application base. No codec. A board with a first stage releases `rfoundry_<board>_install.bin`, the first stage then the application; `tools/release.py` refuses the application alone |
| `rfoundry-<board>-<version>.json` | Its manifest (below) |
| `rfoundry-<version>-<os>-<arch>[.exe]` | rfoundry, one file per system: `linux-x86_64`, `macos-arm64`, `macos-x86_64`, `windows-x86_64` |
| `SHA256SUMS` | SHA-256 of every file above |
| `RadioFoundry-manual-<version>.pdf` | The user manual (the manual workflow) |

`tools/release.py --board <id> --version v26.11` writes the first two (with `--variant amateur`, the image of `make
VARIANT=amateur` as `rfoundry-<board>-amateur-<version>.bin`, its manifest naming the variant: [build-config.md](../build-config.md#variants)) and the sums for a built image into `dist/`
(or `--out DIR`). It refuses an image that reaches into a codec sector or carries the codec's signature word, so a
release can never contain the codec, even by accident.

The manifest (`format` `rfoundry-release/1`, written and checked by `tools/rfoundry/rfoundry/manifest.py`):

```json
{
  "format": "rfoundry-release/1",
  "board": "<id>", "board_name": "<name>",
  "version": "v26.11", "git": "5f034440", "git_full": "5f03444001b77a00ebaef28d3358dd902c03474b",
  "image": {"file": "rfoundry-<id>-v26.11.bin", "size": 220780, "sha256": "dc8b8ed7..."},
  "ranges": [{"addr": "0x0800C000", "size": 220780}],
  "sectors": [3, 4, 5, 6],
  "codec": {"included": false, "profile": "<id>-<codec>-s11", "sectors": [10, 11],
            "windows": [{"addr": "0x080E3A74", "size": 66076}, {"addr": "0x080D19F0", "size": 73858},
                        {"addr": "0x080FFF80", "size": 128}],
            "keep_supported": true},
  "features": "FM,SUBTONE,DMR,...",
  "toolchain": "arm-none-eabi-gcc (Arm GNU Toolchain 13.3.Rel1 (Build arm-13.24)) 13.3.1 20240614",
  "variant": "full", "git_time": 1761840000
}
```

`git_time` is the commit's time, which the codeplug editor compares with a radio's `git_time=` (debug link VERSION) to
offer the release to a build that isn't one. Manifests from before it have none; the site's build then looks the
commit up in its clone. `ranges` says where the `.bin` goes: one range, the application at the board's base. `sectors` are the flash sectors
the image covers. `codec.windows` are where the merge puts the codec: on an STM32 radio the code, the tables (or the joined block)
and the descriptor ([codec-placement.md](../spec/codec-placement.md), "STM32 radios"). `codec.profile` is the id the firmware
reports for a matching codec (`codec=` in the debug link's VERSION, [debuglink.md](../spec/debuglink.md) 3.1).

## Sector maps

Each board's map comes from the board registry. The application region and the codec symbols come from its linker
script. The sectors are the platform's default geometry (`default_sectors` in `platform.toml`) from the application's
start up, or `app+slot` on the HR_C7000; a board's `[flash] sectors` overrides that. The protected range is the flash
from the platform's `flash_base` to the application. It is empty on a board whose bootloader is in the MCU's ROM
(`[flash] family = "dfuse"`): the application starts at `flash_base` and every sector of the chip is in the map.
`tools/boards.py json --for rfoundry` writes them to
`tools/rfoundry/rfoundry/registry.json`, which rfoundry, `tools/release.py` and the stub tools read; `tools/ci.sh`
fails when it is out of date. The STM32 codec windows come from the codec profile (`stockfw.PROFILES`), which rfoundry reads when it loads the
registry.

A flash sector can only be erased whole, so each board's linker script keeps the application out of the sectors that
hold the codec. Whether an update can leave the codec alone (keep) depends on that map:

| Family | Application | Codec | Keep |
|---|---|---|---|
| STM32F4 | The sectors from the application's start to the codec sectors | The tables stay at their stock address, the code moves into the last sectors, a descriptor says where ([codec-placement.md](../spec/codec-placement.md) "STM32 radios") | Yes: the update writes only the application's sectors |
| HR_C7000 | From the application base to the slot | A slot at the end of the region ([codec-placement.md](../spec/codec-placement.md) "HR_C7000 radios") | Per route: through the stub yes (it writes only the image's sectors), through an updater that erases the whole area no |
| STM32H7 | From the first byte of the flash to the codec's sector, or to the end of the first bank on a board with `[codec] none`. An STM32H743 has two banks of eight 128 KB sectors (0x08000000 and 0x08100000, RM0433 Table 15), numbered 0 to 15 in the registry; the linker script keeps the image in the first bank, and rfoundry refuses an image that reaches the codec's sector or the second bank ([the ROM bootloader route](#the-rom-bootloader-route)) | The last sector of the first bank, with the descriptor at its end, on a board with a codec route ([codec-placement.md](../spec/codec-placement.md) "STM32 radios"); none on a board with `[codec] none` | Yes: an update writes only the image's sectors ([the codec on this route](#the-codec-on-this-route) says when a keep is void) |

The exact map of a radio is in its hardware spec (`hardware-<id>.md`) and in `tools/rfoundry/rfoundry/registry.json`.

**What an interrupted update leaves.** All erases come first, then the writes, both in the board's sector order, so
sector 3 (the vector table) is the first one written back: an update cut off before that leaves the bootloader in DFU.
Cut off later, the radio may boot half an application: a crash, never a brick. Enter the bootloader by hand (the
board's key combination at power on) and flash again.

On a board whose bootloader is in the MCU's ROM the first sector of the plan holds the vector table and nothing in
program flash is needed to reach the bootloader, so an install or update cut off anywhere is flashed again the same
way. Every install through a ROM bootloader uses `vector_last` (rfoundry's `plan.py`, the web flasher's `plan.js`
`vectorLast`), at block level: the sector at `app_base` is erased first, every other block is written and read back,
and the vector table's block is written and read back last. A codec sector the plan writes or erases is among those
other blocks. An install cut short leaves no vector table, so at a
normal power-on the radio starts nothing until the same install has run again (the boot pin, not the vectors,
enters the ROM bootloader); no half image runs in between. `--dry-run` prints "order: sector N erased first and
written last (a cut install leaves no application to start)". The way in does depend on the chip's option bytes,
which is one reason no tool writes them ([the ROM bootloader route](#the-rom-bootloader-route)).

An install that moves the codec out of a former layout ([codec-placement.md](../spec/codec-placement.md), "Moving to a
new layout") also uses `vector_last`, by sector: it erases sector 3 first and writes it last. Cut off anywhere before
that last sector, sector 3 is blank, the bootloader stays in DFU and the same install, run again, finishes the move
from the saved copy; no half application runs in between.

## Releases from before the codec moved

Releases made before the codec moved on an STM32F4 radio carried a second range for read-only data and named the old
codec profile (without the layout's suffix, `-s<n>`). rfoundry and the web flasher refuse a manifest with two ranges or with an
old profile, and `tools/stm32_dfu.py` refuses a release image whose manifest lists two ranges.

## Codec handling

rfoundry picks one of two modes; `--dry-run` prints the exact sectors either would erase and write. The ROM
bootloader route has a third, blank, and settles a keep once more after the loader has answered
([the codec on this route](#the-codec-on-this-route)).

**Keep (the default when it is known to be safe).** Erase and write only the sectors the image covers; never touch the codec sectors, so the codec already in the radio stays. Flash can't be read back
over the bootloader (the radio is at RDP level 1), so rfoundry needs another way to know the codec is there:

1. **RadioFoundry is running.** Before the user switches the radio into its bootloader, rfoundry asks over the debug link
   (`rfoundry probe`, or automatically at the start of `rfoundry flash`). VERSION's `codec=` is the firmware's own check,
   the same one that gates DMR voice (on an STM32 radio: the descriptor, the signature word and the CRC-32 of both
   windows at their placed addresses). If it reports the board's codec profile (`codec.id` in the registry), keep is safe. If it reports
   the id of an earlier layout (the profiles' `former` entries, the registry's `codec_former`), the codec sits where this
   release's image goes: rfoundry reads it back over the debug link, checks it and merges it in the new layout
   ([codec-placement.md](../spec/codec-placement.md), "Moving to a new layout").
   The answer is remembered for 30 minutes (rfoundry's settings directory), because the debug link is gone once the
   radio is in its bootloader. The remembered answer is dropped before the first erase of an install that erases a
   codec sector (and on the ROM bootloader route before a Read Unprotect of a board with a codec route), so a cut
   merge is never followed by a keep on the word of a probe from before it. A build from before the codec moved
   reports the profile without the layout's suffix (`-s<n>`): its codec code is in sectors the update overwrites, so
   keep is not safe.
2. **Otherwise the codec is written (merge),** from the copy rfoundry saved from this radio if it has one, else from
   the stock file. A vendor firmware doesn't hold the codec where RadioFoundry keeps it, so `--stock-installed` never makes keep safe.
3. `--codec keep` forces keep in every case. Where the radio is known to have no codec, the keep expects none, so the
   check after flashing doesn't count `codec=absent` as a failure: the radio reported no codec, or the user says it runs
   its stock firmware (`--stock-installed`, or the web flasher's "Original firmware" with or without a version) and no
   stock version of the board holds the codec where RadioFoundry keeps it (the board's `codec.installed` is empty).
   rfoundry and the web flasher (`site/static/js/flasher/decide.js`) apply the same rules.

**The saved copy.** Whenever rfoundry or the web flasher sees a radio running RadioFoundry, it reads the codec over the debug
link (READ_MEM, in the stock, the placed or an earlier placed layout), checks it against the profile's hashes and saves it (rfoundry's
config directory, or the browser). It uses that copy instead of the stock file whenever the radio's codec can't be
kept or checked: no answer over the debug link, the radio back on stock firmware, or no codec reported
(`rfoundry/codecsave.py`, `site/static/js/flasher/codecsave.js`). There is no read-back during the update itself, so
a radio on a build from before the move needs the stock file once unless a copy was saved earlier.

Why a wrong guess is harmless: the firmware never runs the codec without checking it first (descriptor, signature and both CRCs, `boards/<id>/vocoder_<id>.c`). If the bytes in the codec's sectors aren't the expected codec, `present()` is false: FM
works, DMR calls show "No codec: voice off", and the debug link reports `codec=absent`. The fix is to flash again with
`--stock`. rfoundry checks this itself after flashing: it waits for the radio to restart and reads `codec=` again.
One case is not harmless: on a SoC whose program flash has error correction, a flash word that a power cut left half
written can fault when it is read, before any check runs. The ROM bootloader route has a rule for that
([the codec on this route](#the-codec-on-this-route)).

**Merge.** The user supplies the stock firmware file (`--stock`). rfoundry checks it by SHA-256 against the codec
profile, decrypts the two windows, checks their SHA-256 and the signature (`rfoundry/stockfw.py`, the same library
`tools/codec_extract.py` uses), places them as the board's codec map says and writes the descriptor
([codec-placement.md](../spec/codec-placement.md), "STM32 radios"). The image's sectors and the codec sectors are erased and written. The merged codec exists only in memory on the user's machine and in the radio.

**Getting the stock file.** rfoundry only reads a file the user downloaded; it doesn't fetch vendor firmware.

## The bootloader key

An STM32F4 radio's vendor bootloader XORs what it receives with a 1024-byte per-model key. The key is not in this
repository and not in a release. rfoundry and `tools/codec_extract.py` get it, in order:

1. `--key FILE`;
2. derived from the user's stock firmware file (`--stock`, or once with `rfoundry key --stock FILE --save`) and checked
   against the published key hash, so a wrong key can't pass (`stockfw.derive_key`);
3. a key saved earlier with `--save` (rfoundry's settings directory);
4. read from a radio that runs RadioFoundry, over the debug link (`rfoundry/bootkey.py`; the web flasher's `bootkey.js`
   does the same).

So a user needs one file from the vendor's firmware for the first install, which they need anyway for the codec, and
never handles the key. Updates of a radio that runs RadioFoundry need no stock file.

The key belongs to one family:

| `[flash] family` | Bootloader | Key |
|---|---|---|
| `usb-dfu` | The radio maker's, in the first sectors of program flash | The 1024-byte key above; the registry carries its SHA-256 (`key_sha256`) and first bytes |
| `dfuse` | The MCU's own, in ROM | None: the image is written as it is. `key_sha256` is null, `[flash.dfu]` takes no key, and `rfoundry key` refuses the board |
| `serial-loader`, `serial-iap`, `boot-rom` | Serial routes ([below](#the-serial-route-hr_c7000-radios)) | None |

## The ROM bootloader route

A board with `[flash] family = "dfuse"` is installed through the bootloader in its MCU's ROM by plain DfuSe (ST
AN3156). rfoundry's `Dfu.run` without a key (`rfoundry/dfu.py`) and the web flasher's `dfuse` backend do the same
steps; `rfoundry/fakedfu.py` `FakeRom` is the model both are tested against.

| Step | What happens | Why |
|---|---|---|
| Entry | The bootloader's keys at power-on (the board's `[flash.bootloader] steps`), or, where the board's firmware can and RadioFoundry runs on the radio, a restart the tool asks for over the debug link ([below](#the-restart-into-the-bootloader)). Everything after it is the same either way | The bootloader is in ROM: whatever is in program flash, the keys reach it |
| Device check | Before any DFU request. String 1 must not be a keyed bootloader's. The first interface string must be the board's `[flash.dfu] layout`, or without one a region that is the board's whole program flash | Nothing is encrypted, so no key stops an image from reaching the wrong device. Radio makers' bootloaders and other chips' ROM bootloaders have the same USB IDs |
| Idle by state | The protection probe, the Read Unprotect and the erase-and-write run each start with `Dfu.to_idle()` (the web flasher: `toIdle()`): GETSTATUS, then CLRSTATUS only in dfuERROR, a wait in dfuDNBUSY and ABORT in any other state, until dfuIDLE. `wait_idle()`, which sends CLRSTATUS in every state but dfuIDLE, remains the keyed route's way in | A ROM bootloader answers a request its state doesn't allow with a stall and an error. A bootloader that an interrupted run left in dfuDNLOAD-IDLE or dfuUPLOAD-IDLE is taken with ABORT |
| Protection probe | Set Address Pointer to the flash base, then an UPLOAD of 4 bytes. A read-protected chip answers `dfuERROR` with `errVENDOR` (AN3156 4.1). Nothing changes on the radio. Any other answer stops the install | A protected chip refuses every read, erase and write. "Protected" leads to the next step, so it is never concluded from an answer the documents don't describe |
| Read Unprotect | Only with `rfoundry flash --unprotect` (the web flasher: its own confirmed step), after a question. Command `0x92`. The chip removes the read protection and erases the program flash and RAM, except areas its option bytes keep (PCROP, secure-only; an STM32H74x loader V9.0 does not clear PCROP, AN2606 Table 146), and leaves the bus. Both tools take a lost GETSTATUS reply after `0x92` as the command taken: the device may leave at once. A reply other than `dfuDNBUSY` with status 0 is a refusal. Refused when the board lists a protected region in `progmem` (`protect.check_whole`) | It removes the original firmware and its bootloader for good: going back is the maker's procedure with the maker's files |
| Second entry | rfoundry waits until the device has left USB, at most the longest wait, and until at least the least wait has passed since the command. Both come from `cli.unprotect_wait(board)`: least = max(`UNPROTECT_MIN_S` 60, 2 x the platform's `mass_erase_max_s`), most = max(`UNPROTECT_WAIT_S` 120, 2 x least), in seconds. A board whose registry entry has no mass erase time is refused before the Read Unprotect. A USB bus that cannot be listed during the wait does not end the install: it counts as not seen to leave, and the wait runs to its end. Only then does it say to switch the radio off. If a bootloader is already back on the bus at a new address (the radio reset into it with its bootloader key still held), rfoundry writes to it at once and says to leave it on, with no switch-off line. Otherwise, once the radio is back, it prints "in its bootloader again: leave it on and connected while it is written" before the write. A device still on USB at the longest wait gets a warning that the erase is over or was not taken: that wait is at least four times the chip's mass erase bound. The probe runs again and must say unprotected | The chip does not restart by itself after the erase. Twice the bound, because a chip with two flash banks may erase them one after the other, and the RAM erase and option byte change that follow have no time given. The web flasher's `unprotectWait` (`dfuse.js`) applies the same rule with the same two floors, and a test compares it with rfoundry's own figures (the fixture `unprotect-wait.json`) |
| Plan check | Before the device is looked for, and again in `Dfu.run` (`check_plain` in `dfu.py`): every sector the plan erases or writes is in the board's program flash and either overlaps its application region (`app_region`, the linker script's) or is one of the board's codec sectors (`board.codec_sectors()`). A sector outside the application region is written with codec content only, or erased and left blank; an erase-only step is only ever on such a sector. Every block is inside its sector, and the plan has the vector-last order (the sector at `app_base` erased first and written last). An image that reaches past the region is refused, before any Read Unprotect. The web flasher (`checkPlain` in `dfu.js`) checks the same when the write step is drawn, before Connect | The bootloader's address pointer also reaches RAM, system memory and the option bytes. Flash past the application region and the codec's sectors is kept unused because the bootloader must not erase it: on an STM32H743 that is the second bank, where one ROM version leaves an erase early and the next flash command can hang (AN2606 Table 146, V9.1). A codec sector is one that holds a window of a profile's layout, and every layout ends at the descriptor, in the first bank; a test pins that for every board on this route |
| Erase and write | Both tools erase each sector of the plan, then send every 1024-byte block as block 2 right after its own Set Address Pointer. No vendor command, no XOR | The bootloader takes plain DfuSe requests. It places a block at (block number - 2) x wTransferSize + pointer, and AN3156 5.1 calls wTransferSize the length of the buffer the host sent, while the DFU functional descriptor has a field of that name. With block 2 the block's address is the pointer under either reading. The keyed route numbers its blocks from 2 after one Set Address Pointer per sector, as its bootloader expects |
| Whole flash words | Every block is filled with 0xFF to a multiple of 32 bytes (`FLASH_WORD` in `dfu.py`) | An STM32H7 programs 256-bit flash words and a partial one needs a force-write bit (RM0433 4.3.9). The documents don't say whether the bootloader forces the last partial word of a download. An image ends on a 4-byte boundary. A third-party DfuSe tool may not pad |
| Read back | Everything except the vector table's block is read back before that block is written (Set Address Pointer to the block, ABORT, an UPLOAD of block 2, ABORT) and compared with what was sent. The vector table is then written and read back; then Leave. A difference, or a read the bootloader refuses, stops the install without starting the image. Whatever fails once the vector table's write has begun (a write the bootloader reports as failed, a read it refuses, a mismatch, a USB error, a lost device) has the table's sector erased again (`Dfu._unvector`, the web flasher's `unvector`), after dfuIDLE is reached by state as above. If that erase cannot be sent, the message says "Its vector table could not be erased again (...): at a normal power-on the radio may start an image whose vector table did not pass its check". An error that is not the tool's own is reported as "USB error at the vector table: ..." | The bootloader reports no error for a write to a write-protected sector (AN3156 5.1, 5.3), and a block placed at another address than meant would go unseen. A vector table that is in flash and did not pass its check must not be left to start at the next normal power-on. The radio stays in the ROM bootloader, so the same command runs again |
| Leave | Set Address Pointer to `app_base`, a zero-length DNLOAD, GETSTATUS | The bootloader jumps to the image. rfoundry also says what to do if the radio shows nothing: switch it off and on |

**Option bytes are never written.** A ROM bootloader shows them as alternate setting 1 of its DFU interface. No tool
selects an alternate setting, so every request goes to alternate setting 0, the program flash; the fake fails any
request sent after one was selected, and a test runs every path against it. A wrong option byte can lock the chip for
good or take away the way into the bootloader.

### The restart into the bootloader

A board whose firmware restarts into the ROM bootloader when the debug link asks says so in its registry entry:
`[flash.bootloader] restart = true` ([board-registry.md](board-registry.md)), `bootloader_restart` in the views. The
request is `REBOOT` with the ROM bootloader mode ([debuglink.md](../spec/debuglink.md) 3.11), which the board or its
platform may refuse before it answers (reason 5, `hal_system_t.reboot_ok`). `rfoundry flash` then asks a radio that
runs RadioFoundry instead of asking for the keys (`cli.py` `restart_into_bootloader`), and the web flasher offers the
same as a button ([web-flasher.md](web-flasher.md#the-restart-into-the-loader)). Which boards: the registry
(`tools/boards.py get <id> flash.bootloader.restart` prints `true`), and each radio's own page says so.

| Case | rfoundry |
|---|---|
| Who is asked | The radio this run's probe found on the debug link, when its board name is this board's (the rule the codec decision has for a probe). Before the request the port is asked again: the board name and the chip UID must be the probe's. Never a remembered probe, another model, or another radio. With more than one debug link on the host (`debug_links`: the serial ports and the radios in USB network mode) and no `--port`, none is restarted: the probe took the first that answered, which is not the user's choice (the stub route's rule, `stubroute.run`). With `--no-probe`, with no radio found, or with a bootloader already on USB, nothing is asked and the keys' steps follow as before |
| The request | INFO_CHIP, the host's half of the gate (a chip family with a ROM bootloader, RDP level 0), then `REBOOT` mode 1 with the chip UID's low word. `debuglink.py` `Link.restart_into_bootloader` returns the reply's kind (`restart_outcome`) |
| Taken | The radio resets 250 ms after its reply. rfoundry lists USB every 0.5 s for at most `RESTART_WAIT_S` (20 s) and goes on with the device: the device check, the probe and the rest of the table above |
| Refused, reason 5 | "did not restart into its bootloader: it can't right now. Nothing was changed", then the board's `bootloader_restart_hint` when it has one. On a terminal rfoundry asks "Try the restart again?" as often as the user says yes. Otherwise, and with `--yes`, it prints the keys' steps and waits for the bootloader as before |
| Refused for another reason | Said as what it is (transmitting, a read-protected processor, a firmware without the command), then the keys' steps |
| Taken, and no bootloader within the wait | "took the restart, but no bootloader came onto USB within 20 s", with "it still runs RadioFoundry" when the debug link still answers (a check failed again at the reset). On Windows, when the radio left the debug link, the bootloader's driver steps follow: a bootloader without WinUSB is not listed. Then the keys' steps |

The kinds and their words are the same in both tools (`debuglink.py`, `link.js`); the fixture `restart-vectors.json`
holds rfoundry's, and `site/tests/dfuse.test.mjs` compares. `fakedfu.FakeRunning` is the running radio that
rfoundry's tests and `--simulate` talk to: it takes the restart (its bootloader then comes onto the fake bus), refuses
it with a reason, or answers OK and stays up. Both fakes check in the firmware's order (`dl_reboot_check`, then the
platform's `reboot_ok`) and answer the status with a reason byte, as the firmware does. The restart has not been tried on a radio.

Updates of a radio that already runs RadioFoundry use the same route. Its chip is no longer read protected after the
first install, so no Read Unprotect follows.

### The codec on this route

A board of this family with a codec route (`[codec] stock`) has a codec step: `--stock`, `--stock-installed` and
`--codec` apply as on the keyed route, and the codec's sector follows the application region
([Sector maps](#sector-maps)). A board with `[codec] none` has no codec step and takes no stock file. `--key` is
refused on both: the bootloader has no key. Nothing on this route has run on a radio.

A Read Unprotect erases the codec with everything else, and a radio on its maker's firmware cannot be read. So the
first codec always comes from the user's stock file. Later ones come from the copy the tools saved over the debug
link, or from the file again.

| Plan mode | The codec's sectors | Chosen when |
|---|---|---|
| keep | Not in the plan: `--dry-run` shows them as "keep" | The radio runs RadioFoundry and reports the board's codec id, and the loader then confirms it (below) |
| merge | Erased and written whole: the codec, 0xFF, the descriptor in the sector's last block | A stock file, a saved copy, or an image with the codec merged in |
| blank (`plan.BLANK`) | Erased and left blank; nothing is written there | `--codec none` (the web flasher: Install without the codec). The radio runs without DMR voice until an install with `--stock` |

`--codec none` is for this route only; another family refuses it. `--codec keep` never erases a codec sector, on any
route. `make_plan` refuses codec data in keep and blank mode, an image that reaches a codec sector, and blank mode
for a board whose image shares a sector with its codec.

**The hazard.** An STM32H7's program flash has error correction on every 32-byte flash word. A word that a power cut
left half erased or half programmed can read with a double error, and that read is a bus fault, not wrong data (from
the HAL's `HAL_FLASHEx_BusFault_IRQHandler`; the firmware has no handler that recovers from it). The boot check reads
the descriptor and every codec window with plain loads at every start, and the codec read-back over the debug link
reads the same bytes. So "a half-written codec fails its CRC-32" holds on an STM32F4 radio and not here: an image
started over a codec sector that an unfinished install touched could fault at every start. The rule:

> **No image is started over a codec sector that an unfinished install may have touched.** A codec sector is left as
> it is only when the loader shows that the radio is not read protected and holds a vector table. In every other
> case the plan writes the sector whole or erases it whole.

A vector table proves it. Every install on this route erases the sector at the application base first and writes
the vector table's block last, after every other block, the codec's sector included, was written and read back. A
radio with a vector table therefore finished its last install, and its codec sector is as that install left it:
blank, or a codec that was read back, or untouched since an earlier finished install. Blank flash reads clean. A
USB cut leaves whole blocks; only a power cut during an erase or a block write leaves a torn word, and then the
vector table is missing.

The loader's answer is the protection probe's: where the 4 bytes at the application base came, `Dfu.start_blank`
says whether they are 0xFFFFFFFF (the initial stack pointer of a vector table never is). **The keep is settled**
after that answer, before the Read Unprotect question, before the erase question and before `--yes` takes effect
(`settle_keep` in `cli.py`; the web flasher does the same in its write step):

| The loader says | Decision so far | Result |
|---|---|---|
| read protected | merge, or blank | unchanged |
| read protected | keep, whatever its reason (a remembered probe, `--codec keep`) | void: "the radio's program memory is read protected, and removing the protection erases the codec with everything else" |
| not protected, no vector table | merge, or blank | unchanged |
| not protected, no vector table | keep | void: "the radio holds no vector table: its last install did not finish, so its codec sector may be half written" |
| not protected, a vector table | any | unchanged: the codec's sector is whole |

A void keep is decided again, in this order: a saved copy of the codec and not `--codec keep`: merge from it; a
stock file given and not `--codec keep`: merge from it; otherwise the install is refused with nothing changed, and
the message names `--stock FILE` and `--codec none`. Where the decision changes, the codec is taken from its source
(which can still refuse a bad file with nothing erased), the plan is made and checked again, and the new decision
and plan are printed. `--dry-run` asks no loader: it says that a keep holds only for a radio that is not read
protected and has a vector table.

| Radio | Command | Decision |
|---|---|---|
| On the maker's firmware, protected | `flash IMAGE --board <id> --unprotect --stock FILE` | merge from the stock file |
| The same | without `--stock` | refused before the loader is asked; the message names `--stock FILE` and `--codec none` |
| The same | `--codec none` | blank: installed without a codec |
| The same, with a probe remembered from another radio or an earlier state | `flash IMAGE --unprotect` | keep at first; void once the loader says "protected"; merge from a saved copy, or refused |
| Runs RadioFoundry, `codec=<the board's id>` | `flash IMAGE` | keep: the codec's sector is not in the plan. Once per radio the codec is read back over the debug link and saved, as on every board |
| Runs RadioFoundry, `codec=absent` | `flash IMAGE --stock FILE`, or with a saved copy | merge |
| An install was cut (no vector table) | the same command again | a merge runs again whole. A keep is void: merge from the saved copy or the stock file, else refused |
| Any | `flash MERGED_IMAGE` (from `rfoundry merge` or `codecs/`) | merge from the image |

**The order of a merge.** Erase: the sector at the application base first, then the image's other sectors and the
codec's, in the board's order. Write: the image's sectors but the first; then the codec's sector from its start to
its end, in 1024-byte blocks (128 of them in a 128 KB sector, which a plan prints as "blocks 2..129": the codec's
blocks, blocks of 0xFF, and the descriptor in the last); then the rest of the first sector. All of that is read
back. Then the vector table's block is written and read back, and the image is started. Which block holds the
codec's last byte is the layout's (`hardware-<id>.md`).

| Cut | State left | The next install |
|---|---|---|
| During the erases | No vector table. The codec's sector holds the old codec whole, or is blank, or is half erased (a torn word is possible after a power cut) | Finds no vector table: it writes the codec's sector whole (merge) or erases it (blank); it never keeps it |
| While the codec's sector is written | No vector table. The descriptor's block is the sector's last: until every codec block is written, the descriptor's place is blank | The same |
| Inside the sector's last block | No vector table. A part of a descriptor, which fails its own CRC, over codec blocks that were all written before it; a torn word is possible | The same |
| A block the loader did not write (it reports no error) | The read-back before the vector table finds it; the vector table is not written | The same |
| Inside the vector table's block | `Dfu.run` erases the first sector again where it still can. Where it cannot, a part of a vector table may stand: by then every other block, the codec's sector's included, was written and read back | The codec's sector is whole either way |
| After the vector table | Everything was read back | |

A descriptor never validates over a half-written codec: its block is sent after every block before it, a whole
descriptor needs its last word (its CRC), and nothing reads it until an install has finished. It also never stands
in for the check: the firmware runs the codec only after both window CRC-32s match the bytes, and no tool trusts a
descriptor without checking the bytes. Erasing the codec's sector erases the descriptor with the codec: they share
the sector.

An update in keep mode erases and writes the image's sectors only. A keep-mode update that is cut leaves the
codec's sector untouched and no vector table; the next run cannot tell that from a cut merge, so it merges from
the saved copy or the stock file. That is the cost of the rule: after a cut update the codec is written again, and
a user with neither a saved copy nor the file is refused until one is there.

**What the tools cannot rule out.** Another program (a generic DFU tool) can leave a torn codec sector and then
write a vector table. RadioFoundry then faults at every start, in the boot check, until an install through the ROM
bootloader with `--stock` or `--codec none` writes or erases the sector; the ROM bootloader stays reachable. The
firmware does not guard the boot check's flash reads against this.

## The release workflow

`.github/workflows/release.yml`:

| Job | Does |
|---|---|
| `firmware (<toolchain>)` | One job per toolchain: `firmware (Arm)` and `firmware (C-SKY)`. Sets up the pinned compiler with `.github/actions/toolchain` ([ci.md](ci.md#toolchains)): the self-hosted image's, the Arm GNU Toolchain checked by SHA-256, or the csky-elf image, never a source build. Without csky-elf GCC on the runner and without the image, the C-SKY job fails. On a tag runs the gate: `tools/ci.sh --quick --steps registry,host,bench,python,licenses` on the runner, then `tools/ci.sh --quick --steps firmware --toolchain <id>` with the toolchain's compiler (through `tools/docker/csky-toolchain/run.sh` for the image). Then `tools/release_build.sh --toolchain <id>`, the same way: for every board on the toolchain with `[status] release = "stable"` (`tools/boards.py list --toolchain <id> --release stable`), it builds the default image and each build option value's image (`tools/boards.py builds`) and runs `tools/release.py` on each. Uploads them as `firmware-arm` or `firmware-csky` |
| `rfoundry` | On Ubuntu 22.04, macOS 14 (arm64), macOS 13 (x86_64) and Windows 2022: installs the package, runs its tests, builds a single-file executable with PyInstaller 6.11.1 (libusb from the `libusb-package` wheel, the `certifi` CA bundle, and `serial_loader`, `hrc7000_codec` and `stubtool` for the serial route inside) and runs `--version` on it |
| `publish` | On a `v*` tag only, after every toolchain's job and every rfoundry build passed: one `SHA256SUMS` over everything, checks every manifest against its image and that none includes the codec, creates the release if the manual workflow hasn't, uploads with `--clobber` |

On a pull request nothing is published and the release gate doesn't run (the pull request's own checks run the same
tests):

| What the pull request changes | What builds |
|---|---|
| `tools/rfoundry/` (with the `registry.json` a `board.toml` change regenerates), the serial flashing tools, `stub/stubtool.py` | `release.yml`: `version` and `rfoundry`, as artifacts |
| `release.yml` or the toolchain action | `release.yml`: everything, as artifacts |
| A `board.toml`, `board.mk` or `platform.toml`, `tools/release.py`, `tools/release_build.sh`, `tools/boards.py`, the toolchain scripts | `pr-tests.yml`: its `firmware (<toolchain>)` jobs run `tools/release_build.sh` after their builds, so every stable board is packaged as a tag would package it |
 A manual run on a tag rebuilds and
replaces the assets.

The executables are not code-signed. macOS marks downloads as quarantined: `xattr -d com.apple.quarantine
rfoundry-*` (or right-click, Open). Windows SmartScreen asks once.

## The serial route (HR_C7000 radios)

Radios on the serial-loader and serial-iap families are flashed over the programming cable, not DFU. rfoundry
(`rfoundry/serialflash.py`, [tools/rfoundry/README.md](../../tools/rfoundry/README.md)) doesn't speak either protocol
itself; it calls the tools that do:

| Route | Tool | Chosen by `--route auto` when | Codec |
|---|---|---|---|
| stub | `stub/stubtool.py` (`Radio.rfoundry_enter`, `write_image`, `reset_chip`) | VERSION answers on the cable with this board and the debug link's STUB info (op 0, [debuglink.md](../spec/debuglink.md) 3.18) carries its magic | keep: only the image's sectors are written, so the slot stays; merge when the image carries a slot or `--stock` builds one |
| bootloader | `tools/serial_loader.py` `Flasher` | otherwise | merge only where the loader erases the slot, so a plain image without `--stock` is refused |

`--stock` is a program flash read of the radio or the vendor's firmware file, depending on the board;
`tools/hrc7000_codec.py` recognizes it by the code window's SHA-256 and builds the slot. The release package never
carries the codec, so the first install of a release through update mode needs it. Later updates through the stub keep
the slot. Each radio's route, slot and keep rule are in its hardware spec (`hardware-<id>.md`,
[codec-placement.md](../spec/codec-placement.md) "HR_C7000 radios"), and the open items in its bring-up record under
[docs/testing/](../testing/README.md). Host tests (`tools/rfoundry/tests/test_serialflash.py`) run `write_image` against
stubtool's simulated radio and the update mode protocol against serial_loader's simulated loader.

## Testing rfoundry on a radio

Host tests cover the planning, manifests, the codec library, key derivation, the codec decision and complete runs
against a fake bootloader with a flash model (`python3 -m unittest discover -s tools/rfoundry/tests`; `tools/ci.sh` runs
them). `rfoundry flash IMAGE --simulate rfoundry-codec` runs the whole command against that fake. For a board on the
ROM bootloader route, `rfoundry flash --board <id> --simulate` stops at the read protection, and with `--unprotect` it
runs through the unprotect, the second entry, the write and the leave (`--simulate blank`: a chip that is not
protected). A board on that route with a codec route also takes `--stock synthetic` (a made-up stock file) or
`--codec none` there: without either, the simulated install of a radio on stock firmware is refused before the
loader is asked, as a real one is. Where the board's firmware restarts into the bootloader, `--simulate
rfoundry-codec` and `rfoundry-nocodec` are a running radio that takes the restart and `--simulate rfoundry-refuses`
one that refuses it (reason 5; with a codec route it holds its codec, so it needs no stock file), so both are seen
without a radio. The codec step comes before the restart, while the radio still runs RadioFoundry. A simulation
without an image file makes one up. What only a radio can
answer, in this order (the first flash of a new build is always a merge, so the radio has a known codec):

1. `rfoundry detect` with the radio in its bootloader.
2. Merge: `rfoundry flash <image> --stock <stock file> --dry-run`, then without `--dry-run`. The radio restarts;
   rfoundry reports the codec profile and "DMR voice is ready".
3. Keep, which proves the bootloader erases only the listed sectors: with RadioFoundry running, `rfoundry probe`, then
   `rfoundry flash <image>` without `--stock`. The plan must erase only the image's sectors and keep the codec's. After
   the restart rfoundry must report the codec still present. If it reports `absent`, the bootloader erased more than
   asked: keep must be disabled for that board (`keep_codec_supported=False`) and step 2 repairs the radio.
4. A DMR call both ways after step 3 (the codec works, not only its CRC).
