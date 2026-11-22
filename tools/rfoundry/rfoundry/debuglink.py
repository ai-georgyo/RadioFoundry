# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Just enough of the debug link (docs/spec/debuglink.md) to ask a running RadioFoundry radio what it is: VERSION (board,
git, codec=) and INFO_CHIP (the chip UID, to tell radios apart), and READ_MEM for the bootloader key (bootkey.py),
and HWOPT (3.25) for a hardware option an install learned (origversion.py), and REBOOT (3.11) into the processor's
own bootloader for an install through it. Only hwopt_set and restart_into_bootloader change the radio.
Needs pyserial for a serial port; a radio in USB network mode answers over UDP (udp:ADDR) without it.
"""
import ipaddress
import os
import re
import socket
import struct
import subprocess
import sys
import time

VID, PID = 0x1FC9, 0x0094
NET_PID = 0x0096	# the network personality (CDC-NCM, docs/spec/usb-network.md)
UDP_PORT = 3334		# the serial protocols over UDP at the radio's address in network mode
INDIVIDUAL_NET = ipaddress.IPv4Network("12.0.0.0/8")	# the host's address is 12.<DMR ID>, the radio's that xor 1
SOF1, SOF2 = 0xA5, 0x5A
CMD_VERSION, CMD_INFO_CHIP, CMD_READ_MEM, CMD_REBOOT, CMD_HWOPT = 0x02, 0x0B, 0x0C, 0x0D, 0x21
HWOPT_LIST, HWOPT_SET = 0, 1
ST_OK, ST_UNKNOWN, ST_UNAVAILABLE, ST_REFUSED = 0, 1, 4, 7	# a reply's status byte (debuglink.md 2)
REBOOT_BOOTLOADER = 1		# REBOOT's mode 1: reset, then straight into the processor's ROM bootloader
# INFO_CHIP's family byte: the ones whose option word carries the RDP level, and so have a ROM bootloader to enter
CHIP_STM32F4, CHIP_STM32H7 = 0, 2
CHIP_ROM_FAMILIES = (CHIP_STM32F4, CHIP_STM32H7)

# What a radio answered to the restart into its bootloader (REBOOT mode 1, debuglink.md 3.11), by kind. The web
# flasher has the same table (site/static/js/flasher/link.js restartOutcome), and a test compares the two
RESTART_OK = "ok"			# taken: the reply goes out first, the radio resets 250 ms later
RESTART_NOT_NOW = "not-now"		# reason 5: the board or its platform can't do it now (hal_system_t.reboot_ok)
RESTART_TRANSMITTING = "transmitting"	# reason 3
RESTART_PROTECTED = "protected"		# reason 2: no ROM bootloader on this chip, or RDP is not level 0
RESTART_TOKEN = "token"			# reason 1: the token was not the chip UID's low word
RESTART_NO_CHIP = "no-chip"		# reason 4: the radio could not read its chip info
RESTART_UNSUPPORTED = "unsupported"	# a firmware without the command (unknown, or not built in)
RESTART_OTHER = "other"			# anything else: reported as it came
RESTART_REASONS = {1: RESTART_TOKEN, 2: RESTART_PROTECTED, 3: RESTART_TRANSMITTING, 4: RESTART_NO_CHIP,
                   5: RESTART_NOT_NOW}
# The kinds in words, for the ones that are not the board's "not now" (the caller has the board's own hint for that)
RESTART_WHY = {
    RESTART_TRANSMITTING: "the radio is transmitting",
    RESTART_PROTECTED: "its processor is read protected, or has no bootloader of its own to restart into",
    RESTART_TOKEN: "the radio did not accept the request's confirmation word",
    RESTART_NO_CHIP: "the radio could not read its processor's identity",
    RESTART_UNSUPPORTED: "this firmware has no restart command",
}


def restart_outcome(status, data=b""):
    """The kind of a REBOOT reply: its status byte and the bytes after it (the reason, when refused)."""
    if status == ST_OK:
        return RESTART_OK
    if status in (ST_UNKNOWN, ST_UNAVAILABLE):
        return RESTART_UNSUPPORTED
    if status == ST_REFUSED and data:
        return RESTART_REASONS.get(data[0], RESTART_OTHER)
    return RESTART_OTHER


def restart_request(token):
    """REBOOT's payload for the restart into the bootloader: the mode, and the chip UID's low word as the token."""
    return struct.pack("<BI", REBOOT_BOOTLOADER, token)


class LinkError(Exception):
    """status: the reply's status byte when the radio answered with one that isn't OK, with the bytes after it in
    data (a refusal's reason); None when nothing usable came back."""
    status = None
    data = b""


def crc16(data, crc=0xFFFF):
    """CRC-16/CCITT-FALSE"""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(cmd, seq, payload=b""):
    body = bytes([cmd, seq]) + struct.pack("<H", len(payload)) + payload
    return bytes([SOF1, SOF2]) + body + struct.pack("<H", crc16(body))


def parse_frames(buf):
    """Yield (cmd, seq, payload) for each complete, CRC-valid frame in buf (bytes)."""
    i = 0
    while i + 8 <= len(buf):
        if buf[i] != SOF1 or buf[i + 1] != SOF2:
            i += 1
            continue
        n = struct.unpack_from("<H", buf, i + 4)[0]
        end = i + 6 + n + 2
        if end > len(buf):
            return
        body = buf[i + 2:i + 6 + n]
        if struct.unpack_from("<H", buf, i + 6 + n)[0] == crc16(body):
            yield buf[i + 2], buf[i + 3], bytes(buf[i + 6:i + 6 + n])
            i = end
        else:
            i += 1


def parse_kv(text):
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def find_ports():
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    return [p.device for p in list_ports.comports() if p.vid == VID and p.pid == PID]


class UdpSerial:
    """The radio's serial byte stream over UDP (udp:ADDR[:PORT], a radio in USB network mode), shaped like the pyserial
    calls the tools make: read, write, reset_input_buffer, in_waiting, close."""

    def __init__(self, addr, timeout=0.05):
        host, _, port = addr.partition(":")
        self.addr = (socket.gethostbyname(host), int(port or UDP_PORT))
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # The port sendto would take anyway, now: Windows refuses recvfrom on a socket not yet bound (WinError 10022),
        # and a link flushes its input before the first send
        self.sock.bind(("", 0))
        self.timeout = timeout
        self.pending = bytearray()
        self.port = "udp:%s:%d" % self.addr

    def _pull(self, wait):
        self.sock.settimeout(wait)
        try:
            while True:
                data, src = self.sock.recvfrom(2048)
                if src[0] == self.addr[0]:
                    self.pending += data
                self.sock.settimeout(0)
        except (socket.timeout, BlockingIOError, ConnectionResetError):
            pass    # ConnectionResetError: Windows reports an earlier datagram's ICMP port unreachable here

    def read(self, n=1):
        if not self.pending:
            self._pull(self.timeout)
        out = bytes(self.pending[:n])
        del self.pending[:n]
        return out

    def write(self, data):
        self.sock.sendto(bytes(data), self.addr)
        return len(data)

    @property
    def in_waiting(self):
        self._pull(0)
        return len(self.pending)

    def reset_input_buffer(self):
        self._pull(0)
        self.pending.clear()

    def flush(self):
        pass

    def close(self):
        self.sock.close()


def host_addresses():
    """The host's IPv4 addresses, from the system's own listing (no extra package): ip on Linux, ifconfig on macOS,
    ipconfig on Windows. Empty when none of them runs."""
    for cmd in (["ip", "-o", "-4", "addr", "show"], ["ifconfig"], ["ipconfig"]):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        if out:
            return sorted({ipaddress.IPv4Address(a) for a in re.findall(r"(?<![\d.])(\d{1,3}(?:\.\d{1,3}){3})(?![\d.])", out)
                           if all(int(x) < 256 for x in a.split("."))})
    return []


def network_adapter_addresses():
    """Linux: the host's addresses on adapters that are a RadioFoundry radio in network mode (USB 1FC9:0096), whatever
    their range (a fixed link anywhere). None where the system can't say which adapter is a radio."""
    out = []
    root = "/sys/class/net"
    if not os.path.isdir(root):
        return None
    for ifname in sorted(os.listdir(root)):
        dev = os.path.realpath(os.path.join(root, ifname, "device"))
        ids = []
        for f in ("idVendor", "idProduct"):
            try:
                ids.append(int(open(os.path.join(os.path.dirname(dev), f)).read(), 16))
            except (OSError, ValueError):
                break
        if ids != [VID, NET_PID]:
            continue
        try:
            text = subprocess.run(["ip", "-o", "-4", "addr", "show", "dev", ifname], capture_output=True, text=True,
                                  timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        out += [ipaddress.IPv4Address(a) for a in re.findall(r"inet (\d+\.\d+\.\d+\.\d+)", text)]
    return out


def radio_address_candidates(addresses=None, adapter=None):
    """Where a radio in network mode may be, from the host's side of the link (docs/spec/usb-network.md 3, 5.3): the
    host has 12.<DMR ID> and the radio that address xor 1; on a fixed link the host has the radio's address + 1.
    Nothing is sent to a network that isn't a radio's: where the system names the radio's adapter (Linux) only its
    addresses count, so a host without one gets no candidates; elsewhere only a 12.x address does, and a fixed link
    needs --port udp:ADDR."""
    if adapter is None:
        adapter = network_adapter_addresses()
    if adapter is None:     # the adapter can't be told from the others: the range only a radio's link has
        addresses = host_addresses() if addresses is None else addresses
        adapter, addresses = [], [a for a in addresses if a in INDIVIDUAL_NET]
    else:
        addresses = adapter
    out = []
    for a in addresses:
        c = ipaddress.IPv4Address(int(a) ^ 1) if a in INDIVIDUAL_NET else a - 1
        if c not in out:
            out.append(c)
    return out


def find_network_radios(candidates=None, timeout=0.4):
    """udp:ADDR for each radio in USB network mode that answers the debug link's VERSION"""
    found = []
    for ip in radio_address_candidates() if candidates is None else candidates:
        try:
            link = Link("udp:%s" % ip, timeout=timeout)
        except OSError:
            continue
        try:
            link.version()
            found.append("udp:%s" % ip)
        except (LinkError, OSError):
            pass
        finally:
            link.close()
    return found


class Link:
    def __init__(self, port, timeout=1.0):
        if hasattr(port, "write"):	# a stream that is already open: a simulated radio (fakedfu.FakeRunning)
            self.s = port
        elif str(port).startswith("udp:"):
            self.s = UdpSerial(port[4:])
        else:
            import serial
            self.s = serial.Serial(port, 115200, timeout=0.05)
            try:    # an FTDI cable passes received bytes on after 1 ms, not 16 (Linux only)
                self.s.set_low_latency_mode(True)
            except (AttributeError, NotImplementedError, OSError, ValueError):
                pass
        self.timeout = timeout
        self.seq = 0x40

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def request(self, cmd, payload=b""):
        self.seq = (self.seq + 1) & 0xFF
        self.s.reset_input_buffer()
        self.s.write(frame(cmd, self.seq, payload))
        buf = bytearray()
        end = time.monotonic() + self.timeout
        while time.monotonic() < end:
            buf += self.s.read(1100)
            for rcmd, rseq, data in parse_frames(bytes(buf)):
                if rseq == self.seq and rcmd in (cmd | 0x80, 0xFF):
                    if not data or data[0] != 0:
                        e = LinkError("command 0x%02X: status %s" % (cmd, data[0] if data else "?"))
                        if data:
                            e.status, e.data = data[0], data[1:]
                        raise e
                    return data[1:]
        raise LinkError("no answer to command 0x%02X" % cmd)

    def version(self):
        return parse_kv(self.request(CMD_VERSION).decode("ascii", "replace"))

    def read_mem(self, addr, size):
        """READ_MEM in 1000-byte pieces (internal flash, the stock bootloader included)."""
        out = bytearray()
        while len(out) < size:
            n = min(1000, size - len(out))
            r = self.request(CMD_READ_MEM, struct.pack("<IH", addr + len(out), n))
            if len(r) != 4 + n or struct.unpack_from("<I", r)[0] != addr + len(out):
                raise LinkError("READ_MEM answer doesn't match 0x%08X" % (addr + len(out)))
            out += r[4:]
        return bytes(out)

    def hwopts(self):
        """The board's hardware options: [{name, value (-1: none stored), restart, choices, state}]; state ok, ask
        (transmit waits for a person to set it), restart or hidden (the board detected it). A firmware without the
        state key: ask while nothing is stored, else ok."""
        kv = parse_kv(self.request(CMD_HWOPT, bytes([HWOPT_LIST])).decode("ascii", "replace"))
        out = []
        for i in range(int(kv.get("hwopt.count", "0"))):
            value = int(kv.get("hwopt.%d.value" % i, "-1"))
            out.append({"name": kv.get("hwopt.%d.name" % i, ""), "value": value,
                        "restart": kv.get("hwopt.%d.restart" % i) == "1",
                        "choices": kv.get("hwopt.%d.choices" % i, "").split("|"),
                        "state": kv.get("hwopt.%d.state" % i) or ("ask" if value < 0 else "ok")})
        return out

    def hwopt_set(self, option, choice):
        """Store a choice; LinkError when the radio refuses it (status 7: transmitting, or the board can't store it)."""
        self.request(CMD_HWOPT, bytes([HWOPT_SET, option, choice]))

    def chip_uid(self):
        """The 96-bit chip UID as hex, or None (INFO_CHIP: idcode u32, flash_kb u16, uid 12 bytes, ...)."""
        try:
            r = self.request(CMD_INFO_CHIP)
        except LinkError:
            return None
        return r[6:18].hex() if len(r) >= 18 else None

    def chip(self):
        """INFO_CHIP (debuglink.md 3.9) as far as a restart needs it: {"uid" (hex), "token" (the UID's low word, which
        REBOOT wants back), "family", "rdp" (the level; None where the family has none)}. LinkError when the radio
        gives no chip info."""
        r = self.request(CMD_INFO_CHIP)
        if len(r) < 28:
            raise LinkError("INFO_CHIP answered %d bytes" % len(r))
        family = r[28] if len(r) > 28 else CHIP_STM32F4	# firmware before the family byte was STM32F4 only
        return {"uid": r[6:18].hex(), "token": struct.unpack_from("<I", r, 6)[0], "family": family,
                "rdp": r[22] if family in CHIP_ROM_FAMILIES else None}

    def restart_into_bootloader(self, chip):
        """Ask the radio to restart into its processor's ROM bootloader (REBOOT mode 1) and return what it said, a
        RESTART_* kind. chip: this radio's chip(). The host's half of the gate comes first, as rfoundry_debug.py's
        (debuglink.md 3.11): a chip family with a ROM bootloader, at RDP level 0, else nothing is sent. On
        RESTART_OK the radio resets 250 ms after its reply, unless a check fails again by then (it then stays up).
        LinkError only when no reply came."""
        if chip["family"] not in CHIP_ROM_FAMILIES or chip["rdp"] != 0:
            return RESTART_PROTECTED
        try:
            self.request(CMD_REBOOT, restart_request(chip["token"]))
        except LinkError as e:
            if e.status is None:
                raise
            return restart_outcome(e.status, e.data)
        return RESTART_OK


def probe(port=None, log=None):
    """Ask a running RadioFoundry radio for its identity. Returns a dict (board, git, codec, features, uid, port) or None when
    no radio answers. Never raises for a missing radio or a missing pyserial."""
    ports = [port] if port else find_ports()
    if not port and not ports:
        ports = find_network_radios()	# a radio in USB network mode answers over UDP
    if not ports:
        return None
    for p in ports:
        try:
            link = Link(p)
        except Exception as e:
            if log:
                log("could not open %s: %s" % (p, e))
            continue
        try:
            v = link.version()
            uid = link.chip_uid()
        except (LinkError, OSError) as e:
            if log:
                log("%s: no debug link answer (%s)" % (p, e))
            continue
        finally:
            link.close()
        return {"port": p, "board": v.get("board"), "git": v.get("git"), "codec": v.get("codec"),
                "features": v.get("features", ""), "uid": uid}
    return None
