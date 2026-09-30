"""Reified comparisons must not go `unsat` when both operands are ranges.

B31. `_fire_reification_32` / `_fire_reification_64` implement
`g <-> (x <= y)`. With the guard false they must enforce `x > y`, and they did
so with the wrong supports: `x.lb := y.hi + 1` and `y.ub := x.lo - 1`. That asks
x to exceed EVERY value of y -- entailment, not propagation. Against a constant
(y pinned, y.hi == y.lo) the two coincide, which is why the var-const reified
compares that dominate Verilator traffic never showed it. As soon as both sides
are ranges -- a compare against a composed expression (`x < (y | 1)`,
`x < 5 - x`, `v > ite(..)`), or two free variables -- level-0 propagation
emptied a domain and the default engine answered a wrong `unsat`. The correct
supports are `y.lo + 1` and `x.hi - 1`.

Every strict compare `a < b` and every negated `a <= b` reaches this branch,
because `_reify_cmp_var_var` builds them as the inverted `b <= a` guard. The
non-reified form (`bounds_lt`) and a Bool-sorted `ite` never reify, so only the
1-bit forms Verilator emits -- `(= #b1 (ite P #b1 #b0))`, `(= #b1 (__Vbv P))`,
`(= b (ite P ..)) (= b #b1)` -- were exposed.

Found alongside it (same seeds): when an LCG learnt clause went all-false it
returned PROP_CONFLICT without recording itself, so conflict analysis seeded
from a stale `conflict_prop_ref` and learnt a bogus unit clause -- another wrong
`unsat` (the reified `distinct` case below). Analysis now seeds from the clause.

The explain callbacks for both reification templates also cited only the guard
for an x/y tightening, though the bound depends on the other operand too; they
now cite it. That exposed an LCG livelock (the same non-asserting clause, or a
tautology, re-learnt on every conflict), now broken by falling back to the
chronological path for such clauses -- see test_lcg_no_relearn_livelock.

Known, not fixed here: at width 32, a self-referential strict compare
(`y > (y | x)`, `y < y * 1`) converges one value per propagation round (B30).
The old, wrong rule happened to answer some of those instantly; they now take
the CDCL budget like their non-reified forms always did.

Usage:
    direnv exec . pytest tests/formal/test_reified_compare.py -v
"""
from __future__ import annotations

import os
import random
import subprocess
from pathlib import Path

import pytest

from .harness._subprocess_solver import find_binary

_REPO = Path(__file__).resolve().parents[2]
_DV = _REPO / "build" / "dv-solve-smt2"
_Z3 = find_binary(_REPO / "packages" / "python" / "bin" / "z3", "z3")
_PURE = {"DV_NO_BITBLAST": "1", "DV_CDCL_TIME_LIMIT": "20"}
_DEFAULT = {"DV_CDCL_TIME_LIMIT": "20"}
_PREAMBLE = ("(set-logic QF_BV)\n"
             "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n")


@pytest.fixture(autouse=True)
def _need_binary():
    if not _DV.is_file():
        pytest.skip("dv-solve-smt2 binary not built")


def _src(body: str) -> str:
    return _PREAMBLE + body + "\n(check-sat)\n"


def _solve(body: str, pure: bool, extra: dict | None = None) -> str:
    env = {**os.environ, **(_PURE if pure else _DEFAULT), **(extra or {})}
    p = subprocess.run([str(_DV), "--interactive"], input=_src(body),
                       capture_output=True, text=True, timeout=60, env=env)
    return next((l.strip() for l in p.stdout.splitlines()
                 if l.strip() in ("sat", "unsat", "unknown")), "none")


def _z3(body: str) -> str | None:
    if not _Z3:
        return None
    p = subprocess.run([str(_Z3), "-in"], input=_src(body),
                       capture_output=True, text=True, timeout=60)
    out = p.stdout.split()
    return out[0] if out else None


_X8 = "(declare-const x (_ BitVec 8))(declare-const y (_ BitVec 8))\n"

