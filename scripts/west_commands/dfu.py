# SPDX-License-Identifier: GPL-3.0-or-later
"""west dfu: update a running esp-sdr board over USB DFU and confirm the image.

The apps in this repository expose DFU (runtime 2fe3:0005, or 1d50:6002 for
the Ubertooth app, DFU mode 2fe3:ffff) and an MCUboot shell on their CDC-ACM port (MicroPython
its REPL, which reaches the shell through esdr.shell()). This command detaches
the board into DFU mode, downloads the signed image into the secondary slot,
waits until the board has left USB and come back running the new image (on
trial), checks that its shell still answers after a settle time, and only
then confirms it. An image that hangs or crashes is never confirmed, so the
trial watchdog resets it and MCUboot reverts to the previous one.
"""

import glob
import os
import re
import subprocess
import time
from pathlib import Path

from west.commands import WestCommand

# vid:pid of a running app (the Ubertooth app presents the Ubertooth One's IDs) and of DFU mode.
APP_IDS = ("2fe3:0005", "1d50:6002")
DFU_ID = "2fe3:ffff"
USB = Path("/sys/bus/usb/devices")
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def usb_boards():
    """{serial: (port path, product id)} of the attached esp-sdr boards.

    A board left in DFU mode has no serial number: it is keyed "dfu@<port>".
    """
    boards = {}
    for d in USB.glob("*-*"):
        if ":" in d.name or not (d / "idVendor").exists():
            continue
        try:
            ident = f"{(d / 'idVendor').read_text().strip()}:{(d / 'idProduct').read_text().strip()}"
            serial = (d / "serial").read_text().strip() if (d / "serial").exists() else ""
        except OSError:
            continue
        if ident in APP_IDS or ident == DFU_ID:
            boards[serial or f"dfu@{d.name}"] = (d.name, ident)
    return boards


def port_id(port):
    """vid:pid of the device on a USB port, or None."""
    try:
        return f"{(USB / port / 'idVendor').read_text().strip()}:{(USB / port / 'idProduct').read_text().strip()}"
    except OSError:
        return None


def shell_tty(serial):
    # The CDC-ACM interface is 0 in most apps, behind the vendor interface in others.
    links = glob.glob(f"/dev/serial/by-id/usb-Zephyr_Project_ESP-SDR*_{serial}-if0[0-9]")
    return links[0] if links else None


class Shell:
    """The board's Zephyr shell on its CDC-ACM port, or MicroPython's REPL there (apps/micropython),
    whose esdr.shell() runs the same commands: the running Python app is interrupted, and the
    launcher starts again (a soft reset) when the shell is closed."""

    def __init__(self, path):
        import serial

        # A port that just enumerated can be busy for a few seconds (a modem probe on the host).
        for attempt in range(5):
            try:
                self.s = serial.Serial(path, 115200, timeout=0.2)
                time.sleep(0.3)
                self.s.read(65536)
                break
            except serial.SerialException:
                if attempt == 4:
                    raise
                time.sleep(2)
        self.s.write(b"\x03\r\n")
        self.repl = ">>>" in self.read(0.5)

    def read(self, wait):
        end, out = time.time() + wait, b""
        while time.time() < end:
            out += self.s.read(4096)
        return ANSI.sub("", out.decode(errors="replace"))

    def cmd(self, line, wait=1.0):
        if self.repl:
            line = f"import esdr; print(esdr.shell({line!r}))"
        self.s.write((line + "\r\n").encode())
        return self.read(wait)

    def close(self):
        if self.repl:
            self.s.write(b"\x04")
            time.sleep(0.2)
        self.s.close()

    def app_list(self):
        """multiapp's app list; images from before the command's rename call it "app"."""
        out = self.cmd("multiapp list")
        if "command not found" in out:
            out = self.cmd("app list")
        return out


