"""Fuzz family for congruence link-carrying (item 3b), random moduli/offsets
and wrap edges.

Unlike ``test_fuzz_cross_check.py``, this does not go through SMT2/z3: the
op dv-solve's SMT2 frontend compiles `bvadd`/`bvurem` to is not necessarily
the same path `_collect_congruence_links` (dvs_compile.c) analyzes (built
via the Python builder, `P.BIN_ADD`/`P.BIN_MOD`), and that path's wraparound
behavior is itself width-dependent (confirmed in
``test_congruence_link_compile.py``'s module docstring: 32-bit wraps, 8-bit
does not). The oracle here is a direct Python brute-force enumeration of
`x`'s (small) domain window, using the same 32-bit wraparound arithmetic
dv-solve's `ADD`/`SUB` are confirmed to use at that width -- not a
hand-derived formula.

The one invariant checked at scale: `_collect_congruence_links`'s
compile-time UNSAT proof must never fire when a real solution exists (that
would be unsound), and conversely a genuinely-UNSAT fixture must never
report ``SOLVE_OK``. Seeds are fixed; scale with
``FUZZ_N`` (default 300): ``FUZZ_N=2000 pytest tests/formal/test_congruence_link_fuzz.py``
"""
from __future__ import annotations

import os
import random
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
_N = int(os.environ.get("FUZZ_N", "300"))
_SEEDS = list(range(_N))


def _gen_case(seed):
    """Random (lo, hi, m, r, c, is_sub) with x%m==r guaranteed satisfiable
    in [lo, hi], and a window placed so wrap edges (near 0 / near 2^32-1)
    come up often alongside ordinary interior windows."""
    rng = random.Random(seed)
    m = rng.randint(2, 30)
    c = rng.randint(1, 80)
    is_sub = rng.choice([True, False])
    width_span = rng.randint(1, 4000)
    placement = rng.choice(["low", "high", "mid"])
    if placement == "low":
        lo = rng.randint(0, 200)
    elif placement == "high":
        lo = MAXV - width_span - rng.randint(0, 200)
    else:
        lo = rng.randint(0, MAXV - width_span)
    lo = max(0, min(lo, MAXV - width_span))
    hi = lo + width_span
    r = lo % m
    return lo, hi, m, r, c, is_sub


def _achievable_y_residues(lo, hi, m, r, c, is_sub):
    ys = set()
    for x in range(lo, hi + 1):
        if x % m != r:
            continue
        y = (x - c) if is_sub else (x + c)
        y &= MAXV
        ys.add(y % m)
    return ys


def _link_problem(lo, hi, m, r, c, is_sub, y_r):
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


@pytest.mark.parametrize("seed", _SEEDS)
def test_sat_residue_never_falsely_compile_unsat(seed):
    """A `y % m == r` fact that the brute-force oracle confirms reachable
    must never be rejected as a compile-time contradiction."""
    lo, hi, m, r, c, is_sub = _gen_case(seed)
    ys = _achievable_y_residues(lo, hi, m, r, c, is_sub)
    if not ys:
        pytest.skip("no x in window satisfies x%m==r (degenerate window)")
    y_r = random.Random(seed).choice(sorted(ys))
    prob = _link_problem(lo, hi, m, r, c, is_sub, y_r)
    try:
        SolveCtx(prob)
    except CompileUnsatError:
        pytest.fail(
            f"SOUNDNESS: compile-time UNSAT for a reachable case "
            f"(lo={lo}, hi={hi}, m={m}, r={r}, c={c}, is_sub={is_sub}, y_r={y_r})"
        )


@pytest.mark.parametrize("seed", _SEEDS)
def test_unreachable_residue_never_solves_ok(seed):
    """A `y % m == r` fact the oracle confirms unreachable must never
    report `SOLVE_OK` (compile-time UNSAT, search UNSAT, or an honest
    timeout are all fine)."""
    lo, hi, m, r, c, is_sub = _gen_case(seed)
    ys = _achievable_y_residues(lo, hi, m, r, c, is_sub)
    unreachable = [v for v in range(m) if v not in ys]
    if not unreachable:
        pytest.skip("every residue mod m is reachable in this window")
    y_r = random.Random(seed + 1).choice(unreachable)
    prob = _link_problem(lo, hi, m, r, c, is_sub, y_r)
    try:
        with SolveCtx(prob) as ctx:
            ctx.reset()
            rc = ctx.solve(seed=1, time_limit_ms=200)
    except CompileUnsatError:
        return
    assert rc != SOLVE_OK, (
        f"SOUNDNESS: SOLVE_OK for an unreachable residue "
        f"(lo={lo}, hi={hi}, m={m}, r={r}, c={c}, is_sub={is_sub}, y_r={y_r})"
    )
