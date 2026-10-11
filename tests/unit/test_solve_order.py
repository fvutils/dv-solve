"""Solve order (SV `solve a before b`): dvs_builder_set_var_order.

A seeded fair_pick solve is uniform over solutions. A solve order asks for a
different distribution: the earlier stage is chosen first, over the values
that leave the constraints satisfiable, and the rest given it. IEEE 1800's
example: with `s -> d == 0` (s 1 bit, d 32 bits), uniform over solutions makes
s==1 a 1-in-2^32 event; `solve s before d` makes it one in two.
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_EQ, BIN_LTE, BIN_LT, BIN_NEQ, BIN_OR
from dv_solve.ctx import SolveCtx, SOLVE_OK


def _ctx(build):
    b = SolveProblemBuilder()
    try:
        vids = build(b)
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    return SolveCtx(buf), vids


def _draws(build, n, first_seed=1):
    ctx, vids = _ctx(build)
    out = []
    try:
        for s in range(first_seed, first_seed + n):
            ctx.reset()
            assert ctx.solve(seed=s, fair_pick=True) == SOLVE_OK
            out.append(tuple(ctx.get_value(v) for v in vids))
    finally:
        ctx.destroy()
    return out


def _implies(b, not_cond, then):
    # cond -> then, given !cond: !cond || then
    return b.expr_binary(BIN_OR, not_cond, then)


def _s_implies_d0(ordered):
    def build(b):
        b.add_var(0, 1, False, 0, 1)                 # s
        b.add_var(1, 32, False, 0, 0xFFFFFFFF)       # d
        s, d = b.expr_var(0), b.expr_var(1)
        b.add_constraint(_implies(
            b, b.expr_binary(BIN_NEQ, s, b.expr_const(1)),
            b.expr_binary(BIN_EQ, d, b.expr_const(0))))
        if ordered:
            b.set_var_order(0, 1)
        return [0, 1]
    return build


def test_lrm_example_unordered_is_uniform_over_solutions():
    draws = _draws(_s_implies_d0(False), 2000)
    assert sum(s for s, _ in draws) < 10


def test_lrm_example_solve_s_before_d():
    draws = _draws(_s_implies_d0(True), 2000)
    assert all(d == 0 for s, d in draws if s == 1)
    n1 = sum(s for s, _ in draws)
    assert 850 < n1 < 1150


def _wide_before_narrow(b):
    # a (8 bits) before b (1 bit), a != 0 -> b == 0. Smallest-domain-first
    # would decide b first: b == 1 half the time, which forces a == 0. With
    # the order, a is uniform over its 256 values.
    b.add_var(0, 8, False, 0, 255)
    b.add_var(1, 1, False, 0, 1)
    a, bb = b.expr_var(0), b.expr_var(1)
    b.add_constraint(_implies(
        b, b.expr_binary(BIN_EQ, a, b.expr_const(0)),
        b.expr_binary(BIN_EQ, bb, b.expr_const(0))))
    b.set_var_order(0, 1)
    return [0, 1]


def test_order_overrides_smallest_domain_first():
    n = 8000
    draws = _draws(_wide_before_narrow, n)
    assert all(bb == 0 for a, bb in draws if a != 0)
    counts = [0] * 256
    for a, _ in draws:
        counts[a] += 1
    assert counts[0] < 0.02 * n
    exp = n / 256
    chi2 = sum((c - exp) ** 2 / exp for c in counts)
    assert chi2 < 340          # 255 dof, p ~ 3e-4


def test_last_stage_is_uniform_given_the_earlier():
    # m (stage 1) in [0:3]; x <= y over [0:9] unordered. m is uniform, and
    # (x, y) is uniform over the 55 legal cells.
    def build(b):
        b.add_var(0, 2, False, 0, 3)
        b.add_var(1, 8, False, 0, 9)
        b.add_var(2, 8, False, 0, 9)
        b.add_constraint(b.expr_binary(BIN_LTE, b.expr_var(1), b.expr_var(2)))
        b.set_var_order(0, 1)
        return [0, 1, 2]
    n = 8000
    draws = _draws(build, n)
    m_counts = [0] * 4
    cells = {}
    for m, x, y in draws:
        assert x <= y
        m_counts[m] += 1
        cells[(x, y)] = cells.get((x, y), 0) + 1
    assert all(abs(c - n / 4) < 0.05 * n for c in m_counts)
    exp = n / 55
    chi2 = sum((cells.get((x, y), 0) - exp) ** 2 / exp
               for x in range(10) for y in range(10) if x <= y)
    assert chi2 < 99           # 54 dof


def test_stages_in_sequence():
    # c before b before a, with a < b < c on [0:7]: c is uniform over its
    # feasible values 2..7, then b over 1..c-1, then a over 0..b-1.
    def build(b):
        for i in range(3):
            b.add_var(i, 3, False, 0, 7)
        a, bb, c = b.expr_var(0), b.expr_var(1), b.expr_var(2)
        b.add_constraint(b.expr_binary(BIN_LT, a, bb))
        b.add_constraint(b.expr_binary(BIN_LT, bb, c))
        b.set_var_order(2, 1)
        b.set_var_order(1, 2)
        return [0, 1, 2]
    n = 6000
    draws = _draws(build, n)
    c_counts = {}
    for a, bb, c in draws:
        assert a < bb < c
        c_counts[c] = c_counts.get(c, 0) + 1
    assert set(c_counts) == set(range(2, 8))
    assert all(abs(k - n / 6) < 0.15 * n / 6 for k in c_counts.values())


def test_reused_context_matches_fresh():
    reused = _draws(_wide_before_narrow, 40, first_seed=11)
    fresh = []
    for s in range(11, 51):
        ctx, vids = _ctx(_wide_before_narrow)
        try:
            assert ctx.solve(seed=s, fair_pick=True) == SOLVE_OK
            fresh.append(tuple(ctx.get_value(v) for v in vids))
        finally:
            ctx.destroy()
    assert reused == fresh


def test_undeclared_variable_is_an_error():
    b = SolveProblemBuilder()
    try:
        b.add_var(0, 8, False, 0, 255)
        with pytest.raises(ValueError):
            b.set_var_order(5, 1)
        with pytest.raises(ValueError):
            b.set_var_order(0, 300)
    finally:
        b.destroy()
