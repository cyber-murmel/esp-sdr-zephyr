#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""List the firmware images of apps/*/sample.yaml as JSON, for default.nix.

    images.py APPS_DIR [EXCLUDED_APP ...]

One image per test and allowed platform, as twister builds them. A test with
extra_args is a variant and gets its last name component as a suffix
(osmosdr_serial-diag); pytest harness tests need a desk fixture and are left out.

devKey marks an image whose sysbuild signs MCUboot's images with the project's own key
(apps/common/sysbuild/Kconfig.signing, which an app opts into in its Kconfig.sysbuild, and which
defaults on per board). A release build has no such key: default.nix builds these with
ESDR_DEV_KEY=1, MCUboot's public development key, so the images are for the bench only.
"""

import json
import re
import sys
from pathlib import Path

import yaml


def as_list(value):
    if value is None:
        return []
    return value.split() if isinstance(value, str) else list(value)


SIGNING_KCONFIG = "common/sysbuild/Kconfig.signing"


# What the defaults of ESDR_SIGNING may be: "default y", "default n" and "default y if BOARD = "a" ||
# BOARD = "b"". Anything else (a != or a negation, an earlier "default n if", a condition on
# something but the board) would be misread, so it stops the build of the image list.
BOARD_CONDITION = re.compile(r'BOARD\s*=\s*"[^"]+"(?:\s*\|\|\s*BOARD\s*=\s*"[^"]+")*')


def signing_boards(apps):
    """The boards ESDR_SIGNING defaults to y for in Kconfig.signing: a set of names, or {"*"}."""
    path = Path(apps) / SIGNING_KCONFIG
    if not path.exists():
        return set()
    # The config's own block: up to the next unindented line.
    block = re.search(r"^config ESDR_SIGNING\n((?:[ \t].*\n|\n)*)", path.read_text(), re.M)
    if not block:
        sys.exit(f"{path}: no config ESDR_SIGNING, which nix/images.py reads")
    first = path.read_text()[:block.start(1)].count("\n")
    boards = set()
    for number, line in enumerate(block.group(1).splitlines(), first + 1):
        text = re.sub(r"\s+#.*$", "", line.strip())
        if text in ("help", "---help---"):
            break  # the help text runs to the end of the block
        if not re.match(r"default\b", text):
            continue
        m = re.fullmatch(r"default\s+([yn])(?:\s+if\s+(.+))?", text)
        if m and m.group(2) is None:
            if m.group(1) == "y":
                boards.add("*")
        elif m and m.group(1) == "y" and BOARD_CONDITION.fullmatch(m.group(2)):
            boards.update(re.findall(r'"([^"]+)"', m.group(2)))
        else:
            sys.exit(f"{path}:{number}: cannot read `{text}`: nix/images.py takes `default y`, "
                     '`default n` and `default y if BOARD = "a" || BOARD = "b"` of ESDR_SIGNING')
    return boards


def opts_into_signing(apps, app):
    """Whether the app's Kconfig.sysbuild sources the project's signing Kconfig."""
    kconfig = Path(apps) / app / "Kconfig.sysbuild"
    return kconfig.exists() and re.search(
        r'^\s*r?source\s+"[^"]*Kconfig\.signing"', kconfig.read_text(), re.M) is not None


def list_images(apps, exclude=()):
    """The images of APPS_DIR's sample.yaml files, as dicts (main() prints them)."""
    images = {}
    signing = signing_boards(apps)
    for sample in sorted(Path(apps).glob("*/sample.yaml")):
        app = sample.parent.name
        if app in exclude:
            continue
        doc = yaml.safe_load(sample.read_text())
        common = doc.get("common") or {}
        description = " ".join(((doc.get("sample") or {}).get("description") or "").split())
        for test, conf in doc["tests"].items():
            conf = {**common, **(conf or {})}
            if conf.get("harness") == "pytest":
                continue
            extra_args = as_list(conf.get("extra_args"))
            name = app + ("-" + test.rsplit(".", 1)[1] if extra_args else "")
            sysbuild = bool(conf.get("sysbuild", False))
            for platform in as_list(conf.get("platform_allow")):
                board, soc = platform.split("/")[:2]
                key = f"{board}/{name}"
                if key in images:
                    sys.exit(f"{sample}: {key} comes from two tests")
                images[key] = {
                    "app": app,
                    "name": name,
                    "board": board,
                    "chip": soc,
                    "platform": platform,
                    "sysbuild": sysbuild,
                    "devKey": sysbuild and bool(signing & {board, "*"})
                    and opts_into_signing(apps, app),
                    "extraArgs": extra_args,
                    "description": description,
                }
    return list(images.values())


def main():
    json.dump(list_images(sys.argv[1], set(sys.argv[2:])), sys.stdout, indent=1)


if __name__ == "__main__":
    main()
