# SPDX-License-Identifier: GPL-3.0-or-later
"""sip stand-in for GRC Qt GUI exports: wrapping a sink's widget gives the sink back."""


def wrapinstance(obj, cls=None):
    return obj


def unwrapinstance(obj):
    return obj


def isdeleted(obj):
    return False
