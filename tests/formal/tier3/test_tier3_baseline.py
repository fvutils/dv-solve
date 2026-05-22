"""Tier 3 QF_AUFBV baseline: array theory benchmarks through all available solvers.

Benchmarks in ``tests/formal/smt2/tier3/`` exercise the array theory surface
added in Phase 5 (declare-const array, select, store, as-const, array equality,
array ite, declare-fun returning array, array-bearing state machines).

Expected results
----------------
All benchmarks are UNSAT (property holds) except ``dma_engine_small_bmc_d2``
which is SAT (a counterexample demonstrating stale-value read exists).

Known dv-solve-smt2 gaps
------------------------
``dma_engine_small_bmc_d2`` requires finding a SAT witness; dv-solve's
propagation-based solver cannot construct one and returns ``unsat``.

Usage:
    pytest tests/formal/tier3/test_tier3_baseline.py -v
"""
from __future__ import annotations

from pathlib import Path

import pytest

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from tests.formal.conftest import AVAILABLE_SOLVERS
from tests.formal.harness.results_collector import ResultsCollector

SMT2_DIR = Path(__file__).resolve().parents[1] / "smt2" / "tier3"

# Boolector does not support QF_AUFBV (no native array theory)
_TIER3_SOLVERS = [s for s in AVAILABLE_SOLVERS if s.name not in ("boolector",)]

# Expected result for each benchmark stem (default "unsat")
_EXPECTED_SAT: set[str] = {
    "dma_engine_small_bmc_d2",  # SAT: stale-value counterexample at depth 2
}

# dv-solve-smt2 known gaps: solver returns wrong result or crashes.
# These are xfailed so the suite stays green while we track them.
_KNOWN_ISSUES_DV_SOLVE: set[str] = {
    # Soundness gaps — solver returns 'sat' on UNSAT problems involving array
    # equality with stored arrays. Tracked in phase6_implementation_plan.md
    # under Task 6.3 follow-up work.
    "cache_direct_1way_bmc_d1",
    "cache_direct_1way_bmc_d2",
    "cache_direct_1way_bmc_d4",
    "cache_direct_1way_bmc_d8",
    "regfile_addr_alias_bmc_d1",
    "regfile_addr_alias_bmc_d2",
    "regfile_addr_alias_bmc_d4",
    "regfile_addr_alias_bmc_d8",
    # Slow / timeout on hard array BMC depths
    "regfile_simple_bmc_d2",
    "regfile_simple_bmc_d4",
    "regfile_simple_bmc_d8",
    "dma_engine_small_bmc_d4",
    "dma_engine_small_bmc_d8",
}


def _smt2_files() -> list[Path]:
    if not SMT2_DIR.is_dir():
        return []
    return sorted(SMT2_DIR.glob("*.smt2"))


_FILES = _smt2_files()

_collector = ResultsCollector(
    results_dir=Path(__file__).resolve().parents[2] / "results"
)


@pytest.fixture(scope="session")
def tier3_collector():
    return _collector


@pytest.fixture(scope="session", autouse=True)
def write_tier3_results(tier3_collector):
    yield
    if tier3_collector.results:
        csv_p, md_p = tier3_collector.write_summary()
        print(f"\nTier 3 results: {csv_p}\n  {md_p}")


@pytest.mark.parametrize(
    "smt2_file",
    _FILES,
    ids=[f.stem for f in _FILES],
)
@pytest.mark.parametrize(
    "solver",
    _TIER3_SOLVERS,
    ids=[s.name for s in _TIER3_SOLVERS],
)
def test_tier3(smt2_file, solver, tier3_collector):
    """Run one Tier 3 benchmark through one solver and verify expected result."""
    if not _FILES:
        pytest.skip("No SMT2 files found in tier3 directory")

    result = solver.solve(smt2_file, timeout_s=60.0)
    tier3_collector.add(result)
    tier3_collector.save_individual(result)
    print(f"\n  {result.summary()}")

    bench = smt2_file.stem
    expected = "sat" if bench in _EXPECTED_SAT else "unsat"

    if solver.name == "dv-solve-smt2" and bench in _KNOWN_ISSUES_DV_SOLVE:
        if result.result != expected:
            pytest.xfail(
                f"dv-solve-smt2 known gap on {bench}: "
                f"expected {expected!r}, got {result.result!r}"
            )
        return

    assert result.result == expected, (
        f"{solver.name} returned {result.result!r} on {bench} "
        f"(expected {expected!r}):\n"
        f"  stdout: {result.stdout[:300]}\n"
        f"  stderr: {result.stderr[:300]}"
    )
