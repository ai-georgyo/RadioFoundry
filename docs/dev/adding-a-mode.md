<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Adding a Mode

How a digital mode beyond FM and DMR gets into the codeplug, the firmware, the Codeplug Editor and zone sharing. The
format is [channel-modes.md](../spec/channel-modes.md); this page is the order of work. A mode is a **kind** (what a
channel is) and, when its channels share a network or an identity, a **system type**. The generic layer never reads a
mode's bytes: the mode's own module does, in the firmware and in the editor.

## 1. Plan the records

Write the mode's spec first, `docs/spec/<mode>-codeplug.md`, linked from the spec index. Answer each row from
the table at the top of [channel-modes.md](../spec/channel-modes.md), "What a mode needs":

| Question | Home |
|---|---|
| What a channel holds beyond its base record (access code, options, a per-channel target) | The tag's parameter block (2.1): the layout, its default when zeroed, grown only by appending |
| Which base record fields the kind keeps using (contact, RX group list, color code...) | The spec says; the editor shows only those (`recordFields`) |
| Call targets | The contact list when they fit a 24-bit ID with a call type; else targets (5.2), with the address format |
| Radio-wide identity or options | The kind's radio-wide record, key 0x0A00 + kind (5.5) |
| A network, a system ID, sites, control channels, a channel plan | A system type: its block (identity first), its entry classes and their bodies (7) |
| An authentication secret, traffic keys | The system secret (7.4), traffic keys (5.4) with algorithm ids |
| Whether a new channel or system may transmit by default | The tag's flag bit 0, the system's flag bit 1 (listen only) |

Every value must read safely when zeroed or short, and unverified standard values stay out of the spec.

## 2. Claim the ids

Append one row to each of the three registries in the same change: the tables in
[channel-modes.md](../spec/channel-modes.md) 4, `CODEPLUG_KINDS` / `CODEPLUG_SYSTEM_TYPES` in
`core/codeplug/codeplug_modes.h`, and `KINDS` / `SYSTEM_TYPES` in `site/static/js/codeplug/modes.js`. A kind and its
system type share the id. `tools/check_modes.py` (in `tools/ci.sh`) fails when the three differ. Ids are never reused.

## 3. Firmware

| Step | Where |
|---|---|
| A switch | `CONFIG_<MODE>` in `config/features.mk` and `core/config.h`, off until the mode runs ([build-config.md](../build-config.md)) |
| The codec | `core/<mode>/<mode>_codeplug.c`: decodes the parameter block (`codeplug_tag_params`), the type's block and entries (`codeplug_records.h`) into the mode's own structs. Add the switch to `CODEPLUG_MODE_RECORDS` when it reads those families |
| On air | A mode whose frames the core builds describes its physical layer as an `air_profile_t` (symbol rate, levels, deviation, sync patterns; [air-interface.md](../spec/air-interface.md) 2) and runs its engine on an air session (7). Every board's provider then reports how well it fits (4), with no board change; the mode's radio core checks that fit before it offers the mode. Its RAM comes from memory an idle part lends (as P25 does, [p25.md](../spec/p25.md) 7) where it can |
| Built | The registry row's `built` column becomes the switch once an engine runs the kind. Until then a channel of the kind is parked (6) on every build |
| Host test | The codec against the shared vectors (`tests/vectors/channel-modes.txt`), with the switch on and off |

Nothing outside the mode's module tests a kind id: the radio core asks the registry (`codeplug_kind_built`) and the
mode module.

### 3.1 The radio core

An air-framed mode is one row of the air modes' table, `radio_air_mode_t` in `core/radio/radio_air_modes.h`, defined
in its module (`core/radio/radio_<mode>.c`) and listed under its switch in `radio_air_modes.c`. `radio_sm.c` loops
over the table: a new mode adds a row, never a case at each site.

