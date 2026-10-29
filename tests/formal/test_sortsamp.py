"""The bit-blast engine's sorted sampler (`_sortsamp` in dvs_bbsolver.c).

`_sortsamp` answers ordered chains `x_i >= x_{i-1} + c` (strict, gapped or
non-strict) over one shared interval with a uniform sorted subset, with no SAT
call. Here every model is checked against the constraints as written, the
sampler must reach every tuple of a small chain with a uniform spread, and
shapes it must decline (cycle, branch, wrap, a non-interval bound, mixed
domains) must still get a right answer from the rest of the engine.
DV_SIM_CHECK=1 makes the engine re-check every fast-path answer with SAT and
abort on a mismatch.

    env -u LD_LIBRARY_PATH packages/python/bin/python3 -m pytest -q tests/formal/test_sortsamp.py
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
_W = 4
_NAMES = ("x0", "x1", "x2")


def _run(text: str, sortsamp: bool = True):
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env.update(DV_ENGINE="bitblast", DV_SIM_CHECK="1", DV_BB_STATS="1",
               DV_SORTSAMP="1" if sortsamp else "0")
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


def _bv(v: int) -> str:
    return f"(_ bv{v} {_W})"


def _edge(x: str, y: str, c: int) -> str:
    """x >= y + c; c=1 is written as a strict compare, c=0 as >=."""
    if c == 0:
        return f"(bvuge {x} {y})"
    if c == 1:
        return f"(bvugt {x} {y})"
    return f"(bvuge {x} (bvadd {y} {_bv(c)}))"


def _block(seed: int, bounds: dict, edges: list, extra=(), names=_NAMES) -> str:
    s = [f"(reset)(set-option :produce-models true)(set-option :random-seed {seed})"
         "(set-logic QF_BV)"]
    for v in names:
        s.append(f"(declare-fun {v} () (_ BitVec {_W}))")
    for v, b in bounds.items():
        s.append(f"(assert (bvult {v} {_bv(b)}))")
    for x, y, c in edges:
        s.append(f"(assert {_edge(x, y, c)})")
    for e in extra:
        s.append(f"(assert {e})")
    s.append(f"(check-sat)(get-value ({' '.join(names)}))")
    return "\n".join(s) + "\n"


def _ok(vals: dict, bounds: dict, edges: list) -> bool:
    mask = (1 << _W) - 1
    if any(vals[v] >= b for v, b in bounds.items()):
        return False
    return all(vals[x] >= ((vals[y] + c) & mask) for x, y, c in edges)


def _chain(c: int):
    return [("x1", "x0", c), ("x2", "x1", c)]


@pytest.mark.parametrize("c,K,ntup", [
    (1, 6, 20),     # strict: 3-subsets of [0,6)
    (2, 9, 35),     # gap 2: M = 9 - 2 = 7
    (0, 6, 56),     # non-strict: multisets of size 3 over [0,6)
])
def test_chain_uniform_and_complete(c, K, ntup):
    scipy_stats = pytest.importorskip("scipy.stats")
    bounds = {v: K for v in _NAMES}
    edges = _chain(c)
    n = 20 * ntup
    text = "".join(_block(s, bounds, edges) for s in range(1, n + 1))
    verdicts, models, err = _run(text)
    assert verdicts == ["sat"] * n
    assert len(models) == n
    assert len(re.findall(r"\[bb-sortsamp\].*-> sat", err)) == n, err[-2000:]
    counts = {t: 0 for t in itertools.product(range(K), repeat=3)
              if _ok(dict(zip(_NAMES, t)), bounds, edges)}
    assert len(counts) == ntup
    for m in models:
        assert _ok(m, bounds, edges), m
        counts[tuple(m[v] for v in _NAMES)] += 1
    assert all(k > 0 for k in counts.values()), counts
    p = scipy_stats.chisquare(list(counts.values())).pvalue
    assert p > 1e-4, p


_B6 = {v: 6 for v in _NAMES}

# (bounds, edges, extra asserts, expected verdict, [bb-sortsamp] reason or None)
_SHAPES = {
    "cycle-sat": (_B6, [("x1", "x0", 0), ("x2", "x1", 0), ("x0", "x2", 0)], (),
                  "sat", "path"),
    "cycle-unsat": (_B6, [("x1", "x0", 1), ("x2", "x1", 1), ("x0", "x2", 1)], (),
                    "unsat", None),
    "branch": (_B6, [("x1", "x0", 1), ("x2", "x0", 1)], (), "sat", "path"),
    # x2 >= x1 + 3 with x < 14 in 4 bits: x1 + 3 can wrap
    "wrap": ({v: 14 for v in _NAMES}, [("x1", "x0", 1), ("x2", "x1", 3)], (),
             "sat", "wrap"),
    "not-interval": (_B6, _chain(1), (f"(distinct x1 {_bv(2)})",), "sat", "domain"),
    "mixed-domains": ({"x0": 6, "x1": 5, "x2": 6}, _chain(1), (), "sat", "domain"),
    "tight": ({v: 3 for v in _NAMES}, _chain(1), (), "sat", None),
    "empty": ({v: 2 for v in _NAMES}, _chain(1), (), "unsat", None),
}


@pytest.mark.parametrize("sortsamp", [True, False])
@pytest.mark.parametrize("name", list(_SHAPES))
def test_shapes_stay_valid(name, sortsamp):
    bounds, edges, extra, expect, why = _SHAPES[name]
    n = 30
    text = "".join(_block(s, bounds, edges, extra) for s in range(1, n + 1))
    verdicts, models, err = _run(text, sortsamp)
    assert verdicts == [expect] * n
    for m in models:
        assert _ok(m, bounds, edges), m
        if extra:
            assert m["x1"] != 2, m
    reasons = re.findall(r"\[bb-sortsamp\].*-> (\w+)", err)
    if not sortsamp:
        assert reasons == [], reasons
    elif why is not None:
        assert reasons and set(reasons) == {why}, reasons
    if name == "tight" and sortsamp:
        assert all(tuple(m[v] for v in _NAMES) == (0, 1, 2) for m in models)


def test_api_strict_chain_valid_and_complete():
    """Builder x0 < x1 < x2 over [0,5] via BVSatCtx: valid, all 20 tuples reached."""
    from dv_solve.builder import SolveProblemBuilder
    from dv_solve.bvsat import BVSatCtx, BVSAT_SAT
    from dv_solve.problem import BIN_GT
    seen = set()
    for seed in range(400):
        b = SolveProblemBuilder()
        for i in range(3):
            b.add_var(i, 8, False, 0, 5)
        for i in (1, 2):
            b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(i), b.expr_var(i - 1)))
        buf, _sz = b.finalize()
        bb = BVSatCtx(buf)
        try:
            assert bb.check(seed=seed) == BVSAT_SAT
            t = tuple(bb.value(i) for i in range(3))
        finally:
            bb.destroy()
            b.destroy()
        assert t[0] < t[1] < t[2] <= 5, t
        seen.add(t)
    assert len(seen) == 20, len(seen)
