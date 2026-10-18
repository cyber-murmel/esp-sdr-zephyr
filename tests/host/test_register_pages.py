# SPDX-License-Identifier: GPL-3.0-or-later
"""doc/registers/svd2md.py: the register pages match the SVDs (as docs.yml checks)."""

import pytest

from conftest import load

svd2md = load("doc/registers/svd2md.py")


def test_pages_up_to_date(run_main):
    assert run_main(svd2md, "--check") == 0


def test_render_has_regions():
    for svd in sorted((svd2md.Path(svd2md.__file__).parent).glob("*.svd")):
        parts = svd2md.render(svd)
        assert parts and all(isinstance(v, list) and v for v in parts.values()), svd.name


def test_missing_region(tmp_path):
    page = tmp_path / "page.md"
    page.write_text("# nothing here\n")
    with pytest.raises(SystemExit):
        svd2md.update(page, {"summary": ["x"]})
