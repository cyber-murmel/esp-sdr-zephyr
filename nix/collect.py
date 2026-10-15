#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copy one build's images into a result directory, for default.nix.

    collect.py BUILD_DIR APP CHIP OUT_DIR

Per image (MCUboot and the app with sysbuild, the app alone without):
<image>.elf, <image>.bin, <image>.signed.bin (the USB DFU image) and
<image>.config. multiapp adds its FAT volume and the llext files. All of them
at their flash offsets (from the runners' --esp-app-address) go into
flash_args, for "esptool --chip CHIP write-flash @flash_args" in OUT_DIR, and
are merged into merged.bin, to be written at 0x0.
"""

import shutil
import subprocess
import sys
from pathlib import Path

import yaml

# The FAT volume's partition ("fat"), as west dfu --storage and the release write it.
FAT_OFFSET = "0x400000"


def runner_args(zephyr_dir):
    runners = yaml.safe_load((zephyr_dir / "runners.yaml").read_text())
    args = (a.removeprefix("--").split("=", 1) for a in runners["args"]["esp32"])
    return {a[0]: a[1] for a in args if len(a) == 2}


def main():
    build, app, chip, out = Path(sys.argv[1]), sys.argv[2], sys.argv[3], Path(sys.argv[4])
    out.mkdir(parents=True)

    domains = build / "domains.yaml"
    if domains.exists():
        order = yaml.safe_load(domains.read_text())["flash_order"]
        dirs = {name: build / name for name in order}
    else:
        order, dirs = [app], {app: build}

    segments = []
    for name in order:
        zephyr = dirs[name] / "zephyr"
        shutil.copy(zephyr / "zephyr.elf", out / f"{name}.elf")
        shutil.copy(zephyr / "zephyr.bin", out / f"{name}.bin")
        shutil.copy(zephyr / ".config", out / f"{name}.config")
        image = f"{name}.bin"
        if (zephyr / "zephyr.signed.bin").exists():
            image = f"{name}.signed.bin"
            shutil.copy(zephyr / "zephyr.signed.bin", out / image)
        segments.append((runner_args(zephyr)["esp-app-address"], image))

    fat = dirs[app] / "multiapp_fat.img"
    if fat.exists():
        shutil.copy(fat, out / fat.name)
        segments.append((FAT_OFFSET, fat.name))
    llext = dirs[app] / "llext" / "files"
    if llext.is_dir():
        shutil.copytree(llext, out / "llext")

    args = runner_args(dirs[app] / "zephyr")
    flash = ["--flash-mode", args["esp-flash-mode"], "--flash-freq", args["esp-flash-freq"],
             "--flash-size", args["esp-flash-size"]]
    (out / "flash_args").write_text(
        " ".join(flash) + "\n" + "".join(f"{a} {f}\n" for a, f in segments))
    subprocess.run(["esptool", "--chip", chip, "merge-bin", "-o", "merged.bin", *flash,
                    *(x for s in segments for x in s)], cwd=out, check=True)


if __name__ == "__main__":
    main()
