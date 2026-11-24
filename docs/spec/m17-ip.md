<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# M17 over IP: the reflector client

The radio links one M17 channel to one module of an M17 reflector, through the host it is plugged into: the USB
network adapter or PPP ([usb-network.md](usb-network.md), [ppp.md](ppp.md)). While linked, PTT on an M17 channel
sends the call to the reflector instead of the air, the reflector's calls play when nothing is heard on the air, and
SMS goes both ways. Built with `CONFIG_NET_M17` (needs `CONFIG_M17` and `CONFIG_DMR_IP_GATEWAY`;
[build-config.md](../build-config.md)); which radios have it is in [feature-matrix.md](../feature-matrix.md).

| Part | Where |
|---|---|
| Packet codecs, the link, the hand-overs | `core/net/m17_net.c`, `m17_net.h` (the host link task, from `core/net/net.c`; in serial mode from `core/hostlink/hostlink.c`) |
| The network path of an M17 channel | `core/radio/radio_m17.c` (the radio task) |
| Tools > M17 reflector, Home's place line, the picker's rows | `core/ui/ui_m17.c`, `ui_menus.c`, `ui_screens.c` |
| Debug link | section `m17net`, RADIO op 44 sub-op 7 ([debuglink.md](debuglink.md)) |
| Tests | `tests/host` `m17NetTest`, `radioCoreTestM17Net`, `debuglinkM17TestNet`, `uiFlowTest`; `tests/hil/test_m17_reflector.py` |

## 1. Sources

