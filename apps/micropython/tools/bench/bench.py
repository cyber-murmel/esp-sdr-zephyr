#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs a bench script of this folder on a board running apps/micropython, with settings:
each NAME=VALUE becomes a line NAME = VALUE in front of the script (the scripts take their
settings from globals(), with defaults), which mpremote runs on the board.

usage: bench.py PORT SCRIPT [NAME=VALUE ...]
   bench.py /dev/ttyUSB0 egg_sliders.py EGG='"spectrum"' SECS=6 SLIDERS=0
   bench.py /dev/ttyACM0 datalink_auto.py SECS=25 TXG=8 RXG=32

mpremote comes from $MPREMOTE, else the workspace's modules/micropython, else the PATH. The
board resets softly first: each run gets a fresh interpreter, as the launcher gives an app.
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def mpremote():
    if os.environ.get("MPREMOTE"):
        return os.environ["MPREMOTE"].split()
    ws = os.path.abspath(os.path.join(HERE, *[".."] * 5))
    tool = os.path.join(ws, "modules", "micropython", "tools", "mpremote", "mpremote.py")
    if os.path.exists(tool):
        return [sys.executable, tool]
    found = shutil.which("mpremote")
    if found is None:
        sys.exit("mpremote not found: set MPREMOTE")
    return [found]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    port, script, settings = sys.argv[1], sys.argv[2], sys.argv[3:]
    path = script if os.path.exists(script) else os.path.join(HERE, script)
    lines = []
    for s in settings:
        name, sep, value = s.partition("=")
        if not sep or not name.isidentifier():
            sys.exit("setting {!r}: NAME=VALUE".format(s))
        lines.append("{} = {}\n".format(name, value))
    with open(path) as f:
        body = f.read()
    with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as t:
        t.write("".join(lines) + body)
    try:
        return subprocess.call(mpremote() + ["connect", port, "soft-reset", "run", t.name])
    finally:
        os.unlink(t.name)


if __name__ == "__main__":
    sys.exit(main())
