"""Differential coverage for the Verilator constraint-feature checklist gaps.

Runs each gap-probe fixture (see generate_gaps_smt2.py) through both dv-solve
engines (CDCL + bitblast) and z3, enforcing:

  * **Soundness (always):** neither dv-solve engine may *disagree* with z3.
    A dv-solve answer must be z3's answer or ``unknown``/``timeout`` (an honest
    punt). A hard sat-vs-unsat disagreement fails — that's a soundness bug.
  * **Coverage (supported shapes):** for every fixture NOT marked ``_wideunk``,
    each engine must actually answer and match z3 (no ``unknown`` allowed) —
    this is what turns a checklist "GAP" into real, guarded coverage.

``*_wideunk`` fixtures are >64-bit shapes dv-solve answers ``unknown`` today
(documented frontend incompleteness — the value model is 64-bit; see the
checklist). They assert soundness only.

Usage:
    direnv exec . pytest tests/formal/test_gap_coverage.py -v
"""
from __future__ import annotations

from pathlib import Path

import pytest

from .harness.dv_solve_smt2_solver import DvSolveSMT2Solver, DvSolveSMT2BBSolver
from .harness.z3_solver import Z3Solver

GAP_DIR = Path(__file__).resolve().parent / "smt2" / "gaps"
TIMEOUT_S = 10.0
_NON_ANSWERS = {"unknown", "timeout", "error"}

_DV_CDCL = DvSolveSMT2Solver()
_DV_BB = DvSolveSMT2BBSolver()
_Z3 = Z3Solver()


def _files() -> list[Path]:
    return sorted(GAP_DIR.glob("*.smt2")) if GAP_DIR.is_dir() else []


_FILES = _files()


@pytest.fixture(scope="module")
def z3_oracle():
    if not _Z3.is_available():
        pytest.skip("z3 not on PATH; cannot cross-check")
    return _Z3


@pytest.mark.parametrize("smt2_file", _FILES, ids=[f.stem for f in _FILES])
def test_gap_coverage(smt2_file, z3_oracle):
    if not _DV_CDCL.is_available():
        pytest.skip("dv-solve-smt2 binary not built")

    z3 = z3_oracle.solve(smt2_file, timeout_s=TIMEOUT_S).result
    if z3 not in {"sat", "unsat"}:
        pytest.skip(f"z3 returned non-answer: {z3}")

    cdcl = _DV_CDCL.solve(smt2_file, timeout_s=TIMEOUT_S).result
    bb = _DV_BB.solve(smt2_file, timeout_s=TIMEOUT_S).result

    # Soundness: no hard disagreement with the z3 oracle on either engine.
    for eng, res in (("cdcl", cdcl), ("bitblast", bb)):
        if res in {"sat", "unsat"}:
            assert res == z3, (
                f"SOUNDNESS: dv-solve ({eng}) says '{res}', z3 says '{z3}' "
                f"on {smt2_file.stem}"
            )

    # Coverage: supported (non-wide) shapes must be answered, not punted.
    if not smt2_file.stem.endswith("_wideunk"):
        assert cdcl not in _NON_ANSWERS, (
            f"COVERAGE: dv-solve (cdcl) punted ('{cdcl}') on supported shape "
            f"{smt2_file.stem}"
        )
        assert bb not in _NON_ANSWERS, (
            f"COVERAGE: dv-solve (bitblast) punted ('{bb}') on supported shape "
            f"{smt2_file.stem}"
        )
