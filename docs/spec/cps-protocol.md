<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# CPS protocol

How a PC programs the radio over USB. Section 2 describes the open-firmware shim, which is built. Section 3 drafts a native
protocol for later. The wire protocol the open-firmware tools speak is described in `open-firmware-cps.md`. This page says
only what *our* radio does with it.

## 1. On the host port

The radio has one host port: a USB CDC serial port on radios with USB (the board's `board_usb_ids`, 1FC9:0094 on
the STM32 radios), a UART on radios without. The host link
(`core/hostlink`) dispatches on the first byte of a burst:

| First byte | Goes to |
|---|---|
| `0xE0` | the MMDVM hotspot (`mmdvm-protocol.md`) |
| `0xA5 0x5A` | the debug link (`debuglink.md`) |
| anything else | the CPS shim (`CONFIG_CPS_OPENFW`), or ignored without it |

Once a CPS request is seen, the host link stays in CPS mode, and the shim gets every byte and a tick every pass
(`cps_feed(NULL, 0, now)`). The session ends at `C 5` (close) or after 10 s without a byte (`CPS_SESSION_MS`). A
debug link frame (`A5 5A`) arriving between CPS requests is still answered by the debug link, so bench tools can
watch a CPS transfer. While the shim holds part of a request (`cps_mid_request()`), a pause is no request boundary,
so the rest of the request is never taken for a debug or MMDVM frame. Limitation: a request whose length the header
doesn't give (`D`, unknown bytes) that pauses for more than 3 ms after a data byte of `0xA5` may still lose its
next bytes to a debug frame.

### 1.1 The port after a transmission

A radio's own transmission can take its USB port off the bus, or leave the USB controller deaf to the host: the host
sees a device and gets no answer from it, until a power cycle. A radio without VBUS sensing never sees a replug, so a
replug can't clear that. The host link (`core/hostlink`) asks for the port's reattach by itself when a transmission
left the port without a host: the controller from reset, as a power-on has it. Whether that brings back a deaf port is
not confirmed on a radio. The host link does so for a `board.host_port` that has `reattach` and `host`
(`hal_stream_t`): the USB port of `drivers/usb`. A UART host port has neither, and the rule does nothing there.

`host()` reads the port as `HAL_STREAM_HOST_UP` (configured, the bus awake), `HAL_STREAM_HOST_IDLE` (configured, the
bus suspended: a host that put an unused port to sleep, or a pulled cable without VBUS sensing) or
`HAL_STREAM_HOST_NONE` (not configured: no host yet, a host that reset the port and got no answer, the stack down).

| Step | Rule |
|---|---|
| On the air | `radio_on_air()` (the radio in `RS_TX`: a call, data, APRS, a beacon; or a repeater role's keyed slot) or `hotspot_transmitting()` |
| Owed | the most a host had of the port (`UP` above `IDLE`) at the host link's pass before the key-up or during the transmission. The pass before counts because the port can go at the key-up itself |
| Wait | `HOSTLINK_REATTACH_WAIT_MS`, 2 s, from the end of the transmission. A host finds a port that left the bus cleanly by itself in about 1 s; then nothing is asked. A port `UP` again at any time after the end owes nothing. A transmission during the wait restarts it at its own end. Nothing is asked while the radio transmits |
| Ask | `reattach()`, once, at the end of the wait: for a port owed `UP` that isn't `UP`, or one owed `IDLE` that is `NONE` (a host woke a sleeping port, reset it and got no answer). The USB task stops the stack, resets the controller and starts it again after 0.5 s off the bus, and again every 4 s for about a minute until a host takes the port (as a PA key run's flag does, [pa-keyup.md](pa-keyup.md) 4) |
| Not asked | for a port no host had (no cable); one already `NONE` before the key-up (a reattach no host took, a host that gave up on it); one that slept through the transmission and is still `IDLE` after the wait (an idle host, a pulled cable). The next ask needs a host to have had the port again |
| On the air during a reattach | the host link tells the port on every pass (`transmitting()`). No try resets the controller while the radio transmits: a try due then is made at the end, and the next 4 s after it |

