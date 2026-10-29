"""Variable kinds (dvs_builder_set_var_kind): RAND, AUX, RANDC, STATE.

The kind is stored in the problem. Until dvs_randomize() reads it, RANDC and
STATE solve like RAND: a STATE var nothing pins is free.
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import (BIN_EQ, BIN_LT, VAR_AUX, VAR_RAND, VAR_RANDC,
                              VAR_STATE)
from dv_solve.ctx import SolveCtx, SOLVE_OK


def _values(kind, n=1500):
    b = SolveProblemBuilder()
    try:
        b.add_var(0, 4, False, 0, 15)
        b.add_var(1, 4, False, 0, 15)
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_var(1)))
        b.set_var_kind(0, kind)
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    ctx = SolveCtx(buf)
    seen = set()
    try:
        for s in range(1, n + 1):
            ctx.reset()
            assert ctx.solve(seed=s, fair_pick=True) == SOLVE_OK
            x, y = ctx.get_value(0), ctx.get_value(1)
            assert x < y
            seen.add(x)
    finally:
        ctx.destroy()
    return seen


@pytest.mark.parametrize("kind", [VAR_RAND, VAR_RANDC, VAR_STATE])
def test_unpinned_kinds_solve_as_rand(kind):
    assert _values(kind) == set(range(15))


def test_aux_follows_from_constraints():
    b = SolveProblemBuilder()
    try:
        b.add_var(0, 4, False, 0, 15)
        b.add_var(1, 4, False, 0, 15)
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1), b.expr_var(0)))
        b.set_var_kind(1, VAR_AUX)
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    ctx = SolveCtx(buf)
    try:
        for s in range(1, 30):
            ctx.reset()
            assert ctx.solve(seed=s, fair_pick=True) == SOLVE_OK
            assert ctx.get_value(1) == ctx.get_value(0)
    finally:
        ctx.destroy()


def test_bad_kind_or_var_is_an_error():
    b = SolveProblemBuilder()
    try:
        b.add_var(0, 8, False, 0, 255)
        with pytest.raises(ValueError):
            b.set_var_kind(5, VAR_STATE)
        with pytest.raises(ValueError):
            b.set_var_kind(0, 4)
        with pytest.raises(ValueError):
            b.set_var_kind(0, -1)
        b.set_var_kind(0, VAR_RANDC)
    finally:
        b.destroy()
