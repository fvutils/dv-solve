"""The bit-blast engine's div/mul sampler (`_divsamp` in dvs_bbsolver.c).

`_divsamp` draws the free vars from their unary hull, computes the vars a
`v == e` leaf defines, and checks the rest. Of a cycle of definitions it must
keep the one that leaves the narrow vars free: in `p == a * b` with
`(bvudiv p b) == a`, defining `a := p / b` leaves `p` free over 2^64 and every
draw wraps, while `p := a * b` keeps `a` and `b` in range. Every model is
checked against the constraints as written; DV_SIM_CHECK=1 makes the engine
re-check every fast-path answer with SAT and abort on a mismatch.

    env -u LD_LIBRARY_PATH packages/python/bin/python3 -m pytest -q tests/formal/test_divsamp.py
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_BIN = _ROOT / "build" / "dv-solve-smt2"
_M64 = (1 << 64) - 1


def _run(text: str, divsamp: bool = True):
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env.update(DV_ENGINE="bitblast", DV_SIM_CHECK="1", DV_BB_STATS="1",
               DV_DIVSAMP="1" if divsamp else "0")
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


def _mul_div(seed: int) -> str:
    # As Verilator writes it (arith.mul_div_64); the wrapping is what put the
    # udiv definition first.
    return f"""(reset)
