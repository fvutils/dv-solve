"""Congruence link-carrying across `y == x + c` / `y == x - c` (item 3b).

`_collect_congruence_links` (dvs_compile.c) splits x's current (compile-time
declared) domain into a "no-wrap" zone, where `x +/- c` stays inside
`[0, 2^w-1]`, and a "wrap" zone, the rest. When only one zone overlaps x's
domain, the shifted residue is installed on y like any other fact. When both
do, nothing is installed on y directly, but a contradiction between BOTH
zones' candidate residues and y's own `y % m == r` fact still proves
compile-time UNSAT.

Width matters here: dv-solve's `ADD`/`SUB` only wrap modulo 2^w (true
2's-complement semantics) for widths that route through the `bvadd_64`/
`bvadd_const_64` propagators; narrower widths (confirmed with width=8, by
direct experiment) instead treat `x + c` exceeding the width as a plain
infeasible value (no solution at all for that `x`), not a wrapped one. 32
bits -- used throughout here, matching `mod_unsat`/`mod_crt` -- does wrap,
confirmed directly: `x=2**32-1; y=x+1` solves with `y=0`. Do not change this
file's width to something narrower without re-checking that assumption.
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

WIDTH = 32
MAXV = (1 << WIDTH) - 1


def _link_problem(lo, hi, m, r, c, is_sub, y_r):
    """32-bit x in [lo, hi] with `x % m == r`, `y == x +/- c`, `y % m == y_r`."""
    bld = SolveProblemBuilder()
    bld.add_var(0, width=WIDTH, is_signed=False, lo=lo, hi=hi)
    bld.add_var(1, width=WIDTH, is_signed=False, lo=0, hi=MAXV)
    x, y = bld.expr_var(0), bld.expr_var(1)
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, bld.expr_binary(P.BIN_MOD, x, bld.expr_const(m)), bld.expr_const(r)))
    op = P.BIN_SUB if is_sub else P.BIN_ADD
    bld.add_constraint(bld.expr_binary(P.BIN_EQ, y, bld.expr_binary(op, x, bld.expr_const(c))))
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, bld.expr_binary(P.BIN_MOD, y, bld.expr_const(m)), bld.expr_const(y_r)))
    prob, _ = bld.finalize()
    return prob


def test_add_link_no_wrap_installs_shifted_residue():
    """x in [0, 100], x%3==0, y==x+3: x+3 never reaches 2^32, so the shifted
    residue (0+3)%3==0 holds everywhere and is installed on y. A `y%3==0`
    fact is consistent (solves); a `y%3==1` fact is a direct contradiction
    with the installed residue (compile-time UNSAT, not just bounds-UNSAT:
    plenty of x in range still satisfy x%3==0)."""
    prob = _link_problem(0, 100, 3, 0, 3, False, 0)
    with SolveCtx(prob) as ctx:
        ctx.reset()
        rc = ctx.solve(seed=1, time_limit_ms=500)
        assert rc == SOLVE_OK
        x, y = ctx.get_value(0), ctx.get_value(1)
        assert x % 3 == 0 and y == x + 3 and y % 3 == 0

    with pytest.raises(CompileUnsatError):
        SolveCtx(_link_problem(0, 100, 3, 0, 3, False, 1))


def test_sub_link_no_wrap_installs_shifted_residue():
    """x in [50, 100], x%3==2, y==x-3: x-3 never goes negative, so the
    shifted residue (2-3) mod 3 == 2 holds everywhere."""
    prob = _link_problem(50, 100, 3, 2, 3, True, 2)
    with SolveCtx(prob) as ctx:
        ctx.reset()
        rc = ctx.solve(seed=1, time_limit_ms=500)
        assert rc == SOLVE_OK
        x, y = ctx.get_value(0), ctx.get_value(1)
        assert x % 3 == 2 and y == x - 3 and y % 3 == 2

    with pytest.raises(CompileUnsatError):
        SolveCtx(_link_problem(50, 100, 3, 2, 3, True, 0))


def test_add_link_wrap_only_installs_shifted_residue():
    """x in [2^32-3, 2^32-1], x%3==(2^32-3)%3, y==x+3: every x in this tiny
    range overflows 2^32, so the WRAP zone's shifted residue (accounting for
    2^32 mod 3) holds everywhere -- confirmed by direct solve, not just the
    implementation's own formula."""
    lo, hi, m, c = MAXV - 2, MAXV, 3, 3
    r = lo % m
    good = ((lo + c) & MAXV) % m
    bad = next(v for v in range(m) if v != good)
    prob = _link_problem(lo, hi, m, r, c, False, good)
    with SolveCtx(prob) as ctx:
        ctx.reset()
        rc = ctx.solve(seed=1, time_limit_ms=500)
        assert rc == SOLVE_OK
        x, y = ctx.get_value(0), ctx.get_value(1)
        assert x % m == r and y == (x + c) & MAXV and y % m == good

    with pytest.raises(CompileUnsatError):
        SolveCtx(_link_problem(lo, hi, m, r, c, False, bad))


