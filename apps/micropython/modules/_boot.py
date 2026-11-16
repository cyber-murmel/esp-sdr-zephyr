# SPDX-License-Identifier: GPL-3.0-or-later
"""Mounts the board's volumes at every (soft) start: /FLASH: (scripts, the working directory)
and /SD: (recordings, may be missing or unreadable), and sets the bench's RF switch where the
image drives it. In place of the port's _boot.py, which stops at the first volume that fails:
a card without a usable file system took /FLASH: down."""

import os
import sys
import vfs
import zephyr


def _mount(path):
    if not hasattr(zephyr, "FileSystem"):  # an image without file systems
        return False
    try:
        vfs.mount(zephyr.FileSystem(path), path)
    except Exception as e:
        print("{} not mounted: {}".format(path, e))
        return False
    sys.path.append(path + "/lib")
    return True


if _mount("/FLASH:"):
    os.chdir("/FLASH:")
_mount("/SD:")

# The bench's RF switch, on the boards that drive it (the ESP32-C5 image): its path at the
# board's first start; soft resets keep it.
try:
    import rfswitch
    import esdr

    if esdr.first_start():
        print("rfswitch: A{}-B{}".format(*rfswitch.boot()))
    del rfswitch, esdr
except ImportError:
    pass

del _mount, os, sys, vfs, zephyr
