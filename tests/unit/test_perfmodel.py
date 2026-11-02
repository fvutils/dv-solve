"""The expected-cost check: the best case per shape, and our delta from it
(dv_solve.perfmodel; docs/solve_stats_capture_plan.md, S5a)."""
from __future__ import annotations

import os
import random
import subprocess
import sys
from pathlib import Path

import pytest

from dv_solve import perfmodel as P
from dv_solve import stats as S

REPO = Path(__file__).resolve().parents[2]
K = {"cdcl": 2.0}                       # ns per CDCL work unit in these tests


def _class(i, vars_, cons, work, motif="var-cmp-var", solves=50):
    """A class record as a stats file holds it: `work` CDCL units per solve."""
    return {
        "class": f"{i:016x}", "solves": solves, "wall_ns": int(K["cdcl"] * work * solves),
        "paths": {"api": {"solves": solves, "sat": solves}},
        "results": {"sat": solves}, "answered_by": {"cdcl": solves},
        "engine_ran": {"cdcl": solves}, "work": {"cdcl": int(work * solves)},
        "features": {"vars": vars_, "constraints": cons, "nodes": vars_ + cons,
                     "widths": {"8": vars_}, "ops": {"cmp": cons}},
        "motifs": {motif: cons},
    }


def _best(v, c):
    """The best-case work of a shape in these tests: a power law in its size."""
    return 3.0 * v * c ** 0.5


def _train(n=400, seed=1, penalty=1.0):
    """Training data: half the classes `bitwise`. Our arm costs `penalty`
    times the best case on `bitwise` classes; the best arm (the frontier) does
    not. Noise about 1.4x."""
    r = random.Random(seed)
    classes, frontier = [], {}
    for i in range(n):
        v, c = r.randint(1, 200), r.randint(1, 300)
        motif = "bitwise" if i % 2 else "var-cmp-var"
        best = _best(v, c) * 2 ** r.gauss(0, 0.5)
        ours = best * (penalty if motif == "bitwise" else 1.0)
        cl = _class(i, v, c, ours, motif)
        classes.append(cl)
        frontier[cl["class"]] = best / ours
    return {"stats": {"schema": S.FILE_SCHEMA, "dv_solve": "test",
                      "snapshot": {"classes": classes}},
            "frontier": frontier, "ns_per_work": K}


def _snap(*classes):
    return {"snapshot": {"classes": list(classes)}}


def test_features_and_cost():
    c = _class(1, 10, 20, 100)
    assert len(P.features(c)) == len(P.FEATURE_NAMES)
    assert P.domain(c) == "rand"
    assert P.cost(c, K) == pytest.approx(200.0)
    c["paths"] = {"smt2": {"solves": 50}}
    assert P.domain(c) == "smt2"
    assert P.features({"class": "x"}) is None
    # wasted work counts: a bitblast engine that ran first adds its cost
    c["work"]["bitblast"] = 50 * 100
    assert P.cost(c, {"cdcl": 2.0, "bitblast": 1.0}) == pytest.approx(300.0)


def test_planted_slowdown_is_flagged():
    pytest.importorskip("numpy")
    model = P.fit(_train())
    dm = model["domains"]["rand"]
    assert dm["cv_r2"] > 0.8 and dm["cv_rmse"] < 1.0
    probe = _class(999, 50, 100, _best(50, 100))
    slow = _class(998, 50, 100, 16 * _best(50, 100))
    got = {r["class"]: r for r in P.check(_snap(probe, slow), model)}
    assert not got[probe["class"]]["unexpected"]
    assert got[slow["class"]]["unexpected"] and 8 < got[slow["class"]]["ratio"] < 32
    assert got[slow["class"]]["recoverable_ns"] > 0.8 * slow["wall_ns"]


def test_status_quo_is_not_absorbed():
    """A slowdown we have on a whole motif, in our arm of the training data:
    a self-model learns it as normal; the best-case model still flags it."""
    pytest.importorskip("numpy")
    train = _train(penalty=8.0)
    ours = P.fit(train, self_model=True)
    best = P.fit(train)
    field = _class(997, 50, 100, 8 * _best(50, 100), motif="bitwise")
    (r_self,) = P.check(_snap(field), ours)
    (r_best,) = P.check(_snap(field), best)
    assert not r_self["unexpected"], "the self-model absorbs the motif-wide slowdown"
    assert r_best["unexpected"] and r_best["ratio"] > 4
    # the bias report names the motif, measured from the training arms
    gaps = {r["motif"]: r["gap"] for r in P.bias(train)}
    assert gaps["bitwise"] == pytest.approx(8.0, rel=0.01) and gaps["var-cmp-var"] == pytest.approx(1.0)
    assert P.bias(train)[0]["motif"] == "bitwise"


def test_enumeration_floor():
    """A class with few free bits never costs more than trying them all."""
    pytest.importorskip("numpy")
    model = P.fit(_train())
    c = _class(996, 150, 250, 50 * _best(150, 250))
    c["root"] = {"count": 1, "vars": 150, "fixed": 140, "tied": 0, "bits": 1200,
                 "free_bits": 4}
    (r,) = P.check(_snap(c), model)
    assert r["floored"] and r["unexpected"]
    assert r["expected"] == pytest.approx(__import__("math").log2(16 * 250 * K["cdcl"]))


def test_shipped_model_loads():
    m = P.load_model()
    assert m is not None, "src/dv_solve/perf_model.json is missing or stale (FEATURE_VERSION)"
    assert m["target"] == "best-case" and "rand" in m["domains"]
    assert len(m["features"]) == len(P.FEATURE_NAMES)


def test_check_command_and_bundle(tmp_path):
    """The check runs on a real stats file; a bundle carries its result."""
    out = tmp_path / "s.json"
    code = ("from dv_solve.builder import SolveProblemBuilder\n"
            "from dv_solve.ctx import SolveCtx\n"
            "from dv_solve.problem import BIN_LT\n"
            "b = SolveProblemBuilder()\n"
            "for i in (0, 1): b.add_var(i, 8, False, 0, 255)\n"
            "b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_var(1)))\n"
            "with SolveCtx(b.finalize()[0]) as c:\n"
            "    for s in range(8): c.reset(); c.solve(seed=s + 1)\n")
    env = dict(os.environ, DV_STATS=str(out))
    env.pop("LD_LIBRARY_PATH", None)
    env["PYTHONPATH"] = os.pathsep.join([str(REPO / "src"), env.get("PYTHONPATH", "")])
    r = subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, text=True,
                       timeout=120)
    assert r.returncode == 0, r.stderr
    r = subprocess.run([sys.executable, "-m", "dv_solve.perfmodel", "check", str(out)], env=env,
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0 and "classes predicted" in r.stdout, r.stdout + r.stderr
    data = S.load(str(out))
    (row,) = P.check(data)
    assert row["domain"] == "rand"
    b = S.bundle(data, None)
    (c,) = [c for c in b["snapshot"]["classes"] if c["class"] != S.OTHER_CLASS]
    assert c["expected"]["unexpected"] in (0, 1)
    assert S.audit(b) == []
