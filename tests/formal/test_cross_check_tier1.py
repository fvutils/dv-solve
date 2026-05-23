"""Cross-check dv-solve against z3 on tier1 (pure QF_BV) fixtures.

The existing test_cross_check.py only runs tier2/tier3 (yosys-smtbmc
dialect). That dialect doesn't exercise the bvand+range+masked-upper
shape that triggered false-UNSAT in 5 tier1 fixtures (see
benchmark report 2026-05-23). This test closes the gap.

Pass rules mirror test_cross_check.py:
  * dv-solve and z3 both definitive and agree            → pass
  * dv-solve `unknown` / `timeout` / `error`             → skip
  * z3 `unknown` / `timeout` / `error`                   → skip
  * both definitive and disagree                         → FAIL
"""
from __future__ import annotations

from pathlib import Path

import pytest

from .harness.dv_solve_smt2_solver import DvSolveSMT2Solver
from .harness.z3_solver import Z3Solver

TIER1_DIR = Path(__file__).resolve().parent / "smt2" / "tier1"
PER_SOLVER_TIMEOUT_S = 15.0

_NON_ANSWERS = {"unknown", "timeout", "error"}


def _smt2_files() -> list[Path]:
    return sorted(TIER1_DIR.glob("*.smt2")) if TIER1_DIR.is_dir() else []


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
        pytest.skip("z3 not on PATH")
    return _Z3


@pytest.mark.parametrize(
    "smt2_file",
    _FILES,
    ids=[f.stem for f in _FILES],
)
def test_cross_check_tier1(smt2_file, dv_solver, z3_solver):
    dv = dv_solver.solve(smt2_file, timeout_s=PER_SOLVER_TIMEOUT_S)
    z3 = z3_solver.solve(smt2_file, timeout_s=PER_SOLVER_TIMEOUT_S)

    if dv.result in _NON_ANSWERS:
        pytest.skip(f"dv-solve returned non-answer: {dv.result}")
    if z3.result not in {"sat", "unsat"}:
        pytest.skip(f"z3 returned non-answer: {z3.result}")

    assert dv.result == z3.result, (
        f"dv-solve says '{dv.result}', z3 says '{z3.result}' on "
        f"{smt2_file.stem}\n"
        f"  dv-solve stdout: {dv.stdout[:300]}\n"
        f"  z3      stdout: {z3.stdout[:300]}"
    )
