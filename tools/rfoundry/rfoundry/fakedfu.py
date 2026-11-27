# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Fake bootloaders with a model of the chip's flash, for tests and rfoundry's --simulate.

FakeAnyRoad: the AnyRoad bootloader on an STM32F405. It answers the DFU requests of hardware-dm1701.md 15.2, decrypts
each block with the key, and programs a 1 MB flash image the way the chip does: erase sets a whole sector to 0xFF,
programming can only clear bits (writing over programmed bytes is recorded as a violation). Tests check what ends up
in flash, which sectors were erased, and that the codec sectors kept their bytes.

FakeRom: the bootloader in an MCU's ROM, plain DfuSe as AN3156 describes it, over the whole program flash of a board
on that route (family dfuse). No key and no vendor command; a read protection state in which it refuses to read, erase
and write until a Read Unprotect, which erases everything and takes the device off the bus; a second alternate setting
for the option bytes, which nothing may select.

FakeRunning: the same radio while it runs RadioFoundry, as its debug link answers: who it is, and the restart into
its processor's bootloader, which it takes or refuses.
"""
import random
import struct

from . import debuglink
from . import dfu as dfumod
from .plan import BLOCK, FIRST_BLOCK

FLASH_BASE = 0x08000000
FLASH_SIZE = 0x100000


class FakeAnyRoad:
    def __init__(self, board, key, flash=None, strings=None):
        self.board = board
        self.key = key
        self.flash = bytearray(flash if flash is not None else b"\xFF" * FLASH_SIZE)
        assert len(self.flash) == FLASH_SIZE
        self.state = dfumod.ST_IDLE
        self.busy = 0
        self.addr = None
        self.erased = []
        self.vendor = []
        self.blocks = 0
        self.violations = []
        self.exited = False
        self.strings = strings or {"string1": board.dfu_string1, "manufacturer": "STMicroelectronics",
                                   "product": "STM32  BOOTLOADER", "serial": "FAKE0001",
                                   "interfaces": ["@Internal Flash  /0x0800C000/01*016Kg,01*064Kg,07*128Kg"]}

    # flash helpers
    def read(self, addr, n):
        return bytes(self.flash[addr - FLASH_BASE:addr - FLASH_BASE + n])

    def _program(self, addr, data):
        off = addr - FLASH_BASE
        for i, b in enumerate(data):
            old = self.flash[off + i]
            if old != 0xFF and b != 0xFF and old != b:
                self.violations.append(addr + i)
            self.flash[off + i] = old & b

    # pyusb-like
    def ctrl_transfer(self, rtype, req, value, intf, data, timeout=None):
        if rtype == 0xA1 and req == dfumod.DFU_GETSTATUS:
            if self.busy:
                self.busy -= 1
                st = dfumod.ST_DNBUSY
            else:
                st = self.state
            return bytes([0, 1, 0, 0, st, 0])
        assert rtype == 0x21, rtype
        if req in (dfumod.DFU_CLRSTATUS, dfumod.DFU_ABORT):
            self.state = dfumod.ST_IDLE
            return 0
        assert req == dfumod.DFU_DNLOAD
        data = bytes(data or b"")
        self.state = dfumod.ST_DNIDLE
        self.busy = 1
        if value == 0:
            if not data:
                assert self.addr == dfumod.EXIT_ADDR, "exit without set address 0x08000000"
                self.exited = True
            elif data[0] == 0x91:
                self.vendor.append(data[1])
                self.busy = 0
            elif data[0] == 0x41:
                a = struct.unpack("<I", data[1:5])[0]
                sec = self.board.sector_at(a)
                assert sec is not None and sec.addr == a, "erase of 0x%08X, not a sector start" % a
                assert self.vendor == [0x01, 0x31], "erase before the vendor commands"
                self.flash[a - FLASH_BASE:sec.end - FLASH_BASE] = b"\xFF" * sec.size
                self.erased.append(a)
            elif data[0] == 0x21:
                self.addr = struct.unpack("<I", data[1:5])[0]
            else:
                raise AssertionError("unknown command %r" % data)
        else:
            assert value >= FIRST_BLOCK and len(data) == BLOCK, (value, len(data))
            dest = self.addr + (value - FIRST_BLOCK) * BLOCK
            sec = self.board.sector_at(self.addr)
            assert sec is not None and sec.addr == self.addr, "write region 0x%08X is not a sector" % self.addr
            assert dest + BLOCK <= sec.end, "block past the end of its sector"
            off = dest - self.board.app_base
            plain = bytes(b ^ self.key[(off + i) % len(self.key)] for i, b in enumerate(data))
            self._program(dest, plain)
            self.blocks += 1
        return len(data)


class Gone(Exception):
    """What a request to a device that left the bus gets (pyusb raises USBError: No such device)."""


class FakeRom:
    """An MCU's ROM bootloader (AN3156) on the board's chip: the whole program flash from rom_base, erased by the
    registry's sectors, written in flash words.

    protected: read protection is on (RDP level 1). Read, erase and write then answer dfuERROR with errVENDOR and do
    nothing (AN3156 4.1, 5.1, 5.3); Set Address Pointer and Read Unprotect work. Read Unprotect (0x92) erases the
    whole flash, clears the protection and takes the device off the bus (gone) until power_cycle().

    What the documents leave open is modeled the way that shows a mistake: a block that is not whole flash words
    loses its last partial word (partial), an UPLOAD refused for the protection reads nothing (stall_upload: it
    stalls instead), and a block is placed at (block number - 2) x its own length + the pointer, or x transfer_size
    when that is given (the other reading of AN3156 5.1's wTransferSize: the DFU functional descriptor's field).
    A request the state doesn't take is stalled and leaves dfuERROR, as the web flasher's fake has it: ABORT outside
    the idle states (dfuERROR among them, DFU 1.1 A.2.11), CLRSTATUS outside dfuERROR. What rfoundry must never send
    fails the test at once: a vendor command is recorded and answered with an error (refused), a mass erase, an address
    outside the program flash, a request after another alternate setting was selected (alt_selected) and a write to
    it are assertions."""

    def __init__(self, board, flash=None, protected=False, strings=None, stall_upload=False, transfer_size=None):
        self.board = board
        self.transfer_size = transfer_size
        self.base = board.rom_base
        self.size = board.flash_end - board.rom_base
        self.flash = bytearray(flash if flash is not None else b"\xFF" * self.size)
        assert len(self.flash) == self.size
        self.protected = protected
        self.stall_upload = stall_upload
        self.state = dfumod.ST_IDLE
        self.status = 0
        self.busy = 0
        self.pending = None		# what the next GETSTATUS carries out: a command runs on that request
        self.addr = None
        self.alt = 0
        self.alt_selected = []		# every alternate setting ever selected: must stay empty
        self.erased = []
        self.refused = []		# commands a ROM bootloader doesn't have (the keyed route's 91 xx)
        self.requests = []		# (bmRequestType, bRequest, wValue, data or length)
        self.sent = []			# the data of every block written, as it came
        self.blocks = 0
        self.partial = []		# addresses of flash words a short block left unwritten
        self.violations = []
        self.unprotected = False	# a Read Unprotect ran
        self.exited = False
        self.gone = False
        # the strings of a radio's ROM bootloader are not recorded yet (the registry's dfu_string1, dfu_product and
        # dfu_layout are null): these stand in. The layout is the registry's sector map in the DfuSe form
        self.strings = strings or {"string1": "STMicroelectronics", "manufacturer": "STMicroelectronics",
                                   "product": "ROM bootloader (simulated)", "serial": "FAKE0002",
                                   "interfaces": [rom_layout(board), "@Option Bytes  (simulated)"]}

    # flash helpers
    def read(self, addr, n):
        return bytes(self.flash[addr - self.base:addr - self.base + n])

    def _program(self, addr, data):
        off = addr - self.base
        whole = len(data) - len(data) % dfumod.FLASH_WORD
        if whole != len(data):
            self.partial.append(addr + whole)
        for i, b in enumerate(data[:whole]):
            old = self.flash[off + i]
            if old != 0xFF and b != 0xFF and old != b:
                self.violations.append(addr + i)
            self.flash[off + i] = old & b

    def _error(self, status):
        self.state, self.status, self.busy, self.pending = dfumod.ST_ERROR, status, 0, None

    def _in_flash(self, lo, hi):
        return self.base <= lo and hi <= self.base + self.size

    def power_cycle(self):
        """The radio switched off and on into its bootloader again: a new device on the bus."""
        self.gone = False
        self.address = getattr(self, "address", 0) + 1
        self.state, self.status, self.busy, self.pending, self.addr, self.alt = dfumod.ST_IDLE, 0, 0, None, None, 0

    def set_interface_altsetting(self, interface=None, alternate_setting=None):
        self.alt_selected.append(alternate_setting)
        self.alt = alternate_setting

    # pyusb-like
    def ctrl_transfer(self, rtype, req, value, intf, data, timeout=None):
        if self.gone:
            raise Gone("No such device")
        self.requests.append((rtype, req, value, bytes(data) if isinstance(data, (bytes, bytearray)) else data))
        if rtype == 0x01 and req == 0x0B:		# the standard SET_INTERFACE: an alternate setting
            self.set_interface_altsetting(intf, value)
            return 0
        assert self.alt == 0 and not self.alt_selected, "a request after alternate setting %r was selected" % self.alt
        if rtype == 0xA1 and req == dfumod.DFU_GETSTATUS:
            if self.busy:
                self.busy -= 1
                st = dfumod.ST_DNBUSY
            else:
                st = self.state
            reply = bytes([self.status, 1, 0, 0, st, 0])
            if self.pending == "unprotect":
                if self.protected:		# on a chip that isn't protected only the RAM is cleared (AN3156 3)
                    self.flash[:] = b"\xFF" * self.size
                self.protected, self.unprotected, self.gone = False, True, True
            elif self.pending == "leave":
                self.exited = self.gone = True
            self.pending = None
            return reply
        if rtype == 0xA1 and req == dfumod.DFU_UPLOAD:
            if self.state not in (dfumod.ST_IDLE, dfumod.ST_UPIDLE):
                raise Stall("Pipe error")
            if value == 0:				# the Get command: the commands the bootloader has
                return bytes([0x00, dfumod.CMD_SET_ADDRESS, dfumod.CMD_ERASE, dfumod.CMD_READ_UNPROTECT])
            assert value >= FIRST_BLOCK, "UPLOAD block %d" % value
            if self.protected:
                self._error(dfumod.ERR_VENDOR)
                if self.stall_upload:
                    raise Stall("Pipe error")
                return b""
            a = (self.base if self.addr is None else self.addr) + (value - FIRST_BLOCK) * (self.transfer_size or data)
            assert self._in_flash(a, a + data), "read of 0x%08X, outside the program flash" % a
            self.state = dfumod.ST_UPIDLE
            return self.read(a, data)
        assert rtype == 0x21, rtype
        if req == dfumod.DFU_CLRSTATUS:			# only an error is cleared; in any other state it is stalled
            if self.state != dfumod.ST_ERROR:
                self._error(0x0F)
                raise Stall("Pipe error")
            self.state, self.status = dfumod.ST_IDLE, 0
            return 0
        if req == dfumod.DFU_ABORT:				# out of an idle state of a transfer; dfuERROR takes none
            if self.state not in (dfumod.ST_IDLE, dfumod.ST_DNIDLE, dfumod.ST_UPIDLE):
                if self.state != dfumod.ST_ERROR:
                    self._error(0x0F)
                raise Stall("Pipe error")
            self.state, self.status = dfumod.ST_IDLE, 0
            return 0
        assert req == dfumod.DFU_DNLOAD, "request %d" % req
        data = bytes(data or b"")
        if self.state not in (dfumod.ST_IDLE, dfumod.ST_DNIDLE):
            raise Stall("Pipe error")
        self.state = dfumod.ST_DNIDLE
        self.busy = 1
        if value == 0:
            if not data:
                assert self.addr == self.board.app_base, "leave without set address 0x%08X" % self.board.app_base
                self.pending, self.busy, self.state = "leave", 0, 7		# dfuMANIFEST
            elif data[0] == dfumod.CMD_ERASE:
                assert len(data) == 5, "an erase without an address is a mass erase"
                a = struct.unpack("<I", data[1:5])[0]
                assert self._in_flash(a, a + 1), "erase of 0x%08X, outside the program flash" % a
                sec = self.board.sector_at(a)
                assert sec is not None and sec.addr == a, "erase of 0x%08X, not a sector start" % a
                if self.protected:
                    self._error(dfumod.ERR_VENDOR)
                else:
                    self.flash[a - self.base:sec.end - self.base] = b"\xFF" * sec.size
                    self.erased.append(a)
            elif data[0] == dfumod.CMD_SET_ADDRESS:
                a = struct.unpack("<I", data[1:5])[0]
                assert self._in_flash(a, a + 1), "address pointer 0x%08X, outside the program flash" % a
                self.addr = a
            elif data[0] == dfumod.CMD_READ_UNPROTECT:
                assert len(data) == 1, data
                self.pending = "unprotect"
            else:					# not a command of this bootloader (AN3156 Figure 5)
                self.refused.append(data)
                self._error(0x0F)			# errSTALLEDPKT
        else:
            assert value >= FIRST_BLOCK and 2 <= len(data) <= 2048, (value, len(data))
            assert self.addr is not None, "a block before any set address"
            dest = self.addr + (value - FIRST_BLOCK) * (self.transfer_size or len(data))
            sec = self.board.sector_at(dest)
            assert sec is not None, "block at 0x%08X, outside the program flash" % dest
            assert dest + len(data) <= sec.end, "block past the end of its sector"
            if self.protected:
                self._error(dfumod.ERR_VENDOR)
            else:
                self.sent.append(data)
                self._program(dest, data)
                self.blocks += 1
        return len(data)


def rom_layout(board):
    """The board's whole program flash as a DfuSe interface string ("@Internal Flash  /0x08000000/16*128Kg"): what a
    ROM bootloader of its chip shows on alternate setting 0, built from the registry's sectors."""
    runs = []
    for s in board.sectors:
        if runs and runs[-1][1] == s.size:
            runs[-1][0] += 1
        else:
            runs.append([1, s.size])
    return "@Internal Flash  /0x%08X/%s" % (board.rom_base, ",".join("%02d*%03dKg" % (n, size // 1024)
                                                                     for n, size in runs))


def rom_flash(board, rng):
    """A ROM-route chip's program flash with something in every byte (the maker's bootloader and firmware, or an
    earlier install)."""
    size = board.flash_end - board.rom_base
    return bytearray(rng.getrandbits(8 * size).to_bytes(size, "little"))


def stock_flash(board, rng, codec_segments):
    """Flash as stock firmware leaves it: random 'stock code' over the whole application area, the codec windows
    holding the given codec bytes, the bootloader area random too (never touched)."""
    flash = bytearray(rng.getrandbits(8 * FLASH_SIZE).to_bytes(FLASH_SIZE, "little"))
    for addr, data in codec_segments:
        flash[addr - FLASH_BASE:addr - FLASH_BASE + len(data)] = data
    return flash


def synthetic_key(seed=1701):
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(dfumod.KEY_LEN))


