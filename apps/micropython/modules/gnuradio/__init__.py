# SPDX-License-Identifier: GPL-3.0-or-later
"""GNU Radio on the board: the subset that apps/micropython runs (README.rst)."""

import esdr

# The burst buffer of packet flowgraphs (gnuradio/_link.py), taken at the first import, before
# the flowgraph's objects spread over the heap (a small board's, the ESP32-C5's, soon has no
# block this large free): a 64-byte packet at 10 samples a symbol. A larger burst takes a new
# one, which then stays.
TX_RESERVE = 21 * 1024
_txbuf = None
if hasattr(esdr, "fsk_mod"):
    try:
        _txbuf = bytearray(TX_RESERVE)
    except MemoryError:
        pass
# Likewise room for the receiver's state (esdr.fsk_rx_init(), 2880 bytes on the ESP32-C5),
# which the flowgraph's start takes after its objects: given back just before
# (digital/__init__.py), it leaves a hole that the state fits.
RX_RESERVE = 4 * 1024
_rxroom = None
if hasattr(esdr, "fsk_rx_init"):
    try:
        _rxroom = bytearray(RX_RESERVE)
    except MemoryError:
        pass
