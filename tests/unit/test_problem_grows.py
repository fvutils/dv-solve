"""SolveProblem grows instead of overflowing (docs/dynamic_pool_plan.md P4).

It used to be a fixed 64 KiB buffer: a bigger problem got EXPR_NULL back
from the builder calls and compiled without the constraints that did not fit.
It now moves to a bigger buffer when a call does not fit -- the problem is
offset-addressed, so the copy is exact -- also after a context compiled it
(a context reads a problem only during compile and add).
"""
import ctypes

import pytest

from dv_solve.ctx import SolveCtx, SOLVE_OK
from dv_solve.problem import (BIN_ADD, BIN_EQ, EXPR_NULL, SolveProblem,
                              _ProblemHeader)

N = 1000


def _build(sp):
    """v[i+1] == v[i] + 1: one decision fixes them all."""
    for v in range(N):
        assert sp.add_var(v, 16, False, 0, 60000) != EXPR_NULL
    for v in range(N - 1):
        r = sp.add_constraint(sp.expr_binary(
            BIN_EQ, sp.expr_var(v + 1),
            sp.expr_binary(BIN_ADD, sp.expr_var(v), sp.expr_const(1))))
        assert r != EXPR_NULL


def test_layout_matches_c():
    sp = SolveProblem(4096)
    hdr = _ProblemHeader.from_buffer(sp._buf)
    assert hdr.pool_capacity == 4096 - ctypes.sizeof(_ProblemHeader)
    assert hdr.pool_overflow == 0


def test_large_problem_grows_and_solves():
    sp = SolveProblem()
    _build(sp)                       # far past 64 KiB
    assert len(sp._buf) > 65536
    with SolveCtx(sp) as ctx:
        assert ctx.solve(seed=1) == SOLVE_OK
        vals = [ctx.get_value(v) for v in range(N)]
    assert vals == list(range(vals[0], vals[0] + N))


def test_compiled_problem_can_grow():
    """A context reads the problem only while compiling it: the problem may
    move afterwards, and the context's model still validates against it."""
    sp = SolveProblem(4096)
    sp.add_var(0, 8, False, 0, 10)
    sp.add_var(1, 8, False, 0, 10)
    sp.add_constraint(sp.expr_binary(BIN_EQ, sp.expr_var(0),
                                     sp.expr_binary(BIN_ADD, sp.expr_var(1), sp.expr_const(1))))
    with SolveCtx(sp) as ctx:
        assert ctx.solve(seed=1) == SOLVE_OK
        a, b = ctx.get_value(0), ctx.get_value(1)
        old = ctypes.addressof(sp._buf)
        for v in range(2, 2000):
            assert sp.add_var(v, 8, False, 0, 10) != EXPR_NULL
        assert ctypes.addressof(sp._buf) != old          # it moved
        assert ctx.solve(seed=1) == SOLVE_OK
        assert ctx.get_value(0) == ctx.get_value(1) + 1
        assert (a, b) == (a, a - 1)
