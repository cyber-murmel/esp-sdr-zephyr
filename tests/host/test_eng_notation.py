# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/micropython/modules/gnuradio/eng_notation.py (plain Python, run here under CPython):
the engineering notation GRC scripts use for their arguments."""

import pytest

from conftest import load

eng = load("apps/micropython/modules/gnuradio/eng_notation.py", "gnuradio_eng_notation")


@pytest.mark.parametrize("value,text", [
    (2.4e9, "2.4G"), (2.4015e9, "2.4015G"), (125e3, "125k"), (1.0, "1"), (0, "0"),
    (-20e6, "-20M"), (1.5e-3, "1.5m"), (3e-6, "3u"), (4e-12, "4p"), (7e12, "7T"),
])
def test_num_to_str(value, text):
    assert eng.num_to_str(value) == text


def test_precision():
    assert eng.num_to_str(2.41234567e9, 3) == "2.41G"


@pytest.mark.parametrize("text,value", [
    ("2.4G", 2.4e9), ("125k", 125e3), (" 20M ", 20e6), ("1e6", 1e6), ("3u", 3e-6), ("42", 42.0),
])
def test_str_to_num(text, value):
    assert eng.str_to_num(text) == pytest.approx(value)


def test_round_trip():
    for v in (2.437e9, 250e3, 1e-3, 12.5):
        assert eng.str_to_num(eng.num_to_str(v)) == pytest.approx(v)


def test_str_to_num_rejects():
    with pytest.raises(ValueError):
        eng.str_to_num("lots")
