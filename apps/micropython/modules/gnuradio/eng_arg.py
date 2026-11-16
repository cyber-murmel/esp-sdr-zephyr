# SPDX-License-Identifier: GPL-3.0-or-later
"""argparse types of GRC scripts."""

from gnuradio.eng_notation import str_to_num


def eng_float(s):
    return str_to_num(s)


def intx(s):
    return int(s, 0)
