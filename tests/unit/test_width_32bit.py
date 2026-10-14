"""Widths are 32-bit in the IR and the API (pyvsc v2 plan V26).

Variable, constant, extend, extract and concat widths were uint16, so 65535
bits was the ceiling, and a front end had to refuse anything wider. They are
uint32 now. The engine's own ceiling is DVS_MAX_WIDTH (dvs_max_width()),
2^24 bits, which keeps the bit-blaster's node and buffer arithmetic inside 32
bits; a variable or constant past it is refused when it is built.
"""
from __future__ import annotations

import ctypes
import random

import pytest

from dv_solve import lib
from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_EQ, BIN_GT, BIN_LT, EXPR_NULL
from dv_solve.bvsat import BVSatCtx, BVSAT_SAT

_FULL = (1 << 63) - 1


def _solve(build, read, seed=1):
    b = SolveProblemBuilder()
    try:
        build(b)
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    ctx = BVSatCtx(buf)
    try:
        if ctx.check(seed=seed) != BVSAT_SAT:
            return None
        return {v: ctx.value_wide(v, w, s) for v, w, s in read}
    finally:
        ctx.destroy()


def test_max_width():
    assert lib.max_width() == 1 << 24


@pytest.mark.parametrize("width", [65536, 70000, 200003])
def test_var_wider_than_16_bits_of_width(width):
    """`x > 2^(W-1)`: the top bit is set, and the draw is not just 2^(W-1)+1."""
    k = 1 << (width - 1)

    def build(b):
        b.add_var(0, width, False, 0, _FULL)
        b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(0),
                                       b.expr_const_wide(k, width)))

    got = _solve(build, [(0, width, False)])[0]
    assert k < got < (1 << width)
    assert got.bit_length() == width


def test_constant_wider_than_16_bits_of_width_reads_back():
    w = 70000
    k = random.Random(w).getrandbits(w) | (1 << (w - 1))

    def build(b):
        b.add_var(0, w, False, 0, _FULL)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                                       b.expr_const_wide(k, w)))

    assert _solve(build, [(0, w, False)]) == {0: k}


def test_extract_and_concat_past_65535():
    """Bit positions and the concat's lo width past 65535: {hi, lo} with a
    70000-bit lo, then the top 8 bits extracted."""
    lo_w = 70000

    def build(b):
        b.add_var(0, 8, False, 0, 255)
        b.add_var(1, lo_w, False, 0, _FULL)
        b.add_var(2, 8, False, 0, 255)
        cat = b.expr_concat(b.expr_var(0), b.expr_var(1), lo_w)
        top = b.expr_extract(cat, lo_w + 7, lo_w)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(2), top))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_const(0xA5)))

    got = _solve(build, [(2, 8, False)])
    assert got == {2: 0xA5}


def test_extend_past_65535():
    """A 16-bit value zero-extended to 70000 bits compares as its value."""
    def build(b):
        b.add_var(0, 16, False, 0, 0xFFFF)
        b.add_var(1, 70000, False, 0, _FULL)
        b.add_constraint(b.expr_binary(
            BIN_EQ, b.expr_var(1),
            b.expr_extend(b.expr_var(0), 16, 70000, False)))
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_const(3)))

    got = _solve(build, [(0, 16, False), (1, 70000, False)])
    assert got[0] < 3 and got[1] == got[0]


def test_widths_past_the_ceiling_are_refused():
    m = lib.max_width()
    b = SolveProblemBuilder()
    try:
        assert b.add_var(0, m, False, 0, _FULL) != EXPR_NULL
        for w in (0, m + 1):
            with pytest.raises(ValueError, match="width"):
                b.add_var(1, w, False, 0, 1)
        f = b._lib.dvs_builder_expr_const_wide
        n = (m + 1 + 63) // 64
        limbs = (ctypes.c_uint64 * n)(1)
        assert f(b._b, n, limbs, m + 1, 0) == EXPR_NULL
        n = m // 64
        limbs = (ctypes.c_uint64 * n)(1)
        assert f(b._b, n, limbs, m, 0) != EXPR_NULL
    finally:
        b.destroy()


def test_extend_past_the_ceiling_fails_the_solve():
    m = lib.max_width()
    b = SolveProblemBuilder()
    try:
        b.add_var(0, 8, False, 0, 255)
        e = b.expr_extend(b.expr_var(0), 8, m + 8, False)
        b.add_constraint(b.expr_binary(BIN_EQ, e, b.expr_const(5)))
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    with pytest.raises(RuntimeError):
        BVSatCtx(buf)
