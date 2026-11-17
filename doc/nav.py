#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the Previous, Up and Next links of every page.

The reading order is the toctrees of doc/index.rst, from the overview (README.rst) through the five
parts. Each page gets one line of links under its title and the same line at its end, between
markers (<!-- nav-start --> / <!-- nav-end --> in Markdown, .. nav-start / .. nav-end in RST):

- Previous and Next: the neighbours in the reading order, across parts.
- Up: the part's index page; for an index page and the overview, the landing page doc/index.rst.

Links are relative paths, so they work on GitHub; doc/conf.py turns them into page links. The link
text is the target's title. App pages are their READMEs (apps/<app>/README.rst), the overview is
README.rst.

Run: python3 doc/nav.py rewrites the links. --check: exit 1 if a page's links are out of date.
"""

import os
import re
import sys
from pathlib import Path

DOC = Path(__file__).resolve().parent
REPO = DOC.parent
INDEX = DOC / "index.rst"
ADORN = re.compile(r"^([#*=\-~^\"'`])\1{2,}\s*$")
ENTRY = re.compile(r"^   ([\w/.-]+)\s*$")
MD_BLOCK = re.compile(r"\n*<!-- nav-start -->.*?<!-- nav-end -->\n*", re.S)
RST_BLOCK = re.compile(r"\n*\.\. nav-start\n.*?\.\. nav-end\n*", re.S)


def order():
    """The docnames of the toctrees, in order."""
    names, inside = [], False
    for line in INDEX.read_text().splitlines():
        if line.startswith(".. toctree::"):
            inside = True
        elif inside and line and not line.startswith(" "):
            inside = False
        elif inside and (m := ENTRY.match(line)):
            names.append(m.group(1))
    return names


def source(docname):
    """The repository file a docname shows."""
    if docname == "overview":
        return REPO / "README.rst"
    if docname.startswith("apps/"):
        readme = REPO / "apps" / docname[len("apps/"):] / "README.rst"
        if readme.is_file():
            return readme
    for suffix in (".md", ".rst"):
        if (DOC / (docname + suffix)).is_file():
            return DOC / (docname + suffix)
    sys.exit(f"doc/nav.py: no source for {docname}")


def title_line(lines, path):
    """The index of the line that ends the page title."""
    if path.suffix == ".md":
        return next(i for i, line in enumerate(lines) if line.startswith("# "))
    for i in range(len(lines) - 1):
        text, under = lines[i], lines[i + 1]
        if text.strip() and not ADORN.match(text) and not text.startswith("..") \
                and ADORN.match(under):
            return i + 1
    sys.exit(f"doc/nav.py: no title in {path}")


def title(path):
    lines = path.read_text().splitlines()
    i = title_line(lines, path)
    return lines[i][2:].strip() if path.suffix == ".md" else lines[i - 1].strip()


def link(page, target):
    rel = os.path.relpath(target, page.parent)
    if page.suffix == ".md":
        return f"[{title(target)}]({rel})"
    return f"`{title(target)} <{rel}>`__"


def up_of(docname, names):
    part = docname.split("/")[0]
    if docname == "overview" or docname.endswith("/index") or "/" not in docname:
        return INDEX if docname != "overview" else None
    index = f"{part}/index"
    return source(index) if index in names else INDEX


def nav_line(page, prev, up, nxt):
    parts = []
    if prev:
        parts.append("Previous: " + link(page, prev))
    if up:
        parts.append("Up: " + link(page, up))
    if nxt:
        parts.append("Next: " + link(page, nxt))
    return " · ".join(parts)


def rewrite(page, line):
    text = page.read_text()
    md = page.suffix == ".md"
    text = (MD_BLOCK if md else RST_BLOCK).sub("\n\n", text).rstrip("\n") + "\n"
    block = (f"<!-- nav-start -->\n{line}\n<!-- nav-end -->" if md else
             f".. nav-start\n\n{line}\n\n.. nav-end")
    lines = text.split("\n")
    i = title_line(lines, page)
    head, rest = "\n".join(lines[:i + 1]), "\n".join(lines[i + 1:]).lstrip("\n")
    return f"{head}\n\n{block}\n\n{rest.rstrip()}\n\n{block}\n"


def main():
    check = "--check" in sys.argv[1:]
    names = order()
    pages = [source(n) for n in names]
    stale = []
    for k, (name, page) in enumerate(zip(names, pages)):
        prev = pages[k - 1] if k > 0 else None
        nxt = pages[k + 1] if k + 1 < len(pages) else None
        new = rewrite(page, nav_line(page, prev, up_of(name, names), nxt))
        if new != page.read_text():
            stale.append(str(page.relative_to(REPO)))
            if not check:
                page.write_text(new)
    if check and stale:
        sys.exit("out of date, run doc/nav.py: " + ", ".join(stale))
    return 0


if __name__ == "__main__":
    sys.exit(main())
