# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The USB DFU bootloaders: finding one and running a sector plan. Two routes, by the board's family:

usb-dfu, the AnyRoad (TYT/Baofeng STM32) bootloader. Protocol from docs/spec/hardware-dm1701.md 15.1-15.3 (the same
steps tools/stm32_dfu.py drives), with one change: only the sectors in the plan are erased and written, instead of
every application sector. Each block is XORed with the 1024-byte model key; every sector starts a multiple of 1024
bytes after 0x0800C000, so every block starts at key index 0 whichever sectors are skipped.

dfuse, the plain route: ST's DfuSe download to the bootloader in an MCU's own ROM (AN3156), Dfu.run without a key.
The same erase, set address and block requests, with no vendor command and nothing encrypted, every block sent as
block 2 after its own Set Address Pointer, and the image is started at the board's application base. A ROM bootloader
refuses to read, erase or write a read-protected chip: Dfu.probe_protected finds that out without changing anything,
Dfu.read_unprotect removes the protection, which erases the whole program flash.

Option bytes are never written. A ROM bootloader shows them as a second alternate setting of its DFU interface, and
its address pointer reaches them too; a wrong write there can lock the chip for good or take away the way into the
bootloader. Nothing here selects an alternate setting (open_device), and the plain route sends no address outside the
board's program flash and erases no sector past its application region (check_plain).
"""
import hashlib
import os
import re
import struct
import sys
import time

from . import protect
from .plan import BLOCK, FIRST_BLOCK

KEY_LEN = 1024
EXIT_ADDR = 0x08000000		# where the keyed route leaves; the plain route leaves at the board's app_base

# The plain route sends whole flash words: every block is filled with 0xFF to a multiple of this (flash_words). The
# largest flash word of the MCUs on this route is 256 bits, and a partial one is only programmed with a force-write
# bit (RM0433 4.3.9); AN3156 and AN2606 do not say whether the ROM bootloader forces the last partial word of a
# download. An image ends on a 4-byte boundary (the linker script), not on this one. The plan's blocks are BLOCK bytes
# with 0xFF after the image, a multiple of this already: the fill in flash_words holds the rule whatever the block is.
FLASH_WORD = 32

DFU_DNLOAD, DFU_UPLOAD, DFU_GETSTATUS, DFU_CLRSTATUS, DFU_ABORT = 1, 2, 3, 4, 6
ST_IDLE, ST_DNBUSY, ST_DNIDLE, ST_UPIDLE, ST_ERROR = 2, 4, 5, 9, 10
ERR_VENDOR = 0x0B		# bStatus errVENDOR: a ROM bootloader's answer when read protection is on (AN3156 4.1)
CMD_SET_ADDRESS, CMD_ERASE, CMD_READ_UNPROTECT = 0x21, 0x41, 0x92	# DNLOAD block 0, first byte (AN3156 5)
STATE_NAMES = {0: "appIDLE", 1: "appDETACH", 2: "dfuIDLE", 3: "dfuDNLOAD-SYNC", 4: "dfuDNBUSY",
               5: "dfuDNLOAD-IDLE", 6: "dfuMANIFEST-SYNC", 7: "dfuMANIFEST", 8: "dfuMANIFEST-WAIT-RESET",
               9: "dfuUPLOAD-IDLE", 10: "dfuERROR"}


class DfuError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------
# key

def check_key(key, board):
    if len(key) != KEY_LEN:
        raise DfuError("the key is %d bytes, expected %d" % (len(key), KEY_LEN))
    got = hashlib.sha256(key).hexdigest()
    if got != board.key_sha256:
        raise DfuError("this is not the %s bootloader key (SHA-256 %s, expected %s...)"
                       % (board.name, got, board.key_sha256[:16]))
    return key


def load_key(path, board):
    with open(os.path.expanduser(path), "rb") as f:
        return check_key(f.read(), board)


def xor_block(block, key):
    return bytes(a ^ b for a, b in zip(block, key))


def flash_words(data):
    """data filled with 0xFF to whole flash words (FLASH_WORD), as the plain route sends it."""
    return bytes(data) + b"\xFF" * (-len(data) % FLASH_WORD)


def check_plain(plan):
    """The plain route's limits on a plan: Dfu.run checks them before any request, and cli.py before a Read Unprotect
    (an erase of everything must not be followed by a refusal to write). A ROM bootloader takes any address it is
    sent, so two things are held here:

    The program flash only. Its address pointer also reaches RAM, system memory and the option bytes (AN3156 5.2).

    The application region only (the board's linker script, Board.app_region). Flash past it is flash the build
    keeps unused, and it can be unused because the bootloader must not be asked to erase it: one version of the
    STM32H74x ROM bootloader leaves an erase in the second bank early, and a flash command after it can hang (AN2606
    Table 146, V9.1). A build never reaches there; a file that is not this board's image can.

    And every block inside its own sector, as the web flasher's checkPlain has it: a block past the sector's end
    would go to the next one, which was not erased, or past the region.

    And the vector table last (plan vector_last): the sector at the application base is erased first and written
    last, and Dfu.run writes its first block after everything else is written and read back. At a normal power-on the
    chip starts whatever vector table is there; the ROM bootloader is entered by the boot pin, not through the
    vectors. So the order costs nothing, and an install cut anywhere before the end leaves no vector table, and no
    half-written image, to start."""
    board = plan.board
    lo, hi = board.app_region
    for st in plan.writes:
        s = st.sector
        if not (board.rom_base <= s.addr and s.end <= board.flash_end):
            raise DfuError("sector 0x%08X is outside the program flash" % s.addr)
        if not s.overlaps(lo, hi):
            raise DfuError("the image reaches sector %d (0x%08X), outside the %s's application flash 0x%08X..0x%08X: "
                           "it is not an image of this radio. Refusing" % (s.index, s.addr, board.name, lo, hi - 1))
        for num, _ in st.blocks:
            if num < FIRST_BLOCK or s.addr + (num - FIRST_BLOCK + 1) * BLOCK > s.end:
                raise DfuError("block %d is outside sector %d (0x%08X)" % (num, s.index, s.addr))
    last = plan.write_order[-1] if plan.writes else None
    if last and (last.sector.addr != board.app_base or not last.blocks or last.blocks[0][0] != FIRST_BLOCK
                 or not plan.erase_order or plan.erase_order[0].addr != board.app_base):
        raise DfuError("the plan does not erase the sector at the application base 0x%08X first and write it last: "
                       "an install cut short would leave a vector table to start. Refusing" % board.app_base)


# ---------------------------------------------------------------------------------------------------------------
# protocol, over anything with a pyusb-like ctrl_transfer()

class Dfu:
    def __init__(self, dev, intf=0, log=print, sleep=time.sleep):
        self.dev = dev
        self.intf = intf
        self.log = log
        self.sleep = sleep

    def _out(self, req, value, data):
        return self.dev.ctrl_transfer(0x21, req, value, self.intf, data, 5000)

    def get_status(self):
        r = bytes(self.dev.ctrl_transfer(0xA1, DFU_GETSTATUS, 0, self.intf, 6, 5000))
        if len(r) != 6:
            raise DfuError("short GETSTATUS reply (%d bytes)" % len(r))
        return r[0], r[1] | (r[2] << 8) | (r[3] << 16), r[4]

    def wait_idle(self):
        state = status = None
        for attempt in range(20):
            status, _, state = self.get_status()
            if state == ST_IDLE:
                return
            self._out(DFU_CLRSTATUS if attempt < 10 else DFU_ABORT, 0, None)
        raise DfuError("the bootloader stays in state %s (status %s), not dfuIDLE: power-cycle the radio into the "
                       "bootloader and try again" % (STATE_NAMES.get(state, state), status))

    def to_idle(self):
        """dfuIDLE by the standard's own requests (DFU 1.1 A.1), as the web flasher's toIdle: CLRSTATUS leaves dfuERROR,
        ABORT an idle state of a transfer, and dfuDNBUSY is waited out. wait_idle() sends CLRSTATUS in every state,
        which the keyed bootloader takes; a ROM bootloader answers a request its state doesn't allow with a stall and
        an error, so what must reach dfuIDLE from any state on the plain route comes here: the first request of the
        probe, of a Read Unprotect and of a run (a bootloader an interrupted run left in the middle of a transfer),
        and the erase after a failed vector table (_unvector)."""
        state = status = None
        for _ in range(20):
            status, poll_ms, state = self.get_status()
            if state == ST_IDLE:
                return
            if state == ST_DNBUSY:
                self.sleep(max(poll_ms, 1) / 1000.0)
            else:
                self._out(DFU_CLRSTATUS if state == ST_ERROR else DFU_ABORT, 0, None)
        raise DfuError("the bootloader stays in state %s (status %s), not dfuIDLE: power-cycle the radio into the "
                       "bootloader and try again" % (STATE_NAMES.get(state, state), status))

    def wait_dnload_done(self, what):
        for _ in range(4000):
            status, poll_ms, state = self.get_status()
            if state == ST_ERROR or status != 0:
                raise DfuError("%s failed: status %d, state %s" % (what, status, STATE_NAMES.get(state, state)))
            if state == ST_DNIDLE:
                return
            if state != ST_DNBUSY:
                raise DfuError("%s: unexpected state %s" % (what, STATE_NAMES.get(state, state)))
            self.sleep(max(poll_ms, 1) / 1000.0)
        raise DfuError("%s: the bootloader stayed busy" % what)

    def vendor(self, b0, b1):
        self._out(DFU_DNLOAD, 0, bytes([b0, b1]))
        self.get_status()
        self.sleep(0.1)
        self.get_status()
        self.wait_idle()

    def command(self, cmd, addr, what):
        self._out(DFU_DNLOAD, 0, bytes([cmd]) + struct.pack("<I", addr))
        self.wait_dnload_done(what)

    def run(self, plan, key=None, progress=None):
        """Erase the plan's sectors (in the bootloader's order), write each, exit (the radio resets). A codec move erases
        the application's first sector first and writes it last (plan.Plan.write_order): a move cut short leaves no
        application to start.

        key: the board's bootloader key (a usb-dfu board: the vendor commands first, every block XORed). None is the
        plain route to a ROM bootloader (a dfuse board): no vendor command, the blocks as they are in whole flash
        words, and the image started at the board's app_base. Its plan always has the vector table last (check_plain):
        every block but the first one at app_base is written and read back, then that block (the vector table) is
        written and read back, then the image is started. Whatever fails once the vector table's write has begun,
        its sector is erased again (_unvector): a table that is in flash and did not pass its check is not left to
        start at the next normal power-on.

        The plain route sends every block as block 2 right after its own Set Address Pointer, as the web flasher
        does: the bootloader places a block at (block number - 2) x wTransferSize + pointer, and AN3156 5.1 calls
        wTransferSize "length of the data buffer sent by the host" while the DFU functional descriptor has a field of
        that name. With block 2 the product is zero under either reading."""
        board = plan.board
        base = board.app_base
        plain = key is None
        # the wrong route for the board would write an image the bootloader can't take; cli.py tells the two
        # bootloaders apart by their strings before it comes here
        if plain and getattr(board, "key_sha256", None):
            raise DfuError("the %s bootloader takes only blocks encrypted with its key" % board.name)
        if not plain and not getattr(board, "key_sha256", None):
            raise DfuError("the %s bootloader takes no key" % board.name)
        for s in plan.erase:		# make_plan refuses these; a plan built another way is checked again before any erase
            protect.check_absolute(board, board.rom_base, s.addr, s.end, "erase")
        if plain:
            check_plain(plan)		# no address outside the program flash, no sector past the application region
        for st in plan.writes:
            if not plain and (st.sector.addr - base) % KEY_LEN:
                raise DfuError("sector 0x%08X is not key-aligned" % st.sector.addr)
        if plain:
            self.to_idle()		# whatever state an earlier run left; CLRSTATUS only where the standard has it
        else:
            self.wait_idle()
            self.vendor(0x91, 0x01)
            self.vendor(0x91, 0x31)
        for s in plan.erase_order:
            self.log("erase sector %d at 0x%08X" % (s.index, s.addr))
            self.command(CMD_ERASE, s.addr, "erase 0x%08X" % s.addr)
        total, done = plan.total_blocks, 0

        def write(st, blocks):
            nonlocal done
            addr = st.sector.addr
            if not blocks:
                return
            if not plain:
                self.command(CMD_SET_ADDRESS, addr, "set address 0x%08X" % addr)
            for num, blk in blocks:
                if len(blk) != BLOCK:
                    raise DfuError("block of %d bytes" % len(blk))
                at = addr + (num - FIRST_BLOCK) * BLOCK
                if plain:
                    self.command(CMD_SET_ADDRESS, at, "set address 0x%08X" % at)
                    self._out(DFU_DNLOAD, FIRST_BLOCK, flash_words(blk))
                    self.wait_dnload_done("block at 0x%08X" % at)
                else:
                    self._out(DFU_DNLOAD, num, xor_block(blk, key))
                    self.wait_dnload_done("block %d at 0x%08X" % (num, at))
                done += 1
                if progress:
                    progress(done, total)

        if plain and plan.writes:
            # the vector table's block (check_plain: the first one of the last sector) after all the rest, which is
            # read back first: until it is written a cut install has nothing to start
            again = "put the radio in its bootloader again and run the same command"
            *rest, head = plan.write_order
            body = [(st, st.blocks) for st in rest] + [(head, head.blocks[1:])]
            for st, blocks in body:
                write(st, blocks)
            try:
                self.read_back(body)
            except DfuError as e:
                raise DfuError("%s. The vector table is not written yet, so the radio starts nothing until the install "
                               "has run again: %s" % (e, again))
            self.log("write the vector table at 0x%08X" % base)
            try:
                write(head, head.blocks[:1])
                self.read_back([(head, head.blocks[:1])], vectors=True)	# before the start
            except Exception as e:
                # a write the bootloader reports as failed, a read it refuses, a block that reads back different, a
                # lost device: in each the table may be in flash, and nothing has checked it
                what = e if isinstance(e, DfuError) else "USB error at the vector table: %s" % e
                raise DfuError("%s%s: %s" % (what, self._unvector(head), again))
        else:
            for st in plan.write_order:
                write(st, st.blocks)
        exit_addr = base if plain else EXIT_ADDR
        if plain:
            self.log("leave: start the image at 0x%08X" % exit_addr)
        self.command(CMD_SET_ADDRESS, exit_addr, "set address 0x%08X" % exit_addr)
        self._out(DFU_DNLOAD, 0, None)
        try:
            _, _, state = self.get_status()	# the radio resets; it may not answer
        except Exception:
            return
        if plain and state == ST_ERROR:
            # everything is written and read back; only the start is missing
            self.log("the bootloader did not start the image (%s): switch the radio off and on"
                     % STATE_NAMES.get(state, state))

    def read_back(self, parts, vectors=False):
        """The plain route's check of what it wrote, before the image is started: every block of parts ([(step,
        blocks)]) read back and compared with the block sent. A ROM bootloader reports no error for a write it did not
        carry out (a write-protected sector, AN3156 5.1 and 5.3), and a block placed at another address than meant shows
        here too. Each block is read as block 2 right after its own Set Address Pointer, as it was written, so its
        address is the pointer whatever transfer size the bootloader multiplies the block number by (AN3156 5.1).
        UPLOAD is taken in dfuIDLE only (AN3156 figure 3): ABORT before it, and again to leave dfuUPLOAD-IDLE.

        A failure leaves the image unstarted and the radio in its ROM bootloader, where the same command can run
        again; until it has, the radio starts nothing at a normal power-on. Dfu.run says why in its message: before
        the vector table is written there is none, and a failure of the vector table's own check (vectors: these
        blocks are it) erases its sector again."""
        n = sum(len(blocks) for _, blocks in parts)
        if not n:
            return
        self.log("verify: read back %s" % ("the vector table" if vectors else "%d blocks" % n))
        for st, blocks in parts:
            for num, blk in blocks:
                addr = st.sector.addr + (num - FIRST_BLOCK) * BLOCK
                self.command(CMD_SET_ADDRESS, addr, "set address 0x%08X" % addr)
                self._out(DFU_ABORT, 0, None)
                self.wait_idle()
                want = flash_words(blk)
                try:
                    got = bytes(self.dev.ctrl_transfer(0xA1, DFU_UPLOAD, FIRST_BLOCK, self.intf, len(want), 5000))
                except Exception as e:
                    raise DfuError("the block at 0x%08X could not be read back (%s): the image is written but not "
                                   "checked, and was not started" % (addr, e))
                self._out(DFU_ABORT, 0, None)
                self.wait_idle()
                if got != want:
                    at = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), min(len(got), len(want)))
                    raise DfuError("the flash at 0x%08X does not read back what was written there (%d of %d bytes "
                                   "read): the write did not take, and the image was not started"
                                   % (addr + at, len(got), len(want)))

    def _unvector(self, st):
        """The vector table's sector (st: its step) erased again after its write or its check failed, and what that
        leaves to start at a normal power-on, in words for the message. The failure may have left the bootloader in
        any state, dfuERROR after a refused write or read among them, where it takes no ABORT (DFU 1.1 A.2.11):
        to_idle before the erase."""
        try:
            self.to_idle()
            self.command(CMD_ERASE, st.sector.addr, "erase 0x%08X" % st.sector.addr)
        except Exception as e:
            return (". Its vector table could not be erased again (%s): at a normal power-on the radio may start an "
                    "image whose vector table did not pass its check" % e)
        return (". The vector table's sector is erased again, so the radio starts nothing until the install has run "
                "again")

    def probe_protected(self, board):
        """Whether a ROM bootloader's chip is read protected (RDP level 1), where it refuses to read, erase or write.
        A read of 4 bytes at the start of the program flash: the address pointer is set (allowed either way, AN3156
        5.2), then UPLOAD block 2. A protected chip answers dfuERROR with errVENDOR and reads nothing (AN3156 4.1).
        Nothing on the radio changes. True: protected. False: the 4 bytes came. Anything else is an error: "protected"
        leads to an erase of everything, so it is never concluded from an answer the documents don't describe."""
        addr = board.rom_base
        self.to_idle()
        self.command(CMD_SET_ADDRESS, addr, "set address 0x%08X" % addr)
        self._out(DFU_ABORT, 0, None)		# UPLOAD is taken in dfuIDLE; the pointer is also the loader's default
        self.wait_idle()
        got = None
        try:
            got = bytes(self.dev.ctrl_transfer(0xA1, DFU_UPLOAD, FIRST_BLOCK, self.intf, 4, 5000))
        except Exception:
            pass				# a refused request may stall; the status says why
        status, _, state = self.get_status()
        if state == ST_ERROR and status == ERR_VENDOR:
            self.wait_idle()
            return True
        if state != ST_ERROR and status == 0 and got is not None and len(got) == 4:
            self._out(DFU_ABORT, 0, None)	# leave dfuUPLOAD-IDLE
            self.wait_idle()
            return False
        raise DfuError("the bootloader's answer to a read of 4 bytes at 0x%08X is neither the data nor \"read "
                       "protected\" (status %d, state %s, %s bytes): nothing was changed"
                       % (addr, status, STATE_NAMES.get(state, state), "no" if got is None else len(got)))

    def read_unprotect(self, board):
        """Remove a ROM bootloader's read protection (AN3156 5.4). The chip erases its whole program flash and its RAM,
        sets its option bytes back to their defaults (its own doing: nothing is written to them from here; loader V9.0
        of the STM32H74x leaves a PCROP area as it was, AN2606 Table 146) and leaves the bus; the mass erase takes
        tens of seconds (cli.unprotect_wait). Refused when the board lists anything protected in program flash
        (protect.check_whole). Returns once the bootloader has taken the command: the disconnect is the result, and
        the radio must be switched off and put in its bootloader again. A reply that does not come is taken as the
        command taken, as the web flasher takes it: the device may leave the bus at once (AN3156 5.4), and an error
        here would send the user to switch the radio off while it may be erasing. The probe after the second entry
        says whether the protection went."""
        protect.check_whole(board, "progmem", "remove the read protection, which erases the whole")
        self.to_idle()
        self._out(DFU_DNLOAD, 0, bytes([CMD_READ_UNPROTECT]))
        try:
            status, _, state = self.get_status()	# the command runs on this request
        except Exception as e:
            self.log("the bootloader left the bus (%s): it may be erasing" % e)
            return
        if state != ST_DNBUSY or status != 0:
            raise DfuError("the bootloader did not take the Read Unprotect command (status %d, state %s)"
                           % (status, STATE_NAMES.get(state, state)))


