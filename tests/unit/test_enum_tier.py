"""The enumeration tier (_enum_sample in src/c/dvs_search.c).

Where the box tier is off -- solutions are a small part of the root box --
the search decided one variable at a time, so a solution's probability went
as 1 / the solutions next to it. The tier lists every solution once, by a
depth-first walk with propagation, and a solve pins one drawn uniformly.
"""
from __future__ import annotations

import collections
import itertools
import random
import time

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import (BIN_BAND, BIN_BOR, BIN_BXOR, BIN_EQ, BIN_GTE,
                              BIN_LTE, BIN_MUL)
from dv_solve.ctx import SolveCtx, SOLVE_OK, SOLVE_UNSAT, CompileUnsatError


def _build(widths, constrain, bounds=None):
    b = SolveProblemBuilder()
    try:
        for i, w in enumerate(widths):
            lo, hi = bounds[i] if bounds else (0, (1 << w) - 1)
            b.add_var(i, w, False, lo, hi)
        constrain(b)
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    return buf


def _ext(b, i, frm, to):
    return b.expr_extend(b.expr_var(i), frm, to, False)


def _draws(buf, n, nv):
    ctx = SolveCtx(buf)
    seen = collections.Counter()
    try:
        for s in range(1, n + 1):
            ctx.reset()
            assert ctx.solve(seed=s, fair_pick=True) == SOLVE_OK
            seen[tuple(ctx.get_value(i) for i in range(nv))] += 1
    finally:
        ctx.destroy()
    return seen


def _chi2_df(seen, sols, n):
    e = n / len(sols)
    return sum((seen[s] - e) ** 2 / e for s in sols) / (len(sols) - 1)


def _prod_range(lo, hi):
    def c(b):
        for op, k in ((BIN_GTE, lo), (BIN_LTE, hi)):
            p = b.expr_binary(BIN_MUL, _ext(b, 0, 8, 16), _ext(b, 1, 8, 16))
            b.add_constraint(b.expr_binary(op, p, b.expr_const(k, width=16)))
    return _build([8, 8], c)


def test_product_in_a_range_uniform():
    """1000 <= a * b <= 1100 over u8: 418 solutions, 0.6% of the box. Pairs
    with a small operand were drawn up to 1.4x as often as balanced ones
    (chi^2/df 2.1)."""
    sols = {(x, y) for x in range(256) for y in range(256) if 1000 <= x * y <= 1100}
    n = 20 * len(sols)
    seen = _draws(_prod_range(1000, 1100), n, 2)
    assert set(seen) == sols
    assert _chi2_df(seen, sols, n) < 1.3


def test_product_equals_product_uniform():
    """a * b == c * d over [1, 15]: 0.6% of the box; 98 of the 5-bit
    version's 3,631 solutions were never drawn (chi^2/df 2.6)."""
    def c(b):
        b.add_constraint(b.expr_binary(
            BIN_EQ, b.expr_binary(BIN_MUL, _ext(b, 0, 4, 8), _ext(b, 1, 4, 8)),
            b.expr_binary(BIN_MUL, _ext(b, 2, 4, 8), _ext(b, 3, 4, 8))))
    sols = {(a, bb, cc, d) for a, bb, cc, d in itertools.product(range(1, 16), repeat=4)
            if a * bb == cc * d}
    n = 20 * len(sols)
    seen = _draws(_build([4] * 4, c, [(1, 15)] * 4), n, 4)
    assert set(seen) == sols
    assert _chi2_df(seen, sols, n) < 1.3


def test_reused_context_matches_fresh_ones():
    """The list depends on the problem only, and a draw on the solve's seed:
    a context solved again gives what fresh contexts give."""
    buf = _prod_range(1000, 1100)
    ctx = SolveCtx(buf)
    reused = []
    try:
        for s in range(1, 21):
            ctx.reset()
            assert ctx.solve(seed=s, fair_pick=True) == SOLVE_OK
            reused.append((ctx.get_value(0), ctx.get_value(1)))
    finally:
        ctx.destroy()
    fresh = []
    for s in range(1, 21):
        c = SolveCtx(_prod_range(1000, 1100))
        try:
            assert c.solve(seed=s, fair_pick=True) == SOLVE_OK
            fresh.append((c.get_value(0), c.get_value(1)))
        finally:
            c.destroy()
    assert reused == fresh