The rule is `hostlink_reattach_check` and its wiring to the port `hostlink_port_after_tx`
(`core/hostlink/hostlink_policy.c`, host test `hostlinkReattachTest`). It runs in
the host link task, never the radio task: a reattach waits 50 ms for the reply before it to leave. The debug link's
`hotspot` section counts the asks (`usb.reattach_tx`), and each one logs `hostlink: host port lost after a
transmission: reattach N`.

In the network personality the reattach keeps the personality: the host finds the network adapter again, and its
interface goes down and up. Meanwhile the adapter reads as down (`usb.net_up=0`): the gateway sends nothing to it, and
the adapter's stall and quiet timers start again when the host configures it. A frame left queued for the host goes
out then. USRP voice over the adapter ([usb-audio.md](usb-audio.md)) stops with the link: what the radio sends meanwhile
is dropped. A PA key run's reattach does the same there. A program that had the serial port open opens it again.

Limits. A controller that misses both the host's bus reset and the idle bus keeps reading as `UP` and gets no
reattach; one asleep at the key-up that misses the host's bus reset keeps reading as `IDLE`, likewise. Neither is
seen so far: what a deaf controller reads is not known. A pulled cable that reads a bus reset at a transmission gets
one reattach and its retries, with no host to take them.

## 2. The open-firmware shim (`core/cps`, `CONFIG_CPS_OPENFW`)

`core/cps/cps_openfw.c` is the protocol engine with no RTOS or USB in it. `cps_port.c` hooks it to the host link
and the board's reboot. Host tests drive the engine directly (`tests/host/cpsTest.c`, built again with
`STORAGE_FACTORY_KEEP=1` as `cpsKeepTest` for the maker's bytes of 2.3, a power cut at every operation of a commit
included), and `tests/host/cpsSim` serves it on stdin / stdout for `tools/cps_client.py`.

Our own tools can move the same image faster with the debug link's CODEPLUG command: 4 KB blocks, CRC-32 sums so
only changed blocks move, LZ4 both ways and a staged commit ([cps-fast.md](cps-fast.md), `CONFIG_CPS_FAST`).

### 2.1 Request assembly

USB gives no request boundaries. A request is complete when its header says so:

| Request | Length |
|---|---|
| `R` area addr32 len16 | 8 |
| `X`/`W` 1 sector24 | 5 |
| `X`/`W` 2 addr32 len16 data, `X`/`W` 4 / 7 likewise | 8 + len |
| `X`/`W` 3 | 2 |
| `C` 2 (text) | up to the NUL after the text, at most 23 |
| `C` 6 sub | 3 (`C 6 7` clock: 7) |
| other `C` | 2 |
| `D` | ended by 3 ms of quiet (`CPS_GAP_MS`) |

A request still short of the length its header gives waits for the rest for up to 50 ms of quiet (`CPS_STALL_MS`),
then whatever came is handled (usually `-`), so a UART host port that delivers a request in pieces doesn't cut it
short.

A byte that no request starts with (anything but `R`, `W`, `X`, `C`, `D`) is dropped and counted (`cps.junk`), never
answered. A programming cable on the microphone / speaker jack now and then delivers a burst
of noise bytes (`0xFC`, `0xFE`) between requests. Answered `-`, such a burst was read as the reply to the next request.

A request longer than 2048 bytes (`CPS_REQUEST_MAX`) is dropped up to the next quiet gap and answered `-`
(`cps.overflows`). Several requests in one USB packet are handled in order. The debug link's `cps.openfw_eeprom` is
where the image starts in the flash: what a client adds to its addresses (0 on the STM32 radios, 0x700000 on the
HR_C7000 ones; `tools/cps_client.py --offset`).

### 2.2 Reads (`R`)

