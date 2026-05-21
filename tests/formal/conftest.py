"""Pytest configuration for formal verification tests."""
from __future__ import annotations

import pytest

from .harness.boolector_solver import BoolectorSolver
from .harness.bitwuzla_solver import BitwuzlaSolver
from .harness.z3_solver import Z3Solver
from .harness.cvc5_solver import CVC5Solver
from .harness.dv_solve_smt2_solver import DvSolveSMT2Solver
from .harness.results_collector import ResultsCollector

# All solver instances -- tests pick available ones
ALL_SOLVERS = [
    BoolectorSolver(),
    BitwuzlaSolver(),
    Z3Solver(),
    DvSolveSMT2Solver(),
    CVC5Solver(),
]

AVAILABLE_SOLVERS = [s for s in ALL_SOLVERS if s.is_available()]

# Module-scoped results collector shared across the tier1 test run
_collector = ResultsCollector()


@pytest.fixture(scope="session")
def results_collector():
    return _collector


@pytest.fixture(scope="session", autouse=True)
def write_results_on_finish(results_collector):
    """Write summary CSV + Markdown after all formal tests finish."""
    yield
    if results_collector.results:
        csv_p, md_p = results_collector.write_summary()
        print(f"\nResults written to:\n  {csv_p}\n  {md_p}")
