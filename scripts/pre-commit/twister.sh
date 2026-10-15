#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# build.yml "Twister": west twister with the given arguments (paths relative to this repo),
# into twister-out-pre-commit/. NSI_OPT as in run-unit-tests.sh. Passed images are deleted,
# /home is short of space.
set -eu
cd "$(dirname "$0")/../.."
NSI_OPT=${NSI_OPT:--O2} exec west twister "$@" -O twister-out-pre-commit \
  --runtime-artifact-cleanup pass --inline-logs