# (label, body, expected) -- expected verified against z3; the test re-checks
# it against z3 when z3 is available so a fixture can't silently rot.
_CASES = [
    # --- the reported repro and its reification spellings -----------------
    ("bvor rhs, ite form", _X8 +
     "(assert (bvugt x #x10))"
     "(assert (= #b1 (ite (bvult x (bvor y #x01)) #b1 #b0)))", "sat"),
    ("bvor rhs, __Vbv form", _X8 +
     "(assert (bvugt x #x10))"
     "(assert (= #b1 (__Vbv (bvult x (bvor y #x01)))))", "sat"),
    ("bvor rhs, named 1-bit guard", _X8 +
     "(declare-const b (_ BitVec 1))"
     "(assert (bvugt x #x10))"
     "(assert (= b (ite (bvult x (bvor y #x01)) #b1 #b0)))(assert (= b #b1))",
     "sat"),
    ("bvor rhs, mirrored bvugt", _X8 +
     "(assert (bvuge x #x01))"
     "(assert (= #b1 (ite (bvugt (bvor y #x01) x) #b1 #b0)))", "sat"),
    ("bvor rhs, negated bvuge", _X8 +
     "(assert (bvugt x #x10))"
     "(assert (= #b1 (ite (bvuge x (bvor y #x02)) #b0 #b1)))", "sat"),
    ("same var both sides: x < (x | 1)", _X8 +
     "(assert (bvugt x #x10))"
     "(assert (= #b1 (ite (bvult x (bvor x #x01)) #b1 #b0)))", "sat"),
    # --- modular const-minus-var rhs (solution needs wraparound) ----------
    ("x < 5 - x (wraps)", _X8 +
     "(assert (bvugt x #x05))"
     "(assert (= #b1 (ite (bvult x (bvsub #x05 x)) #b1 #b0)))", "sat"),
    ("x < 0 - x (bvneg spelled as sub)", _X8 +
     "(assert (= #b1 (__Vbv (bvult x (bvsub #x00 x)))))", "sat"),
    # --- plain var-var, both ranges, 8-bit (tier 0) and 40-bit (tier 1) ----
    ("var-var ranges w8", _X8 +
     "(assert (bvugt x #x10))(assert (bvult x #xc8))(assert (bvult y #x30))"
     "(assert (= #b1 (ite (bvult x y) #b1 #b0)))", "sat"),
    ("var-var ranges w8, #b0 polarity", _X8 +
     "(assert (bvugt x #x10))(assert (bvult x #xc8))(assert (bvult y #x30))"
     "(assert (= #b0 (ite (bvuge x y) #b1 #b0)))", "sat"),
    ("var-var ranges w40",
     "(declare-const X (_ BitVec 40))(declare-const Y (_ BitVec 40))"
     "(assert (bvugt X (_ bv16 40)))(assert (bvult X (_ bv200 40)))"
     "(assert (bvult Y (_ bv48 40)))"
     "(assert (= #b1 (ite (bvult X Y) #b1 #b0)))", "sat"),
    # --- coordinator repro: ite rhs reached through `not (bvule ..)` ------
    ("not bvule vs ite, w8 selector",
     "(declare-const v (_ BitVec 3))(declare-const w (_ BitVec 8))"
     "(assert (= #b1 (__Vbv (not (bvule v (ite (distinct w w) v #b011))))))",
     "sat"),
    ("not bvule vs ite, w48 selector",
     "(declare-const v (_ BitVec 3))(declare-const w (_ BitVec 48))"
     "(assert (= #b1 (__Vbv (not (bvule v (ite (distinct w w) v #b011))))))",
     "sat"),
    ("not bvule vs ite, bvand mask",
     "(declare-const v (_ BitVec 3))(declare-const w (_ BitVec 8))"
     "(assert (= #b1 (bvand #b1 (__Vbv (not (bvule v"
     " (ite (distinct w w) v #b011)))))))", "sat"),
    # --- LCG: all-false learnt clause seeded from a stale propagator ------
    ("reified distinct (clause-conflict analysis)",
     "(declare-const x (_ BitVec 4))(declare-const y (_ BitVec 4))"
     "(assert (= #b1 (__Vbv (distinct (bvand x (_ bv5 4)) (bvor y x)))))"
     "(assert (bvslt y (_ bv2 4)))", "sat"),
    # --- mirrors: genuinely unsat shapes must STILL be unsat --------------
    ("unsat: x > 0xf0 but x < (y|1) <= 0x0f", _X8 +
     "(assert (bvugt x #xf0))(assert (bvult y #x0e))"
     "(assert (= #b1 (ite (bvult x (bvor y #x01)) #b1 #b0)))", "unsat"),
    ("unsat: var-var ranges disjoint", _X8 +
     "(assert (bvugt x #x40))(assert (bvult y #x30))"
     "(assert (= #b1 (__Vbv (bvult x y))))", "unsat"),
    ("unsat: x < x", _X8 + "(assert (= #b1 (__Vbv (bvult x x))))", "unsat"),
    ("unsat: not (x <= x), w32",
     "(declare-const y (_ BitVec 32))"
     "(assert (= #b0 (ite (bvule y y) #b1 #b0)))", "unsat"),
]


@pytest.mark.parametrize("label,body,expected", _CASES,
                         ids=[c[0] for c in _CASES])
def test_reified_compare(label, body, expected) -> None:
    z = _z3(body)
    if z is not None:
        assert z == expected, f"fixture rot: z3 says {z} for {label}"
    for pure in (True, False):
        got = _solve(body, pure)
        eng = "pure-CDCL" if pure else "default"
        assert got == expected, f"B31 {label} [{eng}]: got {got}, want {expected}"


