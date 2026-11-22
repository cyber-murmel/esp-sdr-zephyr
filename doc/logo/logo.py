#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make the logo's other formats from doc/logo/logo.svg, the one file kept in git.

The logo combines the logos of the Zephyr Project, MicroPython and GNU Radio (README.rst). Its
page is 2048 x 2048 with wide empty margins; the drawing fills x 512 to 1536 and y 256 to 1788.
Small sizes use the drawing cut to a square around it (CROP), so that it stays readable at 16
pixels.

Written to the output directory (default doc/_build_logo/, which git ignores):

- logo.svg              the logo as it is (the docs' sidebar, html_logo)
- favicon.svg           the cut drawing (html_favicon)
- logo-512.png          the logo, 512 pixels, transparent (a social preview, for instance)
- root/favicon.ico      16, 32 and 48 pixels, the cut drawing (browsers ask the site root for it)
- root/apple-touch-icon.png  180 pixels, the cut drawing on white (iOS asks the site root too)

doc/conf.py runs this at the start of every Sphinx build and puts root/ at the site's root
(html_extra_path). The PNGs come from rsvg-convert (librsvg: in shell.nix's dev shell, the CI's
docs job installs librsvg2-bin); the ICO holds them as PNG images, written here.

Run: python3 doc/logo/logo.py [OUTDIR]
"""

import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "logo.svg"
OUT = HERE.parent / "_build_logo"
# x, y, side of the square around the drawing, in the page's units
CROP = (224, 222, 1600)
ICO_SIZES = (16, 32, 48)
TOUCH_SIZE = 180


def cropped(svg):
    """svg with its root's size and viewBox cut to CROP."""
    head, sep, rest = svg.partition("<svg")
    root, gt, body = rest.partition(">")
    x, y, side = CROP
    for old, new in (('width="2048"', f'width="{side}"'), ('height="2048"', f'height="{side}"'),
                     ('viewBox="0 0 2048 2048.0001"', f'viewBox="{x} {y} {side} {side}"')):
        if old not in root:
            sys.exit(f"{SOURCE}: no {old} on the root element; update CROP and cropped()")
        root = root.replace(old, new)
    return head + sep + root + gt + body


def png(svg, size, background=None):
    """svg (a file) rendered as a PNG of size x size pixels."""
    rsvg = shutil.which("rsvg-convert")
    if rsvg is None:
        sys.exit("rsvg-convert not found: use shell.nix's dev shell or install librsvg")
    cmd = [rsvg, "-w", str(size), "-h", str(size), str(svg)]
    if background:
        cmd[1:1] = ["--background-color", background]
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout


def ico(images):
    """An ICO file holding PNG images, {size: png bytes}."""
    head = struct.pack("<HHH", 0, 1, len(images))
    offset = len(head) + 16 * len(images)
    entries, data = b"", b""
    for size, blob in sorted(images.items()):
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(blob),
                               offset + len(data))
        data += blob
    return head + entries + data


def build(out=OUT):
    out = Path(out)
    (out / "root").mkdir(parents=True, exist_ok=True)
    svg = SOURCE.read_text()
    shutil.copyfile(SOURCE, out / "logo.svg")
    favicon = out / "favicon.svg"
    favicon.write_text(cropped(svg))
    (out / "logo-512.png").write_bytes(png(SOURCE, 512))
    (out / "root" / "favicon.ico").write_bytes(ico({s: png(favicon, s) for s in ICO_SIZES}))
    (out / "root" / "apple-touch-icon.png").write_bytes(png(favicon, TOUCH_SIZE, "white"))
    return out


if __name__ == "__main__":
    print(build(sys.argv[1] if len(sys.argv) > 1 else OUT))
