"""Unit fixtures ported from xezim's direct solver-comparison bench.

Five problems from ``~/xezim_research/bench/direct/problems.py`` that
exercise features outside ``tests/perf/rand_benches.py``'s IR (``dist``,
``soft``, ``unique``) or a known gap (``mod_unsat``'s UNSAT proof). These
exist as regression fixtures before enhancement-plan items 2, 3 and 5
change the behavior they probe (see enhancement_plan item 0 steps 2-3).

``mod_unsat`` and ``sum_unique`` are xfail today; the rest already pass.
"""
from __future__ import annotations

import os
from collections import Counter
from pathlib import Path

import pytest

dv = pytest.importorskip("dv_solve")

from dv_solve import problem as P
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SOLVE_OK, SOLVE_UNSAT, SolveCtx

from .dist_harness import assert_gof

_REPO = Path(__file__).resolve().parents[2]

# Point the native loader at the locally-built profiling library when no
# installation is already selected: this file runs against `build-prof`,
# which the resolver does not search by name (mirrors test_rand_benches.py).
if not os.environ.get("DVS_SOLVER_PATH"):
    _lib_dir = _REPO / "build-prof" / "lib"
    if (_lib_dir / "libdv_solve.so").exists():
        os.environ["DVS_SOLVER_PATH"] = str(_lib_dir)

# Matches `n` for these fixtures in xezim_research/bench/direct/problems.py.
N = 2000


def _sample(bld: SolveProblemBuilder, var_ids, width: int, n: int,
           seed_base: int = 1):
    """Seeded solves of `bld`'s finalized problem, histogrammed by `var_ids`.

    Single value per draw if `var_ids` has one entry, else a tuple.
    """
    prob, _ = bld.finalize()
    mask = (1 << width) - 1
    hist: Counter = Counter()
    with SolveCtx(prob) as ctx:
        for i in range(n):
            ctx.reset()
            rc = ctx.solve(seed=seed_base + i, time_limit_ms=5000)
            assert rc == SOLVE_OK, f"solve returned {rc} at i={i}"
            vals = tuple(ctx.get_value(v) & mask for v in var_ids)
            hist[vals[0] if len(var_ids) == 1 else vals] += 1
    return hist


@pytest.mark.xfail(strict=True, reason="dv-solve times out rather than "
                   "proving UNSAT; item 3 (congruence facts) should fix this")
def test_mod_unsat_is_unsat():
    """`x%7==3; y==x+1; y%7==5` over 32 bits is infeasible, but not by bounds.

    dv-solve has no congruence reasoning yet, so it searches until the time
    budget runs out (`SOLVE_TIMEOUT`) instead of proving `SOLVE_UNSAT`. 200ms
    comfortably covers the 13.6ms/176ms(max) TIMEOUT the enhancement plan
    measured, confirmed with `.log/0b-mod_unsat-probe.log` (consistent
    TIMEOUT at 50/100/200/300ms) -- long enough to be a fair probe, short
    enough that a perpetually-failing test stays fast.
    """
    bld = SolveProblemBuilder()
    bld.add_var(0, width=32, is_signed=False, lo=0, hi=(1 << 32) - 1)
    bld.add_var(1, width=32, is_signed=False, lo=0, hi=(1 << 32) - 1)
    x, y = bld.expr_var(0), bld.expr_var(1)
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, bld.expr_binary(P.BIN_MOD, x, bld.expr_const(7)), bld.expr_const(3)))
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, y, bld.expr_binary(P.BIN_ADD, x, bld.expr_const(1))))
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, bld.expr_binary(P.BIN_MOD, y, bld.expr_const(7)), bld.expr_const(5)))
    prob, _ = bld.finalize()
    with SolveCtx(prob) as ctx:
        ctx.reset()
        rc = ctx.solve(seed=1, time_limit_ms=200)
    assert rc == SOLVE_UNSAT


def test_perm16_joint_uniform():
    """`unique` over 16 4-bit vars: a permutation of 0..15.

    Every ordered pair (a0, a1) with a0 != a1 is equally likely (1/240 =
    1/(16*15)). Passes today.
    """
    bld = SolveProblemBuilder()
    for i in range(16):
        bld.add_var(i, width=4, is_signed=False, lo=0, hi=15)
    bld.add_all_different(list(range(16)))

    hist = _sample(bld, [0, 1], width=4, n=N)
    expected = {(i, j): 1 / 240 for i in range(16) for j in range(16) if i != j}
    assert_gof(hist, expected, label="perm16 joint (a0, a1)")


