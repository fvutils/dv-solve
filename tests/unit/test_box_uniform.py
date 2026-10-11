"""The box-rejection tier: a seeded fair_pick solve is uniform over solutions.

The search draws one variable at a time, each uniformly from its domain as
propagated so far. Under a relation between variables that is not uniform over
solutions: with `x <= y` on [0:9], (0,0) and (9,9) came up 5.5% of the time
instead of 1/55 = 1.8% (the SyoSil uniformity test). The tier draws every
variable at once from the root domains and keeps the draw only if it is a
solution, which is exactly uniform. See _box_sample in src/c/dvs_search.c.
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_ADD, BIN_EQ, BIN_LT, BIN_LTE
from dv_solve.ctx import SolveCtx, SOLVE_OK


def _ctx(build):
    b = SolveProblemBuilder()
    try:
        vids = build(b)
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    return SolveCtx(buf), vids


def _draws(build, n, seq_pick=False, first_seed=1):
    ctx, vids = _ctx(build)
    out = []
    try:
        for s in range(first_seed, first_seed + n):
            ctx.reset()
            assert ctx.solve(seed=s, fair_pick=True, seq_pick=seq_pick) == SOLVE_OK
            out.append(tuple(ctx.get_value(v) for v in vids))
    finally:
        ctx.destroy()
    return out


def _grid(n=10, extra_sum=False):
    """x, y in [0:n-1], x <= y; optionally z == x + y (a defined variable)."""
    def build(b):
        b.add_var(0, 8, False, 0, n - 1)
        b.add_var(1, 8, False, 0, n - 1)
        x, y = b.expr_var(0), b.expr_var(1)
        b.add_constraint(b.expr_binary(BIN_LTE, x, y))
        if extra_sum:
            b.add_var(2, 8, False, 0, 255)
            b.add_constraint(b.expr_binary(
                BIN_EQ, b.expr_var(2), b.expr_binary(BIN_ADD, x, y)))
            return [0, 1, 2]
        return [0, 1]
    return build


def _chi2(draws, cells):
    exp = len(draws) / len(cells)
    counts = {c: 0 for c in cells}
    for d in draws:
        counts[d] += 1
    return sum((c - exp) ** 2 / exp for c in counts.values())


GRID_CELLS = [(x, y) for x in range(10) for y in range(10) if x <= y]


def test_grid_uniform_over_solutions():
    draws = _draws(_grid(), 6000)
    assert all(x <= y for x, y in draws)
    # 54 degrees of freedom: p = 1e-4 at about 99. The sequential draw
    # scores ~2000.
    assert _chi2(draws, GRID_CELLS) < 99


def test_seq_pick_keeps_the_sequential_draw():
    # seq_pick is for a solve order, whose distribution is the sequence: the
    # tier stays out, and the corner cells come out ~3x their share.
    draws = _draws(_grid(), 6000, seq_pick=True)
    assert _chi2(draws, GRID_CELLS) > 300


def test_defined_variable_is_not_drawn():
    # z == x + y is fixed by x and y; drawing it too would almost never agree
    # and the tier would turn itself off.
    draws = _draws(_grid(extra_sum=True), 6000)
    assert all(z == x + y for x, y, z in draws)
    assert _chi2([(x, y) for x, y, _ in draws], GRID_CELLS) < 99


def test_a_lt_b_marginal():
    def build(b):
        b.add_var(0, 8, False, 0, 255)
        b.add_var(1, 8, False, 0, 255)
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_var(1)))
        return [0, 1]
    draws = _draws(build, 8000)
    mean_a = sum(a for a, _ in draws) / len(draws)
    # Uniform over solutions: P(a=k) ~ 255-k, mean 85. Sequential: ~95.
    assert abs(mean_a - 85.0) < 3.0


def test_low_acceptance_problem_still_solves():
    # x + y == 200 over 8 bits: 1 in 256 box draws is a solution, so the probe
    # turns the tier off and the search answers.
    def build(b):
        b.add_var(0, 9, False, 0, 255)
        b.add_var(1, 9, False, 0, 255)
        b.add_constraint(b.expr_binary(
            BIN_EQ, b.expr_binary(BIN_ADD, b.expr_var(0), b.expr_var(1)),
            b.expr_const(200)))
        return [0, 1]
    draws = _draws(build, 300)
    assert all(x + y == 200 for x, y in draws)
    assert len({x for x, _ in draws}) > 100


@pytest.mark.parametrize("build", [_grid(), _grid(extra_sum=True)])
def test_reused_context_matches_fresh(build):
    # Whether the tier runs is decided by a fixed-seed probe, so a reused
    # context (reset + solve) gives the same values as a fresh one.
    reused = _draws(build, 50, first_seed=7)
    fresh = []
    for s in range(7, 57):
        ctx, vids = _ctx(build)
        try:
            assert ctx.solve(seed=s, fair_pick=True) == SOLVE_OK
            fresh.append(tuple(ctx.get_value(v) for v in vids))
        finally:
            ctx.destroy()
    assert reused == fresh
