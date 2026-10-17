#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes a launcher app from a GNU Radio Companion flowgraph, laid out as in the badge.team
hatchery: DIR/<id>/ (<id> is the flowgraph's id) with the flowgraph, its export (grcc,
unmodified), an __init__.py that runs the export's main() and a metadata.json whose name,
description and author come from the flowgraph's options block (Title, Description,
Author). Run it again after changing the flowgraph: the export and the metadata follow it,
the category stays, the revision goes up when the export changed.

usage: mkegg.py FLOWGRAPH.grc [-d DIR] [--category CAT] [--grcc PATH]

Copy the folder to apps/ on the board's FLASH or SD drive, or make it in eggs/ to bundle it
with the firmware. grcc comes with GNU Radio (3.10); --grcc or $GRCC names it when it is not
on the PATH.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

import yaml

INIT = """\
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs {id}.py, which GNU Radio Companion exported from {id}.grc (unmodified).
from .{id} import main

main()
"""


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except FileNotFoundError:
        return None


def write(path, text):
    if read(path) != text:
        with open(path, "w") as f:
            f.write(text)
        return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("grc", help="the flowgraph (.grc)")
    ap.add_argument("-d", "--dir", default=".", help="where the app's folder goes")
    ap.add_argument("--category", help="the menu section (receive, transmit, ...)")
    ap.add_argument("--grcc", default=os.environ.get("GRCC", "grcc"))
    a = ap.parse_args()

    with open(a.grc) as f:
        opts = yaml.safe_load(f)["options"]["parameters"]
    app = opts["id"]
    if not re.fullmatch(r"[a-z_][a-z0-9_]*", app):
        sys.exit("{}: the flowgraph's id names the app's folder and module: lower-case letters, "
                 "digits and _".format(app))
    egg = os.path.join(a.dir, app)
    changed = False
    with tempfile.TemporaryDirectory() as tmp:
        # Exported before the app's folder is touched: a failure leaves no half-made app.
        try:
            r = subprocess.run([a.grcc, "-o", tmp, a.grc], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
        except OSError as e:
            sys.exit("{}: {} (--grcc or $GRCC names it)".format(a.grcc, e))
        if r.returncode != 0:
            sys.exit(r.stdout + "grcc failed on " + a.grc)
        os.makedirs(egg, exist_ok=True)
        # The flowgraph travels with its export.
        grc = os.path.join(egg, app + ".grc")
        if os.path.abspath(a.grc) != os.path.abspath(grc):
            shutil.copyfile(a.grc, grc)
        # The export, and the modules of embedded Python blocks next to it.
        for name in sorted(os.listdir(tmp)):
            if name.endswith(".py"):
                changed |= write(os.path.join(egg, name), read(os.path.join(tmp, name)))
    write(os.path.join(egg, "__init__.py"), INIT.format(id=app))

    path = os.path.join(egg, "metadata.json")
    old = json.loads(read(path) or "{}")
    revision = old.get("revision")
    meta = {
        "name": str(opts.get("title") or app),
        "description": str(opts.get("description") or ""),
        "category": a.category or old.get("category") or "uncategorised",
        "author": str(opts.get("author") or ""),
        "revision": (revision or 0) + 1 if changed or not isinstance(revision, int) else revision,
    }
    # Fields of the app's own (hidden, icon, ...) stay.
    meta = dict(old, **meta)
    write(path, json.dumps(meta, indent=2) + "\n")
    print("{}: {} revision {}".format(egg, meta["name"], meta["revision"]))


if __name__ == "__main__":
    main()
