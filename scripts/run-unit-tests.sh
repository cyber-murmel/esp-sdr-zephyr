#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs every unit test (tests/unit/): native_sim, no boards needed, a minute
# or two. See doc/dev/testing.md for the other test tiers.
#
# NSI_OPT=-O2 works around a native_sim/glibc interaction: without an
# optimization level, glibc's _FORTIFY_SOURCE check becomes a warning Zephyr
# treats as an error, in native_sim's own runtime support code (not ours).
# UBSAN=1 builds every suite with the undefined-behaviour sanitizer, fatal on
# the first finding (no recover), into twister-out-ubsan so it keeps the normal
# run's output. Run both passes; see doc/dev/testing.md.
# Runs from the repository root: give relative paths (-O) from there.
set -eu
cd "$(dirname "$0")/.."
if [ "${UBSAN:-0}" = 1 ]; then
	set -- -x=CONFIG_UBSAN=y -x=EXTRA_CFLAGS=-fno-sanitize-recover=undefined \
		--outdir twister-out-ubsan "$@"
fi
NSI_OPT=${NSI_OPT:--O2} exec west twister -p native_sim/native/64 -T tests/unit "$@"
