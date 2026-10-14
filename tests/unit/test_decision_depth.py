"""A search may decide more than 256 variables (_solver_solve_core in
src/c/dvs_search.c).

The search pushes one decision level per decided variable, and its depth was
capped at a fixed 256 levels above the root. A problem with more decision
variables bailed with DVS_SOLVE_TIMEOUT whatever its constraints: a
symbolic select `arr[i] == 7` over 300 elements frees every element, and
the caller fell back to the bit-blasting engine (about 100x slower at 1024
elements). A growable context now sizes the depth to the problem.
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_EQ, BIN_LT
from dv_solve.ctx import SolveCtx, SOLVE_OK


def _select(n):
    """arr[i] == 7 with i < n, as an ite chain over n free 8-bit elements."""
    b = SolveProblemBuilder()
    try:
        for k in range(n):
            b.add_var(k, 8, False, 0, 255)
        idx = n
        b.add_var(idx, 16, False, 0, 0xFFFF)
        v = b.expr_var
        b.add_constraint(b.expr_binary(BIN_LT, v(idx), b.expr_const(n, width=16)))
        chain = v(n - 1)
        for k in range(n - 2, -1, -1):
            chain = b.expr_ite(
                b.expr_binary(BIN_EQ, v(idx), b.expr_const(k, width=16)),
                v(k), chain)
        b.add_constraint(b.expr_binary(BIN_EQ, chain, b.expr_const(7, width=8)))
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    return buf


@pytest.mark.parametrize("n", [300, 1024])
def test_select_over_many_free_elements(n):
    ctx = SolveCtx(_select(n))
    try:
        for seed in (1, 2, 3):
            ctx.reset()
            assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK, seed
            i = ctx.get_value(n)
            assert 0 <= i < n and ctx.get_value(i) == 7
    finally:
        ctx.destroy()