# ---------------------------------------------------------------------------------------------------------------
# USB

def import_usb():
    try:
        import usb.core
        import usb.util
    except ImportError:
        raise DfuError("pyusb is not installed: pip install pyusb libusb-package (or use the rfoundry executable)")
    return usb


def backend():
    """libusb from the libusb-package wheel when present (the single-file executables bundle it), else pyusb's."""
    try:
        import libusb_package
        return libusb_package.get_libusb1_backend()
    except Exception:
        return None


def permission_hint(board=None):
    """What to do when the bootloader can't be opened. board: the one expected, whose bootloader the Windows steps name
    (a ROM bootloader has the keyed ones' USB ID and another name)."""
    if sys.platform == "darwin":
        return ("macOS: close other programs that use the radio (a CPS in a virtual machine with USB passthrough), "
                "unplug and replug the cable")
    if sys.platform.startswith("linux"):
        return ("Linux: allow your user to open the bootloader, then replug the radio:\n"
                "  echo 'SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"0483\", ATTRS{idProduct}==\"df11\", MODE=\"0666\"' "
                "| sudo tee /etc/udev/rules.d/50-rfoundry-dfu.rules\n"
                "  sudo udevadm control --reload-rules && sudo udevadm trigger")
    return windows_driver_steps(board) if board is not None else WINDOWS_DRIVER_STEPS


