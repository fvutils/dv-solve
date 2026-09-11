"""Distribution-*quality* tests for dv-solve randomization (checklist gap 4).

``tests/unit/test_dist.py`` already proves the coarse dist facts (domain
restriction, zero-weight exclusion, ~75/25 aggregate bias). This module goes
after the finer distribution-quality properties a sat oracle can't see, using
the scipy-backed sampling harness (:mod:`tests.unit.dist_harness`):

  * a *free* rand var is ~uniform over its domain (the base randomizer);
  * ``:/`` selects *uniformly within* the shared range (not just the right
    aggregate mass) — the part test_dist only checks in aggregate;
  * ``:=`` vs ``:/`` produce the different marginals the LRM requires;
  * a zero-weight range is fully excluded, remaining mass stays uniform;
  * an all-zero-weight dist degrades gracefully to a uniform draw over the
    full variable domain (dv-solve's documented fallback — see
    ``_pick_value_dist`` in zsp_search.c).

Gap 5 (``solve … before`` distribution effect) is intentionally NOT here:
solve-order is a frontend/scheduler concern (``test_scheduling_graph`` /
``test_icl``), not a native-solver dist property — the C API has no
solve-before primitive.

Run:  direnv exec . pytest tests/unit/test_dist_quality.py -v
"""
from __future__ import annotations

from .dist_harness import (
    BIN_GTE,
    DistProblem,
    assert_gof,
    assert_independent,
    assert_support,
    assert_uniform,
    marginal,
    restrict,
)

# Larger samples than test_dist.py: distribution *shape* needs the counts.
N = 8000


def test_free_var_uniform(libzsp):
    """An unconstrained 4-bit rand var must be ~uniform over 0..15.

    This is the base randomizer underneath every dist — if it were biased,
    every downstream distribution would inherit the skew.
    """
    dp = DistProblem(libzsp)
    dp.add_var(0, width=4, lo=0, hi=15)
    try:
        hist = dp.sample(0, N)
        assert set(hist) == set(range(16)), f"missing values: {set(range(16)) - set(hist)}"
        assert_uniform(hist, range(16), label="free 4-bit var")
    finally:
        dp.close()


def test_range_divided_intra_uniform(libzsp):
    """``{[0:9] :/ 1, [10:19] :/ 3}``: right aggregate AND uniform within each block.

    test_dist only checks the 25/75 split; the *quality* claim is that ``:/``
    spreads its mass uniformly across the range, so all ten values in each
    block are equally likely.
    """
    dp = DistProblem(libzsp)
    dp.add_var(0, width=8, lo=0, hi=19)
    dp.add_dist(0, [(0, 9, 1, False), (10, 19, 3, False)])
    try:
        hist = dp.sample(0, N)
        assert_support(hist, range(20))
        # Aggregate 25 / 75 over the two blocks.
        low = sum(hist.get(v, 0) for v in range(0, 10))
        high = sum(hist.get(v, 0) for v in range(10, 20))
        assert_gof({"low": low, "high": high},
                   {"low": 0.25, "high": 0.75}, label=":/ aggregate 25/75")
        # Intra-block uniformity — the new coverage.
        assert_uniform(restrict(hist, range(0, 10)), range(0, 10),
                       label=":/ low block [0:9] intra-uniform")
        assert_uniform(restrict(hist, range(10, 20)), range(10, 20),
                       label=":/ high block [10:19] intra-uniform")
    finally:
        dp.close()


def test_per_value_weight_uniform(libzsp):
    """``[0:9] := 3``: per-value weight — every value equally likely (uniform)."""
    dp = DistProblem(libzsp)
    dp.add_var(0, width=8, lo=0, hi=9)
    dp.add_dist(0, [(0, 9, 3, True)])
    try:
        hist = dp.sample(0, N)
        assert_uniform(hist, range(10), label=":= per-value uniform over [0:9]")
    finally:
        dp.close()


def test_per_value_vs_range_divided_marginals(libzsp):
    """``:=`` and ``:/`` give the different marginals the LRM requires.

    With a range and a single point:
      * ``{[0:9] := 1, 10 := 1}`` — 11 equally weighted values, so P(10)=1/11.
      * ``{[0:9] :/ 1, 10 :/ 1}`` — two equal *bins*, so P(10)=1/2.
    Same syntax modulo ``:=`` vs ``:/``, an order-of-magnitude different mass
    on the point. This is exactly the semantic test_dist doesn't isolate.
    """
    # Per-value :=  -> P(10) = 1/11
    dp1 = DistProblem(libzsp)
    dp1.add_var(0, width=8, lo=0, hi=10)
    dp1.add_dist(0, [(0, 9, 1, True), (10, 10, 1, True)])
    try:
        h1 = dp1.sample(0, N)
        assert_support(h1, range(11))
        assert_uniform(h1, range(11), label=":= 11 equal values")  # incl. P(10)=1/11
    finally:
        dp1.close()

    # Range-divided :/ -> P(10) = 1/2
    dp2 = DistProblem(libzsp)
    dp2.add_var(0, width=8, lo=0, hi=10)
    dp2.add_dist(0, [(0, 9, 1, False), (10, 10, 1, False)])
    try:
        h2 = dp2.sample(0, N)
        assert_support(h2, range(11))
        p_low = 0.5 / 10  # half the mass spread over the ten low values
        expect = {v: p_low for v in range(10)}
        expect[10] = 0.5
        assert_gof(h2, expect, label=":/ point gets half the mass")
    finally:
        dp2.close()