# ------------------------------------------------------- LCG livelock guard

_LIVELOCK = [
    ("6 - x < x, x > 10 (w4)",
     "(declare-const x (_ BitVec 4))"
     "(assert (= #b1 (ite (bvult (bvsub (_ bv6 4) x) x) #b1 #b0)))"
     "(assert (bvugt x (_ bv10 4)))"),
    ("not (y <= y - 213) (w8)",
     "(declare-const y (_ BitVec 8))"
     "(assert (= #b0 (ite (bvule y (bvsub y (_ bv213 8))) #b1 #b0)))"),
]


@pytest.mark.parametrize("label,body", _LIVELOCK, ids=[c[0] for c in _LIVELOCK])
def test_lcg_no_relearn_livelock(label, body) -> None:
    """Tiny satisfiable instances must be answered by CDCL itself, quickly.

    With correct reification explanations, resolution through an explainer
    that cites current (post-conflict) bounds re-derived the same hole clause
    -- or a tautology -- on every conflict, backjumped to level 0 and undid
    the chronological path's progress: a 4-bit instance spent the whole CDCL
    budget. Analysis now falls back to the chronological path for tautological
    or duplicate learnt clauses. A 5 s budget makes a regression an `unknown`.
    """
    got = _solve(body, pure=True, extra={"DV_CDCL_TIME_LIMIT": "5"})
    assert got == "sat", f"{label}: pure-CDCL got {got}"


# ------------------------------------------------ seeded differential sweep

_CMPS = ["bvult", "bvule", "bvugt", "bvuge", "=", "distinct",
         "bvslt", "bvsle", "bvsgt", "bvsge"]


def _gen(rng: random.Random) -> str:
    # Widths stay <= 16: at 32 bits a self-referential strict compare
    # (`y > (y | x)`) converges one value per propagation round -- the
    # pre-existing B30 slow-convergence class, a hang rather than a wrong
    # answer, and not what this sweep is guarding.
    w = rng.choice([4, 8, 16])

    def c(v: int) -> str:
        return f"(_ bv{v & ((1 << w) - 1)} {w})"

    k = rng.randrange(1 << w)
    rhs = rng.choice([
        f"(bvor y {c(k)})", f"(bvand y {c(k)})", f"(bvxor y {c(k)})",
        f"(bvadd y {c(k)})", f"(bvsub y {c(k)})",
        f"(bvmul y {c(rng.choice([1, 2, 3, 5, k]))})",
        f"(bvsub {c(k)} x)", f"(bvsub {c(k)} y)", f"(bvor x {c(k)})",
        "y", "(bvor y x)", "(bvadd y x)",
    ])
    lhs = rng.choice(["x", "x", "y", f"(bvand x {c(rng.randrange(1 << w))})"])
    a, b = (lhs, rhs) if rng.random() < 0.7 else (rhs, lhs)
    p = f"({rng.choice(_CMPS)} {a} {b})"
    pol = rng.choice(["#b1", "#b0"])
    form = rng.choice(["ite", "vbv", "named"])
    body = (f"(declare-const x (_ BitVec {w}))(declare-const y (_ BitVec {w}))"
            "(declare-const b (_ BitVec 1))\n")
    if form == "ite":
        body += f"(assert (= {pol} (ite {p} #b1 #b0)))\n"
    elif form == "vbv":
        body += f"(assert (= {pol} (__Vbv {p})))\n"
    else:
        body += f"(assert (= b (ite {p} #b1 #b0)))(assert (= b {pol}))\n"
    for _ in range(rng.randrange(0, 4)):
        op = rng.choice(["bvugt", "bvult", "bvuge", "bvule", "bvsgt", "bvslt"])
        body += (f"(assert ({op} {rng.choice(['x', 'y'])}"
                 f" {c(rng.randrange(1 << w))}))\n")
    return body


_N = int(os.environ.get("REIFIED_FUZZ_N", "300"))


@pytest.mark.skipif(not _Z3, reason="z3 not available")
@pytest.mark.parametrize("chunk", range(0, _N, 50))
def test_reified_compare_sweep(chunk) -> None:
    bad = []
    for seed in range(chunk, min(chunk + 50, _N)):
        body = _gen(random.Random(seed))
        z = _z3(body)
        if z not in ("sat", "unsat"):
            continue
        # Short CDCL budget: this checks for disagreements only, and a few
        # generated shapes are slow-but-correct (pre-existing; they escalate).
        got = _solve(body, pure=False, extra={"DV_CDCL_TIME_LIMIT": "2"})
        if got in ("sat", "unsat") and got != z:
            bad.append(f"seed {seed}: dv={got} z3={z}\n{body}")
    assert not bad, "reified-compare disagreements:\n" + "\n".join(bad)
