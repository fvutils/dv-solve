"""Unit tests for SystemVerilog system functions exercised by Verilator:
$countones/$countbits, $clog2, and the composed $onehot/$onehot0.

These map to the EXPR_COUNTONES / EXPR_CLOG2 high-level IR nodes, lowered by
zsp_compile.c to the _fire_countones_32 / _fire_clog2_32 bounds propagators.
The bitblast engine does NOT lower these (zsp_bbsolver.c: "SUM/COUNTONES/CLOG2/
ARRAY_SELECT not yet supported"), so every case here runs on the primary CDCL
engine (SolveCtx).

clog2 convention (see _clog2_32 in zsp_prop_templates.c): clog2(v)=0 for v<=1,
otherwise ceil(log2(v)) -- i.e. the SystemVerilog $clog2 definition.

KNOWN BUG (BUG-1, see docs/verilator_coverage_test_plan.md): the countones/clog2
propagators read Variable.lo/hi directly, which is only valid for TIER-0 vars
(width <= 31 unsigned, or <= 32 signed). When the operand OR result var is
tier-1 (32-bit unsigned, or 33-64 bit), the propagator reads garbage bounds and
returns a silently-wrong model or a spurious UNSAT. The xfail tests at the end
pin that behavior; they must flip to pass when the propagator is made
tier-aware. Because $countones on a 32-bit `int` is a common SV idiom, this is a
real soundness gap, not a corner case.

Maps to Verilator regression tests: t_constraint_sysfunc, t_constraint_countones.
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_LTE
from dv_solve.ctx import SolveCtx, SOLVE_OK, SOLVE_UNSAT, CompileUnsatError


# ------------------------------------------------------------------ #
# Helpers                                                             #
# ------------------------------------------------------------------ #

def _solve(build, seed=1):
    """Build a problem via ``build(b)`` and solve on the primary engine.

    ``build`` returns the list of var_ids to read back. Returns
    ``(result, values_dict)``; values populated only on SOLVE_OK.
    """
    b = SolveProblemBuilder()
    var_ids = build(b)
    buf, _sz = b.finalize()
    try:
        try:
            ctx = SolveCtx(buf)
        except CompileUnsatError:
            return SOLVE_UNSAT, {}
        rc = ctx.solve(seed=seed)
        vals = {}
        if rc == SOLVE_OK:
            for vid in var_ids:
                vals[vid] = ctx.get_value(vid)
        ctx.destroy()
        return rc, vals
    finally:
        b.destroy()


def _popcount(v, width):
    return bin(v & ((1 << width) - 1)).count("1")


def _clog2(v):
    """Reference SV $clog2: 0 for v<=1 else ceil(log2(v))."""
    if v <= 1:
        return 0
    return (v - 1).bit_length()


# A tier-0 result var comfortably holds any popcount/clog2 (<= 32).
_RESW = 8


# ------------------------------------------------------------------ #
# countones / countbits  (tier-0 operand: width <= 31)               #
# ------------------------------------------------------------------ #

@pytest.mark.parametrize("width", [8, 16, 31])
@pytest.mark.parametrize("k", [0, 1, 3])
def test_countones_pin_result(width, k):
    """Pin countones(x)==k; the returned x must have exactly k bits set."""
    def build(b):
        b.add_var(0, _RESW, False, k, k)        # result, pinned to k (tier-0)
        b.add_var(1, width, False, 0, (1 << width) - 1)
        b.add_constraint(b.expr_countones(b.expr_var(0), b.expr_var(1)))
        return [0, 1]

    rc, vals = _solve(build)
    assert rc == SOLVE_OK, "countones==%d over %d bits should be SAT" % (k, width)
    assert vals[0] == k
    assert _popcount(vals[1], width) == k, (
        "operand %#x has %d bits set, expected %d"
        % (vals[1], _popcount(vals[1], width), k)
    )


@pytest.mark.parametrize("width", [8, 16, 31])
def test_countones_forward(width):
    """Pin operand to several values; read back the popcount as result."""
    for opnd in (0, 1, 3, (1 << width) - 1):
        def build(b, o=opnd):
            b.add_var(0, _RESW, False, 0, 32)   # result free (tier-0)
            b.add_var(1, width, False, o, o)    # operand pinned
            b.add_constraint(b.expr_countones(b.expr_var(0), b.expr_var(1)))
            return [0]

        rc, vals = _solve(build)
        assert rc == SOLVE_OK
        assert vals[0] == _popcount(opnd, width), (
            "countones(%#x) got %d, expected %d"
            % (opnd, vals[0], _popcount(opnd, width))
        )


def test_countones_all_ones():
    """countones(x)==width forces x to be all-ones."""
    width = 8
    def build(b):
        b.add_var(0, _RESW, False, width, width)
        b.add_var(1, width, False, 0, (1 << width) - 1)
        b.add_constraint(b.expr_countones(b.expr_var(0), b.expr_var(1)))
        return [1]

    rc, vals = _solve(build)
    assert rc == SOLVE_OK
    assert vals[1] == (1 << width) - 1


def test_countones_zero_forces_zero():
    """countones(x)==0 forces x==0."""
    def build(b):
        b.add_var(0, _RESW, False, 0, 0)
        b.add_var(1, 8, False, 0, 255)
        b.add_constraint(b.expr_countones(b.expr_var(0), b.expr_var(1)))
        return [1]

    rc, vals = _solve(build)
    assert rc == SOLVE_OK
    assert vals[1] == 0


def test_countones_unsat_over_width():
    """countones(x) cannot exceed the operand width."""
    def build(b):
        b.add_var(0, _RESW, False, 9, 9)    # 9 bits set...
        b.add_var(1, 8, False, 0, 255)      # ...in an 8-bit operand: impossible
        b.add_constraint(b.expr_countones(b.expr_var(0), b.expr_var(1)))
        return [1]

    rc, _ = _solve(build)
    assert rc == SOLVE_UNSAT


# ------------------------------------------------------------------ #
# clog2  (tier-0 operand: width <= 31)                               #
# ------------------------------------------------------------------ #

@pytest.mark.parametrize("v", [1, 2, 3, 4, 5, 7, 8, 9, 16, 17, 255])
def test_clog2_forward(v):
    """Pin operand to a known value; read back clog2(operand)."""
    def build(b):
        b.add_var(0, _RESW, False, 0, 32)   # result, free (tier-0)
        b.add_var(1, 16, False, v, v)       # operand pinned (tier-0)
        b.add_constraint(b.expr_clog2(b.expr_var(0), b.expr_var(1)))
        return [0]

    rc, vals = _solve(build)
    assert rc == SOLVE_OK, "clog2 of %d should be SAT" % v
    assert vals[0] == _clog2(v), (
        "clog2(%d) got %d, expected %d" % (v, vals[0], _clog2(v))
    )


# ------------------------------------------------------------------ #
# onehot / onehot0 (composed from countones)                         #
# ------------------------------------------------------------------ #

def test_onehot_exactly_one_bit():
    """$onehot(x) == (countones(x)==1): exactly one bit set."""
    width = 8
    def build(b):
        b.add_var(0, _RESW, False, 1, 1)    # countones result pinned to 1
        b.add_var(1, width, False, 0, (1 << width) - 1)
        b.add_constraint(b.expr_countones(b.expr_var(0), b.expr_var(1)))
        return [1]

    rc, vals = _solve(build)
    assert rc == SOLVE_OK
    assert _popcount(vals[1], width) == 1


def test_onehot0_at_most_one_bit():
    """$onehot0(x) == (countones(x)<=1): zero or one bit set."""
    width = 8
    def build(b):
        b.add_var(0, _RESW, False, 0, width)  # countones result, free
        b.add_var(1, width, False, 0, (1 << width) - 1)
        b.add_constraint(b.expr_countones(b.expr_var(0), b.expr_var(1)))
        b.add_constraint(
            b.expr_binary(BIN_LTE, b.expr_var(0), b.expr_const(1))
        )
        return [1]

    rc, vals = _solve(build)
    assert rc == SOLVE_OK
    assert _popcount(vals[1], width) <= 1


# ------------------------------------------------------------------ #
# BUG-1 regression: tier-1 operand/result must be correct.           #
# These pinned the bug (silent-wrong model / spurious UNSAT) while it #
# was open; the propagators are now tier-aware (read via var_lo64/hi64#
# tighten via ctx_tighten_*_64), so they assert correctness directly. #
# ------------------------------------------------------------------ #

@pytest.mark.parametrize("opw", [32, 40, 48, 64])
def test_countones_tier1_operand(opw):
    """countones over a tier-1 (>=32-bit) pinned operand == true popcount."""
    opnd = 7  # popcount 3
    def build(b):
        b.add_var(0, _RESW, False, 0, 64)
        b.add_var(1, opw, False, opnd, opnd)   # 32-bit unsigned+ => tier-1
        b.add_constraint(b.expr_countones(b.expr_var(0), b.expr_var(1)))
        return [0]

    rc, vals = _solve(build)
    assert rc == SOLVE_OK and vals[0] == 3


def test_countones_tier1_result():
    """A 32-bit unsigned (tier-1) result var solves countones correctly."""
    def build(b):
        b.add_var(0, 32, False, 0, 40)         # 32-bit unsigned => tier-1
        b.add_var(1, 8, False, 7, 7)           # popcount 3
        b.add_constraint(b.expr_countones(b.expr_var(0), b.expr_var(1)))
        return [0]

    rc, vals = _solve(build)
    assert rc == SOLVE_OK and vals[0] == 3


@pytest.mark.parametrize("k", [0, 1, 3, 8, 32])
def test_countones_tier1_operand_backward(k):
    """Pin result==k, free 32-bit (tier-1) operand: model must satisfy."""
    def build(b):
        b.add_var(0, _RESW, False, k, k)
        b.add_var(1, 32, False, 0, (1 << 32) - 1)
        b.add_constraint(b.expr_countones(b.expr_var(0), b.expr_var(1)))
        return [1]

    for seed in range(1, 6):
        rc, vals = _solve(build, seed=seed)
        assert rc == SOLVE_OK
        assert _popcount(vals[1], 32) == k, (
            "seed %d: operand %#x popcount %d != %d"
            % (seed, vals[1], _popcount(vals[1], 32), k)
        )


@pytest.mark.parametrize("resw,opw,operand,expected", [
    (32, 16, 16, 4),
    (8, 32, 16, 4),
    (32, 32, 1000, 10),
    (8, 48, 1 << 40, 40),
])
def test_clog2_tier1(resw, opw, operand, expected):
    """clog2 with tier-1 operand and/or result var reads back correctly."""
    def build(b):
        b.add_var(0, resw, False, 0, 63)
        b.add_var(1, opw, False, operand, operand)
        b.add_constraint(b.expr_clog2(b.expr_var(0), b.expr_var(1)))
        return [0]

    rc, vals = _solve(build)
    assert rc == SOLVE_OK and vals[0] == expected
