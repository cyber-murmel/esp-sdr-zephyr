#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Markdown table of the unit tests' code coverage, for a CI job summary.

Reads the summary gcovr writes for `west twister --coverage` (twister-out/
coverage_summary.json, paths relative to --coverage-basedir) and prints the total and one row
per source file. A firmware C source (lib/, apps/*/src, cmod, ext) that no unit test compiles has
no gcov data at all; those are listed apart, so that the total is not read as the share of
the whole firmware. The host programs under apps/*/tools are left out.

    scripts/coverage-summary.py twister-out/coverage_summary.json >> "$GITHUB_STEP_SUMMARY"
"""

import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def pct(covered, total):
    return f"{100.0 * covered / total:.1f} %" if total else "-"


def cell(entry, kind):
    covered, total = entry.get(f"{kind}_covered", 0), entry.get(f"{kind}_total", 0)
    return f"{pct(covered, total)} ({covered}/{total})" if total else "-"


def unbuilt(measured):
    """Firmware C sources that no unit test compiles."""
    globs = ("lib/**/*.c", "apps/*/src/**/*.c", "apps/*/cmod/**/*.c", "apps/*/ext/**/*.c")
    sources = sorted(p.relative_to(REPO).as_posix() for g in globs for p in REPO.glob(g))
    return [s for s in sources if s not in measured]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("summary", type=Path, help="gcovr's JSON summary")
    ap.add_argument("--title", default="Unit test coverage")
    args = ap.parse_args()

    print(f"## {args.title}\n")
    try:
        data = json.loads(args.summary.read_text())
    except (OSError, ValueError) as e:
        print(f"No coverage summary ({e}).")
        return 0
    files = sorted(data.get("files", []), key=lambda f: f["filename"])
    print(f"Lines **{pct(data.get('line_covered', 0), data.get('line_total', 0))}**, "
          f"functions {pct(data.get('function_covered', 0), data.get('function_total', 0))}, "
          f"branches {pct(data.get('branch_covered', 0), data.get('branch_total', 0))} "
          f"over the {len(files)} files the native_sim unit tests (tests/unit) compile.\n")
    print("| File | Lines | Functions | Branches |")
    print("|---|---:|---:|---:|")
    for f in files:
        print(f"| `{f['filename']}` | {cell(f, 'line')} | {cell(f, 'function')} | "
              f"{cell(f, 'branch')} |")
    rest = unbuilt({f["filename"] for f in files})
    if rest:
        print(f"\n<details><summary>{len(rest)} firmware C sources that no unit "
              "test compiles</summary>\n")
        for s in rest:
            print(f"- `{s}`")
        print("\n</details>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
