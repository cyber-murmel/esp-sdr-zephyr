#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# docs.yml "Build": Doxygen (warnings are errors in the Doxyfile), then Sphinx with -W.
# -E rereads every page, so warnings of unchanged pages count too.
set -eu
cd "$(dirname "$0")/../../doc"
doxygen >/dev/null
sphinx-build -W --keep-going -E -q -b html . _build_sphinx/html
