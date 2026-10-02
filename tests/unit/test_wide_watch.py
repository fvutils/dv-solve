"""Propagators that watch more than four variables.

A propagator normally keeps its watched variables in a four-slot section after
its header. DisjClause (an OR of comparisons) used that section for up to 32
variables, overwriting its watcher chains, so a constraint registered earlier
on one of those variables could stop being woken. AllDifferent and SumEq keep
a longer list in a different layout, which the conflict explainers read as if
it were the four-slot one: the explanation named a bogus variable and missed
the last real one, and an explanation too long to fit was cut short. A
learnt clause missing an antecedent is stronger than the truth and can cut
off real solutions.
"""
from __future__ import annotations

import ctypes
import itertools
import random
import re
from pathlib import Path

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SOLVE_OK, SOLVE_UNSAT, CompileUnsatError, SolveCtx, _SolveOpts
from dv_solve.problem import BIN_EQ, BIN_GT, BIN_LT, BIN_NEQ, BIN_OR

_REPO = Path(__file__).resolve().parents[2]


def test_solve_opts_matches_header() -> None:
    """The Python options struct has every field of dvs_solve_opts_t."""
    hdr = (_REPO / "src" / "c" / "dv_solve.h").read_text()
    body = re.search(r"typedef struct \{([^}]*)\}\s*dvs_solve_opts_t;", hdr).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    c_fields = re.findall(r"(\w+)\s*(?:\[\d+\])?\s*;", body)
    assert [f for f, _ in _SolveOpts._fields_] == c_fields


def _or(b, terms):
    e = terms[0]
    for t in terms[1:]:
        e = b.expr_binary(BIN_OR, e, t)
    return e


@pytest.mark.parametrize("n", [5, 6, 8, 12, 16])
def test_disjunction_keeps_earlier_watchers(n: int) -> None:
    """Constraints on the 5th and later variables of an OR stay enforced."""
    for seed in range(1, 41):
        b = SolveProblemBuilder()
        for i in range(n):
            b.add_var(i, width=8, is_signed=False, lo=0, hi=255)
        # A chain over the OR's later variables, added before the OR.
        for i in range(4, n - 1):
            b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(i), b.expr_var(i + 1)))
        b.add_constraint(_or(b, [b.expr_binary(BIN_EQ, b.expr_var(i), b.expr_const(250 + i % 5))
                                 for i in range(n)]))
        problem, _ = b.finalize()
        with SolveCtx(problem) as ctx:
            assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
            v = [ctx.get_value(i) for i in range(n)]
        assert all(v[i] < v[i + 1] for i in range(4, n - 1)), (seed, v)
        assert any(v[i] == 250 + i % 5 for i in range(n)), (seed, v)


# ------------------------------------------------------------------ #
# Search, with and without clause learning (use_lcg), against brute    #
# force, on problems mixing all-different, sums and wide ORs over      #
# small domains.                                                       #
# ------------------------------------------------------------------ #

_W = 3
_OPS = {BIN_EQ: lambda a, b: a == b, BIN_NEQ: lambda a, b: a != b,
        BIN_LT: lambda a, b: a < b, BIN_GT: lambda a, b: a > b}


def _gen(rng):
    n = rng.randint(4, 6)
    cons = []
    if rng.random() < 0.7:
        cons.append(("alldiff", rng.sample(range(n), rng.randint(3, n))))
    if rng.random() < 0.5:
        r, *xs = rng.sample(range(n), rng.randint(3, min(n, 5)))
        cons.append(("sum", r, xs))
    for _ in range(rng.randint(1, 2)):
        lits = []
        for _ in range(rng.randint(5, 9)):
            if rng.random() < 0.6:
                lits.append(("c", rng.randrange(n), rng.choice(list(_OPS)), rng.randrange(1 << _W)))
            else:
                a, c = rng.sample(range(n), 2)
                lits.append(("v", a, rng.choice(list(_OPS)), c))
        cons.append(("or", lits))
    for _ in range(rng.randint(0, 4)):
        a, c = rng.sample(range(n), 2)
        cons.append(("or", [("v", a, rng.choice(list(_OPS)), c)]))
    return n, cons


def _holds(cons, v):
    for c in cons:
        if c[0] == "alldiff":
            if len({v[i] for i in c[1]}) != len(c[1]):
                return False
        elif c[0] == "sum":
            if v[c[1]] != sum(v[i] for i in c[2]):
                return False
        elif not any(_OPS[op](v[a], k if kind == "c" else v[k]) for kind, a, op, k in c[1]):
            return False
    return True


def _build(n, cons):
    b = SolveProblemBuilder()
    for i in range(n):
        b.add_var(i, width=_W, is_signed=False, lo=0, hi=(1 << _W) - 1)
    for c in cons:
        if c[0] == "alldiff":
            b.add_all_different(c[1])
        elif c[0] == "sum":
            b.add_constraint(b.expr_sum(b.expr_var(c[1]), [b.expr_var(i) for i in c[2]]))
        else:
            b.add_constraint(_or(b, [b.expr_binary(op, b.expr_var(a),
                                                   b.expr_const(k) if kind == "c" else b.expr_var(k))
                                     for kind, a, op, k in c[1]]))
    return b.finalize()[0]


