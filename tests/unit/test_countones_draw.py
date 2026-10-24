"""The exact `$countones` chain draw (enhancement plan item 4a, step 1/n).

`_pick_countones` (dvs_search.c) draws straight from the per-var compile-
time facts persisted by `_compile_countones_facts`
(ctx->countones_bits/n, ctx->known_bits_mask/val) instead of letting the
ordinary CDCL decision search narrow a left-deep chain of
`zero_extend(extract(x,i,i))` bvadds one bit at a time. These tests
exercise the draw itself (every model still satisfies the popcount target
-- and, for the pop32 shape, the extra forced-bits conjunct too -- and the
draw is spread across the feasible values, not biased toward one end) --
there is no separate compile-only test for this item, since the bench
shapes these facts are built from (`tests/perf/rand_benches.py`'s
`_countones`/`_pop32`) never go through `EXPR_COUNTONES` and so have no
analogous "persist" unit test to mirror (see state/notes/4a.md, Finding
1).
"""
from __future__ import annotations

import collections
import math
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


def _countones_chain(bld, x, nbits, acc_width):
    acc = bld.expr_extend(bld.expr_extract(x, 0, 0), 1, acc_width, False)
    for i in range(1, nbits):
        leaf = bld.expr_extend(bld.expr_extract(x, i, i), 1, acc_width, False)
        acc = bld.expr_binary(P.BIN_ADD, acc, leaf)
    return acc


# -- `countones` bench shape: a plain 8-bit popcount, no other conjunct. --

CO_WIDTH = 8
CO_K = 4


def _countones_problem():
    bld = SolveProblemBuilder()
    bld.add_var(0, width=CO_WIDTH, is_signed=False, lo=0, hi=(1 << CO_WIDTH) - 1)
    x = bld.expr_var(0)
    acc = _countones_chain(bld, x, CO_WIDTH, CO_WIDTH)
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, acc, bld.expr_const(CO_K, width=CO_WIDTH)))
    prob, _ = bld.finalize()
    return prob


def _countones_draws(n):
    prob = _countones_problem()
    out = []
    with SolveCtx(prob) as ctx:
        for seed in range(1, n + 1):
            ctx.reset()
            assert ctx.solve(seed=seed, time_limit_ms=200) == SOLVE_OK
            out.append(ctx.get_value(0) & ((1 << CO_WIDTH) - 1))
    return out


def test_countones_every_draw_has_the_target_popcount():
    for v in _countones_draws(300):
        assert bin(v).count("1") == CO_K


def test_countones_draw_is_spread_across_the_feasible_values():
    """C(8,4) == 70 equally likely values; a draw stuck on one value (or a
    narrow run) would catch a `_pick_countones` bug that always sets the
    same subset of bits, e.g. the lowest `k` tracked positions."""
    n = 4000
    feasible = [v for v in range(1 << CO_WIDTH) if bin(v).count("1") == CO_K]
    exp = n / len(feasible)
    cnt = collections.Counter(_countones_draws(n))
    assert set(cnt) <= set(feasible)
    chi2 = sum((cnt.get(v, 0) - exp) ** 2 / exp for v in feasible)
    # dof 69; mean 69, sd ~11.7; p ~ 1e-4 at ~130.
    assert chi2 < 130


# -- `pop32` bench shape: a 32-bit popcount chain AND a forced low nibble,
# the second per-var fact (`known_bits_mask`/`known_bits_val`) overlapping
# the chain's tracked bits. --

POP_WIDTH = 32
POP_K = 16
POP_LOW_NIBBLE = 0xA  # bits [3:0], forced -- 2 of its 4 bits are 1s.


def _pop32_problem():
    bld = SolveProblemBuilder()
    bld.add_var(0, width=POP_WIDTH, is_signed=False, lo=0, hi=(1 << POP_WIDTH) - 1)
    x = bld.expr_var(0)
    acc = _countones_chain(bld, x, POP_WIDTH, 6)
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, acc, bld.expr_const(POP_K, width=6)))
    bld.add_constraint(bld.expr_binary(
        P.BIN_EQ, bld.expr_extract(x, 3, 0),
        bld.expr_const(POP_LOW_NIBBLE, width=4)))
    prob, _ = bld.finalize()
    return prob


def _pop32_draws(n):
    prob = _pop32_problem()
    out = []
    with SolveCtx(prob) as ctx:
        for seed in range(1, n + 1):
            ctx.reset()
            assert ctx.solve(seed=seed, time_limit_ms=200) == SOLVE_OK
            out.append(ctx.get_value(0) & ((1 << POP_WIDTH) - 1))
    return out


def test_pop32_every_draw_satisfies_both_conjuncts():
    forced_popcount = bin(POP_LOW_NIBBLE).count("1")
    for v in _pop32_draws(300):
        assert bin(v).count("1") == POP_K
        assert (v & 0xF) == POP_LOW_NIBBLE
        assert forced_popcount == 2  # sanity on the fixture itself.


def _pop32_truth_by_top_nibble():
    """bin (x[31:28]) -> count of solutions with that top nibble.

    The low nibble (bits [3:0]) is pinned by the second conjunct, so it is
    NOT free: of the 32 bits, 4 are pinned low (2 of them already 1s) and
    4 more are fixed by which top-nibble bin a draw falls in, leaving only
    the 24 middle bits (4..27) truly free. Binning by the top nibble must
    therefore use `C(24, ...)`, not `C(28, ...)` (28 is the *total* free-
    bit count before any binning, but binning by the top nibble consumes 4
    more of those 28 -- see state/notes/4a.md's step-27 correction)."""
    forced_popcount = bin(POP_LOW_NIBBLE).count("1")
    truth = {}
    for top in range(16):
        need = POP_K - forced_popcount - bin(top).count("1")
        if 0 <= need <= 24:
            truth[top] = math.comb(24, need)
    return truth


def test_pop32_draw_is_spread_across_the_feasible_values():
    n = 6000
    truth = _pop32_truth_by_top_nibble()
    total = sum(truth.values())
    cnt = collections.Counter(v >> 28 for v in _pop32_draws(n))
    assert set(cnt) <= set(truth)
    # chi-square over the 16 top-nibble bins (15 dof: p ~ 1e-4 at ~37.7).
    chi2 = sum((cnt.get(b, 0) - n * truth[b] / total) ** 2 / (n * truth[b] / total)
               for b in truth)
    assert chi2 < 40
