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
# zext(e) == x mod k defines e; zg below keeps the residue proposal covered.
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

# Interval union (construct.inside_ranges): x inside two ranges and a value,
# 0.2% of its hull, so a box draw misses; a second leaf cuts one range.
def _rng(lo, hi):
    return f"(and (bvuge x {_bv(lo, _XW)}) (bvule x {_bv(hi, _XW)}))"


_IVS = ({"x": _XW, "y": 4},
        [_or("y", 4, (1, 2, 4)),
         f"(or (or {_rng(1000, 1099)} {_rng(100000, 100499)}) (= x {_bv(250000, _XW)}))",
         f"(not {_rng(100100, 100199)})"],
        lambda x, y: _in(y, (1, 2, 4)) &
        (((x >= 1000) & (x <= 1099)) | ((x >= 100000) & (x <= 100499)) | (x == 250000)) &
        ~((x >= 100100) & (x <= 100199)))


@pytest.mark.parametrize("case,key", [
    ("res", "def"), ("zg", "res"), ("cov", "cov"), ("or", None), ("pin", None),
    ("ivs", None)])
def test_box_proposals(case, key):
    widths, asserts, pred = {"res": _RES, "zg": _ZG, "cov": _COV, "or": _ORH,
                             "pin": _PIN, "ivs": _IVS}[case]
    models, err = _check(widths, asserts, pred)
    if key:
        assert _fires(err, key), err[-2000:]
    if case == "pin":
        tries = re.findall(r"\[bb-divsamp\].*tries=(\d+) -> sat", err)
        assert tries == ["1"] * _N, err[-2000:]
    if case == "ivs":
        tries = [int(t) for t in re.findall(r"\[bb-divsamp\].*tries=(\d+) -> sat", err)]
        assert len(tries) == _N and max(tries) <= 16, err[-2000:]


# A leaf that reads two vars (dist.dist_if): `mode ? (ranges & len==2) :
# (ranges & len==0x6b)`. len has no unary hull, so the box is projected onto
# each var through the ite's arms; without that every draw is rejected.
def _dist_if(seed: int) -> str:
    zl = "((_ zero_extend 24) len)"

    def rng(lo, hi):
        return f"(bvand (__Vbv (bvuge {zl} #x{lo:08x})) (__Vbv (bvule {zl} #x{hi:08x})))"

    def arm(r0, r1, k):
        return (f"(bvand (bvor {rng(*r0)} {rng(*r1)}) "
                f"(ite (__Vbool (__Vbv (bvule #x0000000b #x0000000c))) "
                f"(__Vbv (= {zl} #x{k:08x})) (__Vbv (= {zl} #x00000007))))")
    return f"""(reset)
(set-option :produce-models true)
(set-option :random-seed {seed})
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun len () (_ BitVec 8))
(declare-fun mode () (_ BitVec 1))
(assert (= #b1 (ite (__Vbool mode) {arm((5, 0x40), (1, 4), 2)} {arm((0x81, 0xff), (0x40, 0x80), 0x6b)})))
(check-sat)
(get-value (len mode))
"""


def test_two_var_leaf_projection():
    n = 200
    verdicts, models, err = _run("".join(_dist_if(7000 + i) for i in range(n)))
    assert verdicts == ["sat"] * n
    for m in models:
        assert (m["mode"], m["len"]) in ((1, 2), (0, 0x6b)), m
    assert {m["mode"] for m in models} == {0, 1}
    tries = [int(t) for t in re.findall(r"\[bb-divsamp\].*tries=(\d+) -> sat", err)]
    assert len(tries) == n and max(tries) <= 8, err[-2000:]


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


# x - y <u C and y - x <u C, y a box var drawn first (arith.unsigned_ops):
# x = y - t / y + t, t uniform in [0, C - 1]; the box draw alone misses.
_W = 32