def windows_driver_steps(board=None):
    """Shown on Windows when the bootloader can't be opened. Same steps as docs/user/02-installing.md and the web
    flasher. The name in Zadig's list is the bootloader's product string ([flash.dfu] product); a board whose
    bootloader's strings are not recorded yet is found by its USB ID alone."""
    name = getattr(board, "dfu_product", None) if board is not None else "Digital Radio in USB mode"
    ids = "%04X %04X" % tuple(board.dfu_ids) if board is not None and board.dfu_ids else "0483 DF11"
    pick = '"%s" (USB ID %s)' % (name, ids) if name else "the device with USB ID %s" % ids
    return ("Windows: the bootloader needs the WinUSB driver, installed once with Zadig:\n"
            "  1. Download Zadig from https://zadig.akeo.ie and run it (it needs no install).\n"
            "  2. Put the radio in its bootloader and plug it in.\n"
            "  3. In Zadig's list pick %s. Not listed: Options > List All Devices.\n"
            "  4. Choose WinUSB as the driver and click Install Driver (or Replace Driver). Wait until it says it "
            "succeeded.\n"
            "  5. Unplug the radio, put it back in its bootloader and run rfoundry again." % pick)


WINDOWS_DRIVER_STEPS = windows_driver_steps()	# the keyed bootloaders' name, where the board isn't at hand


