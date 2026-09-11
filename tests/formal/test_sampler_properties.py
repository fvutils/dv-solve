"""Sampler properties of the CDCL engine (B18/B19/B21 regressions).

These guard the three things that have to hold for CDCL to be usable as a
constrained-random sampler at all. Each failed at some point:

  B19  successive solves in ONE process must produce varied models
       (the random phase init used to run only on the solve that allocated
       phase_save, so every later seed returned the identical model)
  B18  a seeded solve must not be slower than an unseeded one
       (a phase-saved value in a domain hole bypassed _pick_avoiding_holes and
       got re-picked every restart -> full wall-clock timeout -> unknown)
  B21  SEQUENTIAL seeds must give uncorrelated samples
       (xorshift64 seeded with 1,2,3,... walked an arithmetic progression;
       Verilator mode uses exactly sequential seeds via ++div_counter)

Plus: seed 0 must stay deterministic, because the BMC/decision path relies on it.

Usage:
    direnv exec . pytest tests/formal/test_sampler_properties.py -v
"""
from __future__ import annotations

import os
import re
import subprocess
import time
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_DV = _REPO / "build" / "dv-solve-smt2"
_VAL = re.compile(r"#b([01]+)")

# Pure CDCL: without this an `unknown` escalates to bitblast and we would be
# testing bitblast's sampler while believing we tested CDCL's.
_PURE = {"DV_NO_BITBLAST": "1", "DV_CDCL_TIME_LIMIT": "10"}

# A domain with a HOLE -- x in {[10:20]} U {[100:150]}. This is the B18 shape.
_HOLED = ("(declare-fun x () (_ BitVec 8))\n"
          "(assert (or (and (bvuge x #x0a) (bvule x #x14)) "
          "(and (bvuge x #x64) (bvule x #x96))))\n")
_PLAIN = "(declare-fun x () (_ BitVec 8))\n(assert (bvult x #xfa))\n"
# Coupled pair: a + b == 100. This is the shape that exposed B21 -- with
# unmixed sequential seeds `a` walked +65 (mod 256) forever. A single free
# variable does NOT exhibit it, so the seed-correlation guards must use this.
_COUPLED = ("(declare-fun a () (_ BitVec 8))\n(declare-fun b () (_ BitVec 8))\n"
            "(assert (= (bvadd a b) #x64))\n")


def _draw(body: str, seeds, env=None, timeout=120, var="x") -> list[int]:
    if not _DV.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    cmds = ["(set-logic QF_BV)", body.rstrip("\n")]
    for s in seeds:
        cmds += [f"(set-option :seed {s})", "(check-sat)", f"(get-value ({var}))"]
    cmds.append("(exit)")
    p = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                       input="\n".join(cmds) + "\n", capture_output=True,
                       text=True, timeout=timeout,
                       env={**os.environ, **_PURE, **(env or {})})
    return [int(m, 2) for m in _VAL.findall(p.stdout)]


@pytest.mark.parametrize("body,label", [(_PLAIN, "plain_range"), (_HOLED, "holed")])
def test_b19_varies_within_one_process(body: str, label: str) -> None:
    """40 sequential seeds in ONE process must not collapse to a single value."""
    vals = _draw(body, range(1, 41))
    assert len(vals) == 40, f"{label}: only got {len(vals)} models"
    assert len(set(vals)) >= 15, \
        f"{label}: {len(set(vals))} distinct of 40 -- sampler is near-constant"


def test_b21_sequential_seeds_are_not_an_arithmetic_progression() -> None:
    """Sequential seeds must not walk a fixed stride.

    The regression: `a + b == 100` returned a = 65, 130, 195, 4, 69, ... i.e.
    +65 (mod 256) forever. Detect it generically -- if one first-difference
    dominates, the "sampler" is really a linear sweep.
    """
    vals = _draw(_COUPLED, range(1, 65), var="a")
    assert len(vals) == 64
    diffs = [(vals[i + 1] - vals[i]) % 256 for i in range(len(vals) - 1)]
    top = max(set(diffs), key=diffs.count)
    share = diffs.count(top) / len(diffs)
    assert share < 0.5, (
        f"stride {top} accounts for {share:.0%} of consecutive differences "
        f"-- samples are a linear sweep, not draws")


def test_b21_collisions_occur_as_iid_sampling_predicts() -> None:
    """A linear sweep never repeats; iid sampling does.

    64 draws of `a` from its 256 legal values: the birthday bound makes >=1
    collision overwhelmingly likely (p ~ 99.9%). Zero collisions signals a
    permutation sweep rather than independent draws.
    """
    vals = _draw(_COUPLED, range(1, 65), var="a")
    assert len(vals) - len(set(vals)) >= 1, \
        "no repeats in 64 draws from 250 values -- samples are not independent"


def test_b18_seeded_solve_on_holed_domain_is_fast() -> None:
    """A seeded solve must not blow the CDCL time budget on a holed domain.

    Was: 5 s -> `unknown`, because the phase-saved value sat in the gap and was
    re-picked every restart. Should be milliseconds. Generous bound so this
    fails on the pathology, not on a slow machine.
    """
    t0 = time.perf_counter()
    vals = _draw(_HOLED, [5], timeout=60)
    dt = time.perf_counter() - t0
    assert vals, "seeded solve on a holed domain returned no model (B18)"
    assert dt < 5.0, f"seeded holed-domain solve took {dt:.1f}s (B18 pathology)"


def test_b18_samples_land_only_in_legal_bands() -> None:
    """Soundness: every drawn value must satisfy the disjunctive constraint."""
    vals = _draw(_HOLED, range(1, 61))
    assert vals
    bad = [v for v in vals if not (0x0A <= v <= 0x14 or 0x64 <= v <= 0x96)]
    assert not bad, f"models outside the constraint: {sorted(set(bad))[:8]}"


def test_b18_both_bands_get_sampled() -> None:
    """Coverage: a sampler stuck in one band of a disjunction is useless.

    The lower band is 11 of 62 solutions (~18%), so 60 draws should hit it.
    """
    vals = _draw(_HOLED, range(1, 61))
    lo = sum(1 for v in vals if v <= 0x14)
    assert lo > 0, "no samples in the lower band"
    assert lo < len(vals), "no samples in the upper band"


def test_seed_zero_stays_deterministic() -> None:
    """The BMC/decision path depends on seed 0 being reproducible."""
    a = _draw(_PLAIN, [0, 0, 0])
    b = _draw(_PLAIN, [0, 0, 0])
    assert len(set(a)) == 1, f"seed 0 varied within a run: {a}"
    assert a == b, f"seed 0 differed across runs: {a} vs {b}"