| Source | Version | License | Used for |
|---|---|---|---|
| [M17 Protocol Specification Part II, Internet Interface](https://github.com/M17-Project/M17_inet) | v1.0 ([release](https://github.com/M17-Project/M17_inet/releases/tag/v1.0), 2026-10-14) | No notice of its own; the repository's LICENSE file is GPL-2.0. Prose and tables only | Everything below: packets, LSTN, timing, addressing |
| [M17 specification](https://github.com/M17-Project/M17_spec), the IP Networking chapter | commit 613d1b9 (2025-02-09), the last with the chapter | GNU Free Documentation License 1.3 | Cross-check of the packets |

Only facts were taken (field layouts, MAGIC values, port, timing). No text and no code: neither source contains code,
and no implementation (reflector, gateway or client) was read. The owner confirmed reading the Part II prose on
2026-11-23. This page restates the facts in its own words; the decisions where the sources are silent are this
firmware's (section 8).

## 2. Transport

| Item | Value |
|---|---|
| Carrier | UDP over IPv4, from the radio's own address on the link (`ipGatewayUdpSend`). The host routes it on (NAT): the radio has no route of its own and no DNS |
| Reflector port | 17000 unless the target says another (the sources make it configurable) |
| Our port | 17000 (`M17_NET_PORT`), bound with `ipGatewayUdpBind` |
| Byte order | Big endian, no padding |
| Addresses | 6 bytes, the M17 address encoding ([m17.md](m17.md) 10) |
| Accepted from | The target's address and port only; anything else is counted (`foreign`) and dropped |
| MAGIC | 4 ASCII characters at the start of every packet. One not known here is ignored (counted `unknown`), as the sources ask |

## 3. Control packets

| Packet | Bytes | Layout | Direction |
|---|---|---|---|
| CONN | 11 | `CONN`, our callsign (6), the module letter `A`-`Z` (1) | to the reflector |
| LSTN | 11 | the same with `LSTN`: listen only | to the reflector |
| ACKN | 4 | `ACKN` | from it: linked |
| NACK | 4 | `NACK` | from it: refused (no such module, blocked, or listen only refused there) |
| PING | 10 | `PING`, the reflector's designation (6) | from it, every 3 s |
| PONG | 10 | `PONG`, our callsign (6) | to it |
| DISC | 10 | `DISC`, the sender's callsign (6) | either way: a disconnect |
| DISC | 4 | `DISC` | from it: our disconnect is done |

The reflector-to-reflector forms of CONN and ACKN (37 bytes) are never ours: a known MAGIC at a length it doesn't have
here is counted `bad` and dropped.

## 4. Data packets

Stream packet, MAGIC `M17 ` (`4D 31 37 20`), 54 bytes:

| Offset | Bytes | Field |
|---|---|---|
| 0 | 4 | MAGIC |
| 4 | 2 | Stream ID: new for each transmission, the same in all its packets |
| 6 | 28 | LSD: DST, SRC, TYPE, META, as in the LSF without its CRC ([m17.md](m17.md) 3) |
| 34 | 2 | FN as on the air, the top bit set on the last frame |
| 36 | 16 | The frame's 16 stream bytes as on the air |
| 52 | 2 | CRC (M17's, [m17.md](m17.md) 2.8), over bytes 0-51 (GAP 1) |

Six packets carry a superframe's 324 bytes; the LSD is the same in each.

Packet-mode packet, MAGIC `M17P` (`4D 31 37 50`):

| Offset | Bytes | Field |
|---|---|---|
| 0 | 4 | MAGIC |
| 4 | 30 | The LSF with its CRC |
| 34 | n - 2 | The packet's data type (1 to 4 bytes, [m17.md](m17.md) 8.1) and data |
| 32 + n | 2 | CRC over the type and the data |

n is 4 to 825. The data has at least one byte after the type, so the packet is at least 38 bytes (37 is the
reflectors' own CONN and ACKN length, and they tell packets apart by length). A packet received is kept when its LSF
CRC and its data CRC hold.

## 5. The link

States (`m17_net.h`, `M17_NET_*`):

| State | Meaning | Left by |
|---|---|---|
| OFF | not linked, not trying | Link |
| CONNECTING | CONN (or LSTN) sent | ACKN: LINKED; NACK: OFF, refused; no answer after 5 CONNs: WAIT |
| LINKED | answering PINGs; streams and SMS flow | silence 30 s: WAIT, lost; the reflector's DISC: WAIT; Unlink: UNLINKING; the host link down: WAIT |
| UNLINKING | DISC sent | the 4-byte DISC (or the reflector's own DISC): OFF; 5 DISCs unanswered: OFF |
| WAIT | to link again later | its time: CONNECTING |

| Timer | Value | What |
|---|---|---|
| CONN retry | 2 s, 5 tries | then WAIT for 30 s, then CONN again (GAP 4) |
| PONG | on every PING (at the next poll), and our own every 5 s | the sources ask for one at least every 10 s |
| Silence | 30 s with nothing from the reflector | the link is lost: CONN again at once |
| DISC retry | 1 s, 5 tries | then OFF without the acknowledgment (the reflector times us out) |
| The reflector's DISC | 30 s | before linking again: it may be going down for a while |
| Host link down | | WAIT; linking again as soon as it is back. Nothing is sent while it is down: a Link asked then, or while the radio is in serial mode, waits in WAIT (`no_net`); an Unlink then is OFF at once, with no DISC |

Anything received from the reflector counts as its life, not only PING. Our own sends don't. Linking to another target
while linked sends one DISC to the old reflector (not waited for), then CONN to the new one. Unlink while not linked
forgets the last target.

A PING while CONNECTING is not answered: the link waits for ACKN. Streams and packets are taken only while LINKED.

## 6. Addressing

| Item | Rule |
|---|---|
| Our callsign in CONN, LSTN, PONG, DISC | our M17 callsign as it is (GAP 2) |
| The module | CONN's 11th byte |
| DST of our stream and SMS | the talk target; broadcast (All call) becomes the module form: the designation left-justified in 8 places, the module letter 9th, `M17-XYZ C` (two spaces after a 6-character one: `URFXYZ  C`). A callsign or a reflector service (`ECHO`, `INFO`) goes as it is (GAP 5) |
| DST received | Legacy reflectors rewrite it to the receiving client, newer ones turn the module form into broadcast. A call from the reflector is shown as is and never as "to us": no reply window opens on it |
| SMS received to the module | taken as to everyone on it (broadcast) |

Legacy reflectors forward only streams addressed to the module, and no packet data; newer ones forward both. A call to
a station through a legacy reflector is therefore not forwarded (unconfirmed, section 8).

## 7. The radio core's network path

Linked to talk (not listen only) and on an M17 channel (`radio_m17.c`):

| Item | Behavior |
|---|---|
| PTT | `tx_prepare` takes the network path: a new stream ID (the random pool where the build has one), the LSD from the channel (DST the talk target, SRC our callsign, TYPE voice 3200 or 1600 on the TX CAN, META zero). The radio core keys no transmitter (`radio_air_mode_t.tx_elsewhere`); the TX state, the TX timer and the time-out run as on the air. The voice path encodes the microphone; the radio task hands each 16-byte frame to the host link task as it comes (FN from 0); PTT released or the time-out: a last silent frame with the end bit |
| Refused | A channel set to encrypt (`TXDENY_REFUSED`: the network path doesn't encrypt, and never sends in the clear what was meant to be encrypted). The shared refusals and M17's own (no callsign, a signing key asked for and missing) as on the air |
| Not done over the network | Signing (the stream goes unsigned), META (text, position) |
| Reflector streams | Played through the voice path while the engine hears nothing on the air and nothing is sent. Voice streams in the clear only (3200 and 1600); data streams and encrypted ones are dropped; a signed stream's tail frames are not played and not checked |
| Jitter | A stream starts with 3 frames in hand (or fewer after 200 ms), then the radio task releases one every 40 ms; more than 5 in hand releases one more each tick. A stream with no frame for 500 ms has ended |
| The air first | A call on the air takes the voice path at once; reflector frames meanwhile are dropped. The two never write the voice path's queue together: each side says it is on, then looks at the other (`air_rx`, `net_pushing`); a hook that finds a hand-over under way keeps its start for its next frame and drops that frame; an air call's end needs no wait (a hand-over that began after its start saw `air_rx` and writes nothing) |
| View | A reflector call shows as a reception: `RADIO_M17_RX` and `PLAYED`, the caller as SRC and originator, the module as `via`, a new `call_seq`; its details and the heard list get it at its end |
| SMS | Sent to the reflector at once (no channel access), into the inbox as sent; one received is kept when it is ours ([m17.md](m17.md) 17.5), with the module form taken as broadcast |
| Listen only, or not linked | PTT, SMS and BERT on the air as without the client; a listen-only link still plays the reflector |

The rings between the tasks (`m17_net.h`): 8 frames from the reflector (320 ms), 4 of ours (160 ms), one SMS each
way. Frames the radio task has no room for are counted (`rx_dropped`, `tx_full`); what waits to go out when the link
drops is dropped (`tx_dropped`).

### 7.1 On the screen

| Where | What |
|---|---|
| Tools > M17 reflector (`m17-reflector`, above M17 BER test) | The state (Linked to / Connecting to `M17-XYZ C`, Not linked, or why: no answer, link lost, closed by the reflector, refused, waiting for the network, no UDP port free), then Reflector (the designation and module typed with the callsign entry: `M17-M17 C`), Address (IPv4, typed with the callsign entry's digits and `.`), Port (default 17000), Listen only, and Link or Unlink. Link needs our M17 callsign. What was typed is RAM only; the page opens on the link in force or the last one |
| Home's place line | On an M17 channel while linked: `Linked M17-XYZ C` where the zone is, on layouts that have a zone line |
| The destination picker | `ECHO` and `INFO` while linked (reflector services; not defined by the sources) |

The row sits above the BER test so the BER test and Heard keep their key paths. See [the M17 user page](../user/24-m17.md).

## 8. Gaps and the decisions taken

Gaps 1, 2, 5 and 7 are decided in one place each in `m17_net.c` (marked `GAP n`), gap 4's timers are constants in
`m17_net.h`, so a live test that settles one changes that place alone. None has been checked against a reflector: the
owner has not agreed to contact one (AGENTS.md rule 8), so every row below is **unconfirmed**.

| # | Gap | Decision | What would settle it |
|---|---|---|---|
| 1 | The stream CRC covers "the entire packet", which can't include the CRC | Sent over bytes 0-51 (MAGIC included). Received when it holds over 0-51 or over 4-51; the second is counted (`crc_alt`) | A capture from a reflector: `crc_alt` going up says the other reading |
| 2 | Older texts put the client's module in the 9th place of CONN's callsign; Part II doesn't | Our callsign as it is (`m17_net_from`) | A CONN accepted, and a PONG keeping the link |
| 3 | Whether the reflector checks that PONG's callsign is CONN's | The same callsign in both | A link that stays up past 30 s |
| 4 | What a client does after NACK, and how often it sends CONN | NACK ends it (no retry); CONN every 2 s, 5 times, then every 30 s | Not needed for interworking |
| 5 | DST of a client's own stream | Broadcast becomes the module form; anything else as it is | A call heard by another client on a legacy and on a newer reflector |
| 6 | Whether reflectors check the M17P data CRC | Always sent right | Not needed |
| 7 | The two-packet stream (`M17H`, `M17D`) of an older text, dropped from Part II | Not spoken: unknown MAGICs | Its return to the specification |
| 8 | The superframe's size (326 in an older text) | 324 (6 x 54), Part II's | Settled |
| 9 | Extended addresses with `#` | Not handled (shown as 12 hex digits, [m17.md](m17.md) 13) | A definition |
| - | Reflector services `ECHO`, `INFO` | Offered as destinations while linked; what they do is the reflector's | A reflector that has them |
| - | Packet mode's own port (17007 in the GFDL chapter) | One port for both: Part II names none | Not needed |
| - | The microphone with no transmitter keyed (a board question, not the protocol's) | The voice path's PCM stream runs for PTT to the reflector as for the air; the board's microphone route is assumed live without the transmitter keyed | A call to the reflector heard elsewhere, or `m17v.tx_mic_peak` above 0 after one, on each radio |

## 9. Not implemented

| Item | Why |
|---|---|
| Name lookup (a reflector by its designation, DNS) | No resolver on the radio: the address is typed |
| Persisting the target | RAM only: a settings field would need `settings_t` room ([settings-store.md](settings-store.md)) |
| IPv6 | The IP gateway is IPv4 only |
| Signing, encryption, META over the network | Section 7 |
| A signature check of reflector streams | Not done; their tail is skipped |
| Reflector-to-reflector links, the one-hop byte | Reflector side only |
| `M17H` / `M17D` | GAP 7 |

## 10. Debug link

Section `m17net` ([debuglink.md](debuglink.md) 3.7): `m17net.built` (0 without `CONFIG_NET_M17`, nothing else),
`state` (`off wait connecting linked unlinking`), `why` (`none user refused no_answer lost disc no_net no_port`),
`reflector`, `module`, `listen`, `ip`, `port`, `rx`, `tx` (a stream coming in, going out), `rx_src` (its caller, a space
as `_`), and the counters `rx_packets tx_packets conns ackns nacks pings pongs discs_tx discs_rx links losses
rx_streams rx_frames tx_streams tx_frames pkt_rx pkt_tx bad crc_alt unknown foreign rx_dropped tx_full tx_dropped
send_busy`.

RADIO op 44 sub-op 7: u8 0 unlink (2 bytes in all); 1 link, 2 link listen only, then the IPv4 address (4 bytes, big
endian), the port (u16 little endian, 0: 17000) and the designation and module as text (`M17-XYZ C`, 3 to 9 bytes).
REFUSED without our M17 callsign, BAD_ARG for a text or address that isn't one. `rfoundry_debug.py` has no command
for it yet; the HIL test sends it raw.

## 11. Tests

| Test | Checks |
|---|---|
| `m17NetTest` | The codecs against packets built in the test from sections 3 and 4 with its own address encoder and CRC; every state and timer of section 5 against a fake gateway; streams and SMS both ways through a fake reflector that sends ours back re-addressed; unknown MAGICs and wrong lengths; the rings; the `m17net` keys and their widest reply |
| `radioCoreTestM17Net` | The whole radio core with the gateway built and the real `m17_net.c`: PTT to the reflector with nothing keyed (FN, end bit, SID, DST, SRC, TYPE, CRC of every packet), a new SID per PTT, a reflector stream played and shown, the gap end, the air first, SMS both ways, an encrypting channel refused, listen only and unlinked back on the air. Every other M17 radio core test runs in it too |
| `uiFlowTest` | The page opens (`screen=m17-reflector`), each entry and its refusals, Link (CONN, LSTN), linked, refused, closed by the reflector, Unlink, no callsign, Home's place line, the picker's rows |
| `uiM17NetOff` | The UI's files with the client on and each M17 part off |
| `debuglinkM17TestNet` | RADIO op 44 sub-op 7 on the real client: link, listen only, unlink, and each refusal (length, kind, text, address, no callsign) |
| `tests/hil/test_m17_reflector.py` | Opt-in: links to the reflector the tester names (`RFOUNDRY_HIL_M17_REFLECTOR`), never a default host |

## 12. Cost

About 9.1 KB of flash on a Cortex-M4 radio and 10.1 KB on a C-SKY one (the client 4.9 KB, the radio core's network
path 2.3 KB, the screens 1.6 KB with their text in every language, the debug link 0.4 KB), and 1.2 to 1.3 KB of RAM
(the rings and slots 0.8 KB, the client's state and counters, the page's text), measured 2026-11-23. On by default
where M17 and the IP gateway are and the image keeps the room a default keeps
([build-config.md](../build-config.md#room-to-grow)); the figures per radio and where it is off are in
[feature-matrix.md](../feature-matrix.md#m17-on-each-radio).