def find(usb, board):
    vid, pid = board.dfu_ids
    try:
        return list(usb.core.find(find_all=True, idVendor=vid, idProduct=pid, backend=backend()))
    except usb.core.NoBackendError:
        raise DfuError("no libusb found: macOS 'brew install libusb', Linux 'apt install libusb-1.0-0', or "
                       "pip install libusb-package")


def _string(usb, dev, index, board=None):
    if not index:
        return None
    try:
        return usb.util.get_string(dev, index)
    except (usb.core.USBError, ValueError) as e:
        if getattr(e, "errno", None) == 13:
            raise DfuError("access denied reading the bootloader's strings\n" + permission_hint(board))
        return None


def _layout(name):
    return " ".join((name or "").split())


BOOT_AREA = 0xC000      # the bootloader's sectors, 0x08000000-0x0800BFFF


def region(name):
    """The first region of a DfuSe interface string ("@name /0xADDR/NN*SSSKg,..."): (start, bytes), None if it has none."""
    m = re.search(r"/0x([0-9a-fA-F]+)/([^/]*)", name or "")
    if not m:
        return None
    unit = {"K": 1024, "M": 1024 * 1024}
    size = sum(int(n) * int(s) * unit.get(u, 1) for n, s, u in re.findall(r"(\d+)\*(\d+)\s*([KM]?)", m.group(2)))
    return int(m.group(1), 16), size


