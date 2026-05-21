"""Tier 2 k-induction baseline: run k-induction SMT2 files through solvers.

For the inductive step, ``unsat`` means the property is proved
(inductive invariant holds at depth k).  ``sat`` means induction fails
at depth k (does not imply the property is false; may need higher k or
auxiliary invariants).

Usage:
    direnv exec . pytest tests/formal/tier2/test_tier2_kind_baseline.py -v
"""
from __future__ import annotations

from pathlib import Path

import pytest

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tests.formal.conftest import AVAILABLE_SOLVERS
from tests.formal.harness.results_collector import ResultsCollector

SMT2_DIR = Path(__file__).resolve().parents[1] / "smt2" / "tier2"

_TIER2_SOLVERS = [s for s in AVAILABLE_SOLVERS if s.name not in ("boolector", "dv-solve-smt2")]



def _kind_files() -> list[Path]:
    if not SMT2_DIR.is_dir():
        return []
    return sorted(SMT2_DIR.glob("*_kind_*.smt2"))


_FILES = _kind_files()

_collector = ResultsCollector(results_dir=Path(__file__).resolve().parents[1] / "results")


@pytest.fixture(scope="session")
def kind_collector():
    return _collector


@pytest.fixture(scope="session", autouse=True)
def write_kind_results(kind_collector):
    yield
    if kind_collector.results:
        csv_p, md_p = kind_collector.write_summary()
        print(f"\nTier 2 k-ind results: {csv_p}\n  {md_p}")


@pytest.mark.parametrize(
    "smt2_file",
    _FILES,
    ids=[f.stem for f in _FILES],
)
@pytest.mark.parametrize(
    "solver",
    _TIER2_SOLVERS,
    ids=[s.name for s in _TIER2_SOLVERS],
)
def test_tier2_kind(smt2_file, solver, kind_collector):
    """Run one k-induction file through one solver."""
    result = solver.solve(smt2_file, timeout_s=60.0)

    kind_collector.add(result)
    kind_collector.save_individual(result)
    print(f"\n  {result.summary()}")

    # k-induction may legitimately fail (sat) if k is too small.
    # We collect data but don't assert unsat.