class FakeUsb:
    """Just enough of pyusb's module surface for cli's device code paths (find, strings, claim)."""

    def __init__(self, devices):
        self._devices = devices

        class _Core:
            class USBError(Exception):
                errno = None

            class NoBackendError(Exception):
                pass

            @staticmethod
            def find(find_all=True, idVendor=None, idProduct=None, backend=None):
                return [d for d in devices if not getattr(d, "gone", False)]	# gone: it left the bus

        class _Util:
            @staticmethod
            def get_string(dev, index):
                return dev.string_by_index(index)

            @staticmethod
            def claim_interface(dev, intf):
                pass

            @staticmethod
            def dispose_resources(dev):
                pass

        self.core = _Core
        self.util = _Util


class FakeUsbDevice(FakeAnyRoad):
    """A FakeAnyRoad that also looks like a pyusb device (descriptors, strings, one DFU interface)."""
    iManufacturer, iProduct, iSerialNumber = 2, 3, 4
    bus, address = 1, 9		# where it enumerated: the bootloader is a new device after the radio restarts

    class _Intf:
        bInterfaceClass, bInterfaceSubClass, bInterfaceNumber, iInterface = 0xFE, 0x01, 0, 5

    def __iter__(self):
        return iter([[self._Intf()]])

    def string_by_index(self, i):
        return {1: self.strings["string1"], 2: self.strings["manufacturer"], 3: self.strings["product"],
                4: self.strings["serial"], 5: self.strings["interfaces"][0]}.get(i)

    def is_kernel_driver_active(self, intf):
        return False

    def set_configuration(self):
        pass