def test_sub_link_wrap_only_installs_shifted_residue():
    """x in [0, 2], y==x-3: every x in this range underflows, so the WRAP
    (borrow) zone's shifted residue holds everywhere."""
    lo, hi, m, c = 0, 2, 3, 3
    r = lo % m
    good = ((lo - c) & MAXV) % m
    bad = next(v for v in range(m) if v != good)
    prob = _link_problem(lo, hi, m, r, c, True, good)
    with SolveCtx(prob) as ctx:
        ctx.reset()
        rc = ctx.solve(seed=1, time_limit_ms=500)
        assert rc == SOLVE_OK
        x, y = ctx.get_value(0), ctx.get_value(1)
        assert x % m == r and y == (x - c) & MAXV and y % m == good

    with pytest.raises(CompileUnsatError):
        SolveCtx(_link_problem(lo, hi, m, r, c, True, bad))


def test_add_link_both_zones_case_split_unsat():
    """x spans the full 32-bit domain, x%5==0, y==x+3: the no-wrap zone
    gives y%5==3 (the common case: x+3 fits, picked by plain search in a
    couple hundred ms) and the wrap zone gives y%5==2 (only x=2^32-1 is
    both in the wrap zone and %5==0 -- a 1-in-2^32 witness, not expected to
    be found by search at all). A `y%5` fact outside {2, 3} is unreachable
    either way: caught at compile time via the case-split proof."""
    m, r, c = 5, 0, 3
    bad = next(v for v in range(m) if v not in (2, 3))
    with pytest.raises(CompileUnsatError):
        SolveCtx(_link_problem(0, MAXV, m, r, c, False, bad))


def test_add_link_both_zones_no_wrap_branch_is_not_over_constrained():
    """Same shape as above, but `y%5==3` (the no-wrap zone's residue) is
    consistent -- picking a fact that only one zone can supply must not be
    rejected as a false compile-time UNSAT just because the other zone
    cannot supply it too."""
    m, r, c = 5, 0, 3
    prob = _link_problem(0, MAXV, m, r, c, False, 3)
    with SolveCtx(prob) as ctx:
        ctx.reset()
        rc = ctx.solve(seed=1, time_limit_ms=500)
        assert rc == SOLVE_OK
        x, y = ctx.get_value(0), ctx.get_value(1)
        assert x % m == r and y == (x + c) & MAXV and y % m == 3


def test_two_link_chain_propagates_in_one_pass():
    """`x%3==0; z==y+4; y==x+3; z%3==1` (note the add order: z's link is
    added to the builder BEFORE y's, so y's link is processed first in
    `_collect_congruence_links`' single reverse-of-insertion-order pass --
    the implementation is not a fixpoint, so this order dependency is real,
    not incidental). x%3==0 and y==x+3 (no wrap, small domain) carries
    y%3==0 onto y; that in turn carries z%3==1 onto z via the second link,
    which contradicts the explicit `z%3==2` below -- caught in the same
    compile pass, with no re-scan needed."""
    bld = SolveProblemBuilder()
    for i in range(3):
        bld.add_var(i, width=WIDTH, is_signed=False, lo=0, hi=50)
    x, y, z = bld.expr_var(0), bld.expr_var(1), bld.expr_var(2)
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, bld.expr_binary(P.BIN_MOD, x, bld.expr_const(3)), bld.expr_const(0)))
    # z's link added first, y's link added second: processed y-then-z.
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, z, bld.expr_binary(P.BIN_ADD, y, bld.expr_const(4))))
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, y, bld.expr_binary(P.BIN_ADD, x, bld.expr_const(3))))
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, bld.expr_binary(P.BIN_MOD, z, bld.expr_const(3)), bld.expr_const(2)))
    prob, _ = bld.finalize()
    with pytest.raises(CompileUnsatError):
        SolveCtx(prob)


def test_two_link_chain_reversed_order_is_still_sound():
    """Same fixture, but z's link added AFTER y's: processed z-then-y, so
    z's link runs before y has a fact to carry -- the chain is not caught
    at compile time (known limitation, not a fixpoint). The problem is
    genuinely UNSAT regardless (x%3==0 forces y%3==0 forces z%3==1, never
    2), so soundness only requires search to still reach SOLVE_UNSAT within
    budget -- it must not report SOLVE_OK."""
    bld = SolveProblemBuilder()
    for i in range(3):
        bld.add_var(i, width=WIDTH, is_signed=False, lo=0, hi=50)
    x, y, z = bld.expr_var(0), bld.expr_var(1), bld.expr_var(2)
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, bld.expr_binary(P.BIN_MOD, x, bld.expr_const(3)), bld.expr_const(0)))
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, y, bld.expr_binary(P.BIN_ADD, x, bld.expr_const(3))))
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, z, bld.expr_binary(P.BIN_ADD, y, bld.expr_const(4))))
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, bld.expr_binary(P.BIN_MOD, z, bld.expr_const(3)), bld.expr_const(2)))
    prob, _ = bld.finalize()
    try:
        with SolveCtx(prob) as ctx:
            ctx.reset()
            rc = ctx.solve(seed=1, time_limit_ms=500)
    except CompileUnsatError:
        return
    assert rc == SOLVE_UNSAT