def rom_layout_ok(interfaces, board):
    """True when a DFU device's first interface string is the one a ROM bootloader of this board's chip shows: the
    layout the board declares ([flash.dfu] layout, read on a radio), else a first region that is the board's whole
    program flash. The plain route has no key that only the right bootloader takes, so this is what keeps an image
    off another chip's ROM bootloader (the same USB IDs). No string at all: refused."""
    names = [_layout(n) for n in interfaces if n]
    if not names:
        return False
    want = getattr(board, "dfu_layout", None)
    if want:
        return names[0] == _layout(want)
    return region(names[0]) == (board.rom_base, board.flash_end - board.rom_base)


def normal_mode(interfaces, board=None):
    """True when the DFU device is the radio's own firmware switched on normally, not its bootloader
    (hardware-dm1701.md 15.1). The normal-mode firmware has the bootloader's USB IDs and strings, but erasing or writing
    through it can reach the bootloader's sectors. A board that declares its bootloader's layout ([flash.dfu] layout)
    accepts only that alt 0 string; any other board refuses a layout that shows only the bootloader's sectors (from
    0x08000000, at most BOOT_AREA bytes), as every original firmware's does. A bootloader shows the application area,
    so one that listed the whole flash would still pass. No layout string at all: nothing to go by, accepted."""
    names = [_layout(n) for n in interfaces if n]
    if not names:
        return False
    want = getattr(board, "dfu_layout", None)
    if want:
        return names[0] != _layout(want)
    regions = [region(n) for n in names]
    return any(r is not None and r[0] == 0x08000000 and r[1] <= BOOT_AREA for r in regions)


