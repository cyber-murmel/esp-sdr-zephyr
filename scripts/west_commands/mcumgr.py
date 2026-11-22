# SPDX-License-Identifier: GPL-3.0-or-later
"""west mcumgr: MCUmgr (Simple Management Protocol) over UDP to an ESP32-S31 multiapp board (doc/dev/network-services.md, phase 2).

The board answers only after `mcumgr arm <minutes>` on its shell. Images must be signed with the project's
key (apps/common/sysbuild/Kconfig.signing); `west build --sysbuild` makes zephyr.signed.bin of the
multiapp image. The protocol is in mcumgrlib.py (UDP port 4996, one datagram per request).

    west mcumgr -t <host> upload build/multiapp/zephyr/zephyr.signed.bin   (marks it for a test boot)
    west mcumgr -t <host> reset
    west mcumgr -t <host> confirm        (after the new image runs and works)
    west mcumgr -t <host> list | echo hello | fs put local /FLASH:/name | fs get /FLASH:/name local
"""

import argparse
import importlib.util
import sys
from pathlib import Path

from west.commands import WestCommand


def load_lib():
    path = Path(__file__).with_name("mcumgrlib.py")
    spec = importlib.util.spec_from_file_location("mcumgrlib", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["mcumgrlib"] = mod
    spec.loader.exec_module(mod)
    return mod


class Mcumgr(WestCommand):
    def __init__(self):
        super().__init__("mcumgr", "MCUmgr (Simple Management Protocol) over UDP to an esp-sdr S31 board: upload, test, confirm, reset, files",
                         __doc__)

    def do_add_parser(self, parser_adder):
        p = parser_adder.add_parser(self.name, help=self.help, description=self.description,
                                    formatter_class=argparse.RawDescriptionHelpFormatter)
        p.add_argument("-t", "--host", "--target", dest="host", required=True,
                       help="the board: IPv4 or IPv6 literal (fe80::1%%eth0 for link-local) or name "
                            "(esdr<mac>.local)")
        p.add_argument("-p", "--port", type=int, default=4996, help="UDP port (default 4996)")
        p.add_argument("--timeout", type=float, default=2.0, help="seconds per try (default 2)")
        p.add_argument("--payload", type=int, default=1444,
                       help="largest datagram payload, bytes (default 1444: MTU 1492 over IPv6)")
        sub = p.add_subparsers(dest="cmd", required=True, metavar="<command>")
        u = sub.add_parser("upload", help="upload a signed image into the second slot and mark it for a test boot")
        u.add_argument("image", help="signed image (zephyr.signed.bin)")
        u.add_argument("--confirm", action="store_true",
                       help="mark it permanent instead of a test (no revert if it does not run)")
        u.add_argument("--no-mark", action="store_true", help="upload only")
        u.add_argument("--reset", action="store_true", help="reset the board when done")
        sub.add_parser("list", help="the images in the slots")
        t = sub.add_parser("test", help="mark an uploaded image for a test boot (its hash, or the second slot's)")
        t.add_argument("hash", nargs="?", help="hex SHA-256 of the image")
        c = sub.add_parser("confirm", help="make the running image (or the given hash) permanent")
        c.add_argument("hash", nargs="?", help="hex SHA-256 of the image")
        sub.add_parser("reset", help="reset the board")
        e = sub.add_parser("echo", help="echo a text (is the gate open?)")
        e.add_argument("text", nargs="?", default="hello")
        sub.add_parser("info", help="MCUmgr parameters and the bootloader")
        f = sub.add_parser("fs", help="files on the board's FAT volume (/FLASH:/...)")
        fs = f.add_subparsers(dest="fscmd", required=True, metavar="put|get|stat|crc")
        fp = fs.add_parser("put", help="upload a file")
        fp.add_argument("local")
        fp.add_argument("remote")
        fg = fs.add_parser("get", help="download a file")
        fg.add_argument("remote")
        fg.add_argument("local")
        for name in ("stat", "crc"):
            fs.add_parser(name, help="size" if name == "stat" else "CRC32").add_argument("remote")
        return p

    def progress(self, what):
        last = [-1]

        def cb(done, total):
            pct = done * 100 // max(1, total)
            if pct != last[0] and (pct % 10 == 0 or done >= total):
                last[0] = pct
                self.inf(f"{what}: {done}/{total} bytes ({pct} %)")
        return cb

    def do_run(self, args, unknown):
        lib = self.lib = load_lib()
        try:
            cl = lib.Client(args.host, args.port, args.timeout, payload=args.payload)
            try:
                self.run_cmd(lib, cl, args)
            finally:
                cl.close()
        except lib.McumgrError as e:
            self.die(str(e))

    def show(self, images):
        for im in images:
            flags = [k for k in ("active", "confirmed", "pending", "permanent", "bootable") if im.get(k)]
            h = im.get("hash", b"")
            self.inf(f"slot {im.get('slot')}: version {self.lib.version_text(im.get('version'))} hash {h.hex()[:16]}... "
                     f"{' '.join(flags)}")

    def run_cmd(self, lib, cl, a):
        if a.cmd == "echo":
            self.inf(cl.echo(a.text))
        elif a.cmd == "info":
            self.inf(f"mcumgr params: {cl.params()}")
            try:
                self.inf(f"bootloader: {cl.bootloader()}, mode {cl.bootloader('mode')}")
            except lib.McumgrError as e:
                self.wrn(f"bootloader info: {e}")
        elif a.cmd == "list":
            self.show(cl.images())
        elif a.cmd == "reset":
            cl.reset()
            self.inf("reset sent")
        elif a.cmd in ("test", "confirm"):
            h = bytes.fromhex(a.hash) if a.hash else None
            if h is None and a.cmd == "test":
                spare = [i for i in cl.images() if not i.get("active") and i.get("slot") == 1]
                if not spare:
                    self.die("no image in the second slot to test")
                h = spare[0]["hash"]
            self.show(cl.mark(h, a.cmd == "confirm"))
        elif a.cmd == "upload":
            image = Path(a.image).read_bytes()
            h = lib.image_hash(image)
            cl.upload_image(image, self.progress("upload"))
            if not a.no_mark:
                self.show(cl.mark(h, a.confirm))
                self.inf("marked for a test boot: reset, then `west mcumgr confirm` once it runs"
                         if not a.confirm else "marked permanent")
            if a.reset:
                cl.reset()
                self.inf("reset sent")
        elif a.cmd == "fs":
            if a.fscmd == "put":
                data = Path(a.local).read_bytes()
                cl.fs_put(a.remote, data, self.progress("put"))
                self.inf(f"{a.remote}: {len(data)} bytes, crc32 {lib.crc32(data):#010x}"
                         f" (board: {self.board_crc(lib, cl, a.remote)})")
            elif a.fscmd == "get":
                data = cl.fs_get(a.remote, self.progress("get"))
                Path(a.local).write_bytes(data)
                self.inf(f"{a.local}: {len(data)} bytes, crc32 {lib.crc32(data):#010x}")
            elif a.fscmd == "stat":
                self.inf(f"{a.remote}: {cl.fs_stat(a.remote)} bytes")
            else:
                self.inf(f"{a.remote}: crc32 {cl.fs_crc(a.remote):#010x}")

    @staticmethod
    def board_crc(lib, cl, path):
        try:
            return f"{cl.fs_crc(path):#010x}"
        except lib.McumgrError as e:
            return str(e)
