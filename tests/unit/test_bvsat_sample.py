"""dvs_bbsolver_sample: near-uniform sampling in the engine (pyvsc v2 plan V19).

pyvsc's XOR-hash sampler built a fresh BV-SAT instance, re-bit-blasting the
problem, for each of up to 17 parity levels of every draw, and diversified the
model at each level: 1.2 s a draw for a 65536-bit `a < b`. The engine now
encodes once on CaDiCaL, makes each plane an assumption literal, finds the
last non-empty level top-first, and diversifies once. Repeated draws on one
context only solve.
"""
from __future__ import annotations

import collections

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_ADD, BIN_EQ, BIN_GT, BIN_LT, BIN_LTE
from dv_solve.bvsat import BVSatCtx, BVSAT_ERROR, BVSAT_SAT, BVSAT_UNSAT

_FULL = (1 << 63) - 1


def _ctx(build, incremental=True):
    b = SolveProblemBuilder()
    try:
        build(b)
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    return BVSatCtx(buf, incremental=incremental)


def _has_cadical():
    def build(b):
        b.add_var(0, 8, False, 0, 255)
    ctx = _ctx(build)
    try:
        return ctx.is_incremental()
    finally:
        ctx.destroy()


needs_cadical = pytest.mark.skipif(
    not _has_cadical(), reason="dv-solve built without CaDiCaL (DVS_WITH_CADICAL=OFF)")


def _small_range(b):
    """x <= 19 on a 64-bit unsigned x (the raw BV-SAT model left 3 of the
    20 values unreached over 400 draws), and a free 8-bit y."""
    b.add_var(0, 64, False, 0, _FULL)
    b.add_var(1, 8, False, 0, 255)
    b.add_constraint(b.expr_binary(BIN_LTE, b.expr_var(0), b.expr_const(19)))


def test_needs_an_incremental_backend():
    ctx = _ctx(_small_range, incremental=False)
    try:
        assert not ctx.is_incremental()
        assert ctx.sample([0, 1], 1) == BVSAT_ERROR
    finally:
        ctx.destroy()


@needs_cadical
def test_small_range_is_covered_evenly():
    ctx = _ctx(_small_range)
    try:
        assert ctx.is_incremental()
        xs, ys = collections.Counter(), set()
        n = 400
        for seed in range(1, n + 1):
            assert ctx.sample([0, 1], seed) == BVSAT_SAT
            x = ctx.value(0)
            assert 0 <= x <= 19
            xs[x] += 1
            ys.add(ctx.value(1))
    finally:
        ctx.destroy()
    assert len(xs) == 20
    # Uniform would be 20 each; a chi-square over 19 dof at p = 0.001 is 43.8.
    chi2 = sum((c - n / 20) ** 2 / (n / 20) for c in xs.values())
    assert chi2 < 43.8, (chi2, sorted(xs.items()))
    # A free 8-bit variable: ~202 distinct of 256 expected over 400 draws.
    assert len(ys) > 170


@needs_cadical
def test_same_seed_same_draw():
    vals = []
    for _ in range(2):
        ctx = _ctx(_small_range)
        try:
            ctx.sample([0, 1], 12345)
            vals.append((ctx.value(0), ctx.value(1)))
        finally:
            ctx.destroy()
    assert vals[0] == vals[1]


@needs_cadical
def test_unsat_is_proved():
    def build(b):
        b.add_var(0, 8, False, 0, 255)
        b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(0), b.expr_const(5)))
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_const(3)))
    ctx = _ctx(build)
    try:
        assert ctx.sample([0], 1) == BVSAT_UNSAT
    finally:
        ctx.destroy()


