# SPDX-License-Identifier: GPL-3.0-or-later
"""scripts/coverage-summary.py: gcovr's summary as the CI job's Markdown table."""

import json

from conftest import load

summary = load("scripts/coverage-summary.py")

DATA = {
    "line_covered": 75, "line_total": 100, "function_covered": 9, "function_total": 10,
    "branch_covered": 0, "branch_total": 0,
    "files": [
        {"filename": "lib/ieee802154/ieee154_phy.c", "line_covered": 70, "line_total": 80,
         "function_covered": 8, "function_total": 8, "branch_covered": 0, "branch_total": 0},
        {"filename": "apps/link/src/rs.c", "line_covered": 5, "line_total": 20,
         "function_covered": 1, "function_total": 2, "branch_covered": 3, "branch_total": 4},
    ],
}


def test_table(tmp_path, run_main, capsys):
    path = tmp_path / "coverage_summary.json"
    path.write_text(json.dumps(DATA))
    assert run_main(summary, path) == 0
    out = capsys.readouterr().out
    assert out.startswith("## Unit test coverage\n")
    assert "Lines **75.0 %**, functions 90.0 %, branches -" in out
    rows = [l for l in out.splitlines() if l.startswith("| `")]
    # Sorted by file name.
    assert rows == [
        "| `apps/link/src/rs.c` | 25.0 % (5/20) | 50.0 % (1/2) | 75.0 % (3/4) |",
        "| `lib/ieee802154/ieee154_phy.c` | 87.5 % (70/80) | 100.0 % (8/8) | - |",
    ]
    # Sources no unit test compiles are listed apart, the measured ones are not.
    assert "firmware C sources that no unit test compiles" in out
    assert "- `apps/micropython/cmod/modesdr.c`" in out
    assert "tools/" not in out.split("<details>")[1]
    assert "- `lib/esp_sdr/src/esp_sdr_rx.c`" in out
    assert "- `apps/link/src/rs.c`" not in out


def test_missing_summary(tmp_path, run_main, capsys):
    assert run_main(summary, tmp_path / "nope.json", "--title", "Coverage") == 0
    out = capsys.readouterr().out
    assert out.startswith("## Coverage\n") and "No coverage summary" in out