class Dfu(WestCommand):
    def __init__(self):
        super().__init__("dfu", "update a running esp-sdr board over USB DFU",
                         __doc__, accepts_unknown_args=False)

    def do_add_parser(self, parser_adder):
        p = parser_adder.add_parser(self.name, help=self.help, description=self.description)
        p.add_argument("-d", "--build-dir", default="build",
                       help="build directory (sysbuild or plain), default: build")
        p.add_argument("-i", "--image", help="signed image, instead of the build directory's")
        p.add_argument("-s", "--serial", help="USB serial of the board; needed with several")
        p.add_argument("--tries", type=int, default=3, help="detach/download attempts")
        p.add_argument("--settle", type=float, default=5.0,
                       help="seconds the new image must keep its shell alive before confirm")
        p.add_argument("--no-confirm", action="store_true",
                       help="leave the image on trial (reverted at the next reset)")
        p.add_argument("--storage", nargs="?", const="", metavar="IMAGE",
                       help="download the multiapp FAT volume (the build's "
                            "multiapp_fat.img, or IMAGE) instead of the firmware")
        p.add_argument("--esptool", action="store_true",
                       help="write MCUboot and the image (and with --storage the FAT "
                            "volume) with esptool over the USB-Serial-JTAG, which the "
                            "running app hands over (its usj command): no button, no DFU")
        p.add_argument("--c6", action="store_true",
                       help="update a P4 board's ESP32-C6 with the build directory's image "
                            "through the P4's REPL (apps/micropython): on the WHY2025 badge "
                            "the P4 hands USB-C J8 to the C6's USB-Serial-JTAG for --c6-time "
                            "seconds and west flash writes it; on the "
                            "ESP32-P4-Function-EV-Board the image goes over SDIO "
                            "(esdr.request, ESDR_REQ_FW_*)")
        p.add_argument("--c6-time", type=int, default=30,
                       help="seconds the P4 leaves J8 to the C6 (default 30)")
        p.add_argument("--dfu-board", action="store_true",
                       help="if --serial is not on USB, update the one board found in DFU "
                            "mode (one an earlier try left there; it has no serial to check)")
        return p

    def find_image(self, args):
        if args.image:
            if not Path(args.image).is_file():
                self.die(f"{args.image}: no such image")
            return Path(args.image)
        b = Path(args.build_dir)
        if (b / "zephyr" / "zephyr.signed.bin").exists():
            return b / "zephyr" / "zephyr.signed.bin"
        dom = b / "domains.yaml"
        if dom.exists():
            m = re.search(r"^default:\s*(\S+)", dom.read_text(), re.M)
            if m and (b / m.group(1) / "zephyr" / "zephyr.signed.bin").exists():
                return b / m.group(1) / "zephyr" / "zephyr.signed.bin"
        self.die(f"no zephyr.signed.bin in {b} (sysbuild with MCUboot?)")

    def find_storage(self, args):
        if args.storage:
            if not Path(args.storage).is_file():
                self.die(f"{args.storage}: no such image")
            return Path(args.storage)
        b = Path(args.build_dir)
        for cand in (b / "multiapp_fat.img", b / "multiapp" / "multiapp_fat.img"):
            if cand.exists():
                return cand
        self.die(f"no multiapp_fat.img in {b} (a multiapp build?)")

    def slot1_last_sector(self, args):
        """Offset of the update slot's last sector, from the build's devicetree."""
        dts = None
        b = Path(args.build_dir)
        for cand in b.glob("*/zephyr/zephyr.dts"):
            if "mcuboot" not in str(cand):
                dts = cand
        if dts is None:
            dts = b / "zephyr" / "zephyr.dts"
        m = re.search(r"slot1_partition: partition@[0-9a-f]+ \{.*?reg = < (0x[0-9a-f]+) (0x[0-9a-f]+) >",
                      dts.read_text(), re.S)
        if not m:
            self.die(f"{dts}: no slot1_partition")
        return int(m.group(1), 16) + int(m.group(2), 16) - 4096

    def esptool_flash(self, args, sn):
        """Everything in one esptool run: its reset at the end starts the app, which takes the
        USB port back."""
        b = Path(args.build_dir)
        mcuboot = b / "mcuboot" / "zephyr" / "zephyr.bin"
        if not mcuboot.exists():
            self.die(f"no {mcuboot}: --esptool needs a sysbuild with MCUboot")
        image = self.find_image(args)
        # The padded image with its confirmed trailer (CONFIG_MCUBOOT_GENERATE_CONFIRMED_IMAGE,
        # apps/common/Kconfig): written plain, the slot keeps an earlier image's trailer, which
        # MCUboot reads as bad magic, and the confirm below fails.
        confirmed = image.with_name("zephyr.signed.confirmed.bin")
        if not args.image and confirmed.exists():
            image = confirmed
        parts = [("0x0", mcuboot), ("0x20000", image)]
        if args.storage is not None:
            parts.append(("0x400000", self.find_storage(args)))
        # A stale update request in the update slot would make MCUboot swap the new image out.
        blank = b / "slot1_trailer_blank.bin"
        blank.write_bytes(b"\xff" * 4096)
        parts.append((hex(self.slot1_last_sector(args)), blank))

        mac = ":".join(sn[i:i + 2] for i in range(0, len(sn), 2))
        usj = f"/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_{mac}-if00"
        if not os.path.exists(usj):
            try:
                sh = Shell(shell_tty(sn))
                sh.cmd("usj", 0.5)
                sh.close()
            except Exception:  # the port goes away while usj answers, as it should
                pass
            if not self.wait_for(lambda: os.path.exists(usj), 15):
                self.die(f"{sn}: no USB-Serial-JTAG ({usj}) after usj")
        argv = ["esptool", "--port", usj, "--before", "default-reset", "--after", "hard-reset",
                "write-flash", "-u", "--flash-mode", "dio", "--flash-freq", "80m",
                "--flash-size", "8MB"]
        for off, path in parts:
            argv += [off, str(path)]
            self.inf(f"{sn}: {off} {path}")
        r = subprocess.run(argv, capture_output=True, text=True)
        if r.returncode != 0:
            self.die(f"{sn}: esptool failed:\n{(r.stdout + r.stderr).strip()[-600:]}")
        if not self.wait_for(lambda: shell_tty(sn) is not None, 60):
            self.die(f"{sn}: did not come back after esptool")
        time.sleep(1.0)
        sh = Shell(shell_tty(sn))
        out = sh.app_list() if args.storage is not None else sh.cmd("kernel version")
        # The image written this way boots unconfirmed: without a confirm the trial watchdog
        # resets it every half minute (there is no older image to revert to).
        if not args.no_confirm:
            state = sh.cmd("mcuboot confirm", 0.5) + sh.cmd("mcuboot")
            if not re.search(r"^confirmed: *1", state, re.M):
                sh.close()
                self.die(f"{sn}: written with esptool, but the confirm failed:\n{state}")
        sh.close()
        self.inf(f"{sn}: written with esptool{'' if args.no_confirm else ' and confirmed'}\n"
                 f"{out.strip()}")

    def c6_link_flash(self, args, sn, sh):
        """A C6 that the ESP-IDF bootloader starts (the EV board's): the image over SDIO.

        The REPL runs in raw mode: each chunk goes as one line of base64 to a helper that hands
        it to esdr.request(ESDR_REQ_FW_WRITE); the C6 writes its other OTA partition, checks the
        CRC and restarts into it.
        """
        import base64
        import binascii

        image = Path(args.build_dir) / "zephyr" / "zephyr.bin"
        if not image.is_file():
            sh.close()
            self.die(f"{image}: no such image (a build for the C6, not sysbuild)")
        data = image.read_bytes()
        data += b"\xff" * (-len(data) % 4)
        chunk = 508

        def raw(code, wait=5.0):
            # In slices: the port's console ring (512 bytes) drops what a whole high-speed
            # packet brings while it still holds input.
            text = code.encode() + b"\x04"
            for i in range(0, len(text), 256):
                sh.s.write(text[i:i + 256])
                sh.s.flush()
                time.sleep(0.002)
            end, out = time.time() + wait, b""
            while time.time() < end and out.count(b"\x04") < 2:
                out += sh.s.read(4096)
            ok, _, rest = out.partition(b"OK")
            res, _, err = rest.partition(b"\x04")
            if b"\x04" not in rest or err.strip(b"\x04>"):
                sh.close()
                self.die(f"{sn}: {code.splitlines()[0][:40]}: {(err or out).decode(errors='replace')}")
            return res.decode(errors="replace")

        sh.s.write(b"\r\x01")
        time.sleep(0.3)
        sh.s.read(65536)
        raw("import esdr, struct, binascii\n"
            "def w(o, d):\n"
            " esdr.request(0x16, data=struct.pack('<I', o) + binascii.a2b_base64(d))\n")
        raw(f"esdr.request(0x15, data=struct.pack('<I', {len(data)}))")
        start = time.time()
        for off in range(0, len(data), chunk):
            raw(f"w({off},'{base64.b64encode(data[off:off + chunk]).decode()}')")
            if off // chunk % 200 == 0:
                self.inf(f"{sn}: {off + chunk}/{len(data)}")
        raw(f"esdr.request(0x17, data=struct.pack('<I', {binascii.crc32(data)}))")
        sh.s.write(b"\x02")
        sh.close()
        self.inf(f"{sn}: C6 written in {time.time() - start:.0f} s ({len(data)} bytes), "
                 "it restarts into the new image")

    def c6_flash(self, args):
        """A P4 board's C6 through the P4: over SDIO where the REPL can, else (the WHY2025
        badge) J8 to the C6, west flash, J8 back."""
        sn, _ = self.find_board(args)
        before = set(glob.glob("/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*"))
        sh = Shell(shell_tty(sn))
        if not sh.repl:
            sh.close()
            self.die(f"{sn}: no MicroPython REPL (--c6 needs apps/micropython on the P4)")
        # The answer spelled so that the echoed line does not contain it.
        sh.s.write(b"import esdr; print('SD' + 'IO' if hasattr(esdr, 'request') and "
                   b"not hasattr(esdr, 'usb_to_c6') else 'J' + '8')\r\n")
        if "SDIO" in sh.read(1.0):
            self.c6_link_flash(args, sn, sh)
            return
        # The P4 leaves USB as it hands J8 over: no answer to wait for.
        sh.s.write(f"import esdr; esdr.usb_to_c6({args.c6_time})\r\n".encode())
        time.sleep(0.5)
        try:
            sh.s.close()
        except Exception:  # serial.SerialException: the port is gone already
            pass
        usj = []
        if not self.wait_for(lambda: usj.extend(set(glob.glob(
                "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*")) - before)
                or usj, 15):
            self.die(f"{sn}: the C6's USB-Serial-JTAG did not appear on J8")
        self.inf(f"{sn}: J8 to the C6 ({usj[0]}), {args.c6_time} s")
        start = time.time()
        r = subprocess.run(["west", "flash", "-d", args.build_dir, "--esp-device", usj[0]],
                           capture_output=True, text=True)
        if r.returncode != 0:
            self.die(f"{sn}: west flash failed:\n{(r.stdout + r.stderr).strip()[-600:]}")
        self.inf(f"{sn}: C6 written in {time.time() - start:.0f} s, waiting for the P4")
        if not self.wait_for(lambda: shell_tty(sn) is not None,
                             args.c6_time - (time.time() - start) + 30):
            self.die(f"{sn}: the P4 did not take J8 back")
        self.inf(f"{sn}: J8 back to the P4")

    def find_board(self, args):
        boards = usb_boards()
        in_dfu = [k for k in boards if k.startswith("dfu@")]
        if args.serial:
            if args.serial in boards:
                return args.serial, boards[args.serial][0]
            if len(in_dfu) == 1 and args.dfu_board:
                # A board stuck in DFU mode (an earlier try failed) has no serial to match.
                self.wrn(f"{args.serial}: not on USB, using the board in DFU mode")
                return args.serial, boards[in_dfu[0]][0]
            hint = " (one board is in DFU mode: --dfu-board updates it)" if len(in_dfu) == 1 else ""
            self.die(f"{args.serial}: not on USB (found: {', '.join(boards) or 'none'}){hint}")
        if len(boards) != 1:
            self.die(f"{len(boards)} esp-sdr boards on USB ({', '.join(boards) or 'none'}): "
                     "pick one with --serial")
        sn = next(iter(boards))
        if sn.startswith("dfu@"):
            self.die(f"{sn}: the board is in DFU mode and has no serial number: "
                     "pass --serial to find its shell after the update")
        return sn, boards[sn][0]

    def wait_for(self, cond, seconds, step=0.2):
        end = time.time() + seconds
        while time.time() < end:
            if cond():
                return True
            time.sleep(step)
        return False

    def dfu_util(self, argv, timeout):
        try:
            return subprocess.run(["dfu-util"] + argv, capture_output=True, text=True,
                                  timeout=timeout)
        except FileNotFoundError:
            self.die("dfu-util not found")
        except subprocess.TimeoutExpired:
            self.wrn(f"dfu-util {' '.join(argv)}: no result after {timeout} s")
            return None

    def download(self, sn, port, image, alt="slot1_image"):
        if port_id(port) != DFU_ID:
            self.dfu_util(["-S", sn, "-d", port_id(port) or APP_IDS[0], "-e"], 15)
        if not self.wait_for(lambda: port_id(port) == DFU_ID, 15):
            self.wrn(f"{sn}: no DFU mode")
            return False
        r = self.dfu_util(["-d", DFU_ID, "-p", port, "-a", alt, "-D",
                           str(image)], 120)
        if r is None:
            return False
        if "Download done" not in r.stdout:
            self.wrn(f"{sn}: download failed: {(r.stdout + r.stderr).strip()[-300:]}")
            return False
        return True

    def do_run(self, args, unknown):
        if args.c6:
            self.c6_flash(args)
            return
        if args.esptool:
            sn, _ = self.find_board(args)
            self.esptool_flash(args, sn)
            return
        storage = args.storage is not None
        image = self.find_storage(args) if storage else self.find_image(args)
        sn, port = self.find_board(args)
        self.inf(f"{sn} ({port}): {image} ({image.stat().st_size} bytes)")

        for attempt in range(1, args.tries + 1):
            if not self.download(sn, port, image, "fat" if storage else "slot1_image"):
                continue
            self.inf(f"{sn}: downloaded (try {attempt}), waiting for the swap")
            # Leave USB first: the old image's shell link can outlive the download.
            if not self.wait_for(lambda: port_id(port) != DFU_ID, 30):
                self.wrn(f"{sn}: did not reset after the download")
                continue
            if not self.wait_for(lambda: port_id(port) in APP_IDS and shell_tty(sn), 90):
                self.wrn(f"{sn}: did not come back")
                continue
            break
        else:
            self.die(f"{sn}: update failed")

        time.sleep(1.0)
        if storage:
            try:
                sh = Shell(shell_tty(sn))
                out = sh.app_list()
                sh.close()
            except Exception as e:  # serial.SerialException, or the link went away
                self.die(f"{sn}: no shell after the volume download ({e})")
            self.inf(f"{sn}: volume written\n{out.strip()}")
            return
        try:
            sh = Shell(shell_tty(sn))
            state = sh.cmd("mcuboot")
            first = re.search(r"Uptime: *(\d+)", sh.cmd("kernel uptime"))
        except Exception as e:  # serial.SerialException, or the link went away
            self.die(f"{sn}: the new image dropped off USB right after booting ({e}); "
                     "not confirmed, MCUboot reverts it after the trial watchdog")
        if re.search(r"^confirmed: *1", state, re.M):
            sh.close()
            self.die(f"{sn}: running a confirmed image: the new one did not boot "
                     "(MCUboot kept or restored the old one)")
        if args.no_confirm:
            sh.close()
            self.inf(f"{sn}: new image on trial, not confirmed")
            return
        self.inf(f"{sn}: new image on trial, checking it for {args.settle:.0f} s")
        sh.close()
        time.sleep(args.settle)
        tty = shell_tty(sn)
        alive = ""
        if tty and port_id(port) in APP_IDS:
            try:
                sh = Shell(tty)
                alive = sh.cmd("kernel uptime")
            except Exception as e:  # the device went away while opening
                alive = str(e)
        up = re.search(r"Uptime: *(\d+)", alive)
        if not up:
            self.die(f"{sn}: the new image stopped answering ({alive.strip()[-120:]!r}); "
                     "not confirmed, MCUboot reverts it after the trial watchdog")
        # The uptime must have grown by the settle time since the first look.
        since = int(first.group(1)) + args.settle * 1000 if first else args.settle * 1000
        if int(up.group(1)) < since:
            self.die(f"{sn}: the board restarted during the check (uptime {up.group(1)} ms): "
                     "not confirmed")
        try:
            # A revert inside the settle time leaves an old, already confirmed image answering.
            if re.search(r"^confirmed: *1", sh.cmd("mcuboot"), re.M):
                sh.close()
                self.die(f"{sn}: the board is running a confirmed image again: the new one "
                         "was reverted, not confirmed")
            out = sh.cmd("mcuboot confirm", 0.5) + sh.cmd("mcuboot")
            sh.close()
        except Exception as e:  # serial.SerialException, or the link went away
            self.die(f"{sn}: the shell went away while confirming ({e}): not confirmed")
        if not re.search(r"^confirmed: *1", out, re.M):
            self.die(f"{sn}: confirm failed:\n{out}")
        self.inf(f"{sn}: confirmed")
