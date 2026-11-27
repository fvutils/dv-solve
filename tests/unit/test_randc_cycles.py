"""randc through dvs_randomize(): every cycle, not only the first, takes each
value the constraints allow exactly once (IEEE 1800 18.4.2).

A cycle whose values do not fill the declared range ends by an UNSAT: once
every value left is excluded, the search must see that no value remains.
Excluded values (holes) are not bounds, so a randc variable the search never
decides -- fixed by propagation, or with nothing but holes left in its range
-- used to land on a hole: the solve returned an excluded value, the UNSAT
never came, and the cycle never ended (one value repeated from then on).
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.handle import RANDOMIZE_OK, SolveHandle
from dv_solve.problem import BIN_LT, BIN_NEQ, VAR_RANDC

_N_CYCLES = 6


def _assert_cycles(h, vid, values, n_cycles=_N_CYCLES, seed0=1234):
    """Each run of len(values) calls is one cycle: every value once."""
    n = len(values)
    got = []
    for i in range(n * n_cycles):
        rc, vals, _ = h.randomize(seed0 + 77 * i)
        assert rc == RANDOMIZE_OK, f"call {i}: rc={rc}"
        got.append(vals[vid])
    for c in range(n_cycles):
        cyc = got[c * n:(c + 1) * n]
        assert sorted(cyc) == sorted(values), (
            f"cycle {c} is {cyc}, not a permutation of {sorted(values)} "
            f"(all values: {got})")


@pytest.mark.parametrize("extend", [False, True])
def test_randc_read_zero_extended_cycles(extend):
    """randc bit [2:0] c; constraint { c != 7; } where c is read
    zero-extended to 32 bits: values 0..6, a hole-only range left at the
    end of each cycle (the declared range 0..7 is never exhausted)."""
    b = SolveProblemBuilder()
    b.add_var(0, width=3, is_signed=False, lo=0, hi=7)
    b.set_var_kind(0, VAR_RANDC)
    v = b.expr_var(0)
    if extend:
        v = b.expr_extend(v, 3, 32)
        k = b.expr_const(7, width=32)
    else:
        k = b.expr_const(7, width=3)
    b.add_constraint(b.expr_binary(BIN_NEQ, v, k))
    h = SolveHandle(b.finalize_bytes(), vars=[(3, False)])
    _assert_cycles(h, 0, list(range(7)))


def test_randc_zero_extended_bound_mid_range():
    """randc bit [3:0] c; constraint { 32'(c) < 10; }: values 0..9 of 0..15."""
    b = SolveProblemBuilder()
    b.add_var(0, width=4, is_signed=False, lo=0, hi=15)
    b.set_var_kind(0, VAR_RANDC)
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_extend(b.expr_var(0), 4, 32),
                                   b.expr_const(10, width=32)))
    h = SolveHandle(b.finalize_bytes(), vars=[(4, False)])
    _assert_cycles(h, 0, list(range(10)), n_cycles=4)


@pytest.mark.parametrize("order", [False, True])
def test_randc_solved_after_another_var_cycles(order):
    """randc bit [1:0] c; rand bit [1:0] x; constraint { c < x; solve x
    before c; }: c takes 0..2. With x decided first, c is fixed by
    propagation (x == 1 leaves c in [0, 0]) or left with only holes."""
    b = SolveProblemBuilder()
    b.add_var(0, width=2, is_signed=False, lo=0, hi=3)   # c
    b.add_var(1, width=2, is_signed=False, lo=0, hi=3)   # x
    b.set_var_kind(0, VAR_RANDC)
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_var(1)))
    if order:
        b.set_var_order(1, 0)
        b.set_var_order(0, 1)
    h = SolveHandle(b.finalize_bytes(), vars=[(2, False), (2, False)])
    _assert_cycles(h, 0, [0, 1, 2])


def test_randc_solved_after_wider_cycles():
    """randc bit [3:0] c; rand bit [3:0] x; constraint { c < x; x < 12;
    solve x before c; }: c takes 0..10 in every cycle."""
    b = SolveProblemBuilder()
    b.add_var(0, width=4, is_signed=False, lo=0, hi=15)   # c
    b.add_var(1, width=4, is_signed=False, lo=0, hi=15)   # x
    b.set_var_kind(0, VAR_RANDC)
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_var(1)))
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(1), b.expr_const(12, width=4)))
    b.set_var_order(1, 0)
    b.set_var_order(0, 1)
    h = SolveHandle(b.finalize_bytes(), vars=[(4, False), (4, False)])
    _assert_cycles(h, 0, list(range(11)), n_cycles=4)
