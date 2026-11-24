<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# M17 in the codeplug

The stored records of M17 channels and systems, on the generic layer of [channel-modes.md](channel-modes.md). Little
endian, except addresses: an M17 address is 6 bytes big endian, as on air ([m17.md](m17.md) 10). The firmware's codec
is `core/m17/m17_codeplug.c` and the base-40 codec `core/m17/m17_addr.c` (`CONFIG_M17`). The Codeplug Editor's is
`site/static/js/codeplug/mode_m17.js`. Both are tested against `tests/vectors/channel-modes.txt` (section 12).

| Id | Kind | System type |
|---|---|---|
| 6 | M17 | 6 (optional) |

A channel of kind 6 runs where the build has `CONFIG_M17` and the board's air interface carries M17
(`CODEPLUG_RUNS_M17` in `core/codeplug/codeplug_modes.h`, which is `radio_m17_fits()`). Elsewhere it is parked
([channel-modes.md](channel-modes.md) 6).

## 1. Addresses

Base 40 as [m17.md](m17.md) 10 gives. One codec in the tree, `m17_addr.c`: the codeplug, the engine and the UI call it.

| Text to address (`m17_addr_from_text`) | Rule |
|---|---|
| Letters | Upper-cased first |
| Trailing spaces | Dropped: `AB1CD` and `AB1CD   ` are the same address |
| Empty or all spaces | Refused: that is the reserved address 0 |
| More than 9 characters (after the trailing spaces) | Refused |
| A character outside `A`-`Z`, `0`-`9`, `-`, `/`, `.` and space | Refused, never read as a space |
| Leading and inner spaces | Kept: they are value 0 like any space |

| Address to text (`m17_addr_text`) | Text |
|---|---|
| Standard, 1 to `EE6B27FFFFFF` | Its characters, no trailing space; `EE6B27FFFFFF` is `.........` |
| Reserved 0, extended `EE6B28000000` to `FFFFFFFFFFFE` | 12 hex digits: the spec gives them no text |
| Broadcast `FFFFFFFFFFFF` | Empty: the UI and the editor show their own word, "All call" |

Examples: `AB1CD` is `9FDD51`, `N0CALL` is `4B13D106`, `W1AW` is `1680B7`.

## 2. Channel parameter block (kind 6)

