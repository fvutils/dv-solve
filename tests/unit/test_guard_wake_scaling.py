"""Waking a guard's propagators costs what the guard has, not the problem
(_wake_var in src/c/dvs_propagate.c).

Every bound change on a variable re-enqueued the propagators it guards by
scanning every propagator, so a search was quadratic in the problem's size:
a symbolic select `arr[i] == 7` over 2000 free elements spent 79% of its
time in that scan (0.55 s per pyvsc randomize, against 0.16 s for
Boolector). The guard index lists each variable's guarded propagators.

The second test checks the index follows the problem as it changes: a
guarded constraint added in a checkpoint scope after a solve is enforced,
and dropped again by the restore.
"""
from __future__ import annotations

import time

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_EQ, BIN_LT
from dv_solve.ctx import SolveCtx, SOLVE_OK


def _select(n):
    b = SolveProblemBuilder()
    try:
        for k in range(n):
            b.add_var(k, 8, False, 0, 255)
        b.add_var(n, 16, False, 0, 0xFFFF)
        v = b.expr_var
        b.add_constraint(b.expr_binary(BIN_LT, v(n), b.expr_const(n, width=16)))
        chain = v(n - 1)
        for k in range(n - 2, -1, -1):
            chain = b.expr_ite(
                b.expr_binary(BIN_EQ, v(n), b.expr_const(k, width=16)),
                v(k), chain)
        b.add_constraint(b.expr_binary(BIN_EQ, chain, b.expr_const(7, width=8)))
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    return buf


def test_large_select_is_not_quadratic():
    n = 4000
    ctx = SolveCtx(_select(n))
    try:
        ctx.reset()
        assert ctx.solve(seed=1, fair_pick=True) == SOLVE_OK
        t = time.perf_counter()
        for seed in (2, 3):
            ctx.reset()
            assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
            i = ctx.get_value(n)
            assert 0 <= i < n and ctx.get_value(i) == 7
        per = (time.perf_counter() - t) / 2
        # About 0.08 s with the index; the scan took 1.2 s here.
        assert per < 0.5, per
    finally:
        ctx.destroy()


def _small(n):
    b = SolveProblemBuilder()
    try:
        for k in range(n):
            b.add_var(k, 8, False, 0, 255)
        b.add_var(n, 8, False, 0, 255)
        v = b.expr_var
        b.add_constraint(b.expr_binary(BIN_LT, v(n), b.expr_const(n, width=8)))
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    return buf


def _scoped_select(n):
    """arr[i] == 9 as an ite chain, added in a scope over _small(n)."""
    b = SolveProblemBuilder()
    try:
        for k in range(n + 1):
            b.add_var(k, 8, False, 0, 255)
        v = b.expr_var
        chain = v(n - 1)
        for k in range(n - 2, -1, -1):
            chain = b.expr_ite(
                b.expr_binary(BIN_EQ, v(n), b.expr_const(k, width=8)),
                v(k), chain)
        b.add_constraint(b.expr_binary(BIN_EQ, chain, b.expr_const(9, width=8)))
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    return buf


def test_index_follows_scoped_adds():
    n = 40
    ctx = SolveCtx(_small(n))
    try:
        assert ctx.solve(seed=1, fair_pick=True) == SOLVE_OK
        cp = ctx.checkpoint()
        assert ctx.add_constraint(_scoped_select(n)) == 0
        for seed in (2, 3, 4):
            assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
            assert ctx.get_value(ctx.get_value(n)) == 9
        ctx.restore(cp)
        hits = 0
        for seed in range(5, 25):
            assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
            hits += ctx.get_value(ctx.get_value(n)) == 9
        assert hits < 5, hits
    finally:
        ctx.destroy()