@needs_cadical
@pytest.mark.parametrize("width", [1024, 65536])
def test_wide_draws_hold_and_vary(width):
    k = 1 << (width - 1)

    def build(b):
        b.add_var(0, width, False, 0, _FULL)
        b.add_var(1, width, False, 0, _FULL)
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_var(1)))
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(1),
                                       b.expr_const_wide(k, width)))
    ctx = _ctx(build)
    try:
        seen = set()
        for seed in range(1, 6):
            assert ctx.sample([0, 1], seed) == BVSAT_SAT
            a, c = ctx.value_wide(0, width), ctx.value_wide(1, width)
            assert a < c < k
            seen.add(a)
        assert len(seen) == 5
    finally:
        ctx.destroy()


@needs_cadical
def test_arithmetic_relation_holds():
    """a + b == K at 256 bits: every draw satisfies it and the draws differ."""
    w, k = 256, (1 << 255) + 12345

    def build(b):
        b.add_var(0, w, False, 0, _FULL)
        b.add_var(1, w, False, 0, _FULL)
        b.add_constraint(b.expr_binary(
            BIN_EQ, b.expr_binary(BIN_ADD, b.expr_var(0), b.expr_var(1)),
            b.expr_const_wide(k, w)))
    ctx = _ctx(build)
    try:
        seen = set()
        for seed in range(1, 9):
            assert ctx.sample([0, 1], seed) == BVSAT_SAT
            a, c = ctx.value_wide(0, w), ctx.value_wide(1, w)
            assert (a + c) % (1 << w) == k
            seen.add(a)
        assert len(seen) == 8
    finally:
        ctx.destroy()


@needs_cadical
def test_stale_once_planes_outweigh_the_problem():
    """Each draw adds its planes to the instance; on a small problem they
    soon outweigh it, and sample_stale says to rebuild."""
    ctx = _ctx(_small_range)
    try:
        assert not ctx.sample_stale()          # nothing encoded yet
        ctx.sample([0, 1], 1)
        assert not ctx.sample_stale()
        for seed in range(2, 2000):
            ctx.sample([0, 1], seed)
            if ctx.sample_stale():
                break
        else:
            raise AssertionError("never stale")
        assert seed > 10
    finally:
        ctx.destroy()


def _msb_rate(width, nvars, which, n=400):
    """Draws of the chain v0 < v1 < ... (unsigned, `width` bits each), one
    fresh context per draw as pyvsc's sampler opens them, and the fraction
    with variable `which`'s top bit set. Uniform over the solutions,
    P(v_i.msb) for a chain of m is the share of sorted m-tuples whose i-th
    element is in the upper half: 1/4 and 3/4 for a < b; 1/8 and 7/8 for the
    ends of a < b < c."""
    def build(b):
        for i in range(nvars):
            b.add_var(i, width, False, 0, _FULL)
        for i in range(nvars - 1):
            b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(i), b.expr_var(i + 1)))
    hits = 0
    for seed in range(1, n + 1):
        ctx = _ctx(build)
        try:
            assert ctx.sample(list(range(nvars)), seed) == BVSAT_SAT
            vals = [ctx.value_wide(i, width) for i in range(nvars)]
        finally:
            ctx.destroy()
        assert all(vals[i] < vals[i + 1] for i in range(nvars - 1))
        hits += vals[which] >> (width - 1)
    return hits / n


@needs_cadical
@pytest.mark.parametrize("width,nvars,which,expect", [
    (128, 2, 0, 0.25),
    (128, 3, 0, 0.125),
    (128, 3, 2, 0.875),
])
def test_bits_above_the_planes_are_uniform(width, nvars, which, expect):
    """The XOR planes cover only the low 16 bits of each variable, so the top
    bits come from diversification alone. A single flip sweep left them with
    the SAT solver's bias, which changed with a CaDiCaL point release: 128-bit
    a < b drew a.msb 13% of the time on CaDiCaL 3.0.0, and a < b < c drew
    a.msb 21.5% on 3.0.1 (expected 25% and 12.5%). Bounds are about 4 sigma
    at n = 400."""
    p = _msb_rate(width, nvars, which)
    sigma = (expect * (1 - expect) / 400) ** 0.5
    assert abs(p - expect) < 4 * sigma, (p, expect)
