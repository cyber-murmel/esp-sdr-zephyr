#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn nix-built images into release assets, for .github/workflows/release.yml.

    release.py RESULT_DIR APPS_DIR TAG OUT_DIR

RESULT_DIR holds <board>/<name>/ per image as default.nix's firmware (nix/collect.py) writes
them, for one board (firmwareByBoard.<board>) or several. Per board, OUT_DIR gets:

- esp-sdr-<name>-<tag>-<board>.bin: the image's merged.bin, written at 0x0 with esptool or
  the web flasher; for the RP2040 esp-sdr-<name>-<tag>-<board>.uf2 instead.
- esp-sdr-<tag>-<board>.zip: everything collect.py wrote for the board's images (ELF files, the
  signed USB DFU images, flash_args, configs, multiapp's llext files).
- firmware-<board>.json: one entry per image for the web flasher (doc/_extra/flasher), which
  lists a board's images per chip by rank: 0 multiapp, 1 the board's osmosdr image when its
  multiapp does not carry it already, 2 the rest. Which apps a multiapp image carries comes from
  its llext files.
"""

import hashlib
import json
import os
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from images import list_images  # noqa: E402

# The apps that make a board an SDR for a host (osmosdr / GNU Radio / SoapySDR), one per chip.
OSMOSDR_APPS = ("osmosdr", "osmosdr_serial", "osmosdr_hs", "osmosdr_bridge")


def multiapp_apps(image_dir):
    """The apps a multiapp image carries: its llext files' names."""
    return {f.stem for f in (image_dir / "llext").glob("*.llext")}


def rank(img, carried):
    """The image's place in the flasher's list for its board and chip."""
    if img["name"] == "multiapp":
        return 0
    if img["name"] == img["app"] and img["app"] in OSMOSDR_APPS and img["app"] not in carried:
        return 1
    return 2


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def write_zip(board_dir, root, out):
    """The board's result tree under ROOT/, the same bytes for the same input."""
    # A store tree of symlinks: walked following them.
    files = sorted(Path(d) / f for d, _, names in os.walk(board_dir, followlinks=True)
                   for f in names)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for path in files:
            info = zipfile.ZipInfo(f"{root}/{path.relative_to(board_dir).as_posix()}")
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if path.suffix == ".elf" else 0o644
            info.external_attr = (0o100000 | mode) << 16
            z.writestr(info, path.read_bytes())


def release_board(board_dir, images, tag, out):
    board = board_dir.name
    by_name = {img["name"]: img for img in images}
    built = sorted(d.name for d in board_dir.iterdir() if d.is_dir())
    unknown = [name for name in built if name not in by_name]
    if unknown:
        sys.exit(f"{board}: no sample.yaml test for {', '.join(unknown)}")

    carried = {}
    for name in built:
        if name == "multiapp":
            carried[by_name[name]["chip"]] = multiapp_apps(board_dir / name)

    entries = []
    for name in built:
        img, src = by_name[name], board_dir / name
        chip_carries = carried.get(img["chip"], set())
        if img["chip"] == "rp2040":
            image, ext = next(src.glob("*.uf2")), "uf2"
        else:
            image, ext = src / "merged.bin", "bin"
        asset = out / f"esp-sdr-{name}-{tag}-{board}.{ext}"
        asset.write_bytes(image.read_bytes())
        entries.append({
            "board": board,
            "chip": img["chip"],
            "app": img["app"],
            "name": name,
            "description": img["description"],
            "file": asset.name,
            "size": asset.stat().st_size,
            "sha256": sha256(asset),
            "rank": rank(img, chip_carries),
            "inMultiapp": name == img["app"] and img["app"] in chip_carries,
        })
    entries.sort(key=lambda e: (e["chip"], e["rank"], e["name"]))

    write_zip(board_dir, f"esp-sdr-{tag}-{board}", out / f"esp-sdr-{tag}-{board}.zip")
    (out / f"firmware-{board}.json").write_text(json.dumps(entries, indent=1) + "\n")
    return entries


def main():
    result, apps, tag, out = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4])
    out.mkdir(parents=True, exist_ok=True)
    images = list_images(apps)
    for board_dir in sorted(d for d in result.iterdir() if d.is_dir()):
        board_images = [img for img in images if img["board"] == board_dir.name]
        if not board_images:
            continue
        for e in release_board(board_dir, board_images, tag, out):
            print(f"{e['file']}  rank {e['rank']}{'  (in multiapp)' if e['inMultiapp'] else ''}")


if __name__ == "__main__":
    main()