def test_zero_weight_range_excluded_rest_uniform(libzsp):
    """``{[0:9] := 0, [10:19] := 1}``: low block never appears, high stays uniform."""
    dp = DistProblem(libzsp)
    dp.add_var(0, width=8, lo=0, hi=19)
    dp.add_dist(0, [(0, 9, 0, True), (10, 19, 1, True)])
    try:
        hist = dp.sample(0, N)
        assert_support(hist, range(10, 20))  # 0..9 must be absent
        assert_uniform(hist, range(10, 20), label="zero-weight excluded, rest uniform")
    finally:
        dp.close()


def test_all_zero_weight_uniform_fallback(libzsp):
    """All-zero dist degrades to a uniform draw over the *full* variable domain.

    dv-solve treats a dist whose total effective weight is 0 as "no usable
    weighting" and falls back to ``_rand_range64(lo, hi)`` over the whole
    declared domain (zsp_search.c:_pick_value_dist). This pins that
    documented behavior: solves still succeed and the result is uniform over
    [0,19] — including values outside the (all-zero) dist ranges.
    """
    dp = DistProblem(libzsp)
    dp.add_var(0, width=8, lo=0, hi=19)
    dp.add_dist(0, [(0, 5, 0, True), (10, 15, 0, False)])
    try:
        hist = dp.sample(0, N)
        assert_support(hist, range(20))
        # Values outside both dist ranges DO appear (fallback = full domain).
        outside = sum(hist.get(v, 0) for v in (6, 7, 8, 9, 16, 17, 18, 19))
        assert outside > 0, "all-zero dist should fall back to the full domain"
        assert_uniform(hist, range(20), label="all-zero dist -> full-domain uniform")
    finally:
        dp.close()


# ------------------------------------------------------------------ #
# Harness extensions: hard-constraint interaction + joint independence #
# ------------------------------------------------------------------ #

def test_two_free_vars_independent(libzsp):
    """Two constraint-independent 3-bit rand vars: jointly uniform AND independent.

    Both are assigned from the same solve; the randomizer must not couple
    them (equal marginals aren't enough — the *joint* must factor).
    """
    dp = DistProblem(libzsp)
    dp.add_var(0, width=3, lo=0, hi=7)
    dp.add_var(1, width=3, lo=0, hi=7)
    try:
        joint = dp.sample_joint([0, 1], N)
        cells = [(x, y) for x in range(8) for y in range(8)]
        assert_support(joint, cells)
        # Each marginal uniform...
        assert_uniform(marginal(joint, 0), range(8), label="var0 marginal")
        assert_uniform(marginal(joint, 1), range(8), label="var1 marginal")
        # ...and the joint factors (no correlation between the two draws).
        assert_independent(joint, range(8), range(8), label="two free 3-bit vars")
    finally:
        dp.close()


def test_dist_renormalizes_under_hard_constraint(libzsp):
    """A hard bound clips the feasible domain; ``:/`` re-normalizes over it.

    ``{[0:9] :/ 1, [10:19] :/ 3}`` with hard ``x >= 5``: values < 5 must
    vanish, the low block shrinks to {5..9} but still carries its 25% share
    (``:/`` weight is per-range, independent of how many values survive), and
    both surviving blocks stay uniform. This drives the feasible-domain
    intersection in ``_pick_value_dist`` (zsp_search.c).
    """
    dp = DistProblem(libzsp)
    dp.add_var(0, width=8, lo=0, hi=19)
    dp.add_dist(0, [(0, 9, 1, False), (10, 19, 3, False)])
    dp.constrain(0, BIN_GTE, 5)      # x >= 5
    try:
        hist = dp.sample(0, N)
        assert_support(hist, range(5, 20))         # soundness: nothing < 5
        low = sum(hist.get(v, 0) for v in range(5, 10))
        high = sum(hist.get(v, 0) for v in range(10, 20))
        assert_gof({"low": low, "high": high},
                   {"low": 0.25, "high": 0.75},
                   label=":/ re-normalized 25/75 over feasible domain")
        assert_uniform(restrict(hist, range(5, 10)), range(5, 10),
                       label="clipped low block {5..9} intra-uniform")
        assert_uniform(restrict(hist, range(10, 20)), range(10, 20),
                       label="high block intra-uniform under constraint")
    finally:
        dp.close()


def test_dist_range_clamped_to_domain(libzsp):
    """A dist range wider than the variable domain is clamped, not overrun.

    ``[10:30] :/ 1`` on an 8-bit var declared ``[0:19]``: the effective range
    is the intersection [10:19]; no value outside the domain appears and the
    surviving range is uniform.
    """
    dp = DistProblem(libzsp)
    dp.add_var(0, width=8, lo=0, hi=19)
    dp.add_dist(0, [(10, 30, 1, False)])
    try:
        hist = dp.sample(0, N)
        assert_support(hist, range(10, 20))
        assert_uniform(hist, range(10, 20), label="dist range clamped to [10:19]")
    finally:
        dp.close()
