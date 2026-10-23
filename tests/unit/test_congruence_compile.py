"""Compile-time congruence facts (enhancement plan item 3a).

`_compile_congruence_facts` (dvs_compile.c) collects `x % m == r` facts
with constant m, r per (alias-resolved) variable and CRT-merges them.
A direct contradiction between two such facts on the same variable is
DVS_COMPILE_UNSAT; consistent facts (as in xezim's `mod_crt` fixture:
x%7==3, x%11==5, x%13==2) compile and solve as before.

This only covers facts collected directly on one variable: carrying a
fact across a link like `y == x + c` is item 3b, not this pass -- see
`test_mod_unsat_is_unsat` in test_xezim_regressions.py, still xfail.
"""
from __future__ import annotations

import os
from pathlib import Path

import pytest

dv = pytest.importorskip("dv_solve")

from dv_solve import problem as P
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SOLVE_OK, SOLVE_UNSAT, SolveCtx, CompileUnsatError

_REPO = Path(__file__).resolve().parents[2]
if not os.environ.get("DVS_SOLVER_PATH"):
    _lib_dir = _REPO / "build-prof" / "lib"
    if (_lib_dir / "libdv_solve.so").exists():
        os.environ["DVS_SOLVER_PATH"] = str(_lib_dir)


def _mod_problem(facts):
    """A 32-bit unsigned var 0 with `x % m == r` for each (m, r) in facts."""
    bld = SolveProblemBuilder()
    bld.add_var(0, width=32, is_signed=False, lo=0, hi=(1 << 32) - 1)
    x = bld.expr_var(0)
    for m, r in facts:
        bld.add_constraint(bld.expr_binary(
            P.BIN_EQ, bld.expr_binary(P.BIN_MOD, x, bld.expr_const(m)),
            bld.expr_const(r)))
    prob, _ = bld.finalize()
    return prob


def test_two_mod_facts_same_modulus_contradiction_is_compile_unsat():
    """`x%7==3 && x%7==4` can never hold: caught before search starts."""
    prob = _mod_problem([(7, 3), (7, 4)])
    with pytest.raises(CompileUnsatError):
        SolveCtx(prob)


def test_two_mod_facts_different_modulus_contradiction_is_compile_unsat():
    """`x%6==1 && x%4==0`: 6 and 4 share a factor of 2, and 1 is odd while
    0 is even, so no x is both -- CRT merge must reject before search."""
    prob = _mod_problem([(6, 1), (4, 0)])
    with pytest.raises(CompileUnsatError):
        SolveCtx(prob)


def test_crt_merge_of_consistent_facts_still_solves():
    """xezim's `mod_crt`: x%7==3, x%11==5, x%13==2 (closed form x=808 mod
    1001, confirmed by direct search below) must still solve, not be
    mistaken for a contradiction."""
    prob = _mod_problem([(7, 3), (11, 5), (13, 2)])
    with SolveCtx(prob) as ctx:
        for seed in range(1, 6):
            ctx.reset()
            rc = ctx.solve(seed=seed, time_limit_ms=200)
            assert rc == SOLVE_OK
            v = ctx.get_value(0) & 0xFFFFFFFF
            assert v % 7 == 3 and v % 11 == 5 and v % 13 == 2


def test_single_mod_fact_unaffected():
    """One `x % m == r` fact alone (nothing to merge) still solves."""
    prob = _mod_problem([(7, 3)])
    with SolveCtx(prob) as ctx:
        ctx.reset()
        rc = ctx.solve(seed=1, time_limit_ms=200)
        assert rc == SOLVE_OK
        assert (ctx.get_value(0) & 0xFFFFFFFF) % 7 == 3


def test_mod_facts_on_aliased_variables_merge_through_the_alias():
    """`x==y && x%7==3 && y%7==4`: x and y are the same variable through
    the `x==y` alias, so this is the same contradiction as the
    same-variable case, just spelled with two names."""
    bld = SolveProblemBuilder()
    bld.add_var(0, width=32, is_signed=False, lo=0, hi=(1 << 32) - 1)
    bld.add_var(1, width=32, is_signed=False, lo=0, hi=(1 << 32) - 1)
    x, y = bld.expr_var(0), bld.expr_var(1)
    bld.add_constraint(bld.expr_binary(P.BIN_EQ, x, y))
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, bld.expr_binary(P.BIN_MOD, x, bld.expr_const(7)), bld.expr_const(3)))
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, bld.expr_binary(P.BIN_MOD, y, bld.expr_const(7)), bld.expr_const(4)))
    prob, _ = bld.finalize()
    with pytest.raises(CompileUnsatError):
        SolveCtx(prob)
