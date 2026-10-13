"""A problem the search gives up on is given up on quickly the next time
(_solver_solve in src/c/dvs_search.c).

DVS_SOLVE_TIMEOUT sends the caller to another engine. A randomize() loop
re-solves one problem, and a problem the search thrashes on paid the whole
default budget on every call. Once a default-budget solve has given up, later
solves of the same problem -- on the same context after reset(), or on a new
context compiled from the same problem -- stop after a small multiple of the
work any solve of it needed, so the caller escalates at once.
"""
from __future__ import annotations

import time

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_ADD, BIN_EQ, BIN_MUL
from dv_solve.ctx import SolveCtx, SOLVE_TIMEOUT

LIMIT_S = 0.3     # the default budget, shortened for the test


def _mul_sq():
    """a * b == c * c + 12345 over 16-bit fields: the search thrashes."""
    b = SolveProblemBuilder()
    try:
        for i in range(3):
            b.add_var(i, 16, False, 0, 0xFFFF)
        b.add_var(3, 32, False, 0, (1 << 32) - 1)
        b.add_var(4, 32, False, 0, (1 << 32) - 1)
        v = b.expr_var
        e = lambda i: b.expr_extend(v(i), 16, 32, False)
        b.add_constraint(b.expr_binary(BIN_EQ, v(3), b.expr_binary(BIN_MUL, e(0), e(1))))
        b.add_constraint(b.expr_binary(BIN_EQ, v(4), b.expr_binary(BIN_MUL, e(2), e(2))))
        b.add_constraint(b.expr_binary(
            BIN_EQ, v(3), b.expr_binary(BIN_ADD, v(4), b.expr_const(12345, width=32))))
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    return buf


def _timed(ctx, seed, **opts):
    ctx.reset()
    t = time.perf_counter()
    rc = ctx.solve(seed=seed, fair_pick=True, **opts)
    return rc, time.perf_counter() - t


def test_thrashing_problem_escalates_at_once(monkeypatch):
    monkeypatch.setenv("DV_CDCL_TIME_LIMIT", str(LIMIT_S))
    buf = _mul_sq()
    ctx = SolveCtx(buf)
    try:
        rc, t = _timed(ctx, 1)
        assert rc == SOLVE_TIMEOUT and t >= LIMIT_S * 0.9
        for seed in (2, 3, 4):
            rc, t = _timed(ctx, seed)
            assert rc == SOLVE_TIMEOUT and t < LIMIT_S / 3, (seed, t)
    finally:
        ctx.destroy()
    # A context compiled afresh from the same problem remembers too.
    ctx = SolveCtx(_mul_sq())
    try:
        rc, t = _timed(ctx, 5)
        assert rc == SOLVE_TIMEOUT and t < LIMIT_S / 3, t
        # A solve with a budget of its own is not cut short.
        rc, t = _timed(ctx, 6, time_limit_ms=int(LIMIT_S * 1000))
        assert rc == SOLVE_TIMEOUT and t >= LIMIT_S * 0.9, t
    finally:
        ctx.destroy()
