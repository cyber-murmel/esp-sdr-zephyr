#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# docs.yml "Register pages": doc/registers/*.svd merge into Espressif's SVDs. Those are
# downloaded once into the workspace's .cache/; the merged SVDs go to a temporary directory.
set -eu
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 doc/registers/svdmerge.py --out "$out" --official ../.cache/esp-sdr-zephyr/svd
