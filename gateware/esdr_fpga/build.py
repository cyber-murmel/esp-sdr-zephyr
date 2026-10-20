# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a bitstream: python -m esdr_fpga.build <variant> [--out DIR]."""

import argparse
import json
import os
import pathlib
import re
import sys

from .platform import MCH2022Platform

VARIANTS = {}


def variant(fn):
    VARIANTS[fn.__name__] = fn
    return fn


@variant
def blinky():
    from .blinky import Blinky
    return Blinky()


@variant
def top():
    from .top import Top
    return Top()


@variant
def misotest():
    from .top import Top
    return Top(miso_test=True)


def report(build_dir, name):
    """Utilisation and fmax from nextpnr's JSON report."""
    with open(build_dir / f"{name}.tim.json") as f:
        rep = json.load(f)
    util = {k: (v["used"], v["available"]) for k, v in rep["utilization"].items() if v["used"]}
    fmax = {k: (v["achieved"], v["constraint"]) for k, v in rep.get("fmax", {}).items()}
    return util, fmax


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("variant", choices=sorted(VARIANTS))
    ap.add_argument("--out", type=pathlib.Path,
                    default=pathlib.Path(os.environ.get("ESDR_FPGA_OUT", "build-gateware")))
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args(argv)

    out = args.out / args.variant
    platform = MCH2022Platform()
    platform.build(
        VARIANTS[args.variant](),
        name="top",
        build_dir=out,
        synth_opts="-dsp -abc9",
        nextpnr_opts=f"--seed {args.seed} --report {out.resolve()}/top.tim.json",
        do_program=False,
    )
    util, fmax = report(out, "top")
    for k, (used, avail) in util.items():
        print(f"{k:16} {used:5} / {avail}")
    ok = True
    for k, (got, want) in fmax.items():
        print(f"fmax {k:30} {got:7.2f} MHz (want {want:.2f})")
        ok &= got >= want
    print(f"bitstream {out / 'top.bin'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
