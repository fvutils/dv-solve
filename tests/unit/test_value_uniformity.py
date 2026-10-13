"""Uniformity of a seeded fair_pick solve where the box tier is off.

A failed value decision split the domain around the value, and the values
next to a run of infeasible ones collected the run's share (_redraw in
src/c/dvs_search.c). Found on pyvsc's F5 shape.
"""
from __future__ import annotations

import collections

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_BAND, BIN_EQ, BIN_SUB
from dv_solve.ctx import SolveCtx, SOLVE_OK

# 255 degrees of freedom: chi-square mean 255, sd 22.6; p ~ 1e-4 at 345.
CHI2_256 = 345


def _ctx(build):
    b = SolveProblemBuilder()
    try:
        build(b)
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    return SolveCtx(buf)


def _draws(build, n, vids):
    ctx = _ctx(build)
    out = collections.Counter()
    try:
        for s in range(1, n + 1):
            ctx.reset()
            assert ctx.solve(seed=s, fair_pick=True) == SOLVE_OK
            out[tuple(ctx.get_value(v) for v in vids)] += 1
    finally:
        ctx.destroy()
    return out


def _chi2(counts, cells):
    n = sum(counts.values())
    exp = n / len(cells)
    return sum((counts.get(c, 0) - exp) ** 2 / exp for c in cells)


def test_gap_in_a_domain_keeps_its_share():
    # zext(s) in [3:130]: s is {3..127} or {-128..-126}. A split around a
    # failed s handed the run -125..2 to its neighbours; drawn again, each
    # feasible s keeps 1/128.
    def build(b):
        b.add_var(0, 8, True, -128, 127)
        b.add_var(1, 32, False, 3, 130)
        b.add_constraint(b.expr_binary(
            BIN_EQ, b.expr_var(1), b.expr_extend(b.expr_var(0), 8, 32, False)))
    n = 12800
    draws = _draws(build, n, [0])
    neg = sum(c for (s,), c in draws.items() if s < 0) / n
    assert abs(neg - 3 / 128) < 0.008
