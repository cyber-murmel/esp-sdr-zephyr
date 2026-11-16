# SPDX-License-Identifier: GPL-3.0-or-later
"""pmt stand-in for GRC exports and their blocks: the constants and constructors exports use to
set up blocks (file_source.set_begin_tag(pmt.PMT_NIL) and the like), and what message passing
needs: symbols, pairs, dictionaries, u8vectors (PDUs are pairs of a metadata dictionary and a
u8vector), numbers. A pmt is a _Pmt holding a plain Python value: str for a symbol, a tuple for
a pair, a dict (keyed by the symbols' strings) for a dictionary, bytes for a u8vector."""


class _Pmt:
    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return "pmt({!r})".format(self.value)

    def __eq__(self, other):
        return isinstance(other, _Pmt) and other.value == self.value

    def __hash__(self):
        return hash(self.value) if not isinstance(self.value, (dict, bytearray)) else id(self)


PMT_NIL = _Pmt(None)
PMT_T = _Pmt(True)
PMT_F = _Pmt(False)
PMT_EOF = _Pmt("EOF")


def intern(s):
    return _Pmt(str(s))


string_to_symbol = intern


def symbol_to_string(p):
    return p.value


def is_symbol(p):
    return isinstance(p, _Pmt) and isinstance(p.value, str)


def to_pmt(x):
    if isinstance(x, _Pmt):
        return x
    if isinstance(x, (bytes, bytearray)):
        return _Pmt(bytes(x))
    if isinstance(x, tuple) and len(x) == 2:
        return cons(to_pmt(x[0]), to_pmt(x[1]))
    if isinstance(x, dict):
        return _Pmt({str(k): to_pmt(v) for k, v in x.items()})
    return _Pmt(x)


from_python = to_pmt


def to_python(p):
    if not isinstance(p, _Pmt):
        return p
    v = p.value
    if isinstance(v, tuple):
        return tuple(to_python(x) for x in v)
    if isinstance(v, dict):
        return {k: to_python(x) for k, x in v.items()}
    return v


def from_long(x):
    return _Pmt(int(x))


def from_uint64(x):
    return _Pmt(int(x))


def from_double(x):
    return _Pmt(float(x))


def from_float(x):
    return _Pmt(float(x))


def from_bool(x):
    return PMT_T if x else PMT_F


def to_long(p):
    return int(p.value)


to_uint64 = to_long


def to_double(p):
    return float(p.value)


to_float = to_double


def to_bool(p):
    return bool(p.value)


def is_null(p):
    return p is PMT_NIL or (isinstance(p, _Pmt) and p.value is None)


def is_number(p):
    return isinstance(p, _Pmt) and isinstance(p.value, (int, float)) and not isinstance(p.value, bool)


def is_integer(p):
    return isinstance(p, _Pmt) and isinstance(p.value, int) and not isinstance(p.value, bool)


def is_real(p):
    return isinstance(p, _Pmt) and isinstance(p.value, float)


def is_bool(p):
    return isinstance(p, _Pmt) and isinstance(p.value, bool)


def eq(a, b):
    return a == b


equal = eqv = eq


# ---- pairs ----


def cons(a, b):
    return _Pmt((a, b))


def is_pair(p):
    return isinstance(p, _Pmt) and isinstance(p.value, tuple) and len(p.value) == 2


def car(p):
    return p.value[0]


def cdr(p):
    return p.value[1]


# ---- dictionaries ----


def make_dict():
    return _Pmt({})


def is_dict(p):
    return isinstance(p, _Pmt) and isinstance(p.value, dict)


def dict_add(d, key, value):
    """A new dictionary with key set (pmt dictionaries are values)."""
    out = dict(d.value) if is_dict(d) else {}
    out[key.value] = value
    return _Pmt(out)


def dict_has_key(d, key):
    return is_dict(d) and key.value in d.value


def dict_ref(d, key, not_found):
    if is_dict(d):
        return d.value.get(key.value, not_found)
    return not_found


def dict_delete(d, key):
    out = dict(d.value) if is_dict(d) else {}
    out.pop(key.value, None)
    return _Pmt(out)


def dict_keys(d):
    return _Pmt(tuple(intern(k) for k in d.value)) if is_dict(d) else PMT_NIL


# ---- u8vectors ----


def init_u8vector(n, data):
    return _Pmt(bytes(data[:n]) if len(data) >= n else bytes(data) + bytes(n - len(data)))


def make_u8vector(n, fill):
    return _Pmt(bytes([fill & 255]) * n)


def is_u8vector(p):
    return isinstance(p, _Pmt) and isinstance(p.value, bytes)


is_uniform_vector = is_u8vector


def u8vector_elements(p):
    return p.value


def length(p):
    v = p.value
    return len(v) if isinstance(v, (bytes, tuple, dict, str)) else 0


def u8vector_ref(p, k):
    return p.value[k]
