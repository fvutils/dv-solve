"""Tier 2 BMC baseline: run all generated BMC SMT2 files through available solvers.

For BMC problems, ``unsat`` means the assertion holds at the given depth
(no counterexample found).  ``sat`` means a counterexample was found.

Usage:
    direnv exec . pytest tests/formal/tier2/test_tier2_bmc_baseline.py -v
"""
from __future__ import annotations

from pathlib import Path

import pytest

# Re-use the formal harness solvers from the parent package
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tests.formal.conftest import AVAILABLE_SOLVERS
from tests.formal.harness.results_collector import ResultsCollector

SMT2_DIR = Path(__file__).resolve().parents[1] / "smt2" / "tier2"

# Boolector doesn't support declare-sort (QF_UFBV); skip it for Tier 2
_TIER2_SOLVERS = [s for s in AVAILABLE_SOLVERS if s.name not in ("boolector", "dv-solve-smt2")]

# Benchmarks where we expect assertion violations (known model limitations)
_KNOWN_ISSUES: set[str] = set()



def _bmc_files() -> list[Path]:
    """Discover all BMC .smt2 files in the tier2 directory."""
    if not SMT2_DIR.is_dir():
        return []
    return sorted(SMT2_DIR.glob("*_bmc_*.smt2"))


_FILES = _bmc_files()

# Module-scoped collector
_collector = ResultsCollector(results_dir=Path(__file__).resolve().parents[1] / "results")


@pytest.fixture(scope="session")
def tier2_collector():
    return _collector


@pytest.fixture(scope="session", autouse=True)
def write_tier2_results(tier2_collector):
    yield
    if tier2_collector.results:
        csv_p, md_p = tier2_collector.write_summary()
        print(f"\nTier 2 BMC results: {csv_p}\n  {md_p}")


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
def test_tier2_bmc(smt2_file, solver, tier2_collector):
    """Run one Tier 2 BMC file through one solver and verify unsat."""
    result = solver.solve(smt2_file, timeout_s=60.0)

    tier2_collector.add(result)
    tier2_collector.save_individual(result)
    print(f"\n  {result.summary()}")

    bench_key = smt2_file.stem
    if bench_key in _KNOWN_ISSUES:
        if result.result != "unsat":
            pytest.xfail(f"Known issue: {bench_key} returned {result.result}")
        return

    assert result.result == "unsat", (
        f"{solver.name} returned '{result.result}' on {smt2_file.stem}:\n"
        f"  stdout: {result.stdout[:200]}\n"
        f"  stderr: {result.stderr[:200]}"
    )
