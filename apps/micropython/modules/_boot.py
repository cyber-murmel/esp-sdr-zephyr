# SPDX-License-Identifier: GPL-3.0-or-later
"""Mounts the board's volumes at every (soft) start: /FLASH: (scripts, the working directory)
and /SD: (recordings, may be missing or unreadable). In place of the port's _boot.py, which
stops at the first volume that fails: a card without a usable file system took /FLASH: down."""

import os
import sys
import vfs
import zephyr


def _mount(path):
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

del _mount, os, sys, vfs, zephyr
