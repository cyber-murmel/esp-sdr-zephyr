#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""List the blobs of west projects as JSON, for default.nix.

    blobs.py PROJECT_PATH=MODULE_YML ...

What "west blobs fetch" would fetch: each blob's path in the workspace, URL
and sha256.
"""

import json
import sys

import yaml


def main():
    blobs = []
    for arg in sys.argv[1:]:
        project, module_yml = arg.split("=", 1)
        with open(module_yml) as f:
            module = yaml.safe_load(f)
        for blob in module.get("blobs", []):
            blobs.append({
                "path": f"{project}/zephyr/blobs/{blob['path']}",
                "url": blob["url"],
                "sha256": blob["sha256"],
            })
    json.dump(blobs, sys.stdout, indent=1)


if __name__ == "__main__":
    main()
