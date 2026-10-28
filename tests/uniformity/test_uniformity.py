"""Checks of the uniformity harness itself, and the soundness of every arm.

Uniformity verdicts do not gate here: the 0.x samplers are known not to be
uniform (that is what the harness measures, see README.md). What gates:
  - the statistics reject a biased sampler and accept a uniform one;
  - the `uniform` arm passes every family (the harness's own noise floor);
  - every dv-solve arm answers every call with a model inside the solution set.
"""
from __future__ import annotations

import random
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))

import corpus  # noqa: E402
import families  # noqa: E402
import uniformity as U  # noqa: E402

_BUILD = _HERE.parents[1] / "build"
_TOOLS = U.Tools(_BUILD / "dv-solve-smt2", _BUILD / "bench_vlt_replay")
_HAVE_TOOLS = _TOOLS.smt2.exists() and _TOOLS.replay.exists()

pytest.importorskip("z3")
pytest.importorskip("scipy")

FAMILIES = families.all_problems()


def test_score_accepts_uniform_rejects_biased():
    space = [(v,) for v in range(50)]
    r = random.Random(7)
    good = [space[r.randrange(50)] for _ in range(2000)]
    assert U.score(good, space, ["x"], 1e-3)["verdict"] == "PASS"
    # Half the mass on one value: the shape of a sampler stuck near its base model.
    bad = [space[0] if r.random() < 0.5 else space[r.randrange(50)] for _ in range(2000)]
    s = U.score(bad, space, ["x"], 1e-3)
    assert s["verdict"] == "FAIL" and "joint" in s["failed_tests"]


def test_distinct_test_catches_a_stuck_sampler():
    """N far below the solution count: only the distinct test has power."""
    space = [(v,) for v in range(4000)]
    r = random.Random(3)
    good = [space[r.randrange(4000)] for _ in range(600)]
    assert U.score(good, space, ["x"], 1e-3)["verdict"] == "PASS"
    near = [space[r.randrange(300) * 13] for _ in range(600)]   # 300 values, spread out
    s = U.score(near, space, ["x"], 1e-3)
    assert s["verdict"] == "FAIL" and "distinct" in s["failed_tests"]


def test_score_flags_unsound_and_unanswered():
    space = [(1,), (2,)]
    assert U.score([(1,), (3,)], space, ["x"], 1e-3)["verdict"] == "UNSOUND"
    assert U.score([(1,), None], space, ["x"], 1e-3)["verdict"] == "UNANSWERED"


def test_seeds_reproducible_and_spread():
    a, b = U.seeds(100, 1), U.seeds(100, 1)
    assert a == b and len(set(a)) == 100 and all(0 < s < 2 ** 31 for s in a)


def test_families_enumerable():
    for p in FAMILIES:
        sp = U.enumerate_solutions(p, 4096)
        assert sp, f"{p.name}: empty or more than 4096 solutions"


def test_uniform_arm_passes_every_family():
    for p in FAMILIES:
        sp = U.enumerate_solutions(p, 4096)
        r = U.score(U.draw("uniform", p, max(1000, 5 * len(sp)), 1, _TOOLS, sp)[0],
                    sp, p.names, 1e-3)
        assert r["verdict"] == "PASS", (p.name, r)


@pytest.mark.skipif(not _HAVE_TOOLS, reason="needs build/dv-solve-smt2 and bench_vlt_replay")
@pytest.mark.parametrize("arm", ["default", "cdcl", "verilator", "api-cache"])
def test_arm_sound(arm):
    """Every model of every arm satisfies the constraints (small N)."""
    bad = []
    for p in FAMILIES:
        sp = U.enumerate_solutions(p, 4096)
        samples, _ = U.draw(arm, p, 40, 3, _TOOLS, sp)
        r = U.score(samples, sp, p.names, 1e-3)
        if r["verdict"] in ("UNSOUND", "UNANSWERED"):
            bad.append((p.name, r["verdict"], r.get("offspace"), r["answered"]))
    assert not bad, bad


@pytest.mark.skipif(not _HAVE_TOOLS, reason="needs build/dv-solve-smt2 and bench_vlt_replay")
@pytest.mark.parametrize("name", ["fam.dep_range", "fam.sum_eq", "fam.xor_parity"])
def test_api_cache_moves(name):
    """A reused compiled context gives a new sample per seed (resample), not
    re-flips of one cached model: in 0.3 these hit 1-9 distinct solutions of
    hundreds over 200 calls."""
    p = next(f for f in FAMILIES if f.name == name)
    sp = U.enumerate_solutions(p, 4096)
    samples, _ = U.draw("api-cache", p, 200, 5, _TOOLS, sp)
    r = U.score(samples, sp, p.names, 1e-3)
    assert r["verdict"] not in ("UNSOUND", "UNANSWERED"), r
    assert len(set(samples)) >= min(len(sp), 200) // 3, (name, len(set(samples)), len(sp))


def test_corpus_roundtrip(tmp_path):
    sess = tmp_path / "s.smt2"
    sess.write_text("(set-logic QF_BV)\n(check-sat)\n"
                    "(reset)\n(set-option :random-seed 5)\n(set-logic QF_BV)\n"
                    "(declare-fun x () (_ BitVec 4))\n(assert (bvult x #x3))\n"
                    "(check-sat)\n(assert (= x #x1))\n(check-sat)\n(get-value ( x))\n")
    f = corpus.extract(sess, "t.x", tmp_path / "c")
    (p,) = corpus.load(f)
    assert p.names == ["x"]
    # The stack at the first sat check-sat: the parity assert after it is dropped.
    assert not any("#x1" in l for l in p.body)
    assert len(U.enumerate_solutions(p, 100)) == 3
