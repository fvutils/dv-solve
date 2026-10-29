"""The bit-blast engine's unique sampler (`_uniqsamp` in dvs_bbsolver.c).

`_uniqsamp` answers an all-different group (a clique of `!=`/`distinct` plus
unary bounds over one shared domain) by partial Fisher-Yates, with no SAT call.
Here every model is checked against the constraints as written, the sampler
must reach every tuple of a small problem with a uniform spread, and shapes it
must decline (not a clique, different domains, more vars than values) must
still get a right answer from the rest of the engine. DV_SIM_CHECK=1 makes the
engine re-check every fast-path answer with SAT and abort on a mismatch.

    env -u LD_LIBRARY_PATH packages/python/bin/python3 -m pytest -q tests/formal/test_uniqsamp.py
"""
from __future__ import annotations

import itertools
import os
import re
import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_BIN = _ROOT / "build" / "dv-solve-smt2"


def _run(text: str, uniqsamp: bool = True):
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env.update(DV_ENGINE="bitblast", DV_SIM_CHECK="1", DV_BB_STATS="1",
               DV_UNIQSAMP="1" if uniqsamp else "0")
    p = subprocess.run([str(_BIN)], input=text, capture_output=True,
                       text=True, env=env, timeout=120)
    assert "MISMATCH" not in p.stderr, p.stderr[-2000:]
    verdicts = re.findall(r"^(sat|unsat|unknown)$", p.stdout, re.M)
    models = []
    for blk in re.findall(r"^\(\(.*?\)\)$", p.stdout, re.M | re.S):
        vals = {}
        for name, lit in re.findall(r"\((\w+) (#[bx][0-9a-fA-F]+)\)", blk):
            vals[name] = int(lit[2:], 2 if lit[1] == "b" else 16)
        models.append(vals)
    return verdicts, models, p.stderr


def _block(seed: int, w: int, bounds: dict, neqs: list, names) -> str:
    s = [f"(reset)(set-option :produce-models true)(set-option :random-seed {seed})"
         "(set-logic QF_BV)"]
    for v in names:
        s.append(f"(declare-fun {v} () (_ BitVec {w}))")
    for v, b in bounds.items():
        s.append(f"(assert (bvult {v} (_ bv{b} {w})))")
    for grp in neqs:
        s.append(f"(assert (distinct {' '.join(grp)}))")
    s.append(f"(check-sat)(get-value ({' '.join(names)}))")
    return "\n".join(s) + "\n"


def _ok(vals: dict, bounds: dict, neqs: list) -> bool:
    if any(vals[v] >= b for v, b in bounds.items()):
        return False
    return all(len({vals[v] for v in g}) == len(g) for g in neqs)


def test_clique_uniform_and_complete():
    """3 vars < 5, all different: 60 tuples, every one reached, chi-square flat."""
    scipy_stats = pytest.importorskip("scipy.stats")
    names = ("x", "y", "z")
    bounds = {v: 5 for v in names}
    neqs = [list(names)]
    n = 1200
    text = "".join(_block(s, 3, bounds, neqs, names) for s in range(1, n + 1))
    verdicts, models, err = _run(text)
    assert verdicts == ["sat"] * n
    assert len(models) == n
    assert re.search(r"\[bb-uniqsamp\].*-> sat", err), err[-2000:]
    counts = {t: 0 for t in itertools.permutations(range(5), 3)}
    for m in models:
        assert _ok(m, bounds, neqs), m
        counts[(m["x"], m["y"], m["z"])] += 1
    assert len(counts) == 60 and all(c > 0 for c in counts.values()), counts
    p = scipy_stats.chisquare(list(counts.values())).pvalue
    assert p > 1e-4, p


@pytest.mark.parametrize("uniqsamp", [True, False])
@pytest.mark.parametrize("bounds,neqs,expect", [
    # not a clique: x!=y, y!=z (x==z allowed)
    ({"x": 5, "y": 5, "z": 5}, [["x", "y"], ["y", "z"]], "sat"),
    # different domains
    ({"x": 5, "y": 4, "z": 5}, [["x", "y", "z"]], "sat"),
    # k > |D|: 3 vars over 2 values
    ({"x": 2, "y": 2, "z": 2}, [["x", "y", "z"]], "unsat"),
    # k == |D|: only permutations
    ({"x": 3, "y": 3, "z": 3}, [["x", "y", "z"]], "sat"),
])
def test_shapes_stay_valid(bounds, neqs, expect, uniqsamp):
    names = ("x", "y", "z")
    n = 40
    text = "".join(_block(s, 3, bounds, neqs, names) for s in range(1, n + 1))
    verdicts, models, _ = _run(text, uniqsamp)
    assert verdicts == [expect] * n
    for m in models:
        assert _ok(m, bounds, neqs), m


def test_api_all_different_valid_and_complete():
    """Builder add_all_different over BVSatCtx: valid, and all 60 tuples reached."""
    from dv_solve.builder import SolveProblemBuilder
    from dv_solve.bvsat import BVSatCtx, BVSAT_SAT
    seen = set()
    for seed in range(600):
        b = SolveProblemBuilder()
        for i in range(3):
            b.add_var(i, 8, False, 0, 4)
        b.add_all_different([0, 1, 2])
        buf, _sz = b.finalize()
        bb = BVSatCtx(buf)
        try:
            assert bb.check(seed=seed) == BVSAT_SAT
            t = tuple(bb.value(i) for i in range(3))
        finally:
            bb.destroy()
            b.destroy()
        assert len(set(t)) == 3 and all(0 <= v <= 4 for v in t), t
        seen.add(t)
    assert len(seen) == 60, len(seen)
