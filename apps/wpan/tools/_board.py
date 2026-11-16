# SPDX-License-Identifier: GPL-3.0-or-later
"""What the wpan tools share: a board's shell port and the shell's colour codes."""

import glob
import re

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def port_of(board):
    """A board's shell port: a /dev path, or its USB serial number (as in /dev/serial/by-id)."""
    if board.startswith("/"):
        return board
    ports = glob.glob(f"/dev/serial/by-id/*_{board}-if00")
    if len(ports) != 1:
        raise SystemExit(f"{board}: {len(ports)} matching ports in /dev/serial/by-id")
    return ports[0]
