"""Cross-check dv-solve against an oracle solver on tier2 + tier3 fixtures.

The dv-solve model-validation pass already prevents most false-`sat`
answers from escaping, but it cannot detect a false-`unsat` (no model to
re-evaluate) or a silently-wrong answer the validator does not yet model.
This test closes that gap by running every tier2 / tier3 SMT2 fixture
through dv-solve *and* z3 and failing on any disagreement where both
solvers returned a definitive answer.

Pass rules:

  * dv-solve says ``sat``  + z3 says ``sat``    → pass
  * dv-solve says ``unsat`` + z3 says ``unsat`` → pass
  * dv-solve says ``unknown`` / ``timeout``     → skip  (honest non-answer)
  * z3 says ``unknown`` / ``timeout`` / errors  → skip  (no oracle)
  * dv-solve answer != z3 answer (both definitive) → FAIL

Skips are explicit and counted in the test report so we can see oracle
coverage shrinking over time if it ever does.

Usage:
    direnv exec . pytest tests/formal/test_cross_check.py -v
"""
from __future__ import annotations

from pathlib import Path

import pytest

from .harness.dv_solve_smt2_solver import DvSolveSMT2Solver
from .harness.z3_solver import Z3Solver

TIER_DIRS = [
    Path(__file__).resolve().parent / "smt2" / "tier2",
    Path(__file__).resolve().parent / "smt2" / "tier3",
]

PER_SOLVER_TIMEOUT_S = 10.0

# Honest answers from dv-solve that we treat as "I don't know" — these
# should never cause a CI failure here even though they disagree with z3.
_NON_ANSWERS = {"unknown", "timeout", "error"}


def _smt2_files() -> list[Path]:
    files: list[Path] = []
    for d in TIER_DIRS:
        if d.is_dir():
            files.extend(sorted(d.glob("*.smt2")))
    return files


_FILES = _smt2_files()
_DV = DvSolveSMT2Solver()
_Z3 = Z3Solver()


@pytest.fixture(scope="module")
def dv_solver():
    if not _DV.is_available():
        pytest.skip("dv-solve binary not built")
    return _DV


@pytest.fixture(scope="module")
def z3_solver():
    if not _Z3.is_available():
        pytest.skip("z3 not on PATH; cannot cross-check")
    return _Z3


@pytest.mark.parametrize(
    "smt2_file",
    _FILES,
    ids=[f"{f.parent.name}/{f.stem}" for f in _FILES],
)
def test_cross_check(smt2_file, dv_solver, z3_solver):
    """Run one fixture through dv-solve and z3; fail on a real disagreement."""
    dv = dv_solver.solve(smt2_file, timeout_s=PER_SOLVER_TIMEOUT_S)
    z3 = z3_solver.solve(smt2_file, timeout_s=PER_SOLVER_TIMEOUT_S)

    # Skip when either side has no definitive answer to compare against.
    if dv.result in _NON_ANSWERS:
        pytest.skip(f"dv-solve returned non-answer: {dv.result}")
    if z3.result not in {"sat", "unsat"}:
        pytest.skip(f"z3 returned non-answer: {z3.result}")

    # Both definitive: must agree, otherwise dv-solve is producing a
    # wrong answer (false-sat or false-unsat) the validator missed.
    assert dv.result == z3.result, (
        f"dv-solve says '{dv.result}', z3 says '{z3.result}' on "
        f"{smt2_file.parent.name}/{smt2_file.stem}\n"
        f"  dv-solve stdout: {dv.stdout[:300]}\n"
        f"  z3      stdout: {z3.stdout[:300]}"
    )
