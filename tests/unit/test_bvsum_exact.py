"""Exact backward bounds through an unsigned add that cannot carry (A4b in
docs/accuracy_closure_plan.md).

`zext(a) + zext(b) + zext(c) == K` used to narrow only the operands' lower
bounds: the modular backward step r - b wrapped as an interval and was
skipped. _bvsum_exact_back (dvs_prop_templates.c) now sets the integer
bounds when the sum cannot carry, and _explain_bvsum_lifted explains them.

Random small problems, judged by brute force: sums of zero-extended
variables of mixed widths, some of whose sum widths can carry, under `==`,
`<=` or `>=` a constant, plus a few side constraints so the search
conflicts and learns. Every verdict must match the enumeration, and every
model must satisfy the constraints. Solves run with and without clause
learning, since a learnt clause is where a bound's explanation is used.
"""
from __future__ import annotations

import itertools
import random

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, SOLVE_OK, SOLVE_UNSAT, CompileUnsatError
from dv_solve.problem import BIN_ADD, BIN_EQ, BIN_LTE, BIN_GTE, BIN_LT, BIN_NEQ, BIN_OR


def _problem(rng):
    n = rng.choice([2, 3, 4])
    widths = [rng.randint(2, 5) for _ in range(n)]
    # Some sums can carry. At sw == max(widths) an operand is as wide as the
    # sum, so only its current bound (from a decision or a guarded bound
    # below) rules a carry out: the case an explanation must cite.
    sw = rng.choice([max(widths), max(widths), max(widths) + 1, max(widths) + 2, 8, 16])
    k = rng.randrange(1 << sw)
    rel = rng.choice([BIN_EQ, BIN_EQ, BIN_LTE, BIN_GTE])
    side = []
    for _ in range(rng.randint(0, 3)):
        i, j = rng.sample(range(n), 2)
        side.append((rng.choice([BIN_LT, BIN_NEQ, BIN_LTE]), i, j))
    guards = []          # (a < c1) or (b <= c2): a bound that holds on some branches
    for _ in range(rng.randint(0, 2)):
        i, j = rng.sample(range(n), 2)
        guards.append((i, rng.randrange(1 << widths[i]), j, rng.randrange(1 << widths[j])))
    pins = []
    if rng.random() < 0.3:
        i = rng.randrange(n)
        pins.append((i, rng.randrange(1 << widths[i])))
    return widths, sw, k, rel, side, guards, pins


def _holds(m, widths, sw, k, rel, side, guards, pins):
    s = sum(m) & ((1 << sw) - 1)
    ok = {BIN_EQ: s == k, BIN_LTE: s <= k, BIN_GTE: s >= k}[rel]
    for op, i, j in side:
        ok = ok and {BIN_LT: m[i] < m[j], BIN_NEQ: m[i] != m[j], BIN_LTE: m[i] <= m[j]}[op]
    ok = ok and all(m[i] < c1 or m[j] <= c2 for i, c1, j, c2 in guards)
    return ok and all(m[i] == v for i, v in pins)


def _build(widths, sw, k, rel, side, guards, pins):
    b = SolveProblemBuilder()
    try:
        for i, w in enumerate(widths):
            b.add_var(i, width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
        def ext(i):
            v = b.expr_var(i)
            return v if widths[i] == sw else b.expr_extend(v, widths[i], sw, False)
        acc = ext(0)
        for i in range(1, len(widths)):
            acc = b.expr_binary(BIN_ADD, acc, ext(i))
        b.add_constraint(b.expr_binary(rel, acc, b.expr_const(k, width=sw)))
        for op, i, j in side:
            b.add_constraint(b.expr_binary(op, b.expr_var(i), b.expr_var(j)))
        for i, c1, j, c2 in guards:
            b.add_constraint(b.expr_binary(
                BIN_OR, b.expr_binary(BIN_LT, b.expr_var(i), b.expr_const(c1, width=widths[i])),
                b.expr_binary(BIN_LTE, b.expr_var(j), b.expr_const(c2, width=widths[j]))))
        for i, v in pins:
            b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(i), b.expr_const(v, width=widths[i])))
        buf, _ = b.finalize()
    finally:
        b.destroy()
    return buf


# BVSUM_N=k multiplies the sweep (8 chunks by default).
_CHUNKS = 8 * int(__import__("os").environ.get("BVSUM_N", "1"))


@pytest.mark.parametrize("chunk", range(_CHUNKS))
def test_bvsum_matches_enumeration(chunk):
    rng = random.Random(0xA4B0 + chunk)
    for _ in range(300):
        p = _problem(rng)
        widths = p[0]
        sat = any(_holds(m, *p) for m in itertools.product(*(range(1 << w) for w in widths)))
        try:
            ctx = SolveCtx(_build(*p))
        except CompileUnsatError:        # unsat found while compiling
            assert not sat, f"wrong compile-time unsat: {p}"
            continue
        with ctx:
            # use_lcg: the SMT-LIB2 front end learns clauses, so a bound's
            # explanation is used there (the API default does not learn).
            for seed, lcg in ((0, False), (1, False), (0, True), (1, True), (2, True)):
                ctx.reset()
                rc = ctx.solve(seed=seed, fair_pick=bool(seed), use_lcg=lcg)
                if sat:
                    assert rc == SOLVE_OK, f"wrong unsat: {p} seed={seed} lcg={lcg}"
                    m = [ctx.get_value(i) & ((1 << w) - 1) for i, w in enumerate(widths)]
                    assert _holds(m, *p), f"wrong model {m}: {p} seed={seed} lcg={lcg}"
                else:
                    assert rc == SOLVE_UNSAT, f"wrong sat (rc={rc}): {p} seed={seed} lcg={lcg}"