| Row member | What the mode gives |
|---|---|
| `mode`, `key`, `name`, `features` | Its `radio_mode_t`, the debug link's and JSON's key (`radio_mode_key`), the word a display shows (`radio_mode_name`, never translated), its part of the feature string. Every table that names modes reads these: no copy of the names elsewhere |
| `fits`, `take`, `receives` | Whether the board's provider carries it (any task); a codeplug channel of its kind becomes the mode; what scan and dual watch may visit |
| `begin`, `end`, `running`, `retuned`, `apply` | The engine started on the channel and stopped; a retune; settings changed during a transmission |
| `poll`, `rx`, `heard`, `played`, `view_off` | Every tick, the engine into the mode's part of `radio_view_t`; what scan and the LED see |
| `tx_deny` | Refusals of its own beyond the shared checks, as new `tx_deny_t` values appended at the end (their debug link names in `debuglink_view.c` `deny_key`, under the mode's switch) |
| `auto_tx` | Transmissions of its own: `ONCE` (a packet) or `HOLD` (a test run until it says stop or the TX time-out) |
| `ops` | Transmit (`radio_air_ops_t`, `radio_air.h`) |

Requests from other tasks are `RREQ_<MODE>_*` with a `radio_req_<mode>_*` call each (`radio.h`); the radio task acts
on them. Work that takes long (a signature) runs one step a radio tick; `radio_task.c` sleeps a tick after a long one
([watchdog.md](../spec/watchdog.md) 4).

### 3.2 Memory

The air modes run one at a time, so they share one block: `radio_air_mem_get(mode, len)` at begin,
`radio_air_mem_put(mode)` at end (`core/radio/radio_air_mem.h`). A mode whose voice codec is in the tree runs in the
DMR vocoder's loan where the platform lends it (`hal_vocoder_t.lend`); the others, and every mode on a platform
without a loan, share a static arena sized for the largest built block (a union of the modes' memory types). Put
the engine and the voice path in one type, `<mode>_mem_t`, with its size asserted against a budget macro, and keep
statics outside the block small: they cost RAM on every radio that builds the mode, running or not.

### 3.3 Voice

A mode with its own codec does what P25 and M17 do (`core/p25/p25Voice.c`, `core/m17/m17_voice.c`): the codec runs in
the audio block callback on the board's PCM audio (`hal_audio_t`), fed through queues by the engine, with its working
memory in the mode's block. No RTOS calls and no waits in the voice path or the engine, so the host tests run them. A
mode whose voice is the DMR vocoder's (NXDN) goes through the DMR voice path instead and cannot use the loan.

### 3.4 Debug link and HIL

The mode's view section (`core/debuglink/debuglink_view_<mode>.c`, a `DL_VIEW_*` before `DL_VIEW_COUNT`) answers
`<mode>.built=0` in a build without it, so a HIL test can skip. A RADIO op for its settings and benches goes through
the `radio_req_<mode>_*` calls. Nothing secret is ever printed. The HIL tests (`tests/hil/test_<mode>.py`) get an
entry in `tests/hil/features.toml` and skip without a radio.

### 3.5 Fit

Measure the mode on every board (default and maximal; `tools/ci.sh` builds each switch off) and write the bytes in
[feature-matrix.md](../feature-matrix.md). Every default image keeps at least 3 KB of flash and 1.5 KB of linked RAM
free ([build-config.md](../build-config.md#room-to-grow)): a board that misses it turns the mode's parts off in its
`config.mk`, each with a `flash:` or `RAM:` comment and the bytes, in an order the mode's spec gives. A part the
board can't fit is `CONFIG_X ?= n`, never `BOARD_UNSUPPORTED` (that is for what the hardware can't do).

### 3.6 The radio's screens

The mode shares the screens: no menu of its own ([principles.md](../ux/principles.md)). What it adds:

| Piece | Where |
|---|---|
| Home and the call screen | a call builder in `ui_screens.c` (`call_builders[]`) that fills the shared `ui_call_t`; its chips and talk line |
| The mode's name | `ui_mode_name`, from `radio_mode_name` |
| Quick settings | its tile table (`quick_tables`), or FM's |
| The talk target picker | the mode's rows, asked from the mode (`pick_<mode>_fill`) |
| The channel editor | the mode's rows, shown only on its channels |
| Settings | rows by intent (Talk & listen, Privacy, Location, My radio), never a group per mode |
| Messages | a provider in the message table (`ui_svc_msg.c`), when the mode carries text |
| Addresses | `ui_addr_t` with a kind for the mode's addresses, drawn by `ui_sb_addr` |

Each new screen gets a `uiFlowTest` flow that opens it and checks `screen=<name>`; the gallery is regenerated
(`make -C tests/host ui-gallery`). Build the UI with each of the mode's sub-switches off (as `uiM17Off` does). Text
goes through `TR()` and into every catalog ([translating.md](translating.md)).

## 4. Codeplug Editor

Add `site/static/js/codeplug/mode_<mode>.js` and name it in the registry row, with the feature the radio's probe
reports for the mode. A kind module gives:

| Member | Meaning |
|---|---|
| `id`, `systemType` | The kind, and the system type its channels need (null: none) |
| `contacts`, `recordFields` | Whether its targets are contacts; the base record fields it uses |
| `ownFreqs` | Whether the channel keeps its own frequencies (false: the system gives them) |
| `defaultTx`, `fallback`, `flags` | Transmit on a new channel; whether it has a fallback channel; its flag bits 4-7 by name |
| `newParams`, `decode`, `encode` | The parameter block. `encode(params, prev)` keeps the bytes past what it knows |
| `fields()` | The form's fields, declared (`editor/modefields.js`: int, hex, freq, bool, select, list, hexbytes, text) |
| `validate`, `settings` | Problems of a channel; the radio-wide record's fields, when it has one |

A system type module gives `id`, `idLen`, `defaultTx`, `newFields`, `decode`, `encode`, `identity` (the text the
Systems tab shows), `fields()`, `classes` (each entry class: its fields, `decode` and `encode` of the body, whether
its entries are control channels), `secret` (null or its fields and check), `ownedKeys` (records of the type's own
family deleted with the system) and `validate`.

Text a user reads goes in `site/i18n/*.json` under `editor.<mode>.*`. Add the module to `site/tests/modes.test.mjs`:
the vectors, a round trip byte for byte, and its validation.

## 5. Sharing

Zone sharing carries a kind's tag and its system's bytes as they are ([zone-share.md](../spec/zone-share.md) 4.2),
within its limits (a parameter block of 64 octets, a type's block of 200, entry bodies of 64), so a mode that keeps
within them needs nothing there. A mode whose channels refer to targets needs the target part of zone sharing first.

## 6. Docs

Add the switch to [feature-matrix.md](../feature-matrix.md), and the mode to the user manual's
[Programming the radio](../user/04-programming.md#digital-modes-and-systems) when users see something new. The mode's
spec gets a "What this firmware does" checklist, a "Tests" section and a "Not written" list; the user manual a page
for the mode, linked from the chapters it touches (channels and the VFO, messages, scan, settings, the radio
reference), with the translated pages under `docs/i18n/`.
