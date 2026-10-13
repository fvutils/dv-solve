"""Uniformity of a seeded fair_pick solve where the box tier is off.

Causes of skew found on pyvsc's F5 shape:

* A failed value decision split the domain around the value, and the values
  next to a run of infeasible ones collected the run's share (_redraw in
  src/c/dvs_search.c).
* `t == x & 127` did not narrow x, so with t pinned the search drew x at
  ~2/256 a try and fell back to splitting (_mrw_runmask in src/c/dvs_sv.c).
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


def test_pinned_mask_narrows_its_operand():
    # t == zext(b) & 0x7F with t pinned: b is t or t + 128, each half the time.
    def build(b):
        b.add_var(0, 8, False, 0, 255)
        b.add_var(1, 32, False, 0, (1 << 32) - 1)
        c32 = lambda k: b.expr_const(k, width=32)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1), b.expr_binary(
            BIN_BAND, b.expr_extend(b.expr_var(0), 8, 32, False), c32(0x7F))))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1), c32(33)))
    ctx = _ctx(build)
    seen = collections.Counter()
    try:
        for s in range(1, 2001):
            ctx.reset()
            # A budget the enumerating search could not meet.
            assert ctx.solve(seed=s, fair_pick=True, max_conflicts=10,
                             max_restarts=1) == SOLVE_OK
            seen[ctx.get_value(0)] += 1
    finally:
        ctx.destroy()
    assert set(seen) == {33, 161}
    assert abs(seen[33] / 2000 - 0.5) < 0.05


def test_mid_run_mask_narrows_its_operand():
    # t == zext(x) & 0xFF00 (a run not at bit 0) with t pinned: x's high byte
    # is fixed and its low byte free.
    def build(b):
        b.add_var(0, 16, False, 0, 0xFFFF)
        b.add_var(1, 32, False, 0, (1 << 32) - 1)
        c32 = lambda k: b.expr_const(k, width=32)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1), b.expr_binary(
            BIN_BAND, b.expr_extend(b.expr_var(0), 16, 32, False), c32(0xFF00))))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1), c32(0x5A00)))
    draws = _draws(build, 8000, [0])
    assert all(x >> 8 == 0x5A for (x,) in draws)
    # The low byte is drawn, uniformly.
    assert _chi2(draws, [(0x5A00 | lo,) for lo in range(256)]) < CHI2_256


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
