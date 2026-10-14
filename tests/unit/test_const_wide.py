"""A constant wider than 64 bits is one node (dvs_builder_expr_const_wide).

A front end could only spell a wide constant as a chain of 64-bit limbs
joined by `concat`: 2n - 1 nodes for n limbs. An unsized top limb types
itself by its value (32 or 64 bits), not by the bits it holds, so pyvsc's
chain for a 65535-bit constant came to 65536 bits, over the width cap, and
the bit-blaster refused the problem. EXPR_CONST_WIDE carries its limbs
inline at an exact width: SV elaboration types it at its own width and
signedness, the bit-blaster blasts it directly, the CDCL compile declines
it as it does any > 64-bit operand, and the validator evaluates it up to
128 bits.
"""
from __future__ import annotations

import random

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_ADD, BIN_EQ, BIN_LT, EXPR_NULL
from dv_solve.bvsat import BVSatCtx, BVSAT_SAT

_FULL = (1 << 63) - 1   # "full range" for a var of 64 bits or more


def _solve(build, read, seed=1):
    """Build with `build(b)`, BV-SAT check, and return {var: value} for the
    (var, width, signed) triples in `read`, or None when not SAT."""
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


@pytest.mark.parametrize("width", [65, 128, 200, 1000, 65535])
def test_eq_reads_back(width):
    k = random.Random(width).getrandbits(width) | (1 << (width - 1))

    def build(b):
        b.add_var(0, width, False, 0, _FULL)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                                       b.expr_const_wide(k, width)))

    assert _solve(build, [(0, width, False)]) == {0: k}


def test_lt_at_the_widest_constant():
    """`a < b; b < 2^(W-1)` at W = 65535: pyvsc's concat chain for 2^65534
    was 1024 limbs with a 64-bit top limb, 65536 bits, and was refused."""
    w = 65535
    k = 1 << (w - 1)

    def build(b):
        b.add_var(0, w, False, 0, _FULL)
        b.add_var(1, w, False, 0, _FULL)
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_var(1)))
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(1),
                                       b.expr_const_wide(k, w)))

    got = _solve(build, [(0, w, False), (1, w, False)])
    assert got is not None
    assert got[0] < got[1] < k


def test_signed_constant_sign_extends():
    """A 70-bit signed -5 in a 128-bit signed context is -5, not 2^70 - 5."""
    def build(b):
        b.add_var(0, 128, True, -_FULL - 1, _FULL)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                                       b.expr_const_wide(-5, 70, is_signed=True)))

    assert _solve(build, [(0, 128, True)]) == {0: -5}


def test_unsigned_constant_zero_extends():
    """The same 70-bit pattern unsigned is 2^70 - 5 in a 128-bit context."""
    def build(b):
        b.add_var(0, 128, False, 0, _FULL)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                                       b.expr_const_wide(-5, 70)))

    assert _solve(build, [(0, 128, False)]) == {0: (1 << 70) - 5}


def test_narrow_operand_takes_the_constant_width():
    """x == a + 2^99, with an 8-bit a: the sum is 100 bits wide."""
    k = 1 << 99

    def build(b):
        b.add_var(0, 100, False, 0, _FULL)
        b.add_var(1, 8, False, 0, 255)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1),
                                       b.expr_const(200)))
        b.add_constraint(b.expr_binary(
            BIN_EQ, b.expr_var(0),
            b.expr_binary(BIN_ADD, b.expr_var(1), b.expr_const_wide(k, 100))))

    assert _solve(build, [(0, 100, False)]) == {0: k + 200}


def test_up_to_64_bits_is_a_plain_constant():
    def build(b):
        b.add_var(0, 40, False, 0, (1 << 40) - 1)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                                       b.expr_const_wide(0x12_3456_789A, 40)))

    assert _solve(build, [(0, 40, False)]) == {0: 0x12_3456_789A}


def test_bad_arguments_are_refused():
    import ctypes
    b = SolveProblemBuilder()
    try:
        limbs = (ctypes.c_uint64 * 2)(1, 2)
        f = b._lib.dvs_builder_expr_const_wide
        assert f(b._b, 2, limbs, 0, 0) == EXPR_NULL         # no width
        assert f(b._b, 1, limbs, 100, 0) == EXPR_NULL       # too few limbs
        assert f(b._b, 2, limbs, 65536, 0) == EXPR_NULL     # over the cap
        assert f(b._b, 2, limbs, 100, 0) != EXPR_NULL
    finally:
        b.destroy()