| Area | Answer |
|---|---|
| 1 | SPI flash at the physical address (the STM32 tools add the +128 KB offset themselves). Up to 32768 bytes per request (2045 in the original firmware, open-firmware-cps.md 2.1). The VFO A record (EEPROM 0x7590) is replaced by the live VFO (2.3) |
| 2 | the 64 KB EEPROM image (`openfw_eeprom` + addr), same rules |
| 5 | MCU ROM from `board.cps->rom_base`, inside `rom_size` (per radio, table below) |
| 9 | radio info, 46 bytes, structure version 3 (`open-firmware-cps.md` 4): radio type from `board.cps->radio_type()` (table below), git hash, build date `YYYYMMDDhhmmss` (the commit's time in UTC, as `git_time=` in `debuglink.md`; empty without git), flash id `0x40 << 8 \| log2(size)` (W25Q128: `0x4018`), features 0 |
| 10 | SPI flash security registers (`board.cps->security_read`) |
| 6, 7, 8, other | `-` (screen capture, WAV and AMBE are not offered) |

A range that runs off the end of the flash or the EEPROM image is refused with `-` before any data goes out.

The board's `hal_cps_t` (`board.cps`) supplies the radio-specific answers:

| Answer | Meaning |
|---|---|
| `radio_type()` | The radio type byte the CPS identifies the radio by. A board may report two values by hardware version |
| `rom_base`, `rom_size` | The internal flash range area 5 reads |

Each radio's values are in its hardware spec.

### 2.3 Writes (`X`, `W`)

`X 1 sector` reads the 4 KB sector into the shared storage buffer, `X 2 addr len data` merges bytes into it and
`X 3` writes it through the sector journal (`settings-store.md` 4). The commit reply comes after the sector is
written and verified (about 100 ms). Rules:

- **Writable**: the EEPROM image (0 to 64 KB) and the open-firmware flash image (`openfw_flash` to `openfw_end`). Anything
  else is refused at `X 1`. That includes the settings ring, the journal and the native area.
- **Foreign journal**: while the settings / journal region holds foreign data (`settings-store.md` 5),
  every `X 1` is refused, because a commit would erase the journal. Reads still work, so a backup can be taken
  first.
- **Quick-key slots** (EEPROM 0x7524-0x7537) hold the key assignment (`ui.md` 3.1) and are written like any
  other codeplug byte. A CPS that writes back a stale copy (an old codeplug file) brings back that file's keys, the
  same as for channels. Foreign values there are ignored: without RadioFoundry's marker in slot 0 the keys are the
  board's.
- **Calibration** (`calibration_addr`, `calibration_size` in the board's flash map; for example 512 bytes inside
  a 4 KB sector): only the window's bytes take the CPS's values, and the rest of
  the sector is kept. A board with no calibration window (size 0) takes the sector as plain data. *Open decision: refuse calibration writes entirely.*
- **The maker's bytes** (`hal_flash_map_t.factory`, a board that builds with `STORAGE_FACTORY_KEEP`;
  `settings-store.md` 4.1): a sector holding some is written like any other, and its factory bytes stay. `X 1` reads
  the stored sector, so a CPS that sends none of the range's bytes in `X 2` changes nothing there; bytes it does send
  for the range are dropped at `X 3`, which is acknowledged all the same (a CPS writing whole sectors from another
  radio's image keeps working). A sector that is factory bytes whole is never erased: `X 3` is acknowledged and
  changes nothing. A CPS that reads back to verify sees the radio's bytes in the range, not the ones it sent. Which
  boards list ranges: [feature-matrix.md](../feature-matrix.md).
- **VFO A** (EEPROM 0x7590): a commit whose VFO A record differs from what the radio would show (the CPS edited the
  VFO) is decoded into the settings store's VFO fields. A record written back unchanged is ignored. The VFO
  follows after the next boot or reload.
- `X 2` bytes outside the pending sector are ignored. `X 2` or `X 3` without an `X 1` is refused. `X 1` while a
  sector is pending drops it and is refused.
- `X 4` (EEPROM write) is acknowledged: the radio emulates the EEPROM in flash, and the tools write it with
  `X 1-3`. `X 7` (WAV buffer) is refused.
- `W 1-4` is the STM32 family's "fake write": acknowledged, nothing done.
- After the session (`C 5`, or the 10 s time-out), if anything was committed, the codeplug is re-read
  (`codeplug_reload`, `codeplug_serial` +1). The radio core re-reads its zone and channel without a reboot.

### 2.4 Control (`C`)

Every `C` request is answered `-`, which the open-firmware tools treat as "done".

| Request | Effect |
|---|---|
| `C 0` - `C 4` | programming screen, text, backlight: nothing (the radio keeps its own screen) |
| `C 5` | close: drop a pending sector, re-read the codeplug if written |
| `C 6 0` | save the settings, then reboot about 500 ms after the reply |
| `C 6 1` | reboot without saving |
| `C 6 2` | save the settings now (`settings_flush`) |
| `C 6` other | nothing |
| other | nothing |

`D` (the data-service host commands, `open-firmware-cps.md` 5) is answered `D sub 0`, meaning failed: there is no
data service in RadioFoundry yet.

### 2.5 qdmr

qdmr's open-firmware driver opens with `C 0`, `C 1`, `C 2` text, `C 3`, `C 6 3/4` and `C 6 2`. It reads area 9,
then area 1 in 32-byte blocks over its regions: EEPROM 0x80-0x6060 and 0x7500-0xB000, custom data 0x20000-0x211A0,
codeplug 0x9B000-0xAEE60. It writes with `X 1/2/3` per sector and ends with `C 5` and, for a write, `C 6 0`.
`tools/cps_client.py` does the same (`--regions qdmr`, the default) and is the test client:

```
python3 tools/cps_client.py info
python3 tools/cps_client.py read backup.bin                  # flat image: file offset = flash address
python3 tools/cps_client.py verify backup.bin
python3 tools/cps_client.py write edited.bin [--no-reboot]
python3 tools/cps_client.py --exec "tests/host/cpsSim --populate" info     # against the simulator
```

### 2.6 Counters

The debug link's `storage` view section (`debuglink.md` 3.7) shows `cps.requests reads commits refused overflows
last session pending`. A commit whose bytes for a factory range were dropped counts as a commit here and in storage's
`storage.factory` counters (`settings-store.md` 4.1), shown only on a board that keeps factory bytes.

### 2.7 Open decisions

1. A board with two radio types for one radio (by panel type, say): qdmr treats both as the same radio.
2. Area 1 reads of the settings area expose the privacy keys (`settings-store.md` 3.1). Should area 1 be limited
   to the open-firmware image?
3. Area 10 (security registers) is readable, factory calibration included. Should it be read-only, as now, or
   refused?
4. Calibration window writes (2.3).
5. A CPS writing VFO A while the user turns the VFO: the user's change wins until the next save, and the CPS's at
   the next reload.

## 3. Native protocol (draft, not built)

For a future native CPS and a qdmr driver. Same port and same first-byte dispatch, under its own first byte
(proposed `0xC0`, unused by MMDVM, the debug link and the open-firmware protocol).

- **Framing**: the debug link's framing (`debuglink.md` 2): length, command, payload, CRC-16, with a sequence number
  so a lost reply can be retried without doing the operation twice.
- **Records, not addresses**: `GET type index` / `PUT type index record` / `DELETE type index` for channel, zone,
  contact, listen list, general settings; `LIST type` returns the used bitmap. Records are the native records
  (`codeplug-native.md` 3), versioned per type.
- **Transactions**: `BEGIN` / `COMMIT` / `ABORT`. Puts inside a transaction are staged and applied in one pass, so
  a cut cable leaves the old codeplug, not half of the new one.
- **Settings**: `SETTINGS_GET` / `SETTINGS_PUT` by field name, never the privacy keys over a read.
- **Info**: the radio info of area 9 plus a capability list: the record types and versions the firmware knows.

Open: whether the open-firmware shim and the native protocol both stay forever, or the shim goes once qdmr speaks the
native one.