@pytest.mark.parametrize("sub", ["y_minus_x", "x_minus_y"])
def test_cov_sub_box_base(sub):
    d = "(bvsub y x)" if sub == "y_minus_x" else "(bvsub x y)"
    asserts = [f"(bvugt y {_bv(1 << 31, _W)})",
               "(bvult x y)" if sub == "y_minus_x" else "(bvugt x y)",
               f"(bvult {d} {_bv(1000, _W)})"]
    text = "".join(_session({"x": _W, "y": _W}, asserts, 6000 + i) for i in range(200))
    verdicts, models, err = _run(text)
    assert verdicts == ["sat"] * 200
    for m in models:
        x, y = m["x"], m["y"]
        dd = (y - x if sub == "y_minus_x" else x - y) % (1 << _W)
        assert y > (1 << 31) and (x < y if sub == "y_minus_x" else x > y) and dd < 1000, m
    assert len({m["x"] for m in models}) > 190
    tries = [int(t) for t in re.findall(r"\[bb-divsamp\].*cov=1 .*tries=(\d+) -> sat", err)]
    assert len(tries) == 200 and max(tries) <= 8, err[-2000:]


# A partial order the box draw alone rarely hits (construct.cmp_web): the
# chain a < b < c is drawn jointly, d's range is narrowed along the edges
# that cannot wrap. "wrap": a + 0xfff0 wraps for every a >= 16, so that edge
# must not narrow d (d > a - 16 there).
@pytest.mark.parametrize("g", [10, 0xFFF0], ids=["offset", "wrap"])
def test_chain_proposal(g):
    import numpy as np
    from scipy.stats import chisquare
    w, C = 16, 1000
    asserts = [f"(bvugt a {_bv(100, w)})", "(bvult a b)", "(bvult b c)",
               f"(bvult c {_bv(C, w)})", f"(bvult (bvadd a {_bv(g, w)}) d)",
               "(bvult d c)"]
    widths = {"a": w, "b": w, "c": w, "d": w}
    text = "".join(_session(widths, asserts, 7000 + i) for i in range(_N))
    verdicts, models, err = _run(text)
    assert verdicts == ["sat"] * _N
    for m in models:
        a, b, c, d = m["a"], m["b"], m["c"], m["d"]
        assert 100 < a < b < c < C and (a + g) % (1 << w) < d < c, m
    hits = re.findall(r"\[bb-divsamp\].* chains=(\d+) .*-> sat", err)
    assert len(hits) == _N and all(h != "0" for h in hits), err[-2000:]
    # Exact marginals: N[a, c] solutions per (a, c), split over b and d.
    av = np.arange(C)[:, None]
    cv = np.arange(C)[None, :]
    nb = np.clip(cv - av - 1, 0, None)
    dlo = av + g + 1 if g < 0x8000 else av + g + 1 - (1 << w)
    nd = np.where(av >= (1 << w) - g if g >= 0x8000 else True,
                  np.clip(cv - np.maximum(dlo, 0), 0, None), 0)
    ok = (av > 100) & (cv > av)
    n = np.where(ok, nb * nd, 0)
    marg = {"a": n.sum(1), "c": n.sum(0)}
    for v, other, cnt in (("b", nd, None), ("d", nb, None)):
        m = np.zeros(C, dtype=np.int64)
        wt = np.where(ok, other, 0)
        for x in range(C):
            if v == "b":
                inside = (av < x) & (cv > x)
            else:
                inside = (np.maximum(dlo, 0) <= x) & (dlo <= x) & (cv > x)
            m[x] = wt[inside & ok].sum()
        marg[v] = m
    for v, h in marg.items():
        cells = h.reshape(-1, 50).sum(1)
        obs = np.bincount([mm[v] // 50 for mm in models], minlength=len(cells))
        keep = cells > 0
        assert obs[~keep].sum() == 0, v
        exp = cells[keep] * _N / cells.sum()
        p = chisquare(obs[keep], exp).pvalue
        assert p > 1e-3, (v, p, list(obs[keep]), [round(e, 1) for e in exp])


# --- ext(v) == e defines v ---------------------------------------------------
# construct.signed_mix: `sext(c) == a - sext(b)` left c free over 2^16 and every
# draw was rejected. c := the low bits of e, evaluated at the extended width,
# and the leaf stays a check, so only the e that ext(c) reproduces pass.

def _sx(v, f, t):
    return (v | (((1 << t) - 1) ^ ((1 << f) - 1))) if v >> (f - 1) & 1 else v


@pytest.mark.parametrize("kind", ["sign", "zero"])
def test_extend_definition(kind):
    from collections import Counter
    from scipy.stats import chisquare
    widths = {"a": 8, "b": 4, "c": 6}
    if kind == "sign":
        e = f"(bvsub a {_zx('b', 4)})"
        f = lambda a, b: (a - b) & 0xFF
        ext = lambda c: _sx(c, 6, 8)
    else:
        e = f"(bvudiv a (bvor {_zx('b', 4)} #x01))"
        f = lambda a, b: a // (b | 1)
        ext = lambda c: c
    asserts = [f"(= ((_ {kind}_extend 2) c) {e})"]
    text = "".join(_session(widths, asserts, 9000 + i) for i in range(_N))
    verdicts, models, err = _run(text)
    assert verdicts == ["sat"] * _N
    sols = [(a, b) for a in range(256) for b in range(16)
            if any(ext(c) == f(a, b) for c in range(64))]
    for m in models:
        assert ext(m["c"]) == f(m["a"], m["b"]), m
    hits = re.findall(r"\[bb-divsamp\].* def=(\d+) .*-> sat", err)
    assert len(hits) == _N and all(h == "1" for h in hits), err[-2000:]
    for i, v in enumerate("ab"):
        h = Counter(s[i] >> (4 if v == "a" else 0) for s in sols)
        cells = sorted(h)
        obs = Counter(m[v] >> (4 if v == "a" else 0) for m in models)
        assert set(obs) <= set(cells), v
        exp = [h[c] * _N / len(sols) for c in cells]
        p = chisquare([obs[c] for c in cells], exp).pvalue
        assert p > 1e-3, (v, p)


# --- zext(v) sides are edges -------------------------------------------------
# control.solve_before_array: arr[i] <= zext(n), n 4-bit, arr[i] 8-bit; the
# box draw of arr[i] over 0..255 was always rejected. zext(n) counts as n for
# the edge, and the edge trims the arr[i] range ("range") or enumerated domain
# ("set") to n's upper bound.
@pytest.mark.parametrize("dom", ["range", "set"])
def test_edge_zext_star(dom):
    from collections import Counter
    from itertools import product
    from scipy.stats import chisquare
    xs = ["x0", "x1", "x2", "x3"]
    vals = [1, 3, 7, 12, 40, 200]
    widths = {"n": 4, **{x: 8 for x in xs}}
    asserts = ["(bvuge n #x2)"]
    for x in xs:
        if dom == "set":
            asserts.append(_or(x, 8, vals))
        asserts.append(f"(bvule {x} {_zx('n', 4)})")
    text = "".join(_session(widths, asserts, 9500 + i) for i in range(_N))
    verdicts, models, err = _run(text)
    assert verdicts == ["sat"] * _N
    xv = range(256) if dom == "range" else vals
    for m in models:
        assert m["n"] >= 2 and all(m[x] <= m["n"] and m[x] in xv for x in xs), m
    hits = re.findall(r"\[bb-divsamp\].* chains=(\d+) .*-> sat", err)
    assert len(hits) == _N, err[-2000:]
    # Exact weights: per n, k = #values <= n for each x.
    wn = {n: sum(1 for v in xv if v <= n) ** 4 for n in range(2, 16)}
    tot = sum(wn.values())
    wx = Counter()
    for n, w in wn.items():
        k = sum(1 for v in xv if v <= n)
        for v in xv:
            if v <= n:
                wx[v] += w // k
    for v, h in (("n", wn), ("x0", wx)):
        cells = sorted(c for c in h if h[c] * _N / tot >= 5)
        obs = Counter(m[v] for m in models)
        rest = _N - sum(obs[c] for c in cells)
        erest = _N - sum(h[c] * _N / tot for c in cells)
        o = [obs[c] for c in cells] + ([rest] if erest > 1e-9 else [])
        e = [h[c] * _N / tot for c in cells] + ([erest] if erest > 1e-9 else [])
        p = chisquare(o, e).pvalue
        assert p > 1e-3, (v, p, o, [round(x, 1) for x in e])
