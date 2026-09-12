"""SolveProblem.add_at_least_one, and the BIN_NEQ workaround it used to carry.

The helper spelled each `v != 0` term as `(v < 0) OR (v > 0)` with the comment
"BIN_NEQ has a native solver bug". No such bug is reachable: BIN_NEQ against a
constant is correct at the constraint root and as an OR leaf, which is the only
shape this helper builds. That was measured before the workaround was removed,
including against the pre-change library, so the removal is not resting on a
fix made in the same breath.

The workaround was also only accidentally correct. `v < 0` is vacuously false
for an UNSIGNED variable, so the pair collapsed to the intended `v > 0`; for a
SIGNED variable `(v < 0) OR (v > 0)` is a different constraint from `v != 0`
only in that it was written the long way -- but the helper never declared which
it assumed, so nothing stopped a signed caller from relying on the difference.

These tests exercise the semantics rather than the encoding, so they stay
meaningful whichever way the helper spells the term.
"""
from __future__ import annotations

import itertools

import pytest

from dv_solve.problem import SolveProblem, BIN_EQ, EXPR_NULL
from dv_solve.ctx import SolveCtx, CompileIncompleteError, CompileUnsatError


def _solve(n_vars, pins, seed, signed=False):
    """at_least_one over n_vars booleans, with `pins` forcing (var, value).

    Returns "UNSAT", or the list of assigned values.
    """
    sp = SolveProblem()
    lo, hi = (-1, 1) if signed else (0, 1)
    width = 2 if signed else 1
    for i in range(n_vars):
        sp.add_var(i, width=width, is_signed=signed, lo=lo, hi=hi)
    sp.add_at_least_one(list(range(n_vars)))
    for var, val in pins:
        sp.add_constraint(
            sp.expr_binary(BIN_EQ, sp.expr_var(var), sp.expr_const(val)))
    try:
        ctx = SolveCtx(sp)
    except CompileUnsatError:
        return "UNSAT"
    except CompileIncompleteError as e:
        pytest.fail("add_at_least_one did not compile: %s" % e)
    try:
        if ctx.solve(seed=seed) != 0:
            return "UNSAT"
        vals = [ctx.get_value(i) for i in range(n_vars)]
        assert ctx.validate_model() == 0, (
            "model violates the problem: %s" % vals)
        return vals
    finally:
        ctx.destroy()


@pytest.mark.parametrize("n", [1, 2, 3, 4])
@pytest.mark.parametrize("seed", [1, 2, 7])
def test_at_least_one_uses_neq(n, seed):
    """Exhaustive over every subset of variables pinned to zero.

    Pinning all of them must be UNSAT -- that is the case a broken `!= 0`
    would get wrong by admitting the all-zero assignment. Pinning any proper
    subset must still solve, with a non-zero value among the free variables.
    """
    for k in range(n + 1):
        for combo in itertools.combinations(range(n), k):
            got = _solve(n, [(v, 0) for v in combo], seed)
            if k == n:
                assert got == "UNSAT", (
                    "n=%d all pinned to 0 must be UNSAT, got %s" % (n, got))
                continue
            assert got != "UNSAT", "n=%d pins=%s should solve" % (n, combo)
            for v in combo:
                assert got[v] == 0, "pin violated: %s" % got
            assert any(got), "all-zero assignment satisfies at_least_one: %s" % got


def test_at_least_one_signed_negative_counts():
    """A negative value is non-zero, so it satisfies the constraint.

    The old `(v < 0) OR (v > 0)` spelling agreed here; the point of the test is
    that the current one does too, for a signed variable where the two halves
    of that disjunction were doing different work."""
    sp = SolveProblem()
    sp.add_var(0, width=2, is_signed=True, lo=-1, hi=-1)   # pinned to -1
    sp.add_var(1, width=2, is_signed=True, lo=0, hi=0)     # pinned to 0
    sp.add_at_least_one([0, 1])
    ctx = SolveCtx(sp)
    try:
        assert ctx.solve(seed=1) == 0, "-1 is non-zero; this must be SAT"
        assert ctx.get_value(0) == -1
        assert ctx.validate_model() == 0
    finally:
        ctx.destroy()


def test_at_least_one_empty_is_noop():
    """No variables: nothing to assert."""
    sp = SolveProblem()
    sp.add_var(0, width=1, is_signed=False, lo=0, hi=1)
    assert sp.add_at_least_one([]) == EXPR_NULL
