"""Unit tests for SystemVerilog `inside` edge shapes exercised by Verilator:
multi-range unions, open/unbounded ranges ([a:$] / [$:b]), and wildcard `==?`.

The plain `inside {a, b, [lo:hi]}` set/range cases are already covered by
test_inset_unary.py (expr_in_set / expr_in_range). This file adds the shapes
that test_inset_unary.py does NOT cover, using the higher-level builder wrapper
(SolveProblemBuilder + SolveCtx) rather than the raw-ctypes _wire harness.

Open ranges: SystemVerilog `[a:$]` lowers to `a <= x <= TYPE_MAX` and `[$:b]` to
`0 <= x <= b` -- there is no special "$" sentinel in the IR; the frontend
substitutes the type bound. So an open range is just a range whose open side is
the width min/max.

Wildcard `x inside {8'b1010_??01}` lowers to `(x & care_mask) == (pattern &
care_mask)`, built here from BIN_BAND + BIN_EQ (there is no dedicated masked-eq
node -- confirmed against the C builder API).

Maps to Verilator tests: t_inside, t_inside_unbounded*, t_inside_wild,
t_case_inside_with_x, and the multi-range `inside` forms.
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_BAND, BIN_EQ
from dv_solve.ctx import (
    SolveCtx, SOLVE_OK, SOLVE_UNSAT, CompileUnsatError,
)


# ------------------------------------------------------------------ #
# Helpers                                                             #
# ------------------------------------------------------------------ #

def _solve(build, seed=1):
    b = SolveProblemBuilder()
    vids = build(b)
    buf, _sz = b.finalize()
    try:
        try:
            ctx = SolveCtx(buf)
        except CompileUnsatError:
            return SOLVE_UNSAT, {}
        rc = ctx.solve(seed=seed)
        vals = {v: ctx.get_value(v) for v in vids} if rc == SOLVE_OK else {}
        ctx.destroy()
        return rc, vals
    finally:
        b.destroy()


def _collect(build, n_seeds=48):
    """Solve across seeds; return the set of value-0 assignments seen."""
    b = SolveProblemBuilder()
    vids = build(b)
    buf, _sz = b.finalize()
    seen = set()
    try:
        try:
            ctx = SolveCtx(buf)
        except CompileUnsatError:
            return seen
        for s in range(1, n_seeds + 1):
            ctx.reset()
            if ctx.solve(seed=s) == SOLVE_OK:
                seen.add(ctx.get_value(vids[0]))
        ctx.destroy()
        return seen
    finally:
        b.destroy()


# ------------------------------------------------------------------ #
# Multi-range union                                                  #
# ------------------------------------------------------------------ #

def _mkranges(b, pairs):
    return [(b.expr_const(lo), b.expr_const(hi)) for (lo, hi) in pairs]


def test_multi_range_membership_and_coverage():
    """x inside {[1:3],[10:12],[20:22]}: every model in the union, and each
    disjoint range is reachable across seeds."""
    pairs = [(1, 3), (10, 12), (20, 22)]
    allowed = set()
    for lo, hi in pairs:
        allowed |= set(range(lo, hi + 1))

    def build(b):
        b.add_var(0, 8, False, 0, 255)
        b.add_constraint(b.expr_in_ranges(b.expr_var(0), _mkranges(b, pairs)))
        return [0]

    seen = _collect(build)
    assert seen, "expected at least one model"
    assert seen <= allowed, "values outside the union: %s" % (seen - allowed)
    # Each of the three ranges should be hit at least once.
    for lo, hi in pairs:
        assert any(lo <= v <= hi for v in seen), (
            "range [%d:%d] never reached; seen=%s" % (lo, hi, sorted(seen))
        )


def test_multi_range_unsat_hole():
    """A point-constraint into a gap between ranges is UNSAT."""
    def build(b):
        b.add_var(0, 8, False, 7, 7)   # 7 is in the gap between [1:3] and [10:12]
        b.add_constraint(
            b.expr_in_ranges(b.expr_var(0), _mkranges(b, [(1, 3), (10, 12)]))
        )
        return [0]

    rc, _ = _solve(build)
    assert rc == SOLVE_UNSAT


# ------------------------------------------------------------------ #
# Open / unbounded ranges                                            #
# ------------------------------------------------------------------ #

def test_open_range_low_bounded():
    """[a:$] == [a, MAX]: only the low side constrains."""
    a = 200
    def build(b):
        b.add_var(0, 8, False, 0, 255)
        b.add_constraint(
            b.expr_in_ranges(b.expr_var(0), _mkranges(b, [(a, 255)]))
        )
        return [0]

    seen = _collect(build)
    assert seen, "expected models"
    assert all(v >= a for v in seen), "value below open low bound: %s" % sorted(seen)
    assert max(seen) == 255 or len(seen) > 1  # upper side is unconstrained


def test_open_range_high_bounded():
    """[$:b] == [0, b]: only the high side constrains."""
    bnd = 5
    def build(b):
        b.add_var(0, 8, False, 0, 255)
        b.add_constraint(
            b.expr_in_ranges(b.expr_var(0), _mkranges(b, [(0, bnd)]))
        )
        return [0]

    seen = _collect(build)
    assert seen
    assert all(v <= bnd for v in seen), "value above open high bound: %s" % sorted(seen)


# ------------------------------------------------------------------ #
# Wildcard  ==?                                                      #
# ------------------------------------------------------------------ #

@pytest.mark.parametrize("pattern,care_mask,free_bits", [
    (0xA1, 0xF3, 2),   # 8'b1010_??01 -> bits 2,3 free
    (0x00, 0xFF, 0),   # fully specified
    (0x00, 0x00, 8),   # all wildcard -> any value
    (0x5A, 0x0F, 4),   # low nibble specified, high nibble free
])
def test_wildcard_masked_eq(pattern, care_mask, free_bits):
    """x ==? pattern  <=>  (x & care_mask) == (pattern & care_mask).

    Scope is *correctness of the masked-eq shape*: every model matches the care
    bits, a fully-specified pattern pins x exactly, and free bits genuinely vary.

    NB: this deliberately does NOT assert enumeration of all 2**free_bits values
    -- the seed sweep only reaches ~8 distinct free-nibble values regardless of
    fair_pick, which is the known solution-diversity limitation (see the
    verilator-dropin memory), a separate concern from wildcard correctness."""
    def build(b):
        b.add_var(0, 8, False, 0, 255)
        lhs = b.expr_binary(BIN_BAND, b.expr_var(0), b.expr_const(care_mask))
        rhs = b.expr_const(pattern & care_mask)
        b.add_constraint(b.expr_binary(BIN_EQ, lhs, rhs))
        return [0]

    seen = _collect(build, n_seeds=96)
    assert seen, "expected models"
    for v in seen:
        assert (v & care_mask) == (pattern & care_mask), (
            "value %#x violates care bits (mask %#x, pat %#x)"
            % (v, care_mask, pattern)
        )
    if free_bits == 0:
        assert seen == {pattern & care_mask}, (
            "fully-specified pattern should pin x exactly; saw %s" % sorted(seen)
        )
    else:
        assert len(seen) > 1, (
            "free bits present but only one value seen: %s" % sorted(seen)
        )
