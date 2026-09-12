"""The reified `x <= y` propagator must work in both directions when guard=0.

`guard ↔ (x ≤ y)` has two enforcement branches. The guard=1 branch always
tightened both operands (`x.ub ≤ y.hi` and `y.lb ≥ x.lo`). The guard=0 branch
-- which enforces `x > y` -- tightened only `x.lb ≥ y.hi + 1`, and the header
comment said so: "stub -- only one direction".

One direction was enough for as long as reification only ever ran against a
CONSTANT, where the missing side is pinned and carries no information. It stops
being enough the moment both operands are variables, which is what generalising
reification to all six comparisons over two materialised operands made routine.
`j < k + 2` reaches the propagator as `¬(r ≤ j)` with x=r and y=j, so ONLY the
y direction can bound j. Without it j kept its full domain and the search had to
guess -- and `solve` answering NO-SOLUTION is indistinguishable, to a caller,
from the problem being unsat.

Why the existing coverage probes could not catch it: they build every case with
all variables free and check that the returned assignment satisfies the
constraint. For a disjunction that is satisfiable by *either* side, and the side
that still propagates answers first. Every test here therefore forces the other
disjunct FALSE, so the reified comparison is the only way through.
"""
import ctypes

import pytest

from dv_solve.problem import (SolveProblem, BIN_ADD, BIN_EQ, BIN_LT, BIN_LTE,
                              BIN_GT, BIN_GTE, BIN_NEQ, BIN_OR)
from dv_solve.ctx import SolveCtx, CompileUnsatError

#: var 0 = j (the one under test), var 1 = k (pinned, to kill the other arm).
_J, _K = 0, 1


def _problem(width=8, is_signed=False, lo=0, hi=255):
    sp = SolveProblem()
    for i in (_J, _K):
        sp.add_var(i, width=width, is_signed=is_signed, lo=lo, hi=hi)
    return sp


def _pin(sp, vid, val):
    sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(vid),
                                     sp.expr_const(val)))


def _forced(sp, op, rhs_expr, k_val=3):
    """`k == k_val` and `(k < k_val) || (j <op> rhs)`.

    The first disjunct is false by construction, so the solver can only answer
    by propagating through the reified comparison.
    """
    _pin(sp, _K, k_val)
    sp.add_constraint(sp.expr_binary(
        BIN_OR,
        sp.expr_binary(BIN_LT, sp.expr_var(_K), sp.expr_const(k_val)),
        sp.expr_binary(op, sp.expr_var(_J), rhs_expr)))


def _solve(sp, n=4, seed=1):
    """[(j, k), ...] for up to *n* distinct solutions; [] if none."""
    ctx = SolveCtx(sp)
    try:
        ids = (ctypes.c_uint32 * 2)(_J, _K)
        n_ok, sols = ctx.solve_n(n, ids, 2, base_seed=seed)
        if n_ok:
            assert ctx.validate_model() == 0, "model violates its own problem"
        return [tuple(s) for s in sols[:n_ok]]
    finally:
        ctx.destroy()


# -- the regression ---------------------------------------------------------

@pytest.mark.parametrize("seed", [1, 7, 99])
def test_lt_against_an_arithmetic_operand_is_solvable(seed):
    """`k == 3 ; (k<3) || (j < k+2)`. Five of j's 256 values work.

    This is the shape that regressed: it reported NO-SOLUTION. The seeds are
    varied because a blind search is exactly the failure mode -- one lucky seed
    would hide it.
    """
    sp = _problem()
    _forced(sp, BIN_LT, sp.expr_binary(BIN_ADD, sp.expr_var(_K),
                                       sp.expr_const(2)))
    sols = _solve(sp, seed=seed)
    assert sols, "satisfiable (j in 0..4), reported unsolvable"
    for j, k in sols:
        assert k == 3 and j < 5, (j, k)


@pytest.mark.parametrize("op,ok", [
    (BIN_LT, lambda j: j < 5),
    (BIN_LTE, lambda j: j <= 5),
    (BIN_GT, lambda j: j > 5),
    (BIN_GTE, lambda j: j >= 5),
    (BIN_EQ, lambda j: j == 5),
    (BIN_NEQ, lambda j: j != 5),
], ids=["lt", "lte", "gt", "gte", "eq", "neq"])
def test_every_comparison_reifies_against_an_arithmetic_operand(op, ok):
    """All six, so a future operand swap cannot quietly break one of them.

    Only `<` was broken, because it is the one arm that puts the arithmetic
    result in x (`a < b` is `¬(b ≤ a)`) while ALSO negating the guard. `>=` has
    the same operand order without the negation and `>` has the negation
    without the order, and both kept working -- which is why the defect needed
    all six to be visible as a pattern rather than a one-off.
    """
    sp = _problem()
    _forced(sp, op, sp.expr_binary(BIN_ADD, sp.expr_var(_K), sp.expr_const(2)))
    sols = _solve(sp)
    assert sols, "satisfiable, reported unsolvable"
    for j, k in sols:
        assert k == 3 and ok(j), (j, k)


def test_the_unsat_case_is_still_unsat():
    """`k==3, j==1 ; (k<3) || (j >= k+2)` has no model.

    The counterweight to every assertion above: a propagator made stronger by
    tightening more is also a propagator that can tighten a domain to empty
    when it should not. This pins both variables so the answer is a fact about
    the encoding, not about the search.
    """
    sp = _problem()
    _pin(sp, _J, 1)
    try:
        sp2 = sp
        _forced(sp2, BIN_GTE, sp.expr_binary(BIN_ADD, sp.expr_var(_K),
                                             sp.expr_const(2)))
        assert _solve(sp2) == []
    except CompileUnsatError:
        pass          # detected at compile time instead: also correct


def test_propagation_reaches_the_declared_bound_without_search():
    """With j pinned to a legal value the problem must be SAT, and with an
    illegal one it must be UNSAT -- the pair separates "the encoding is right"
    from "the search got lucky"."""
    for j_val, expect_sat in ((0, True), (4, True), (5, False), (200, False)):
        sp = _problem()
        _pin(sp, _J, j_val)
        _forced(sp, BIN_LT, sp.expr_binary(BIN_ADD, sp.expr_var(_K),
                                           sp.expr_const(2)))
        try:
            sols = _solve(sp)
        except CompileUnsatError:
            sols = []
        assert bool(sols) is expect_sat, "j == %d" % j_val


def test_signed_operands_at_the_representable_edge():
    """The ±1 in both directions must not run off the end of the domain.

    An 8-bit signed j spans [-128, 127]; the guard=0 branch computes `y.hi + 1`
    and `x.lo - 1`, so a bound sitting on either edge is where an unguarded
    increment would wrap and invent a domain.
    """
    sp = _problem(width=8, is_signed=True, lo=-128, hi=127)
    _pin(sp, _K, -128)
    sp.add_constraint(sp.expr_binary(
        BIN_OR,
        sp.expr_binary(BIN_LT, sp.expr_var(_K), sp.expr_const(-128)),
        sp.expr_binary(BIN_GT, sp.expr_var(_J), sp.expr_var(_K))))
    sols = _solve(sp)
    assert sols, "j > -128 is satisfiable by 255 of 256 values"
    for j, k in sols:
        assert k == -128 and j > -128, (j, k)
