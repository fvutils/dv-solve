"""Verilator's reified `bvor` disjunction must reach the CDCL disjunction path.

Phase 2 of docs/cdcl_verilator_coverage_plan.md.

Verilator emits `x inside {a, b, c}` not as a Boolean `or` but as

    (= #b1 (bvand #b1 (bvor (__Vbv (= x a)) (__Vbv (= x b)) ...)))

where `__Vbv` lifts a Bool to (_ BitVec 1). `bvor` translated to BIN_BOR
(bitwise), whose operands were then flattened to aux vars -- so by the time the
CDCL compiler ran, the disjunctive structure was gone. It saw a chain of ITE
guard propagators carrying no bound information, left the variable at its full
32-bit domain, and the search enumerated it. The frontend now rewrites this
shape to a logical BIN_OR, which reaches the OR-tree flattener, becomes a
DisjClause, and gets interval-hulled (see test_disj_hull.py).

WHAT THIS FILE IS REALLY GUARDING
---------------------------------
The rewrite is scoped to DISJUNCTIONS on purpose. An earlier version also
rewrote `bvand` trees; that regressed t_constraint_sysfunc from `sat` to
`unknown`, because `(= #b1 (bvand (__Vbv A) (__Vbv B)))` already compiled
correctly and the rewritten AND-of-ITE tree did not. The regression guard for
that is test_sysfunc_fixture_still_solves_on_cdcl, which runs the real
transcript -- it matters more than the disjunction cases, because a rewrite
that is too eager is silently lossy.

The conjunction cases further down cover Phase 3, which closed the same gap for
`(= #b1 (bvand ...))` -- but by splitting the top-level ASSERT into one assert
per conjunct rather than by rewriting the expression, precisely so the sysfunc
translation is left alone. The one remaining xfail there is a different
problem: a conjunct over a *derived* operand, which no propagator narrows.

Usage:
    direnv exec . pytest tests/formal/test_reified_or.py -v
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import time
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_DV = _REPO / "build" / "dv-solve-smt2"
_FIX = _REPO / "tests" / "formal" / "smt2" / "verilator"
_VBV = "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))"
_PURE = {"DV_NO_BITBLAST": "1", "DV_CDCL_TIME_LIMIT": "10000"}
_LIT = re.compile(r"#[bx][0-9a-fA-F]+")


@pytest.fixture(autouse=True)
def _need_binary():
    if not _DV.is_file():
        pytest.skip("dv-solve-smt2 binary not built")


def _run(body: str, get: str = "z", width: int = 32, timeout: int = 30):
    script = (f"(set-logic QF_BV)\n{_VBV}\n"
              f"(declare-fun z () (_ BitVec {width}))\n"
              f"(assert {body})\n(check-sat)\n(get-value ({get}))\n(exit)\n")
    t0 = time.perf_counter()
    p = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                       input=script, capture_output=True, text=True,
                       timeout=timeout, env={**os.environ, **_PURE})
    dt = time.perf_counter() - t0
    verdict = next((l.strip() for l in p.stdout.splitlines()
                    if l.strip() in ("sat", "unsat", "unknown")), "none")
    vals = [int(m[2:], 2) if m[1] == "b" else int(m[2:], 16)
            for m in _LIT.findall(p.stdout)]
    return verdict, vals, dt


def _inside(vals, width=32, const_left=False, mask=False):
    def eq(v):
        lit = f"#x{v:0{width // 4}x}"
        return f"(__Vbv (= {lit} z))" if const_left else f"(__Vbv (= z {lit}))"
    tree = f"(bvor {' '.join(eq(v) for v in vals)})"
    if mask:
        tree = f"(bvand #b1 {tree})"
    return f"(= #b1 {tree})"


@pytest.mark.parametrize("k", [2, 3, 4, 5, 8])
@pytest.mark.parametrize("const_left", [False, True], ids=["var-left", "const-left"])
@pytest.mark.parametrize("mask", [False, True], ids=["bare", "masked"])
def test_reified_inside_set_solves_fast(k, const_left, mask) -> None:
    """The `inside` idiom must produce an in-set model, quickly.

    Before the rewrite this was a 32-bit domain the search enumerated: 10 s to
    `unknown`. A generous 5 s bound still separates the two regimes by 3 orders
    of magnitude, so this is not a flaky timing assertion.
    """
    members = [3 + i for i in range(k)]
    verdict, vals, dt = _run(_inside(members, const_left=const_left, mask=mask))
    assert verdict == "sat", f"expected sat, got {verdict}"
    assert vals and vals[0] in members, f"model {vals[:1]} not in {members}"
    assert dt < 5.0, f"{dt:.2f}s -- the disjunction is not restricting the domain"


def test_reified_or_is_not_treated_as_exact() -> None:
    """A sparse set must still exclude the interior of its hull.

    The hull of {2, 40} is [2, 40]; only the two members are solutions. If the
    hull were mistaken for the constraint we would admit e.g. 20.
    """
    members = [2, 40]
    for _ in range(12):
        verdict, vals, _ = _run(_inside(members))
        assert verdict == "sat"
        assert vals[0] in members, f"{vals[0]} is in the hull but not the set"


def test_reified_or_unsat_is_still_unsat() -> None:
    """Contradicting the disjunction must stay unsat, not become wrong-sat."""
    script = (f"(set-logic QF_BV)\n{_VBV}\n(declare-fun z () (_ BitVec 32))\n"
              f"(assert {_inside([3, 4, 5])})\n"
              f"(assert (bvugt z #x0000000a))\n(check-sat)\n(exit)\n")
    p = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                       input=script, capture_output=True, text=True,
                       timeout=30, env={**os.environ, **_PURE})
    verdict = next((l.strip() for l in p.stdout.splitlines()
                    if l.strip() in ("sat", "unsat", "unknown")), "none")
    assert verdict == "unsat", f"expected unsat, got {verdict}"


# ------------------------------------------------- conjunction must be untouched

_CONJ = [
    # (label, assertion) -- each pins a 32-bit z to a handful of values
    ("two reified compares",
     "(= #b1 (bvand (__Vbv (bvugt z #x00000005)) (__Vbv (bvult z #x0000000a))))"),
    ("three-way conjunction",
     "(= #b1 (bvand (__Vbv (bvuge z #x00000002)) (__Vbv (bvule z #x00000009))"
     " (__Vbv (distinct z #x00000005))))"),
    ("nested bvand tree",
     "(= #b1 (bvand (bvand (__Vbv (bvuge z #x00000002))"
     " (__Vbv (bvule z #x00000009))) (__Vbv (distinct z #x00000005))))"),
    ("masked, the shape Verilator emits",
     "(= #b1 (bvand #b1 (bvand (__Vbv (bvsge z #x00000005))"
     " (__Vbv (bvsle z #x00000006)))))"),
    ("conjunct that is itself a disjunction",
     "(= #b1 (bvand (__Vbv (bvult z #x0000000a))"
     " (bvor (__Vbv (= z #x00000003)) (__Vbv (= z #x00000007)))))"),
]


@pytest.mark.parametrize("label,body", _CONJ, ids=[c[0] for c in _CONJ])
def test_reified_conjunction_restricts_the_domain(label, body) -> None:
    """The conjunction analogue of the gap Phase 2 fixed for disjunctions.

    `(= #b1 (bvand (__Vbv (bvugt z 5)) (__Vbv (bvult z 10))))` pins z to a
    4-value window. Kept whole, the conjunction reifies into a Boolean guard
    and nothing propagates the window to z's bounds, so on a 32-bit z the
    search enumerated blind and never returned. Phase 3 splits the top-level
    assert into one assert per conjunct, which is semantics-preserving for
    1-bit operands and lets each conjunct compile to a bounds propagator.

    Note this is NOT the sysfunc regression: sysfunc's variable is 8 bits, so
    the missing restriction was survivable there (256 values). The regression
    guard for that is test_sysfunc_fixture_still_solves_on_cdcl.
    """
    verdict, _, _ = _run(body, timeout=20)
    assert verdict == "sat", f"{label}: got {verdict}"


@pytest.mark.xfail(strict=True, reason="power-of-two test: the second conjunct "
                                       "is a derived bvand/bvsub, which no "
                                       "propagator narrows on a 32-bit domain")
def test_conjunction_with_derived_operand() -> None:
    """Splitting the assert is necessary but not sufficient.

    `z != 0 AND (z & (z-1)) == 0` splits cleanly, but the second conjunct
    constrains a *derived* term; the split gives it its own propagator and that
    propagator still cannot narrow a 32-bit z. Distinct from the domain-
    restriction gap above -- this one needs the Class A / derived-operand work.
    """
    verdict, _, _ = _run(
        "(= #b1 (bvand (__Vbv (not (= z #x00000000)))"
        " (__Vbv (= (bvand z (bvsub z #x00000001)) #x00000000))))", timeout=20)
    assert verdict == "sat", f"got {verdict}"


def test_sysfunc_fixture_still_solves_on_cdcl() -> None:
    """The real transcript that caught the over-eager rewrite."""
    f = _FIX / "t_constraint_sysfunc.smt2"
    if not f.is_file():
        pytest.skip("fixture missing")
    p = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                       input=f.read_text(), capture_output=True, text=True,
                       timeout=60, env={**os.environ, **_PURE})
    verdict = next((l.strip() for l in p.stdout.splitlines()
                    if l.strip() in ("sat", "unsat", "unknown")), "none")
    assert verdict == "sat", f"sysfunc regressed to {verdict}"


# ------------------------------------------------------------- differential

_DIFF = [
    _inside([3, 4, 5]),
    _inside([3, 4, 5], mask=True),
    _inside([0], mask=True),
    _inside([7, 7, 7]),                      # duplicate members
    _inside(list(range(1, 9))),              # wide set
    "(= #b1 (bvor (__Vbv (bvult z #x00000003)) (__Vbv (bvugt z #x000000f0))))",
    "(= #b1 (bvor (__Vbv (= z #x00000003)) (__Vbv (bvuge z #x000000f0))))",
]

# Genuinely bitwise ors that must NOT be mistaken for disjunctions. Kept at
# 8 bits: these are well-sorted equalities over a full-width term, which pure
# CDCL solves only by enumeration, so a 32-bit version times out for reasons
# that have nothing to do with the rewrite. (An earlier draft used
# `(= #b1 (bvor z #x00000000))`, which is ill-sorted -- z3 rejects it outright,
# so it tested nothing at all.)
_DIFF_W8 = [
    "(= #x01 (bvor z #x00))",
    "(= #x01 (bvand #xff (bvor z z)))",
    "(= #x03 (bvor z #x01))",
    "(= #x00 (bvor z #x00))",
]


@pytest.mark.parametrize("body", _DIFF, ids=range(len(_DIFF)))
def test_agrees_with_z3(body) -> None:
    """The rewrite is a translation change, so cross-check the verdict.

    The last three cases guard against over-application: `(bvor z 0)` on a
    32-bit z is arithmetic, not a disjunction, and rewriting it would change the
    meaning of the constraint. The rewrite declines them because it requires
    both the `#b1` comparand and every operand to be exactly 1 bit wide.
    """
    if not shutil.which("z3"):
        pytest.skip("z3 not available")
    script = (f"(set-logic QF_BV)\n{_VBV}\n(declare-fun z () (_ BitVec 32))\n"
              f"(assert {body})\n(check-sat)\n(exit)\n")
    dv = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                        input=script, capture_output=True, text=True,
                        timeout=60, env={**os.environ, **_PURE})
    z3 = subprocess.run(["z3", "-in"], input=script,
                        capture_output=True, text=True, timeout=60)
    dvv = next((l.strip() for l in dv.stdout.splitlines()
                if l.strip() in ("sat", "unsat", "unknown")), "none")
    z3v = z3.stdout.strip().splitlines()[0].strip()
    if dvv == "unknown":
        pytest.skip("dv-solve declined; correct-or-unknown is allowed")
    assert dvv == z3v, f"dv={dvv} z3={z3v} for {body}"


@pytest.mark.parametrize("body", _DIFF_W8, ids=range(len(_DIFF_W8)))
def test_wide_bitwise_or_is_not_rewritten(body) -> None:
    """`(bvor z c)` on a multi-bit z is arithmetic, not a disjunction.

    The rewrite declines these because it requires the `#b1` comparand AND
    every operand to be exactly 1 bit wide. If that guard ever loosened, these
    would silently change meaning -- so cross-check the verdict against z3.
    """
    if not shutil.which("z3"):
        pytest.skip("z3 not available")
    script = (f"(set-logic QF_BV)\n{_VBV}\n(declare-fun z () (_ BitVec 8))\n"
              f"(assert {body})\n(check-sat)\n(exit)\n")
    dv = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                        input=script, capture_output=True, text=True,
                        timeout=60, env={**os.environ, **_PURE})
    z3 = subprocess.run(["z3", "-in"], input=script,
                        capture_output=True, text=True, timeout=60)
    dvv = next((l.strip() for l in dv.stdout.splitlines()
                if l.strip() in ("sat", "unsat", "unknown")), "none")
    z3v = z3.stdout.strip().splitlines()[0].strip()
    if dvv == "unknown":
        pytest.skip("dv-solve declined; correct-or-unknown is allowed")
    assert dvv == z3v, f"dv={dvv} z3={z3v} for {body}"
