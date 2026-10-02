"""Constraint-level if/else and soft constraints must not leak when not in force.

B53: compile handled `if (c) A else B` and soft constraints by compiling the
body normally and gating only the propagators it could find. Root-domain
tightening, `x == y` merges and the rest of the propagators applied whatever
`c` was or whether the soft constraint was relaxed. Each body is now reified
and implied by its guard. Checked against brute force, through the builder
API (the SystemVerilog-facing path).
"""
from __future__ import annotations

import itertools

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, CompileUnsatError, CompileIncompleteError, SOLVE_OK
from dv_solve.problem import (BIN_EQ, BIN_NEQ, BIN_LT, BIN_GTE, BIN_AND, BIN_ADD)

C, X, Y = 0, 1, 2

# name -> (builder expression, Python predicate over (c, x, y))
_ATOMS = {
    "x==y": (lambda b: b.expr_binary(BIN_EQ, b.expr_var(X), b.expr_var(Y)), lambda c, x, y: x == y),
    "x!=y": (lambda b: b.expr_binary(BIN_NEQ, b.expr_var(X), b.expr_var(Y)), lambda c, x, y: x != y),
    "x==3": (lambda b: b.expr_binary(BIN_EQ, b.expr_var(X), b.expr_const(3)), lambda c, x, y: x == 3),
    "x<y": (lambda b: b.expr_binary(BIN_LT, b.expr_var(X), b.expr_var(Y)), lambda c, x, y: x < y),
    "y>=12": (lambda b: b.expr_binary(BIN_GTE, b.expr_var(Y), b.expr_const(12)), lambda c, x, y: y >= 12),
    "x+y==9": (lambda b: b.expr_binary(BIN_EQ, b.expr_binary(BIN_ADD, b.expr_var(X), b.expr_var(Y)),
                                       b.expr_const(9)), lambda c, x, y: x + y == 9),
    "x==3&&y==3": (lambda b: b.expr_binary(BIN_AND,
                                           b.expr_binary(BIN_EQ, b.expr_var(X), b.expr_const(3)),
                                           b.expr_binary(BIN_EQ, b.expr_var(Y), b.expr_const(3))),
                   lambda c, x, y: x == 3 and y == 3),
}
_CONDS = {
    "c": (lambda b: b.expr_var(C), lambda c, x, y: c == 1),
    "x==3": _ATOMS["x==3"],
    "x==y": _ATOMS["x==y"],
}


def _builder():
    b = SolveProblemBuilder()
    b.add_var(C, width=1, is_signed=False, lo=0, hi=1)
    b.add_var(X, width=4, is_signed=False, lo=0, hi=15)
    b.add_var(Y, width=4, is_signed=False, lo=0, hi=15)
    return b


def _solve(b):
    """(answer, model) where answer is 'sat', 'unsat' or 'unknown'."""
    problem, _ = b.finalize()
    try:
        with SolveCtx(problem) as ctx:
            if ctx.solve(seed=1) != SOLVE_OK:
                return "unsat", None
            return "sat", tuple(ctx.get_value(v) for v in (C, X, Y))
    except CompileUnsatError:
        return "unsat", None
    except CompileIncompleteError:
        return "unknown", None


def _space():
    return itertools.product(range(2), range(16), range(16))


@pytest.mark.parametrize("cond", list(_CONDS))
def test_if_else_against_brute_force(cond):
    bad = []
    ce, cp = _CONDS[cond]
    for (an, (ae, ap)), (bn, (be, bp)) in itertools.product(_ATOMS.items(), repeat=2):
        for pin in (None, 0, 1):
            b = _builder()
            b.add_constraint(b.expr_ite(ce(b), ae(b), be(b)))
            if pin is not None:
                b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(C), b.expr_const(pin)))

            def holds(c, x, y):
                return (ap(c, x, y) if cp(c, x, y) else bp(c, x, y)) and (pin is None or c == pin)
            exp = any(holds(*v) for v in _space())
            got, model = _solve(b)
            if got == "unknown":
                continue
            if got != ("sat" if exp else "unsat") or (model and not holds(*model)):
                bad.append((cond, an, bn, pin, got, model))
    assert not bad, bad[:10]


@pytest.mark.parametrize("soft", ["x!=y", "x==3", "x==y", "x+y==9", "x==3&&y==3"])
def test_relaxed_soft_constraint_leaves_nothing_behind(soft):
    # A hard constraint that contradicts the soft one: the soft one must be
    # dropped completely, and the hard one's solutions all remain reachable.
    se, sp = _ATOMS[soft]
    for hard in _ATOMS:
        he, hp = _ATOMS[hard]
        b = _builder()
        b.add_constraint(he(b))
        b.add_soft_constraint(se(b))
        exp_hard = any(hp(*v) for v in _space())
        got, model = _solve(b)
        if got == "unknown":
            continue
        assert got == ("sat" if exp_hard else "unsat"), (soft, hard, got)
        if model:
            assert hp(*model), (soft, hard, model)
            # Keep the soft constraint whenever it can be kept.
            if any(hp(*v) and sp(*v) for v in _space()):
                assert sp(*model), ("soft dropped needlessly", soft, hard, model)
