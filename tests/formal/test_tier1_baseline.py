"""Tier 1 baseline: run all generated SMT2 files through available solvers.

Each (smt2_file, solver) pair is a separate test case.  All Tier 1 problems
are satisfiability checks from RandSMT2Emitter, so the expected result is
``sat``.

Usage:
    direnv exec . pytest tests/formal/test_tier1_baseline.py -v
"""
from __future__ import annotations

from pathlib import Path

import pytest

from .conftest import AVAILABLE_SOLVERS

SMT2_DIR = Path(__file__).resolve().parent / "smt2" / "tier1"


# Benchmarks where RandSMT2Emitter produces overconstrained SMT2 due to
# bitvector width-inference bugs (e.g. multiplication overflow).  Results
# are still collected but the SAT assertion is relaxed.
_KNOWN_EMITTER_ISSUES = {"arrayordering"}


def _smt2_files() -> list[Path]:
    """Discover all .smt2 files in the tier1 directory."""
    if not SMT2_DIR.is_dir():
        return []
    return sorted(SMT2_DIR.glob("*.smt2"))


_FILES = _smt2_files()


@pytest.mark.parametrize(
    "smt2_file",
    _FILES,
    ids=[f.stem for f in _FILES],
)
@pytest.mark.parametrize(
    "solver",
    AVAILABLE_SOLVERS,
    ids=[s.name for s in AVAILABLE_SOLVERS],
)
def test_tier1(smt2_file, solver, results_collector):
    """Run one SMT2 file through one solver and verify SAT."""
    result = solver.solve(smt2_file, timeout_s=30.0)
    results_collector.add(result)
    results_collector.save_individual(result)
    print(f"\n  {result.summary()}")

    if smt2_file.stem in _KNOWN_EMITTER_ISSUES:
        if result.result != "sat":
            pytest.xfail(f"Known emitter issue: {smt2_file.stem} returned {result.result}")
        return
    assert result.result == "sat", (
        f"{solver.name} returned '{result.result}' on {smt2_file.stem}:\n"
        f"  stdout: {result.stdout[:200]}\n"
        f"  stderr: {result.stderr[:200]}"
    )
