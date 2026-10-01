"""Clause learning must never remove a solution.

Each problem here is satisfiable and was answered `unsat` (or never answered)
because conflict analysis learnt a clause stronger than the constraints imply.
They are small on purpose: the step-checker build (-DDVS_STEP_CHECK=ON) checks
every learnt clause against the problem, and these are the shapes that broke
it. See docs/soundness_coverage_plan.md.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"

_HDR = "(set-logic QF_BV)\n" + "".join(
    f"(declare-const {v} (_ BitVec 4))\n" for v in "xyz")


def _check(asserts: str) -> str:
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    p = subprocess.run([str(_EXE)], input=_HDR + asserts + "(check-sat)\n",
                       capture_output=True, text=True, timeout=20)
    return p.stdout.split()[0]


@pytest.mark.parametrize("name,asserts", [
    # B50: the `!=` explanation left out the narrowed variable's own previous
    # bound (z - x != z raises (z - x).lo from 14 to 15 only because it was 14).
    ("b50_ne_own_bound",
     "(assert (distinct (bvsub z x) z))(assert (bvugt (bvsub z x) (_ bv14 4)))"),
    # B50, same defect through bvand and a reified distinct.
    ("b50_band_distinct",
     "(assert (bvule (bvand y z) x))(assert (bvugt (bvadd x y) x))"
     "(assert (bvugt y z))(assert (distinct (bvand y z) (_ bv0 4)))"),
    # B51: a conflict made by the search itself (an exhausted split) was
    # blamed on whichever propagator had failed last.
    ("b51_stale_conflict_propagator",
     "(assert (= (bvsub z x) (_ bv12 4)))(assert (bvult (bvadd x y) y))"
     "(assert (bvult x z))"),
    # The propagation that empties a domain must be explained, not taken as
    # the UIP: otherwise the learnt clause is already true at the root and
    # the search re-derives the same conflict for ever.
    ("emptying_propagation_is_resolved",
     "(assert (= (bvand y z) (_ bv3 4)))(assert (bvule (bvand y z) x))"),
])
def test_satisfiable(name, asserts):
    assert _check(asserts) == "sat"
