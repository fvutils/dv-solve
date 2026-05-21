"""Formal verification benchmark harness.

Provides solver wrappers, result collection, and the FormalSolver protocol
used by the Tier 1 baseline tests.
"""

from .protocol import FormalResult, FormalSolver
from .results_collector import ResultsCollector
