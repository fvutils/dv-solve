"""A Boolean `ite` used as a constraint must apply only the branch taken.

B53: compile handled `(ite c A B)` at the constraint root by compiling each
branch normally and then gating the propagators it could find -- only the
last one, or one off by one. Everything else the compile did applied
whichever way `c` went: root-domain tightening, `x == y` merging, and the
outright `unsat` of a `false` branch. `(ite (= x y) false true)`, which just
says `x != y`, came back `unsat`. Each branch is now reified and implied by
its guard. Found by the soundness generator (docs/soundness_coverage_plan.md)
the first time it produced Boolean `ite`.
"""
from __future__ import annotations

import itertools
import subprocess
from pathlib import Path

import pytest

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"

_HDR = ("(set-logic QF_BV)(declare-const x (_ BitVec 3))(declare-const y (_ BitVec 3))"
        "(declare-const z (_ BitVec 6))")


def _check(asserts: str) -> str:
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    p = subprocess.run([str(_EXE)], input=_HDR + asserts + "(check-sat)",
                       capture_output=True, text=True, timeout=30)
    return p.stdout.split()[0]


@pytest.mark.parametrize("asserts", [
    "(assert (ite (= x y) false true))",
    "(assert (ite (= z z) (bvuge y x) (bvugt (_ bv1 3) (_ bv4 3))))",
    "(assert (ite (= z (_ bv5 6)) false (bvuge y x)))",
    "(assert (ite (= z (_ bv5 6)) (bvuge y x) (bvult y x)))",
    "(assert (ite (= x (_ bv2 3)) (= y z) (= y x)))",
])
def test_satisfiable_ite_constraint(asserts):
    assert _check(asserts) == "sat"


_CONDS = ["(= x (_ bv3 3))", "(= x y)", "(bvult x y)"]
_BRANCHES = ["false", "true", "(= y (_ bv5 3))", "(bvuge y x)", "(= y x)",
             "(distinct y (_ bv0 3))", "(bvult (bvadd x y) (_ bv2 3))"]


def _holds(t: str, x: int, y: int) -> bool:
    env = {
        "false": False, "true": True,
        "(= x (_ bv3 3))": x == 3, "(= x y)": x == y, "(bvult x y)": x < y,
        "(= y (_ bv5 3))": y == 5, "(bvuge y x)": y >= x, "(= y x)": y == x,
        "(distinct y (_ bv0 3))": y != 0, "(bvult (bvadd x y) (_ bv2 3))": (x + y) % 8 < 2,
    }
    return env[t]


def test_ite_constraints_against_brute_force():
    bad = []
    for c, a, b in itertools.product(_CONDS, _BRANCHES, _BRANCHES):
        for extra, pred in [("", lambda x, y: True),
                            ("(assert (= x (_ bv3 3)))", lambda x, y: x == 3),
                            ("(assert (distinct x (_ bv3 3)))", lambda x, y: x != 3)]:
            exp = any((_holds(a, x, y) if _holds(c, x, y) else _holds(b, x, y)) and pred(x, y)
                      for x in range(8) for y in range(8))
            got = _check(f"(assert (ite {c} {a} {b})){extra}")
            if got != ("sat" if exp else "unsat"):
                bad.append((c, a, b, extra, got))
    assert not bad, bad[:10]
