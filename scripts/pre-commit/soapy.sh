#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# build.yml "SoapySDR module", in build-soapy/ as there (kept, so rebuilds are incremental).
set -eu
cd "$(dirname "$0")/../.."
cmake -S apps/osmosdr/soapy -B build-soapy -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-soapy