(set-option :produce-models true)
(set-option :random-seed {seed})
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(declare-fun a () (_ BitVec 64))
(declare-fun b () (_ BitVec 64))
(declare-fun p () (_ BitVec 64))
(assert (= #b1 (bvand (__Vbv (bvuge a #x0000000000000001)) (__Vbv (bvule a #x00000000ffffffff)))))
(assert (= #b1 (bvand (__Vbv (bvuge b #x0000000000000001)) (__Vbv (bvule b #x000000000000ffff)))))
(assert (= #b1 (__Vbv (= p (bvmul a b)))))
(assert (= #b1 (__Vbv (= (bvudiv p b) a))))
(check-sat)
(get-value (a b p))
"""


@pytest.fixture(scope="module", autouse=True)
def _built():
    if not _BIN.exists():
        pytest.skip(f"{_BIN} not built")


@pytest.mark.parametrize("divsamp", [True, False])
def test_mul_div_models(divsamp):
    n = 40
    text = "".join(_mul_div(1000 + i) for i in range(n))
    verdicts, models, err = _run(text, divsamp)
    assert verdicts == ["sat"] * n
    assert len(models) == n
    for m in models:
        a, b, p = m["a"], m["b"], m["p"]
        assert 1 <= a <= 0xFFFFFFFF and 1 <= b <= 0xFFFF
        assert p == (a * b) & _M64 and p // b == a
    if divsamp:
        # The sampler answers each call on its first draw, and the draws vary.
        hits = re.findall(r"\[bb-divsamp\].*tries=(\d+) -> sat", err)
        assert len(hits) == n and all(t == "1" for t in hits), err[-2000:]
        assert len({m["a"] for m in models}) >= n - 2
        assert len({m["b"] for m in models}) >= n // 2


# --- Box-var proposals (residue, change of variable) and the OR hull --------
# A var whose hull has 2^16 values or more is a box var (drawn, not
# enumerated); the proposals apply to those, so `x` below is 18 bits and the
# rest are small. The solutions are enumerated (x vectorised with numpy): the
# models must satisfy the constraints, and each var's histogram (x bucketed
# to 16 cells) must match the one over all solutions. Fixed seeds: a pass
# is stable.

_N = 1500
_XW = 18


def _bv(v: int, w: int) -> str:
    return "#b" + format(v, f"0{w}b")


def _zx(e: str, n: int) -> str:
    return f"((_ zero_extend {n}) {e})"


def _session(widths: dict, asserts: list, seed: int) -> str:
    decl = "".join(f"(declare-fun {v} () (_ BitVec {w}))\n" for v, w in widths.items())
    body = "".join(f"(assert (= #b1 (__Vbv {a})))\n" for a in asserts)
    return f"""(reset)
(set-option :produce-models true)
(set-option :random-seed {seed})
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
{decl}{body}(check-sat)
(get-value ({' '.join(widths)}))
"""


def _hists(widths: dict, pred):
    """Exact per-var histograms over all solutions; x is the box var."""
    import itertools
    import numpy as np
    xs = np.arange(1 << widths["x"], dtype=np.int64)
    sh = widths["x"] - 4
    small = [v for v in widths if v != "x"]
    hist = {v: {} for v in widths}
    for tup in itertools.product(*(range(1 << widths[v]) for v in small)):
        env = dict(zip(small, tup))
        cnt = np.bincount(xs[pred(x=xs, **env)] >> sh, minlength=16)
        n = int(cnt.sum())
        if not n:
            continue
        for v in small:
            hist[v][env[v]] = hist[v].get(env[v], 0) + n
        for b, c in enumerate(cnt):
            if c:
                hist["x"][b] = hist["x"].get(b, 0) + int(c)
    return hist, sh


def _check(widths, asserts, pred, divsamp=True):
    from scipy.stats import chisquare
    text = "".join(_session(widths, asserts, 5000 + i) for i in range(_N))
    verdicts, models, err = _run(text, divsamp)
    assert verdicts == ["sat"] * _N
    assert len(models) == _N
    for m in models:
        assert bool(pred(**m)), m
    hist, sh = _hists(widths, pred)
    for v, h in hist.items():
        cells = sorted(h)
        if len(cells) < 2:
            continue
        tot = sum(h.values())
        obs = [0] * len(cells)
        idx = {c: i for i, c in enumerate(cells)}
        for m in models:
            obs[idx[m[v] >> sh if v == "x" else m[v]]] += 1
        exp = [h[c] * _N / tot for c in cells]
        p = chisquare(obs, exp).pvalue
        assert p > 1e-3, (v, p, obs, [round(e, 1) for e in exp])
    return models, err


def _fires(err, key):
    hits = re.findall(r"\[bb-divsamp\].* %s=(\d+) .*-> sat" % key, err)
    return len(hits) == _N and all(h != "0" for h in hits)


def _in(v, vals):
    r = v == vals[0]
    for c in vals[1:]:
        r = r | (v == c)
    return r


def _or(var, w, vals):
    return "(or " + " ".join(f"(= {var} {_bv(c, w)})" for c in vals) + ")"


# x mod k == e, k from an OR-of-equalities domain, e < k (arith.mod_align).
_RES = ({"x": _XW, "k": 4, "e": 3},
        [_or("k", 4, (3, 5, 6)), f"(bvult {_zx('e', 1)} k)",
         f"(= (bvurem x {_zx('k', _XW - 4)}) {_zx('e', _XW - 3)})"],
        lambda x, k, e: _in(k, (3, 5, 6)) & (e < k) & (x % max(k, 1) == e))

# The frontend lowers bvurem by a symbolic m to ite(m == 0, x, x mod m).
_ZG = ({"x": _XW, "m": 4},
       [_or("m", 4, (0, 3, 5, 7)), f"(= (bvurem x {_zx('m', _XW - 4)}) {_bv(0, _XW)})"],
       lambda x, m: _in(m, (0, 3, 5, 7)) & ((x if m == 0 else x % max(m, 1)) == 0))

# x + y <=u C at x's width (wraps), y from an OR domain (arith.addr_len).
_COV = ({"x": _XW, "y": 4},
        [_or("y", 4, (1, 2, 4, 8)),
         f"(bvule (bvadd x {_zx('y', _XW - 4)}) {_bv(4999, _XW)})"],
        lambda x, y: _in(y, (1, 2, 4, 8)) & ((x + y) % (1 << _XW) <= 4999))

# The OR hull alone.
_ORH = ({"x": _XW, "y": 8},
        [_or("y", 8, (1, 2, 4)), f"(bvuge x {_bv(1000, _XW)})"],
        lambda x, y: _in(y, (1, 2, 4)) & (x >= 1000))


# Bits an extract leaf pins (cons.addr_align): the box draw keeps them, so
# no draw is rejected for them.
_PIN = ({"x": _XW, "y": 4},
        [_or("y", 4, (1, 2, 4)), "(= ((_ extract 1 0) x) #b00)", "(= ((_ extract 9 9) x) #b1)",
         f"(bvuge x {_bv(1000, _XW)})", f"(bvule x {_bv(200001, _XW)})"],
        lambda x, y: _in(y, (1, 2, 4)) & (x & 3 == 0) & ((x >> 9) & 1 == 1) &
        (x >= 1000) & (x <= 200001))


@pytest.mark.parametrize("case,key", [
    ("res", "res"), ("zg", "res"), ("cov", "cov"), ("or", None), ("pin", None)])
def test_box_proposals(case, key):
    widths, asserts, pred = {"res": _RES, "zg": _ZG, "cov": _COV, "or": _ORH,
                             "pin": _PIN}[case]
    models, err = _check(widths, asserts, pred)
    if key:
        assert _fires(err, key), err[-2000:]
    if case == "pin":
        tries = re.findall(r"\[bb-divsamp\].*tries=(\d+) -> sat", err)
        assert tries == ["1"] * _N, err[-2000:]


# Shapes the proposals must decline: a signed compare, and an add narrower
# than its operand. They still give valid, well-spread models.
_SG = 1 << (_XW - 1)
_DECL = {
    "signed": ({"x": _XW, "y": 4},
               [_or("y", 4, (1, 2, 4)),
                f"(bvsle (bvadd x {_zx('y', _XW - 4)}) {_bv(4999, _XW)})"],
               lambda x, y: _in(y, (1, 2, 4)) &
               ((((x + y) % (1 << _XW)) ^ _SG) - _SG <= 4999)),
    "width": ({"x": _XW, "y": 4},
              [_or("y", 4, (1, 2, 4)),
               f"(bvule ((_ extract 15 0) (bvadd x {_zx('y', _XW - 4)})) #x1387)"],
              lambda x, y: _in(y, (1, 2, 4)) & (((x + y) & 0xFFFF) <= 4999)),
}


@pytest.mark.parametrize("case", sorted(_DECL))
def test_box_declines(case):
    widths, asserts, pred = _DECL[case]
    text = "".join(_session(widths, asserts, 5000 + i) for i in range(200))
    verdicts, models, err = _run(text)
    assert verdicts == ["sat"] * 200
    for m in models:
        assert bool(pred(**m)), m
    assert len({m["x"] for m in models}) > 100
