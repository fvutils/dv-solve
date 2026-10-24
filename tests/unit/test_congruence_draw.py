"""The `R+M*k` draw rule (enhancement plan item 3c, step 2/n).

`_pick_mod_cong` (dvs_search.c) draws straight from the per-var merged
congruence fact persisted by `_compile_congruence_facts`
(ctx->cong_m/cong_r, item 3c step 1/n, tests/c/test_congruence_persist.c)
instead of letting `_pick_mod_aligned` fix up an already-drawn value one
`x%m==r` constraint at a time. These tests exercise the draw itself (every
model still satisfies every mod fact, and the draw is spread across the
feasible values, not biased toward one end) -- test_congruence_compile.py
only covers the compile-time merge, never a drawn model.
"""
from __future__ import annotations

import collections
import os
from pathlib import Path

import pytest

dv = pytest.importorskip("dv_solve")

from dv_solve import problem as P
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SOLVE_OK, SolveCtx

_REPO = Path(__file__).resolve().parents[2]
if not os.environ.get("DVS_SOLVER_PATH"):
    _lib_dir = _REPO / "build-prof" / "lib"
    if (_lib_dir / "libdv_solve.so").exists():
        os.environ["DVS_SOLVER_PATH"] = str(_lib_dir)

# 20-bit domain, same three moduli as xezim's `mod_crt` fixture (7, 11,
# 13 -> merged modulus 1001) but narrow enough that the solution count
# (~1048) is cheap to enumerate directly in the test, rather than the
# ~4.3M of the real 32-bit bench (tests/perf/rand_benches.py's `_mod_crt`).
WIDTH = 20
DOMAIN = 1 << WIDTH
FACTS = ((7, 3), (11, 5), (13, 2))
R = next(v for v in range(1001) if all(v % m == r for m, r in FACTS))
N_BINS = 64
BIN_SIZE = DOMAIN // N_BINS


def _mod_problem():
    bld = SolveProblemBuilder()
    bld.add_var(0, width=WIDTH, is_signed=False, lo=0, hi=DOMAIN - 1)
    x = bld.expr_var(0)
    for m, r in FACTS:
        bld.add_constraint(bld.expr_binary(
            P.BIN_EQ, bld.expr_binary(P.BIN_MOD, x, bld.expr_const(m)),
            bld.expr_const(r)))
    prob, _ = bld.finalize()
    return prob


def _truth_bins():
    """bin -> count of solutions in that bin, over the whole domain."""
    cnt = collections.Counter()
    for v in range(R, DOMAIN, 1001):
        cnt[v // BIN_SIZE] += 1
    return cnt


def _draws(n):
    prob = _mod_problem()
    out = []
    with SolveCtx(prob) as ctx:
        for seed in range(1, n + 1):
            ctx.reset()
            assert ctx.solve(seed=seed, time_limit_ms=200) == SOLVE_OK
            out.append(ctx.get_value(0) & (DOMAIN - 1))
    return out


def test_every_draw_satisfies_every_mod_fact():
    for v in _draws(300):
        assert all(v % m == r for m, r in FACTS)


def test_draw_is_spread_across_the_feasible_values():
    """Not every draw landing on the same k (or a narrow run of them) --
    the old _pick_mod_aligned path, which starts from a uniform draw over
    [lo,hi] and corrects it one constraint at a time, would already pass
    this; it is here to catch a `_pick_mod_cong` bug that always returns
    the same `v0` (k stuck at 0) or only ever the top of its range."""
    n = 6000
    truth = _truth_bins()
    total = sum(truth.values())
    cnt = collections.Counter(v // BIN_SIZE for v in _draws(n))
    assert set(cnt) <= set(truth)
    # chi-square over the populated bins (N_BINS-1 d.o.f: mean 63, sd ~11.2;
    # p ~ 1e-4 at ~115).
    chi2 = sum((cnt.get(b, 0) - n * truth[b] / total) ** 2 / (n * truth[b] / total)
               for b in truth)
    assert chi2 < 115