The tag ([channel-modes.md](channel-modes.md) 2): an optional system of type 6, flag bit 0 transmit allowed. The
block, 12 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Bits 0-1 squelch: 0 CAN (calls on the channel's CAN), 1 any (every call), 2 addressed (the CAN, and DST is us, broadcast or the destination); 3 reads as 0. Bit 2: voice and data, Codec 2 at 1600 bit/s; clear: voice, 3200 bit/s. Bits 3-7 reserved: written 0, kept as found |
| 1 | 1 | CAN, bits 0-3 (0-15). Bits 4-7 are not read |
| 2 | 1 | TX CAN: bit 7 set = bits 0-3 hold it; clear = the CAN |
| 3 | 6 | Destination, big endian. All zero = broadcast (below) |
| 9 | 1 | Transmit encryption: 0 none, 1 scrambler, 2 AES. Any other value, or one this build lacks (`CONFIG_M17_SCRAMBLER`, `CONFIG_M17_AES`), reads as none |
| 10 | 2 | Traffic key ID, little endian: the key of the channel's system for that encryption (6) |

**Broadcast.** In memory, broadcast is `M17_ADDR_BROADCAST` (`FFFFFFFFFFFF`), as on air. The parameter block is the
only place it is stored as zeros: a zeroed block calls everyone. Decoding reads zero as broadcast; encoding stores
both `FFFFFFFFFFFF` and 0 as zeros. A block cut short of byte 9 has no destination: broadcast.

A short block reads with its missing fields at their defaults; a block that is absent or zeroed is CAN 0, CAN
squelch, 3200 bit/s, All call, in the clear. With flag bit 0 clear, receive only. A longer block from a newer
writer reads as usual, and a write from the radio keeps what follows byte 11.

The base record carries:

| Record field | Use |
|---|---|
| Name, RX and TX frequency, power, TX time-out, scan skip | As any channel |
| Mode, receive only | DMR, receive only, always (the safe stand-in), unless the tag's flag bit 1 says the record is a channel of its own |
| Contact, RX group list, DMR ID | Kept, not read: an M17 destination is an address in the block |

### 2.1 What a channel's call is set up from

`m17_conv_load`: the block, our address (3.1), and the system the tag names.

| Tag's system | `system` | Transmit allowed by the system |
|---|---|---|
| None | none | yes |
| An M17 system | it | its flag bit 1 ([channel-modes.md](channel-modes.md) 7.1): a listen-only system never transmits |
| Missing, or not of type 6 | none | no |

The channel's own transmit flag (tag flag bit 0) still applies.

### 2.2 Writing from the radio

`m17_conv_save` writes a channel as [channel-modes.md](channel-modes.md) 2.4 gives: the base record as the safe
stand-in, then the tag. A channel that is M17 already keeps its tag's system, fallback and other flags, byte 0's
reserved bits and the block's bytes after byte 11 (`codeplug_tag_rewrite`). Otherwise the tag is new, transmit
allowed from the channel (as for NXDN), reserved bits 0. A TX CAN equal to the CAN is stored as "not set".

## 3. Radio-wide record (kind 6)

Key 0x0A00 + 6 ([channel-modes.md](channel-modes.md) 5.5), 60 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 6 | Our address, big endian. 0: none (3.1) |
| 6 | 1 | Bit 0 Sign M17 calls (needs a signing key, 4). Bit 1 Send position in calls (with a GNSS fix, at most every 5 s). Bits 2-7 reserved: written 0, kept as found |
| 7 | 1 | GNSS station type: 0 fixed, 1 mobile, 2 handheld, 15 other ([m17.md](m17.md) 4.2); 3-14 read as 0 |
| 8 | 52 | Call text, UTF-8, zero padded. Sent as META text whenever it is not empty. The text ends at its first zero byte |

Absent, short or zeroed: no address, no signing, no position, fixed station, no text. The radio writes the record
(`m17_wide_save`, a codeplug settings write, a storage job's work) when the user types our callsign: the first 60
bytes, the flags' reserved bits and anything a later layout appended kept.

### 3.1 Our address

`m17_own_addr`:

| Record's address | Ours |
|---|---|
| A standard address | It |
| 0 | The radio name (the callsign field of the general settings, [open-firmware-cps.md](open-firmware-cps.md) 7.3.2) where it encodes as an address (1), else none |
| Extended or broadcast | None, and said so: the UI shows "M17 callsign not valid" and PTT is refused. The radio name never stands in for it |

## 4. System type 6

A system record ([channel-modes.md](channel-modes.md) 7.1) with identity length 0 and an empty type block: an M17
system is no network to match by, it holds keys. Its radio ID field is not used. It has no fallback channel and no
control channels. Its flag bit 0 ("needs its secret") is not used: signing is the station's choice.

### 4.1 The signing key (system secret)

Key 0x0300 + n ([channel-modes.md](channel-modes.md) 7.4), a secret (8):

| Offset | Size | Field |
|---|---|---|
| 0 | 32 | d, the P-256 private key, big endian ([p256.md](p256.md) 3) |
| 32 | 33 | Q = d·G compressed: 02 or 03, then X |

| Length | Read as (`m17_system_signing_key`) |
|---|---|
| 65 or more | d and Q. A byte 32 other than 02 or 03: Q is not stored |
| 32 | d alone, valid: the radio derives Q at its key check |
| Any other | Missing |

d is valid when 1 ≤ d ≤ n - 1, n the order of the curve ([p256.md](p256.md) 2, 9). The codec compares d with n's
bytes, every byte whatever d holds, and needs no curve code. An invalid d is missing. The radio checks d·G = Q once
per codeplug change. The caller wipes d after use, inside the secret work count ([p256.md](p256.md) 10).

**Which system signs** (`m17_signing_system`): the channel's system when it is an M17 system with a secret, else the
first M17 system (in system number order) that has one. The VFO and a channel on no system take the latter. None:
signing is off and Sign M17 calls can't be on. A system added by a zone share has no secret (7), so it never signs.

### 4.2 Station keys (system entries, class 128)

Key 0x0200 + n ([channel-modes.md](channel-modes.md) 7.2), class 128 (the type's own), group 0xFF:

| Body offset | Size | Field |
|---|---|---|
| 0 | 6 | The station's address, big endian |
| 6 | 33 | Its public key, compressed: 02 or 03, then X |

An entry shorter than 39 bytes, or whose key does not start with 02 or 03, is skipped; a longer one is read for its
first 39. Station keys are not secrets. A station may have several keys. Every one is the user's: a zone share
never brings one in (7).

| Call | Order |
|---|---|
| `m17_station_key(system, addr, i, key)` | The i-th key of addr: the system's own entries first, then every other M17 system's, each in entry number order. Verification tries at most 4 keys of a signer |
| `m17_station_key_at(i, ...)` | Every station key of every M17 system, in entry number order: the Station Keys list |
| `m17_station_known(addr)` | Any key on file for addr: whether "Not signed" is worth saying |

## 5. Fingerprints

The first 8 bytes of SHA-256 over the 33-byte compressed key, shown as 16 hex digits in groups of 4
(`6232 0E4E 537B 9C98`). The radio (`m17_key_fp`), the debug link and the Codeplug Editor show the same. A stored key
needs no curve work to show it.

## 6. Traffic keys

The generic traffic key record ([channel-modes.md](channel-modes.md) 5.4), system type 6's numbering:

| Algorithm id | Encryption | Key | Subtype from the length ([m17.md](m17.md) 6) |
|---|---|---|---|
| 1 | Scrambler | 1, 2 or 3 bytes, the seed; not all zero | 8, 16 or 24 bits |
| 2 | AES | 16, 24 or 32 bytes | AES-128, -192, -256 |

The key ID is 0-65535, unique for its algorithm within a system. `m17_system_key` returns a key only where the
channel's system is an M17 system and the length fits the algorithm. A key of another length reads as missing, never
cut or padded. An all-zero scrambler seed reads as missing: it would leave the generator at zero and send in the
clear. A key read but refused is wiped from the caller's buffer, and the generic reader wipes its own copy.

## 7. Sharing

A zone share ([zone-share.md](zone-share.md) 4.2) carries an M17 channel's tag as it is: a 12-byte block, inside the
share's limit of 64. A system at "identity only" goes as its type, flags and name; with no identity octets, a
receiver never has it, so its channels come in with no system, receive only, counted unavailable. A system at "whole
system with key" goes with its station keys (39-byte bodies). It is added at the receiver as a system of its own:
there is no identity to match an existing one by.

The signing key goes only at the whole level, and only when the sender turns keys on. A radio has no such switch and
never sends it. The share's limit for a secret is 64 octets, so a sender with keys on sends d alone (32 bytes).
Traffic keys are never shared.

**A radio imports neither.** An offer comes over the air from anyone, unauthenticated, and its screen shows a zone's
name and channel count, not keys. A station key is a trust anchor: one that came in a share would make the sender's
signatures under any callsign read "Signature OK" (`m17_station_key` reads every M17 system's keys), and a few of
them for one callsign would push that station's real key past the 4 keys a verification tries. A secret that came
in would sign the user's calls with a key the sender knows (`m17_signing_system` falls back to any M17 system with
one). So the import (`core/codeplug/zone_import.c`) adds the M17 system without its class-128 entries and without
its secret: its channels work, receive and transmit as the tag and the system's flags say, and signatures on them
are checked against the user's own station keys only. A key the user wants trusted, or a signing key, is added in
the Codeplug Editor, where it is shown with its fingerprint. The record keeps the station keys, for a reader that
shows them (the editor); a radio never stores them. `zoneImportM17Test` checks it, a crafted record with four
station keys for one callsign and a secret among it.

## 8. Secrets

The signing key (4.1) and the traffic keys (6) are secrets ([channel-modes.md](channel-modes.md) 8). After a write of
the codeplug settings area that holds one, the storage journal's copy of the sector is erased
([settings-store.md](settings-store.md) 4), so the debug link's storage dump never reads a key. Station keys are
public.

## 9. The Codeplug Editor

`mode_m17.js` on the channel-mode layer. The fields:

| Where | Field | Rule |
|---|---|---|
| Channel | CAN | 0-15 |
| Channel | Receive every CAN | Squelch "any"; off is "CAN". A block set to "addressed" by another tool keeps it |
| Channel | Voice | 3200 bit/s, or 1600 bit/s + data |
| Channel | Destination | A callsign (1, leading spaces kept as the radio keeps them), or All call (blank, or the word shown). An address no callsign spells reads and types as `0x` and 12 hex digits |
| Channel | Encryption | Off, Scrambler, AES. Offered as the connected radio's features say (`M17_SCRAMBLER`, `M17_AES`): none in the amateur and commercial variants, both with no radio connected |
| Channel | Key | The channel's system's keys of that encryption, by ID with their size |
| Radio tab | M17 callsign | The address of 3. Blank: the radio name stands in |
| Radio tab | Call text | Printable ASCII, at most 52 characters. A text left as it was keeps its stored bytes, also ones that aren't UTF-8 |
| Radio tab | Send position in calls, Sign calls | The flags of 3 |
| System | Signing key | 64 hex digits, hidden until asked for. Generate makes one with WebCrypto (P-256). The public key is shown with its fingerprint, and Copy public key copies it (65 bytes, 04 X Y, in hex). The editor stores Q with d (65 bytes), computing d·G itself |
| System | Station keys | Callsign and public key: 64 bytes (X Y), 65 starting with 04, or 33 starting with 02 or 03, in hex. Checked on the curve, stored compressed. The fingerprint column shows 5; a key WebCrypto won't import says so there |
| System | Traffic keys | Key ID and the key in hex, hidden until asked for. The algorithm follows the length |

The problems list:

| Code | English |
|---|---|
| `m17_callsign_chars` | Callsign: letters, digits, - / . only |
| `m17_callsign_long` | Callsign: up to 9 characters |
| `m17_callsign_all` | For everyone, choose All call (a destination typed as `ALL`, `ALLCALL`, `BROADCAST`, `EVERYONE` or `CQ`) |
| `m17_key_length` | Scrambler keys are 1 to 3 bytes, AES keys 16, 24 or 32 |
| `m17_key_zero` | A scrambler key can't be all zero |
| `m17_public_key` | Not a P-256 public key |
| `m17_sign_no_key` | Sign calls needs a signing key |
| `m17_key_missing` | The channel's key isn't in its system |
| `m17_text_ascii` | Call text: ASCII letters, digits and symbols only |

A signing key that isn't valid (d out of range, or a stored Q that isn't d's) is `mode_secret_bad`. A secret of a
length the radio doesn't read (4.1) is kept whole as found. A file export without keys leaves out the signing key and
the traffic keys, not the station keys.

## 10. qdmr

qdmr's own `m17:` channel element is dropped on import in version 1 (note `channel_type`). M17 channels and systems
travel in the `radioFoundry` block ([qdmr-yaml.md](qdmr-yaml.md) 3.7), keys never written.

## 11. Not stored

Per-zone records, targets and target lists: M17 uses none. The station type is not on the editor's Radio tab yet (it
keeps the byte).

## 12. Tests

| Test | Covers |
|---|---|
| `tests/host/m17CodeplugTest` (and `m17CodeplugClearTest`, the build without the scrambler and AES) | The vectors `m17conv`, `m17wide`, `m17call`, `m17secret` of `tests/vectors/channel-modes.txt`; every character of the alphabet; a channel's set-up and its write from the radio; our address; traffic keys of every length; the signing system; station key order; the storage journal after key writes, CPS commits, boots and power cuts |
| `tests/host/zoneImportM17Test` | Zone sharing: export and import of M17 channels, a whole system with station keys, an identity-only one, a d-only secret, the undo |
| `site/tests/modes.test.mjs` | The same vectors against `mode_m17.js`; the public key encodings against WebCrypto; the round trip through the image; the problems list; the fields per radio feature |
| `site/tests/qdmr.test.mjs` | qdmr's `m17:` channels dropped; the `radioFoundry` block with M17 |
