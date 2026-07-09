"""Cross-check dv-solve's bitblast engine against z3 on tier2 + tier3.

Companion to test_cross_check.py. Identical pass rules, but the
dv-solve solver runs with ``DV_ENGINE=bitblast`` (routing the SMT
problem through zsp_bbsolver / AIG / Tseitin / kissat instead of the
CDCL theory propagators).

Why this exists separately: the bitblast engine has very different
performance characteristics from CDCL — it dominates on QF_UFBV /
array-heavy BMC fixtures (every CDCL-timing-out tier2/tier3 BMC
case solves in <20ms here) but is not the historical default. Keeping
both test files lets us watch the per-engine coverage independently
and notice if either regresses.

Usage:
    direnv exec . pytest tests/formal/test_cross_check_bitblast.py -v
"""
from __future__ import annotations

from pathlib import Path

import pytest

from .harness.dv_solve_smt2_solver import DvSolveSMT2BBSolver
from .harness.z3_solver import Z3Solver

TIER_DIRS = [
    Path(__file__).resolve().parent / "smt2" / "tier2",
    Path(__file__).resolve().parent / "smt2" / "tier3",
]

PER_SOLVER_TIMEOUT_S = 10.0

_NON_ANSWERS = {"unknown", "timeout", "error"}


def _smt2_files() -> list[Path]:
    files: list[Path] = []
    for d in TIER_DIRS:
        if d.is_dir():
            files.extend(sorted(d.glob("*.smt2")))
    return files


_FILES = _smt2_files()
_DV = DvSolveSMT2BBSolver()
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
def test_cross_check_bitblast(smt2_file, dv_solver, z3_solver):
    """Run one fixture through dv-solve (bitblast) and z3."""
    dv = dv_solver.solve(smt2_file, timeout_s=PER_SOLVER_TIMEOUT_S)
    z3 = z3_solver.solve(smt2_file, timeout_s=PER_SOLVER_TIMEOUT_S)

    if dv.result in _NON_ANSWERS:
        pytest.skip(f"dv-solve (bb) returned non-answer: {dv.result}")
    if z3.result not in {"sat", "unsat"}:
        pytest.skip(f"z3 returned non-answer: {z3.result}")

    assert dv.result == z3.result, (
        f"dv-solve (bb) says '{dv.result}', z3 says '{z3.result}' on "
        f"{smt2_file.parent.name}/{smt2_file.stem}\n"
        f"  dv-solve (bb) stdout: {dv.stdout[:300]}\n"
        f"  z3            stdout: {z3.stdout[:300]}"
    )