def _ksubsets_by_sum(vals, k, s):
    """Count of k-subsets of `vals` summing to `s`.

    Ported verbatim from xezim_research/bench/direct/problems.py -- pure
    combinatorics, not solver code.
    """
    dp = [[0] * (s + 1) for _ in range(k + 1)]
    dp[0][0] = 1
    for v in vals:
        for j in range(k, 0, -1):
            for t in range(s, v - 1, -1):
                dp[j][t] += dp[j - 1][t - v]
    return dp[k][s]


def _sum_unique_truth():
    """P(a0 == v) for `a[8]` in [1,32], `unique`, `sum(a) == 100`."""
    full = range(1, 33)
    tot = _ksubsets_by_sum(full, 8, 100)
    out = {}
    for v in full:
        k = _ksubsets_by_sum([u for u in full if u != v], 7, 100 - v)
        if k:
            out[v] = k / (8 * tot)
    return out


@pytest.mark.xfail(reason="item 5: ordering-chain / counting-weight bias, "
                   "mild (+0.011, p=3e-18 per enhancement plan)")
def test_sum_unique_binned_by_a0():
    """`a[8]` in [1,32], `unique`, `sum(a) == 100`, binned by a0."""
    bld = SolveProblemBuilder()
    for i in range(8):
        bld.add_var(i, width=8, is_signed=False, lo=0, hi=255)
        bld.add_constraint(bld.expr_in_range(
            bld.expr_var(i), bld.expr_const(1), bld.expr_const(32)))
    bld.add_all_different(list(range(8)))
    acc = bld.expr_var(0)
    for i in range(1, 8):
        acc = bld.expr_binary(P.BIN_ADD, acc, bld.expr_var(i))
    bld.add_constraint(bld.expr_binary(P.BIN_EQ, acc, bld.expr_const(100)))

    hist = _sample(bld, [0], width=8, n=N)
    assert_gof(hist, _sum_unique_truth(), label="sum_unique (a0)")


def test_dist_pruned_buckets():
    """`x dist {0:=10, [1:10]:=1, [11:255]:/10}; x != 5`: buckets 10:9:10 (18.5.4)."""
    bld = SolveProblemBuilder()
    bld.add_var(0, width=8, is_signed=False, lo=0, hi=255)
    bld.add_dist(0, [
        {"lo": 0, "hi": 0, "weight": 10},
        {"lo": 1, "hi": 10, "weight": 1},
        {"lo": 11, "hi": 255, "weight": 10, "is_per_value": False},
    ])
    bld.add_constraint(bld.expr_binary(P.BIN_NEQ, bld.expr_var(0), bld.expr_const(5)))

    hist = _sample(bld, [0], width=8, n=N)
    bucketed: Counter = Counter()
    for v, c in hist.items():
        bucket = 0 if v == 0 else (1 if v <= 10 else 2)
        bucketed[bucket] += c
    assert_gof(bucketed, {0: 10 / 29, 1: 9 / 29, 2: 10 / 29}, label="dist_pruned buckets")


def test_soft_conflict_later_wins():
    """`a < b; soft a > 100; soft b < 50`: the later soft wins (18.5.14.1).

    Confirmed experimentally (`.log/0b-soft_conflict-probe.log`, 200 seeded
    solves, 0 violations) before writing this truth table: `b < 50` (added
    second) always wins over `a > 100` (added first), matching
    xezim_research/bench/direct/problems.py's comment despite the lower
    numeric priority (1) on the constraint that loses.

    The marginal shape of `b` is NOT uniform over `a < b < 50` pairs
    (that would give P(b) proportional to b); dv-solve picks variables in
    declaration order, so `a` is drawn uniformly over its propagated
    domain `[0, 48]` (tightened from `b < 50` before `a` is ever bound),
    then `b` is drawn uniformly over `(a, 49]`. That gives
    P(b=k) = (1/49) * sum_{a=0}^{k-1} 1/(49-a), confirmed against a 20000
    seed reference sample (`.log/0b-soft_conflict-probe.log`) and only
    then baked in here. This is solver variable-ordering behavior, not an
    LRM requirement; re-derive this table if the native solver's ordering
    heuristic changes.
    """
    bld = SolveProblemBuilder()
    bld.add_var(0, width=8, is_signed=False, lo=0, hi=255)
    bld.add_var(1, width=8, is_signed=False, lo=0, hi=255)
    a, b = bld.expr_var(0), bld.expr_var(1)
    bld.add_constraint(bld.expr_binary(P.BIN_LT, a, b))
    bld.add_soft_constraint(bld.expr_binary(P.BIN_GT, a, bld.expr_const(100)), priority=1)
    bld.add_soft_constraint(bld.expr_binary(P.BIN_LT, b, bld.expr_const(50)), priority=0)

    hist = _sample(bld, [1], width=8, n=N)
    expected = {k: sum(1.0 / (49 - a) for a in range(k)) / 49.0 for k in range(1, 50)}
    assert_gof(hist, expected, label="soft_conflict (b)")
