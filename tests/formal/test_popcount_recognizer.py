"""The SMT2 front end turns Verilator's `$countones(x)` lowering into one
countones constraint (`_tr_popcount` in smt2_frontend.c).

Verilator writes `$countones(x)` as a bvadd tree with one leaf per bit of x:
`(bvand x #x..01)` for bit 0 and `(bvlshr (bvand x 2^i) i)` for bit i. Every
case here is checked against the sum as written, evaluated in Python, so the
near misses the recognizer must refuse (a missing or repeated bit, a second
variable, a wrong shift) are checked as well as the shapes it accepts. Each
runs on the default route, CDCL and bitblast, with the recognizer on and off.

    env -u LD_LIBRARY_PATH packages/python/bin/python3 -m pytest -q tests/formal/test_popcount_recognizer.py
"""
from __future__ import annotations

import os
import random
import re
import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_BIN = _ROOT / "build" / "dv-solve-smt2"

_ENGINES = [None, "cdcl", "bitblast"]


def _c(v: int, w: int) -> str:
    return f"(_ bv{v} {w})"


def _leaf(var: str, w: int, mask_bit: int, shift: int, swap: bool) -> str:
    a = (_c(1 << mask_bit, w), var) if swap else (var, _c(1 << mask_bit, w))
    e = f"(bvand {a[0]} {a[1]})"
    return e if shift == 0 else f"(bvlshr {e} {_c(shift, w)})"


def _tree(leaves: list[str], shape: str, rng: random.Random) -> str:
    if shape == "nary":
        return "(bvadd " + " ".join(leaves) + ")" if len(leaves) > 1 else leaves[0]
    if shape == "left":
        e = leaves[0]
        for l in leaves[1:]:
            e = f"(bvadd {e} {l})"
        return e
    # balanced / random split
    if len(leaves) == 1:
        return leaves[0]
    k = rng.randint(1, len(leaves) - 1)
    return f"(bvadd {_tree(leaves[:k], shape, rng)} {_tree(leaves[k:], shape, rng)})"


def _value(vals: dict, spec, w: int) -> int:
    s = 0
    for var, mb, sh, _ in spec:
        s += (vals[var] & (1 << mb)) >> sh
    return s & ((1 << w) - 1)


def _run(text: str, engine, popcnt: bool):
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    if engine:
        env["DV_ENGINE"] = engine
    env["DV_POPCNT"] = "1" if popcnt else "0"
    p = subprocess.run([str(_BIN)], input=text, capture_output=True,
                       text=True, env=env, timeout=60)
    out = p.stdout.split()
    assert out and out[0] in ("sat", "unsat"), p.stdout + p.stderr
    vals = {}
    for name, lit in re.findall(r"\((\w+) (#[bx][0-9a-fA-F]+)\)", p.stdout):
        vals[name] = int(lit[2:], 2 if lit[1] == "b" else 16)
    return out[0], vals


def _cases():
    rng = random.Random(7)
    cases = []
    for w in (2, 3, 8, 16, 32, 64):
        for shape in ("left", "nary", "split"):
            for kind in ("exact", "missing", "dup", "mixed", "badshift"):
                spec = [("x", i, i, rng.random() < 0.3 and i == 0) for i in range(w)]
                if kind == "missing":
                    spec.pop(rng.randrange(w))
                elif kind == "dup":
                    j = rng.randrange(w)
                    spec[rng.randrange(w)] = spec[j]
                elif kind == "mixed":
                    i = rng.randrange(w)
                    spec[i] = ("y", i, i, False)
                elif kind == "badshift":
                    i = rng.randrange(1, w)
                    spec[i] = ("x", i, i - 1, False)
                rng.shuffle(spec)
                cmp = rng.choice(["=", "bvuge", "bvule"])
                k = rng.randint(0, w + 1)
                cases.append((w, shape, kind, spec, cmp, k, rng.randint(0, 1 << 30)))
    return cases


@pytest.mark.skipif(not _BIN.exists(), reason="build/dv-solve-smt2 not built")
@pytest.mark.parametrize("case", _cases(), ids=lambda c: f"w{c[0]}-{c[1]}-{c[2]}-{c[4]}{c[5]}")
def test_popcount_shapes(case):
    w, shape, kind, spec, cmp, k, seed = case
    rng = random.Random(seed)
    leaves = [_leaf(v, w, mb, sh, sw) for v, mb, sh, sw in spec]
    tree = _tree(leaves, shape, rng)
    text = (f"(set-logic QF_BV)\n(declare-fun x () (_ BitVec {w}))\n"
            f"(declare-fun y () (_ BitVec {w}))\n"
            f"(assert ({cmp} {tree} {_c(k, w) if k < (1 << w) else _c(0, w)}))\n"
            "(check-sat)\n(get-value (x y))\n")
    kk = k if k < (1 << w) else 0
    # Brute force the answer where small enough; else trust sat models only.
    want = None
    if w <= 8:
        hold = {"=": lambda s: s == kk, "bvuge": lambda s: s >= kk,
                "bvule": lambda s: s <= kk}[cmp]
        want = "sat" if any(hold(_value({"x": a, "y": b}, spec, w))
                            for a in range(1 << w)
                            for b in (range(1 << w) if kind == "mixed" else [0])) else "unsat"
    if kind == "exact":   # the recognizer fires: one countones, no bvadd
        env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
        p = subprocess.run([str(_BIN)], input=text, capture_output=True, text=True,
                           env=dict(env, DV_LOG="1"), timeout=60)
        assert re.search(r"qfbv-route: .* arith=0 .* other=1", p.stderr), p.stderr
    for engine in _ENGINES:
        for popcnt in (True, False):
            verdict, vals = _run(text, engine, popcnt)
            if want is not None:
                assert verdict == want, (engine, popcnt, text)
            if verdict == "sat":
                s = _value(vals, spec, w)
                ok = {"=": s == kk, "bvuge": s >= kk, "bvule": s <= kk}[cmp]
                assert ok, (engine, popcnt, vals, s, text)
            elif kind == "exact":
                # popcount(x) ranges over 0..w: unsat only if no count fits.
                assert not any({"=": c == kk, "bvuge": c >= kk, "bvule": c <= kk}[cmp]
                               for c in range(w + 1)), (engine, popcnt, text)
