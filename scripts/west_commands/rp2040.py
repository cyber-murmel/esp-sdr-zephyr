# SPDX-License-Identifier: GPL-3.0-or-later
"""west rp2040: the RP2040 of the MCH2022 badge (boards/badgeteam/mch2022_rp2040).

apps/mch2022_coproc takes a request into the ROM's USB bootloader, where picotool loads an image
and reads the flash; the stock firmware has no such thing, there it takes SELECT at power-up. The
image is one with the badge's own bootloader in mind (rp2040_image.py), which picotool will not
start by itself: it reboots afterwards.
"""

import hashlib
import importlib.util
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from west.commands import WestCommand

BADGE_ID = (0x16D0, 0x0F9A)
ROM_ID = (0x2E8A, 0x0003)
FLASH_BYTES = 2 * 1024 * 1024

# esp_sdr/mch2022_coproc.h
IF_LINK = 6
IF_WEBUSB_ESP32 = 4
REQ_GET_INFO = 0x01
REQ_GET_STATS = 0x02
REQ_BOOTSEL = 0x22
REQ_GET_LOG = 0x60
WEBUSB_GET_FW_VER = 0x27
INFO_MAGIC = 0x31325043
STATS_FMT = "<2Q2Q2I2I2I2I2Q2I"
STATS_NAMES = (
    "uart_rx", "uart_tx", "ring_overrun", "uart_overrun", "uart_framing", "stale_dropped",
)

# The stock application, for a rollback from it (doc/index.rst of the board).
STOCK_APP_SHA256 = "ebfb25a013d1f8574a04e953363666b76b160b2fba140ec774e8485e5be015fc"

BOOTSEL_BY_HAND = """\
The firmware on the badge has no way into the ROM's bootloader. By hand:
  USB out, battery switch off, hold SELECT, plug USB in, let go."""