def _check(n, cons, sat, seeds, lcg_modes=(0, 1)):
    try:
        ctx = SolveCtx(_build(n, cons))
    except CompileUnsatError:
        assert not sat, cons
        return
    with ctx:
        for lcg in lcg_modes:
            for s in seeds:
                ctx.reset()
                rc = ctx._lib.dvs_solver_solve(ctx._ctx, ctypes.byref(_SolveOpts(
                    seed=s, use_lcg=lcg, fair_pick=1, max_conflicts=200, max_restarts=50)))
                if rc == SOLVE_OK:
                    v = [ctx.get_value(i) for i in range(n)]
                    assert _holds(cons, v), ("wrong model", lcg, s, cons, v)
                elif rc == SOLVE_UNSAT:
                    assert not sat, ("wrong unsat", lcg, s, cons)
                # SOLVE_TIMEOUT is an honest answer


# Two ORs of 6 and 9 comparisons plus two `!=`: with the overlapping watch
# lists, about 1 solve in 40 returned v2 == v3.
_WIDE_OR_CASE = (4, [
    ("or", [("c", 1, BIN_NEQ, 6), ("c", 3, BIN_GT, 5), ("c", 0, BIN_GT, 5), ("v", 1, BIN_LT, 0),
            ("c", 1, BIN_LT, 2), ("c", 0, BIN_EQ, 2), ("v", 1, BIN_EQ, 0), ("c", 1, BIN_EQ, 6),
            ("c", 0, BIN_EQ, 0)]),
    ("or", [("v", 1, BIN_EQ, 2), ("c", 0, BIN_EQ, 6), ("v", 2, BIN_NEQ, 0), ("c", 2, BIN_LT, 6),
            ("c", 3, BIN_EQ, 1), ("v", 3, BIN_LT, 0)]),
    ("or", [("v", 3, BIN_NEQ, 2)]),
    ("or", [("v", 1, BIN_NEQ, 0)]),
])


def test_wide_or_models_hold() -> None:
    _check(*_WIDE_OR_CASE, sat=True, seeds=range(1, 201))


@pytest.mark.parametrize("seed", range(8))
def test_search_agrees_with_brute_force(seed: int) -> None:
    """With and without clause learning, on random small problems."""
    rng = random.Random(seed)
    for _ in range(40):
        n, cons = _gen(rng)
        sat = any(_holds(cons, v) for v in itertools.product(range(1 << _W), repeat=n))
        _check(n, cons, sat, seeds=(1, 7))


# ------------------------------------------------------------------ #
# Variables merged by `x == y`. Compile keeps one of the two; anything #
# addressed to the other by id must reach the one that is kept.       #
# ------------------------------------------------------------------ #

def _merged_pair(extra=None):
    b = SolveProblemBuilder()
    for i in range(3):
        b.add_var(i, width=8, is_signed=False, lo=0, hi=255)
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_var(1)))
    if extra:
        extra(b)
    return b.finalize()[0]


def test_all_different_over_merged_variables_is_unsat() -> None:
    with pytest.raises(CompileUnsatError):
        SolveCtx(_merged_pair(lambda b: b.add_all_different([0, 1, 2])))


@pytest.mark.parametrize("var", [0, 1])
def test_pin_reaches_merged_variable(var: int) -> None:
    with SolveCtx(_merged_pair()) as ctx:
        ctx._lib.dvs_solver_pin_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int64]
        for seed in range(1, 6):
            ctx.reset()                                   # also removes pins
            assert ctx._lib.dvs_solver_pin_var(ctx._ctx, var, 77) == 0
            assert ctx.solve(seed=seed) == SOLVE_OK
            assert ctx.get_value(0) == ctx.get_value(1) == 77


@pytest.mark.parametrize("var", [0, 1])
def test_exclude_reaches_merged_variable(var: int) -> None:
    def narrow(b):
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_const(2)))
    with SolveCtx(_merged_pair(narrow)) as ctx:
        ctx._lib.dvs_solver_exclude_value.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                                      ctypes.c_int64]
        assert ctx._lib.dvs_solver_exclude_value(ctx._ctx, var, 0) == 0
        for seed in range(1, 6):
            ctx.reset()
            assert ctx.solve(seed=seed) == SOLVE_OK
            assert ctx.get_value(0) == ctx.get_value(1) == 1


@pytest.mark.parametrize("var", [0, 1])
def test_dist_reaches_merged_variable(var: int) -> None:
    def dist(b):
        b.add_dist(var, [{"lo": 5, "hi": 5, "weight": 1000},
                         {"lo": 0, "hi": 255, "weight": 1, "is_per_value": False}])
    seen = []
    with SolveCtx(_merged_pair(dist)) as ctx:
        for seed in range(1, 61):
            ctx.reset()
            assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
            seen.append(ctx.get_value(0))
    assert seen.count(5) > 40, seen


@pytest.mark.parametrize("n, width", [(17, 8), (3, 33)])
def test_unsupported_all_different_is_refused(n: int, width: int) -> None:
    """Refused, not left unenforced: model validation does not check it."""
    from dv_solve.ctx import CompileUnsupportedError
    b = SolveProblemBuilder()
    for i in range(n):
        b.add_var(i, width=width, is_signed=False, lo=0, hi=(1 << width) - 1)
    b.add_all_different(list(range(n)))
    with pytest.raises(CompileUnsupportedError):
        SolveCtx(b.finalize()[0])
