# SPDX-License-Identifier: GPL-3.0-or-later
"""pmt stand-in for GRC exports: the constants and constructors exports use to set up blocks
(file_source.set_begin_tag(pmt.PMT_NIL) and the like). Values are plain Python objects; the
board's blocks carry no tags or messages."""


class _Pmt:
    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return "pmt({!r})".format(self.value)

    def __eq__(self, other):
        return isinstance(other, _Pmt) and other.value == self.value


PMT_NIL = _Pmt(None)
PMT_T = _Pmt(True)
PMT_F = _Pmt(False)
PMT_EOF = _Pmt("EOF")


def intern(s):
    return _Pmt(str(s))


string_to_symbol = intern


def to_pmt(x):
    return x if isinstance(x, _Pmt) else _Pmt(x)


def to_python(p):
    return p.value if isinstance(p, _Pmt) else p


def from_long(x):
    return _Pmt(int(x))


def from_double(x):
    return _Pmt(float(x))


def from_bool(x):
    return PMT_T if x else PMT_F


def is_null(p):
    return p is PMT_NIL or (isinstance(p, _Pmt) and p.value is None)


def cons(a, b):
    return _Pmt((a, b))


def make_dict():
    return _Pmt({})
