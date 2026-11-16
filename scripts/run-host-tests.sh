#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the host tests of the Python tools (tests/host/, pytest) under coverage and prints the
# report; further arguments go to pytest. Needs pytest, coverage and PyYAML (the nix shell
# has them). Runs from the repository root, which gets .coverage (and host-coverage/ with
# "coverage html --rcfile tests/host/coveragerc").
set -eu
cd "$(dirname "$0")/.."
python3 -m coverage run --rcfile tests/host/coveragerc -m pytest -q tests/host "$@"
python3 -m coverage report --rcfile tests/host/coveragerc