def test_failed_walk_is_paid_once_per_problem():
    """a * b == c * d over u8 has too many solutions to list. The walk gives
    up within its budget, and fresh contexts of the same problem do not walk
    again (the Verilator front end compiles one per solve)."""
    def build():
        def c(b):
            b.add_constraint(b.expr_binary(
                BIN_EQ, b.expr_binary(BIN_MUL, _ext(b, 0, 8, 16), _ext(b, 1, 8, 16)),
                b.expr_binary(BIN_MUL, _ext(b, 2, 8, 16), _ext(b, 3, 8, 16))))
        return _build([8] * 4, c, [(1, 255)] * 4)
    ts = []
    for s in range(1, 12):
        c = SolveCtx(build())
        try:
            t = time.perf_counter()
            assert c.solve(seed=s, fair_pick=True) == SOLVE_OK
            ts.append(time.perf_counter() - t)
            a, b, cc, d = (c.get_value(i) for i in range(4))
            assert a * b == cc * d
        finally:
            c.destroy()
    later = sorted(ts[1:])
    assert later[len(later) // 2] < 0.002, ts


_OPS = {BIN_BAND: lambda x, y: x & y, BIN_BOR: lambda x, y: x | y,
        BIN_BXOR: lambda x, y: x ^ y}


def test_matches_brute_force():
    """Random 5-bit problems over &, |, ^ and products (equality, range,
    product-equality, product-range): the verdict is the brute-force one,
    every model is a solution, and a small solution set is covered."""
    rng = random.Random(7)
    W, WW = 5, 12
    for trial in range(300):
        op = rng.choice(list(_OPS))
        f = _OPS[op]
        ranges = []
        for _ in range(2):
            lo = rng.randrange(0, 32)
            ranges.append((lo, rng.randrange(lo, 32)))
        shape = rng.choice(["eq", "range", "prod_eq", "prod_range"])
        k = rng.randrange(0, 32)
        k2 = rng.randrange(k, 64)
        K = rng.randrange(1, 600)

        def c(b):
            def r():
                return b.expr_binary(op, _ext(b, 0, W, WW), _ext(b, 1, W, WW))
            def p():
                return b.expr_binary(BIN_MUL, _ext(b, 0, W, WW), r())
            kc = lambda v: b.expr_const(v, width=WW)
            if shape == "eq":
                b.add_constraint(b.expr_binary(BIN_EQ, r(), kc(k)))
            elif shape == "range":
                b.add_constraint(b.expr_binary(BIN_GTE, r(), kc(k)))
                b.add_constraint(b.expr_binary(BIN_LTE, r(), kc(k2)))
            elif shape == "prod_eq":
                b.add_constraint(b.expr_binary(BIN_EQ, p(), kc(K)))
            else:
                b.add_constraint(b.expr_binary(BIN_GTE, p(), kc(K)))
                b.add_constraint(b.expr_binary(BIN_LTE, p(), kc(K + 40)))

        def holds(x, y):
            rv = f(x, y)
            if shape == "eq":
                return rv == k
            if shape == "range":
                return k <= rv <= k2
            pv = (x * rv) & ((1 << WW) - 1)
            return pv == K if shape == "prod_eq" else K <= pv <= K + 40
        sols = {(x, y) for x in range(ranges[0][0], ranges[0][1] + 1)
                for y in range(ranges[1][0], ranges[1][1] + 1) if holds(x, y)}
        try:
            ctx = SolveCtx(_build([W, W], c, ranges))
        except CompileUnsatError:
            assert not sols, (trial, shape)
            continue
        n = 40 * len(sols) if len(sols) <= 20 else 4
        seen = set()
        try:
            for s in range(1, max(n, 4) + 1):
                ctx.reset()
                rc = ctx.solve(seed=s, fair_pick=True, time_limit_ms=2000)
                if not sols:
                    assert rc == SOLVE_UNSAT, (trial, shape, rc)
                    break
                assert rc == SOLVE_OK, (trial, shape, rc)
                v = (ctx.get_value(0), ctx.get_value(1))
                assert v in sols, (trial, shape, v)
                seen.add(v)
        finally:
            ctx.destroy()
        if sols and len(sols) <= 20:
            assert seen == sols, (trial, shape, ranges, sorted(sols - seen))
