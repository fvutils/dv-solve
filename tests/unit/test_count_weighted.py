"""A seeded fair_pick solve is uniform over solutions on shapes that defeat
value-at-a-time sampling (docs/accuracy_closure_plan.md).

Each shape has far fewer solutions under some values than others:
- `cond_weight`: a condition variable selects a length limit.
- `order_chain`: an ordering chain.
- `sum_unique`, `linear_sys`, `sum3_const`: sums under an equality.

Drawing the first-decided variable uniformly, or deciding one variable at a
time, skews the result. The problems and their exact truth come from
tests/perf/rand_benches.py. Draws are pooled over several solver sessions,
because a session's tier choice (box, enumeration or search) is made once per
context, and one lucky or unlucky choice should not decide the test.

The gate is the plan's: a chi-square test at p > 1e-3, with the bins whose
expected count is below 5 pooled. The seeds are fixed, so a result is
deterministic.

Tests marked xfail(strict) are known failures. The step that fixes each one
removes its mark.
"""
from __future__ import annotations

import random
from collections import Counter

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, SOLVE_OK
from tests.formal.soundness.doors import _lower
from tests.formal.soundness.ir import ev
from tests.perf.rand_benches import Bench

SESSIONS = 8
PER_SESSION = 500
ALPHA = 1e-3


def _draw(b: Bench, order, sessions=SESSIONS, per=PER_SESSION, base=1):
    """Models (dicts) from `sessions` fresh contexts, variables declared in `order`."""
    ids = {k: i for i, k in enumerate(order)}
    out = []
    for s in range(sessions):
        bld = SolveProblemBuilder()
        try:
            for k in order:
                w = b.widths[k]
                bld.add_var(ids[k], width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
            for c in b.cons:
                bld.add_constraint(_lower(bld, c, ids))
            buf, _ = bld.finalize()
        finally:
            bld.destroy()
        rng = random.Random(base * 1000 + s)
        with SolveCtx(buf) as ctx:
            for _ in range(per):
                ctx.reset()
                assert ctx.solve(seed=rng.getrandbits(63) + 1, fair_pick=True) == SOLVE_OK
                out.append({k: ctx.get_value(ids[k]) & ((1 << b.widths[k]) - 1) for k in order})
    return out


def _pooled_chi2p(hist: Counter, truth: dict, n: int) -> float:
    from scipy.stats import chi2
    stat, df, po, pe = 0.0, -1, 0, 0.0
    for k, p in truth.items():
        if n * p >= 5:
            stat += (hist.get(k, 0) - n * p) ** 2 / (n * p)
            df += 1
        else:
            po += hist.get(k, 0)
            pe += n * p
    if pe > 0:
        stat += (po - pe) ** 2 / pe
        df += 1
    return float(chi2.sf(stat, max(df, 1)))


def _check(name, order=None):
    b = Bench(name)
    key, truth = b.bins[0], b.bins[1]
    models = _draw(b, order or b.names)
    assert all(all(ev(c, m) for c in b.cons) for m in models), "a model breaks a constraint"
    hist = Counter(key(m) for m in models)
    off = [k for k in hist if k not in truth]
    assert not off, f"draws outside the solution space: {off[:5]}"
    p = _pooled_chi2p(hist, truth, len(models))
    top = sorted(truth, key=lambda k: -abs(hist.get(k, 0) / len(models) - truth[k]))[:3]
    worst = {k: (round(hist.get(k, 0) / len(models), 4), round(truth[k], 4)) for k in top}
    assert p > ALPHA, f"{name}: chi2 p={p:.3g}; worst bins (got, truth): {worst}"


# cond_weight, with the condition declared first (as an SV class would) and
# last. Before A1 the first was biased: the box tier's probe passed by luck,
# and the solves its draws missed went to the search.
def test_cond_weight_condition_first():
    _check("cond_weight", order=["kind", "len"])


def test_cond_weight_condition_last():
    _check("cond_weight", order=["len", "kind"])


@pytest.mark.xfail(strict=True, reason="A3: uniform value draws in MRV order")
@pytest.mark.parametrize("k", [0, 5, 9])
def test_order_chain(k):
    _check(f"order_chain_a{k}")


@pytest.mark.xfail(strict=True, reason="A4: sums under an equality")
@pytest.mark.parametrize("name", ["sum_unique", "linear_sys", "sum3_const"])
def test_equality_sums(name):
    _check(name)
