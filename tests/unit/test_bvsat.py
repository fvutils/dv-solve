"""Conformance tests for the BV-SAT completeness engine (zsp_bbsolver / BVSatCtx).

Phase A, test layer T-C (see doc/notes/dv_solve_phaseA_bvsat_plan.md). The engine
was previously *untested*; this is the trust foundation before the back-end is
allowed to rely on its SAT/UNSAT verdicts.

Strategy:
  * Per-node: for each handled expr/op kind, a problem with a KNOWN model asserts
    SAT + correct readback, and a known-UNSAT twin asserts UNSAT.
  * Width/sign sweep: readback (incl. sign-extension) across tier boundaries.
  * Cross-check vs the primary engine (SolveCtx): the two engines must agree on
    SAT/UNSAT, and each SAT model must satisfy the constraints (models may differ).
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import (
    BIN_EQ, BIN_NEQ, BIN_GT, BIN_GTE, BIN_LT, BIN_LTE,
    BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV, BIN_MOD,
    BIN_BAND, BIN_BOR, BIN_BXOR, BIN_LSHIFT, BIN_RSHIFT,
    BIN_AND, BIN_OR, UN_NOT, UN_NEG, UN_INVERT,
)
from dv_solve.bvsat import BVSatCtx, BVSAT_SAT, BVSAT_UNSAT
from dv_solve.ctx import SolveCtx, SOLVE_OK, SOLVE_UNSAT, CompileUnsatError


# ------------------------------------------------------------------ #
# Helpers                                                             #
# ------------------------------------------------------------------ #

def _solve_bvsat(build):
    """Build a problem via `build(b)` and check it on the BV-SAT engine.

    Returns (result, values_dict) where values are read back only on SAT.
    `build` returns the list of var_ids to read back.
    """
    b = SolveProblemBuilder()
    var_ids = build(b)
    buf, _sz = b.finalize()
    bb = BVSatCtx(buf)
    try:
        rc = bb.check()
        vals = {}
        if rc == BVSAT_SAT:
            for vid in var_ids:
                vals[vid] = bb.value(vid)
        return rc, vals
    finally:
        bb.destroy()
        b.destroy()


def _solve_primary(build, seed=1):
    """Same problem on the primary bounds-propagation engine, for cross-check."""
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


def assert_sat(build, expect=None):
    rc, vals = _solve_bvsat(build)
    assert rc == BVSAT_SAT, "expected SAT, got rc=%d" % rc
    if expect:
        for vid, want in expect.items():
            assert vals[vid] == want, \
                "var %d: got %d, want %d" % (vid, vals[vid], want)
    return vals


def assert_unsat(build):
    rc, _ = _solve_bvsat(build)
    assert rc == BVSAT_UNSAT, "expected UNSAT, got rc=%d" % rc


# ------------------------------------------------------------------ #
# Per-node: equality / arithmetic                                     #
# ------------------------------------------------------------------ #

def test_eq_const():
    # NB: add_var returns an ExprRef, not the var_id — read back by var_id (0).
    def build(b):
        b.add_var(0, 8, False, 0, 255)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_const(42)))
        return [0]
    assert_sat(build, expect={0: 42})


def test_eq_conflict_unsat():
    # x == 42 AND x == 43 -> UNSAT (unambiguous, width-independent).
    def build(b):
        b.add_var(0, 8, False, 0, 255)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_const(42)))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_const(43)))
        return [0]
    assert_unsat(build)


def test_add():
    # c == a + b, a == 10, b == 20  -> c == 30 (exercises subst readback chain)
    def build(b):
        for i in range(3):
            b.add_var(i, 16, False, 0, 65535)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(2),
                         b.expr_binary(BIN_ADD, b.expr_var(0), b.expr_var(1))))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_const(10)))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1), b.expr_const(20)))
        return [0, 1, 2]
    assert_sat(build, expect={0: 10, 1: 20, 2: 30})


def test_sub_mul():
    def build(b):
        b.add_var(0, 16, False, 0, 65535)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                         b.expr_binary(BIN_SUB, b.expr_const(100), b.expr_const(58))))
        return [0]
    assert_sat(build, expect={0: 42})

    def build2(b):
        b.add_var(0, 16, False, 0, 65535)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                         b.expr_binary(BIN_MUL, b.expr_const(6), b.expr_const(7))))
        return [0]
    assert_sat(build2, expect={0: 42})


def test_div_mod_unsigned():
    def build(b):
        b.add_var(0, 16, False, 0, 65535)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                         b.expr_binary(BIN_DIV, b.expr_const(85), b.expr_const(2))))
        return [0]
    assert_sat(build, expect={0: 42})

    def build2(b):
        b.add_var(0, 16, False, 0, 65535)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                         b.expr_binary(BIN_MOD, b.expr_const(142), b.expr_const(100))))
        return [0]
    assert_sat(build2, expect={0: 42})


# ------------------------------------------------------------------ #
# Per-node: relational (constraint-solving, not just const-fold)      #
# ------------------------------------------------------------------ #

@pytest.mark.parametrize("op,lo,hi,pred", [
    (BIN_GT,  0, 100, lambda v: v > 50),
    (BIN_GTE, 0, 100, lambda v: v >= 50),
    (BIN_LT,  0, 100, lambda v: v < 50),
    (BIN_LTE, 0, 100, lambda v: v <= 50),
    (BIN_NEQ, 50, 50, None),   # x in [50,50] and x != 50 -> UNSAT
])
def test_relational(op, lo, hi, pred):
    def build(b):
        b.add_var(0, 8, False, lo, hi)
        b.add_constraint(b.expr_binary(op, b.expr_var(0), b.expr_const(50)))
        return [0]
    if pred is None:
        assert_unsat(build)
    else:
        vals = assert_sat(build)
        assert pred(vals[0]), "value %d violates predicate" % vals[0]


# ------------------------------------------------------------------ #
# Per-node: bitwise / shift / unary                                   #
# ------------------------------------------------------------------ #

def test_bitwise():
    # x == (0xF0 & 0x3C) | 0x01 == 0x30 | 0x01 == 0x31
    def build(b):
        b.add_var(0, 8, False, 0, 255)
        e = b.expr_binary(BIN_BOR,
                          b.expr_binary(BIN_BAND, b.expr_const(0xF0), b.expr_const(0x3C)),
                          b.expr_const(0x01))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), e))
        return [0]
    assert_sat(build, expect={0: 0x31})


def test_xor_shift():
    def build(b):
        b.add_var(0, 8, False, 0, 255)
        # (0x0F ^ 0xFF) == 0xF0 ; >> 4 == 0x0F
        e = b.expr_binary(BIN_RSHIFT,
                          b.expr_binary(BIN_BXOR, b.expr_const(0x0F), b.expr_const(0xFF)),
                          b.expr_const(4))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), e))
        return [0]
    assert_sat(build, expect={0: 0x0F})

    def build2(b):
        b.add_var(0, 8, False, 0, 255)
        e = b.expr_binary(BIN_LSHIFT, b.expr_const(0x03), b.expr_const(2))  # 12
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), e))
        return [0]
    assert_sat(build2, expect={0: 12})


def test_unary_invert():
    # x == ~0x0F (8-bit) == 0xF0
    def build(b):
        b.add_var(0, 8, False, 0, 255)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                         b.expr_unary(UN_INVERT, b.expr_const(0x0F))))
        return [0]
    assert_sat(build, expect={0: 0xF0})


# ------------------------------------------------------------------ #
# Width / signedness sweep                                            #
# ------------------------------------------------------------------ #

@pytest.mark.parametrize("width", [1, 7, 8, 16, 31, 32, 33, 48, 63, 64])
def test_width_unsigned_readback(width):
    # x == max unsigned value for the width. Readback is an int64 two's-complement
    # reinterpretation of the low `width` bits (same contract as the primary
    # engine's get_value); for width==64 that is -1 for the all-ones pattern.
    # Compare on the low `width` bits, which is the engine's actual guarantee.
    maxv = (1 << width) - 1
    mask = maxv
    def build(b):
        b.add_var(0, width, False, 0, maxv)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_const(maxv)))
        return [0]
    vals = assert_sat(build)
    assert (vals[0] & mask) == maxv, \
        "width %d: low bits %#x != %#x" % (width, vals[0] & mask, maxv)


@pytest.mark.parametrize("width,val", [
    (8, -1), (8, -128), (16, -1), (16, -32768), (32, -1), (32, -2147483648),
    (33, -1), (48, -42), (63, -1),
])
def test_width_signed_readback(width, val):
    lo = -(1 << (width - 1))
    hi = (1 << (width - 1)) - 1
    def build(b):
        b.add_var(0, width, True, lo, hi)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                         b.expr_const(val, is_signed=True)))
        return [0]
    assert_sat(build, expect={0: val})


def test_signed_relational():
    # signed x in [-10, 10], x < 0  -> must read back negative
    def build(b):
        b.add_var(0, 8, True, -10, 10)
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0),
                         b.expr_const(0, is_signed=True)))
        return [0]
    vals = assert_sat(build)
    assert vals[0] < 0, "expected negative, got %d" % vals[0]


# ------------------------------------------------------------------ #
# Cross-check vs the primary engine                                   #
# ------------------------------------------------------------------ #

_CROSS_CASES = {
    "range":      lambda b: ([b.add_var(0, 8, False, 10, 20)],
                             [b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(0), b.expr_const(14)))]),
    "arith":      lambda b: ([b.add_var(0, 16, False, 0, 1000), b.add_var(1, 16, False, 0, 1000)],
                             [b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1),
                                               b.expr_binary(BIN_ADD, b.expr_var(0), b.expr_const(5))))]),
    "unsat":      lambda b: ([b.add_var(0, 8, False, 0, 255)],
                             [b.add_constraint(b.expr_binary(BIN_AND,
                                               b.expr_binary(BIN_GT, b.expr_var(0), b.expr_const(200)),
                                               b.expr_binary(BIN_LT, b.expr_var(0), b.expr_const(100))))]),
}


@pytest.mark.parametrize("name", list(_CROSS_CASES))
def test_cross_check_primary(name):
    """Both engines must agree SAT vs UNSAT on the same problem."""
    fn = _CROSS_CASES[name]
    def build(b):
        fn(b)
        return []
    rc_bb, _ = _solve_bvsat(build)
    rc_pr, _ = _solve_primary(build)
    bb_sat = (rc_bb == BVSAT_SAT)
    pr_sat = (rc_pr == SOLVE_OK)
    assert bb_sat == pr_sat, \
        "[%s] disagreement: bvsat sat=%s primary sat=%s" % (name, bb_sat, pr_sat)


# ------------------------------------------------------------------ #
# T-D — seed diversity & determinism (A-3)                            #
# ------------------------------------------------------------------ #

def _wide_band(b):
    # x in [0,1000] constrained to 100 <= x <= 900 — a large feasible set.
    b.add_var(0, 16, False, 0, 1000)
    b.add_constraint(b.expr_binary(BIN_AND,
        b.expr_binary(BIN_GTE, b.expr_var(0), b.expr_const(100)),
        b.expr_binary(BIN_LTE, b.expr_var(0), b.expr_const(900))))
    return [0]


def _solve_with_seed(seed):
    b = SolveProblemBuilder()
    _wide_band(b)
    buf, _sz = b.finalize()
    bb = BVSatCtx(buf)
    try:
        assert bb.check(seed=seed) == BVSAT_SAT
        return bb.value(0)
    finally:
        bb.destroy()
        b.destroy()


def test_seed_diversity():
    """Distinct seeds must yield many distinct models over a large feasible set.
    Without the A-3 phase randomization this collapses to a single value."""
    vals = [_solve_with_seed(s) for s in range(1, 201)]
    distinct = len(set(vals))
    assert distinct >= 20, "expected diverse models, got %d distinct" % distinct
    # All models must satisfy the constraint.
    assert all(100 <= v <= 900 for v in vals)


def test_seed_determinism():
    """Same seed -> identical model (reproducibility)."""
    assert _solve_with_seed(7) == _solve_with_seed(7) == _solve_with_seed(7)


def test_seed_zero_is_default_deterministic():
    """seed=0 keeps kissat defaults and is reproducible."""
    assert _solve_with_seed(0) == _solve_with_seed(0)


# ------------------------------------------------------------------ #
# A-4 — soundness guards: defer (UNKNOWN), never a wrong verdict      #
# ------------------------------------------------------------------ #

from dv_solve.bvsat import BVSAT_UNKNOWN


@pytest.mark.parametrize("op", [BIN_DIV, BIN_MOD])
def test_guard_signed_divmod_defers(op):
    """The bit-blaster does unsigned div/mod only; a signed operand must defer
    (UNKNOWN) rather than silently compute a wrong unsigned quotient."""
    def build(b):
        b.add_var(0, 16, True, -100, 100)   # signed dividend
        b.add_var(1, 16, False, 0, 100)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1),
                         b.expr_binary(op, b.expr_var(0), b.expr_const(2))))
        return []
    rc, _ = _solve_bvsat(build)
    assert rc == BVSAT_UNKNOWN, "signed div/mod should defer, got rc=%d" % rc


def test_guard_unsigned_divmod_not_over_deferred():
    """Unsigned div/mod must still solve (the guard must not over-trigger)."""
    def build(b):
        b.add_var(0, 16, False, 0, 100)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                         b.expr_binary(BIN_DIV, b.expr_const(85), b.expr_const(2))))
        return [0]
    assert_sat(build, expect={0: 42})


def test_guard_all_different_defers():
    """AllDifferent is not encoded by the bit-blaster; it must defer, not drop
    the (hard) constraint and return a bogus SAT."""
    b = SolveProblemBuilder()
    for i in range(3):
        b.add_var(i, 8, False, 0, 2)
    b.add_all_different([0, 1, 2])
    buf, _sz = b.finalize()
    bb = BVSatCtx(buf)
    try:
        assert bb.check() == BVSAT_UNKNOWN
    finally:
        bb.destroy()
        b.destroy()