class FakeRomUsbDevice(FakeRom):
    """A FakeRom that also looks like a pyusb device: one DFU interface with an alternate setting for each of its
    interface strings, the program flash first, then the option bytes."""
    iManufacturer, iProduct, iSerialNumber = 2, 3, 4
    bus, address = 1, 11

    class _Alt:
        bInterfaceClass, bInterfaceSubClass, bInterfaceNumber = 0xFE, 0x01, 0

        def __init__(self, alt):
            self.bAlternateSetting, self.iInterface = alt, 5 + alt

    def __iter__(self):
        return iter([[self._Alt(i) for i in range(len(self.strings["interfaces"]))]])

    def string_by_index(self, i):
        names = dict(enumerate(self.strings["interfaces"], 5))
        return {1: self.strings["string1"], 2: self.strings["manufacturer"], 3: self.strings["product"],
                4: self.strings["serial"]}.get(i, names.get(i))

    def is_kernel_driver_active(self, intf):
        return False

    def set_configuration(self):
        pass


class FakeRunning:
    """A radio that runs RadioFoundry, as the open stream debuglink.Link talks to: VERSION, INFO_CHIP and REBOOT
    (docs/spec/debuglink.md 3.9, 3.11); every other command is unknown. loader: the same radio's fake bootloader (a
    FakeRomUsbDevice), off the bus while the firmware runs and on it once a restart into the bootloader is taken.

    refuse: the reason the restart is refused with, before anything happens (5: the board's rule says not now, 3:
    transmitting), or None: taken. A test changes it between two requests, as a user who does what the board's hint
    says. stays: the radio answers OK and stays up (a check failed again at the reset, debuglink.md 3.11). rdp: the
    read protection level INFO_CHIP reports. requests: the payload of every REBOOT that came in. After a restart
    that was taken the port is gone: a write fails as on a device that left."""
    port = "sim"

    def __init__(self, board, loader, refuse=None, stays=False, rdp=0, family=debuglink.CHIP_STM32H7, name=None,
                 uid=bytes(range(0x21, 0x2D))):
        self.board, self.loader = board, loader
        self.refuse, self.stays, self.rdp, self.family = refuse, stays, rdp, family
        self.name = name or board.debuglink_names[0]
        self.uid = uid
        self.running = True
        self.requests = []
        self.out = bytearray()
        loader.gone = True

    def probe(self):
        """What debuglink.probe() returns for this radio, its port the stream itself"""
        return {"port": self, "board": self.name, "git": "00000000", "codec": "none", "features": "",
                "uid": self.uid.hex()}

    def enter_by_hand(self):
        """Switched off and on with the bootloader's key held: the firmware is gone, the bootloader is on the bus"""
        self.running = False
        self.loader.power_cycle()

    def _chip(self):
        r = bytearray(29)
        struct.pack_into("<IH", r, 0, 0x20036450, (self.board.flash_end - self.board.rom_base) // 1024)
        r[6:18] = self.uid
        r[22], r[23] = self.rdp, {0: 0xAA, 2: 0xCC}.get(self.rdp, 0x55)
        r[24:28] = b"\xFF" * 4
        r[28] = self.family
        return bytes(r)

    def _answer(self, cmd, payload):
        ok = bytes([debuglink.ST_OK])
        if cmd == debuglink.CMD_VERSION:
            return ok + ("proto=1\ngit=00000000\nboard=%s\nfeatures=FM\ncodec=none\n" % self.name).encode()
        if cmd == debuglink.CMD_INFO_CHIP:
            return ok + self._chip()
        if cmd != debuglink.CMD_REBOOT:
            return bytes([debuglink.ST_UNKNOWN])
        self.requests.append(bytes(payload))
        if len(payload) != 5 or payload[0] > debuglink.REBOOT_BOOTLOADER:
            return bytes([2])					# bad argument
        mode, token = struct.unpack("<BI", payload)
        to_loader = mode == debuglink.REBOOT_BOOTLOADER
        refused = lambda reason: bytes([debuglink.ST_REFUSED, reason])	# noqa: E731
        # the firmware's checks in its order (dl_reboot_check, then the platform's reboot_ok): the chip info, the
        # token, transmitting, the protection, and last the board's own rule, which is about the bootloader mode only
        if self.refuse == 4:
            return refused(4)
        if token != struct.unpack_from("<I", self.uid)[0]:
            return refused(1)
        if self.refuse == 3:
            return refused(3)
        if to_loader and (self.family not in debuglink.CHIP_ROM_FAMILIES or self.rdp != 0):
            return refused(2)
        if self.refuse is not None and (to_loader or self.refuse != 5):
            return refused(self.refuse)
        if not self.stays:
            self.running = False				# the reply goes out first, then the reset
            if to_loader:
                self.loader.power_cycle()
        return ok + b"\x00"					# the status, and reason 0

    # the pyserial calls debuglink.Link makes
    def write(self, data):
        if not self.running:
            raise OSError("device disconnected")
        for cmd, seq, payload in debuglink.parse_frames(bytes(data)):
            self.out += debuglink.frame(cmd | 0x80, seq, self._answer(cmd, payload))
        return len(data)

    def read(self, n=1):
        out = bytes(self.out[:n])
        del self.out[:n]
        return out

    def reset_input_buffer(self):
        self.out.clear()

    def close(self):
        pass


class Stall(Exception):
    """What a device does with a request it doesn't take (pyusb raises USBError: Pipe error)."""


class FakeOriginalFirmware:
    """A radio's original firmware running normally, as origversion.py's dfu-info read sees it: a DFU device with the
    bootloader's USB IDs (docs/spec/hardware-md9600.md 15.1). answer: the 4 bytes UPLOAD block 0 returns after `A2 02`,
    or None for a device that stalls that command (a bootloader). A request the read must never send fails the test;
    every request is kept in `requests`."""
    iManufacturer, iProduct, iSerialNumber = 2, 3, 4
    bus = 1

    class _Intf:
        bInterfaceClass, bInterfaceSubClass, bInterfaceNumber, iInterface = 0xFE, 0x01, 0, 5

    BOOTLOADER_ANSWER = b"\x54\x02\x10\x06"	# the bootloader's answer to A2 02 ('T')
    BOOTLOADER_INTERFACES = ["@Internal Flash   /0x0800C000/01*016Kg,01*064Kg,07*128Kg"]

    def __init__(self, board, answer=b"\x56\x00\x60\x09", state=dfumod.ST_IDLE, address=7, interfaces=None):
        self.board = board
        self.answer = answer
        self.state = state
        self.address = address
        self.selected = False
        self.requests = []
        self.strings = {"string1": board.dfu_string1, "manufacturer": board.dfu_string1, "product": board.dfu_product,
                        "serial": "00000000010C",
                        "interfaces": interfaces or ["@Internal Flash   /0x08000000/03*016Kg",
                                                     "@SPI Flash Memory /0x00000000/16*064Kg"]}

    def ctrl_transfer(self, rtype, req, value, intf, data, timeout=None):
        self.requests.append((rtype, req, value, bytes(data) if isinstance(data, (bytes, bytearray)) else data))
        assert intf == 0 and value == 0, "wIndex %r / wValue %r" % (intf, value)
        if rtype == 0xA1 and req == dfumod.DFU_GETSTATUS:
            assert data == 6
            st = self.state
            if st == 3:		# dfuDNLOAD-SYNC: the command runs after this reply, the next one finds it done
                self.state = 5
                st = 4
            return bytes([0, 0, 0, 0, st, 0])
        if rtype == 0xA1 and req == 2:		# UPLOAD
            assert data == 1024 and self.state == dfumod.ST_IDLE and self.selected, "UPLOAD out of sequence"
            self.selected = False
            return self.answer
        assert rtype == 0x21, "request type 0x%02X" % rtype
        if req == dfumod.DFU_CLRSTATUS:
            self.state = dfumod.ST_IDLE if self.state == dfumod.ST_ERROR else dfumod.ST_ERROR
            return 0
        if req == dfumod.DFU_ABORT:
            assert not data
            self.state = dfumod.ST_IDLE
            return 0
        assert req == dfumod.DFU_DNLOAD, "request %d" % req
        assert bytes(data or b"") == b"\xA2\x02", "DNLOAD %r: only A2 02 may be sent" % (data,)
        assert self.state == dfumod.ST_IDLE
        if self.answer is None:
            raise Stall("Pipe error")
        self.state = 3
        self.selected = True
        return 2

    def __iter__(self):
        return iter([[self._Intf()]])

    def string_by_index(self, i):
        return {1: self.strings["string1"], 2: self.strings["manufacturer"], 3: self.strings["product"],
                4: self.strings["serial"], 5: self.strings["interfaces"][0]}.get(i)

    def is_kernel_driver_active(self, intf):
        return False

    def set_configuration(self):
        pass