def fingerprint(usb, dev, board=None):
    """The strings a bootloader shows: string 1, manufacturer, product, serial, DFU interface names. board: the one
    expected, for the hint when access is denied."""
    fp = {"string1": _string(usb, dev, 1, board), "manufacturer": _string(usb, dev, dev.iManufacturer, board),
          "product": _string(usb, dev, dev.iProduct, board), "serial": _string(usb, dev, dev.iSerialNumber, board),
          "interfaces": []}
    for cfg in dev:
        for intf in cfg:
            if intf.bInterfaceClass == 0xFE and intf.bInterfaceSubClass == 0x01:
                fp["interfaces"].append(_string(usb, dev, intf.iInterface, board))
    return fp


def open_device(usb, dev, board=None):
    """Claim the device's DFU interface and return its number. No alternate setting is ever selected, so every
    request goes to alternate setting 0, the program flash. A ROM bootloader's alternate setting 1 is the chip's
    option bytes, which are never selected or written. board: the one expected, for the hint when it can't be opened."""
    intf = None
    for cfg in dev:
        for i in cfg:
            if i.bInterfaceClass == 0xFE and i.bInterfaceSubClass == 0x01:
                intf = i.bInterfaceNumber
                break
    if intf is None:
        raise DfuError("the device has no DFU interface")
    try:
        try:
            if dev.is_kernel_driver_active(intf):
                dev.detach_kernel_driver(intf)
        except (NotImplementedError, usb.core.USBError):
            pass
        dev.set_configuration()
        usb.util.claim_interface(dev, intf)
    except usb.core.USBError as e:
        if e.errno == 13 or "Access denied" in str(e):
            raise DfuError("access denied opening the bootloader\n" + permission_hint(board))
        if e.errno == 16 or "busy" in str(e).lower():
            raise DfuError("the bootloader is busy (another program has it open)\n" + permission_hint(board))
        raise DfuError("cannot open the bootloader: %s\n%s" % (e, permission_hint(board)))
    return intf


def release(usb, dev):
    try:
        usb.util.dispose_resources(dev)
    except Exception:
        pass