def image_tool():
    """boards/badgeteam/mch2022_rp2040/support/rp2040_image.py as a module."""
    path = (Path(__file__).resolve().parents[2] /
            "boards/badgeteam/mch2022_rp2040/support/rp2040_image.py")
    spec = importlib.util.spec_from_file_location("rp2040_image", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def usb():
    """pyusb, in the dev shell's python."""
    try:
        import usb.core
    except ImportError:
        sys.exit("needs pyusb: run it in the workspace's nix shell")
    return usb.core


def find(ids, serial=None):
    """The device with this vendor and product, of this serial if given; None if none."""
    core = usb()
    for dev in core.find(idVendor=ids[0], idProduct=ids[1], find_all=True):
        if serial is None or ids == ROM_ID or dev.serial_number == serial:
            return dev
    return None


def wait_for(ids, serial=None, seconds=20.0):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        dev = find(ids, serial)
        if dev is not None:
            return dev
        time.sleep(0.1)
    return None


def has_link(dev):
    """Whether this is apps/mch2022_coproc, not the stock firmware."""
    return any(i.bInterfaceNumber == IF_LINK for i in dev.get_active_configuration())


def picotool(*args):
    tool = shutil.which("picotool")
    if tool is None:
        sys.exit("needs picotool: run it in the workspace's nix shell")
    subprocess.run([tool, *args], check=True)


def enter_rom(serial, wait=60.0):
    """The badge in the ROM's USB bootloader, by request if its firmware has one."""
    if find(ROM_ID) is not None:
        return
    dev = find(BADGE_ID, serial)
    if dev is not None and has_link(dev):
        dev.ctrl_transfer(0x41, REQ_BOOTSEL, 0, IF_LINK, None)
    else:
        print(BOOTSEL_BY_HAND)
    if wait_for(ROM_ID, seconds=wait) is None:
        sys.exit("no ROM bootloader on USB")


def load(image, serial):
    """A UF2 into flash, checked; then the badge's own bootloader starts it."""
    enter_rom(serial)
    picotool("load", "-v", str(image))
    picotool("reboot")
    dev = wait_for(BADGE_ID, serial)
    if dev is None:
        sys.exit("the badge did not come back on USB (SELECT at power-up still works)")
    print(describe(dev))


def describe(dev):
    version = dev.ctrl_transfer(0xA1, WEBUSB_GET_FW_VER, 0, IF_WEBUSB_ESP32, 1)[0]
    line = f"serial {dev.serial_number}, firmware 0x{version:02x}"
    if not has_link(dev):
        return line + ": stock"
    info = dev.ctrl_transfer(0xC1, REQ_GET_INFO, 0, IF_LINK, 64).tobytes()
    magic, major, minor, features, rosc, reason, uptime, mode, test, link, _ = struct.unpack_from(
        "<IBBHIII4B", info)
    if magic != INFO_MAGIC:
        return line + f": bad magic 0x{magic:08x}"
    return (f"{line}: apps/mch2022_coproc API {major}.{minor}, features 0x{features:04x}, "
            f"up {uptime / 1000:.1f} s, reset mode {mode}, link {'open' if link else 'closed'}, "
            f"test {test}, ROSC 0x{rosc:08x}, reset reason 0x{reason:08x}")


class Rp2040(WestCommand):
    def __init__(self):
        super().__init__("rp2040", "the MCH2022 badge's RP2040", __doc__)

    def do_add_parser(self, parser_adder):
        p = parser_adder.add_parser(self.name, help=self.help, description=self.description)
        p.add_argument("--serial", help="the badge's USB serial number, if there are several")
        sub = p.add_subparsers(dest="what", required=True)
        sub.add_parser("info", help="what the badge runs")
        sub.add_parser("bootsel", help="into the ROM's USB bootloader")
        f = sub.add_parser("flash", help="a build, or a UF2 made for this board, into flash")
        f.add_argument("image", nargs="?", default="build",
                       help="a build directory or a .uf2 (default: build)")
        r = sub.add_parser("rollback", help="back to the stock firmware")
        r.add_argument("image", type=Path,
                       help="the stock application (rp2040_firmware.bin), its UF2, or a whole "
                            "flash saved by backup")
        b = sub.add_parser("backup", help="save the whole flash")
        b.add_argument("out", type=Path)
        sub.add_parser("log", help="the firmware's log since it was last read")
        sub.add_parser("stats", help="the firmware's counters")
        return p

    def do_run(self, args, unknown):
        getattr(self, "do_" + args.what)(args)

    def need_app(self, args):
        dev = find(BADGE_ID, args.serial)
        if dev is None:
            sys.exit("no badge on USB")
        if not has_link(dev):
            sys.exit("the badge runs the stock firmware")
        return dev

    def do_info(self, args):
        if find(ROM_ID) is not None:
            picotool("info", "-a")
            return
        dev = find(BADGE_ID, args.serial)
        if dev is None:
            sys.exit("no badge on USB")
        print(describe(dev))

    def do_bootsel(self, args):
        enter_rom(args.serial)
        print("in the ROM's bootloader")

    def do_flash(self, args):
        image = Path(args.image)
        if image.is_dir():
            image = image / "zephyr" / "zephyr.uf2"
        if not image.is_file():
            sys.exit(f"no {image}")
        load(image, args.serial)

    def do_rollback(self, args):
        data = args.image.read_bytes()
        if args.image.suffix == ".uf2":
            load(args.image, args.serial)
        elif len(data) == FLASH_BYTES:
            enter_rom(args.serial)
            picotool("load", "-v", "-t", "bin", "-o", "0x10000000", str(args.image))
            picotool("reboot")
            print(describe(wait_for(BADGE_ID, args.serial)))
        else:
            if hashlib.sha256(data).hexdigest() != STOCK_APP_SHA256:
                print("warning: not the stock application this tool knows")
            with tempfile.TemporaryDirectory() as tmp:
                uf2 = Path(tmp) / "stock.uf2"
                uf2.write_bytes(image_tool().image(data)[0])
                load(uf2, args.serial)

    def do_backup(self, args):
        enter_rom(args.serial)
        picotool("save", "-a", str(args.out), "-t", "bin")
        digest = hashlib.sha256(args.out.read_bytes()).hexdigest()
        Path(str(args.out) + ".sha256").write_text(f"{digest}  {args.out.name}\n")
        print(f"{args.out}: sha256 {digest}; still in the ROM's bootloader: "
              "picotool reboot starts the firmware")

    def do_log(self, args):
        dev = self.need_app(args)
        while True:
            chunk = dev.ctrl_transfer(0xC1, REQ_GET_LOG, 0, IF_LINK, 256).tobytes()
            if not chunk:
                break
            sys.stdout.write(chunk.decode(errors="replace"))

    def do_stats(self, args):
        dev = self.need_app(args)
        raw = dev.ctrl_transfer(0xC1, REQ_GET_STATS, 0, IF_LINK, 256).tobytes()
        v = struct.unpack_from(STATS_FMT, raw)
        for k, name in enumerate(STATS_NAMES):
            print(f"{name:14} ESP32 {v[2 * k]:>12}  FPGA {v[2 * k + 1]:>12}")
        print(f"link           out {v[12]} in {v[13]}")
        print(f"ESP32 resets   {v[14]} ({v[15]} into the application by the reset lock)")
