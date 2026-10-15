#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""List the firmware images of apps/*/sample.yaml as JSON, for default.nix.

    images.py APPS_DIR [EXCLUDED_APP ...]

One image per test and allowed platform, as twister builds them. A test with
extra_args is a variant and gets its last name component as a suffix
(osmosdr_serial-diag); pytest harness tests need a desk fixture and are left out.
"""

import json
import sys
from pathlib import Path

import yaml


def as_list(value):
    if value is None:
        return []
    return value.split() if isinstance(value, str) else list(value)


def main():
    apps, exclude = Path(sys.argv[1]), set(sys.argv[2:])
    images = {}
    for sample in sorted(apps.glob("*/sample.yaml")):
        app = sample.parent.name
        if app in exclude:
            continue
        doc = yaml.safe_load(sample.read_text())
        common = doc.get("common") or {}
        for test, conf in doc["tests"].items():
            conf = {**common, **(conf or {})}
            if conf.get("harness") == "pytest":
                continue
            extra_args = as_list(conf.get("extra_args"))
            name = app + ("-" + test.rsplit(".", 1)[1] if extra_args else "")
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
                    "sysbuild": bool(conf.get("sysbuild", False)),
                    "extraArgs": extra_args,
                }
    json.dump(list(images.values()), sys.stdout, indent=1)


if __name__ == "__main__":
    main()
