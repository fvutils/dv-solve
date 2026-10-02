"""Array selects solved with clause learning on, checked against brute force.

`r = a[idx]` compiles to one guard per element, `g_i <-> (idx == i)`, and an
equality `r == a[i]` that only fires while `g_i` holds. An explanation from
that equality is only valid under the guard, so the learnt clause must cite
`g_i`; without it the clause holds for every index and prunes solutions that
pick another element. Array selects are the only source of guard-gated
propagators since B53, and only the builder emits them, so the soundness
campaign cannot reach this; the mutation run (docs/soundness_coverage_plan.md
§P4) showed nothing else did either.

B66, found by this test on its first run: the select compiled `a[i]` as the
variable `base + i` without resolving `x == y` merges, so with `a1 == a2`
merged away the dropped element was never constrained (wrong models and a
wrong `sat`, with or without learning). The B46 alias-bypass class again.
"""
from __future__ import annotations

import ctypes
import itertools
import random

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, CompileUnsatError, CompileIncompleteError, SOLVE_OK, SOLVE_UNSAT, _SolveOpts
from dv_solve.problem import BIN_ADD, BIN_EQ, BIN_NEQ, BIN_LT, BIN_LTE, BIN_OR

N = 3                    # elements a[0..N-1], variables 0..N-1
IDX, R = N, N + 1
W = 3                    # element and result width
NAMES = [f"a{i}" for i in range(N)] + ["idx", "r"]
HI = [(1 << W) - 1] * N + [N - 1, (1 << W) - 1]

_OPS = {BIN_EQ: lambda x, y: x == y, BIN_NEQ: lambda x, y: x != y,
        BIN_LT: lambda x, y: x < y, BIN_LTE: lambda x, y: x <= y}


def _atom(rng):
    """(build(b), predicate(values), text) for one random comparison."""
    op = rng.choice(list(_OPS))
    x = rng.randrange(N + 2)
    shape = rng.randrange(3)
    if shape == 0:
        k = rng.randrange(HI[x] + 1)
        return (lambda b: b.expr_binary(op, b.expr_var(x), b.expr_const(k)),
                lambda v: _OPS[op](v[x], k), f"{NAMES[x]} {op} {k}")
    y = rng.choice([i for i in range(N + 2) if i != x])
    if shape == 1:
        return (lambda b: b.expr_binary(op, b.expr_var(x), b.expr_var(y)),
                lambda v: _OPS[op](v[x], v[y]), f"{NAMES[x]} {op} {NAMES[y]}")
    k = rng.randrange(2 * (1 << W))
    return (lambda b: b.expr_binary(op, b.expr_binary(BIN_ADD, b.expr_var(x), b.expr_var(y)),
                                    b.expr_const(k)),
            lambda v: _OPS[op](v[x] + v[y], k), f"{NAMES[x]}+{NAMES[y]} {op} {k}")


def _problem(seed):
    rng = random.Random(seed)
    cons = []
    for _ in range(rng.randrange(3, 7)):
        if rng.random() < 0.3:
            (b1, p1, t1), (b2, p2, t2) = _atom(rng), _atom(rng)
            cons.append((lambda b, b1=b1, b2=b2: b.expr_binary(BIN_OR, b1(b), b2(b)),
                         lambda v, p1=p1, p2=p2: p1(v) or p2(v), f"({t1}) || ({t2})"))
        else:
            cons.append(_atom(rng))
    return cons


def _truth(cons):
    for v in itertools.product(*(range(h + 1) for h in HI)):
        if v[R] == v[v[IDX]] and all(p(v) for _, p, _ in cons):
            return True
    return False


def _solve(cons, lcg, seed):
    b = SolveProblemBuilder()
    for i, h in enumerate(HI):
        b.add_var(i, width=W if i != IDX else 2, is_signed=False, lo=0, hi=h)
    b.add_constraint(b.expr_array_select(0, N, b.expr_var(R), b.expr_var(IDX)))
    for build, _, _ in cons:
        b.add_constraint(build(b))
    try:
        with SolveCtx(b.finalize()[0]) as ctx:
            rc = ctx._lib.dvs_solver_solve(ctx._ctx, ctypes.byref(
                _SolveOpts(seed=seed, use_lcg=lcg, time_limit_ms=10000)))
            if rc == SOLVE_UNSAT:
                return "unsat", None
            if rc != SOLVE_OK:
                return "unknown", None
            return "sat", tuple(ctx.get_value(i) for i in range(N + 2))
    except CompileUnsatError:
        return "unsat", None
    except CompileIncompleteError:
        return "unknown", None


@pytest.mark.parametrize("block", range(8))
def test_array_select_with_learning_matches_brute_force(block):
    for seed in range(block * 200, block * 200 + 200):
        cons = _problem(seed)
        sat = _truth(cons)
        text = " && ".join(t for _, _, t in cons)
        for lcg in (0, 1):
            for s in (1, 2, 3):
                ans, model = _solve(cons, lcg, s)
                if ans == "unknown":
                    continue
                assert ans == ("sat" if sat else "unsat"), \
                    f"problem {seed} lcg={lcg} seed={s}: got {ans}, expected sat={sat}: {text}"
                if model:
                    assert model[R] == model[model[IDX]] and all(p(model) for _, p, _ in cons), \
                        f"problem {seed} lcg={lcg}: bad model {model}: {text}"
