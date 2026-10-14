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


# ------------------------------------------------------------------ #
# A <= 64-bit variable against a wide constant (SV elaboration)       #
# ------------------------------------------------------------------ #
#
# The context of `x > K` is K's width, so a 65-bit K made it a 65-bit
# comparison, which only the bit-blaster takes. Elaboration now decides it
# by the variable's range, or rewrites it at the variable's own type, where
# the CDCL engine compiles it.

from dv_solve.ctx import (  # noqa: E402
    CompileIncompleteError, CompileUnsatError, SolveCtx, SOLVE_OK, SOLVE_UNSAT)
from dv_solve.problem import BIN_GT, BIN_GTE, BIN_LTE, BIN_NEQ  # noqa: E402


def _cdcl(var_w, var_s, op, k, kw, ks, var_left=True, seeds=range(1, 9),
          tmp_path=None, monkeypatch=None):
    """Solve `x op K` (or `K op x`) on the CDCL engine. Returns the list of
    x values (as Python ints of x's type), None when UNSAT, or "uncompiled";
    and the uncompiled-constraint log."""
    log = tmp_path / "dv.log"
    monkeypatch.setenv("DV_LOG", str(log))
    b = SolveProblemBuilder()
    try:
        lo, hi = ((-(1 << (var_w - 1)), (1 << (var_w - 1)) - 1) if var_s
                  else (0, min((1 << var_w) - 1, _FULL)))
        b.add_var(0, var_w, var_s, lo, hi if var_w < 64 or var_s else -1)
        x, kc = b.expr_var(0), b.expr_const_wide(k, kw, is_signed=ks)
        b.add_constraint(b.expr_binary(op, x, kc) if var_left
                         else b.expr_binary(op, kc, x))
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    text = lambda: log.read_text() if log.exists() else ""  # noqa: E731
    try:
        ctx = SolveCtx(buf)
    except CompileUnsatError:
        return None, text()
    except CompileIncompleteError:
        return "uncompiled", text()
    try:
        out = []
        for seed in seeds:
            ctx.reset()
            rc = ctx.solve(seed=seed, fair_pick=True)
            if rc == SOLVE_UNSAT:
                return None, text()
            assert rc == SOLVE_OK
            v = ctx.get_value(0) & ((1 << var_w) - 1)
            if var_s and v >> (var_w - 1):
                v -= 1 << var_w
            out.append(v)
        return out, text()
    finally:
        ctx.destroy()


def test_u64_upper_half_bound_compiles(tmp_path, monkeypatch):
    """`x > 2**63` on an unsigned 64-bit x, with 2**63 as pyvsc types it
    (65-bit signed): compiled, every draw in the upper half."""
    xs, log = _cdcl(64, False, BIN_GT, 1 << 63, 65, True,
                    tmp_path=tmp_path, monkeypatch=monkeypatch)
    assert log == ""
    assert all(x > (1 << 63) for x in xs) and len(set(xs)) > 1


def test_constant_on_the_left(tmp_path, monkeypatch):
    xs, log = _cdcl(64, False, BIN_LTE, (1 << 64) - 10, 66, True, var_left=False,
                    tmp_path=tmp_path, monkeypatch=monkeypatch)
    assert log == ""
    assert all(x >= (1 << 64) - 10 for x in xs)


def test_signed_variable_against_a_signed_wide_constant(tmp_path, monkeypatch):
    """A signed 16-bit s > a 70-bit signed -3: a signed comparison of -3."""
    xs, log = _cdcl(16, True, BIN_GT, -3, 70, True,
                    tmp_path=tmp_path, monkeypatch=monkeypatch)
    assert log == ""
    assert all(x > -3 for x in xs) and len(set(xs)) > 1


@pytest.mark.parametrize("var_s,op,k,kw,ks,expect", [
    # 2**63 as 65-bit signed is above every signed 64-bit value.
    (True, BIN_GT, 1 << 63, 65, True, None),
    (True, BIN_LTE, 1 << 63, 65, True, "all"),
    # 2**64 is above every unsigned 64-bit value.
    (False, BIN_EQ, 1 << 64, 66, False, None),
    (False, BIN_NEQ, 1 << 64, 66, False, "all"),
    # -2**64 is below every signed 64-bit value.
    (True, BIN_GTE, -(1 << 64), 70, True, "all"),
    (True, BIN_LT, -(1 << 64), 70, True, None),
    # An unsigned x against a negative signed K: the context is unsigned, so
    # K reads as 2**70 - 1, above every x.
    (False, BIN_LT, -1, 70, True, "all"),
    (False, BIN_GT, -1, 70, True, None),
])
def test_decided_by_the_range(var_s, op, k, kw, ks, expect, tmp_path, monkeypatch):
    xs, log = _cdcl(64, var_s, op, k, kw, ks,
                    tmp_path=tmp_path, monkeypatch=monkeypatch)
    assert log == ""
    if expect is None:
        assert xs is None
    else:
        assert xs is not None and len(set(xs)) > 1


def test_signed_variable_in_an_unsigned_context_is_left_to_the_bit_blaster(
        tmp_path, monkeypatch):
    """A signed s against an unsigned 70-bit K: s is sign-extended and read
    unsigned, so its value in the comparison is not its own. Not rewritten."""
    xs, log = _cdcl(16, True, BIN_LT, 5, 70, False, seeds=[1],
                    tmp_path=tmp_path, monkeypatch=monkeypatch)
    assert xs == "uncompiled" and "cdcl-uncompiled" in log


def test_a_refused_constant_is_an_error_not_a_crash():
    """A wide constant over the width cap is EXPR_NULL; a comparison built on
    it must fail to elaborate, not read the null operand."""
    b = SolveProblemBuilder()
    try:
        b.add_var(0, 100, False, 0, _FULL)
        k = b.expr_const_wide(1, 65536)
        assert k == EXPR_NULL
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), k))
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    with pytest.raises(Exception):
        BVSatCtx(buf)
