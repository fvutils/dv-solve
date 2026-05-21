"""Benchmark: soft constraint relaxation overhead.

Measures throughput when soft constraints conflict with hard constraints
and must be relaxed.  Compares against a baseline without soft constraints
to quantify the assumption-based relaxation overhead.

x      -- 8-bit variable [0, 255]

Hard:  x >= 100
Soft:  x == 5  (priority 10, conflicts with hard -> relaxed)
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class SoftRelaxBaseline:
    """Baseline: just hard constraint, no soft."""
    x: zdc.rand(domain=(100, 255), default=100)


@zdc.dataclass
class SoftRelaxWithConflict:
    """Hard + conflicting soft (must be relaxed each solve)."""
    x: zdc.rand(domain=(0, 255), default=100)

    @zdc.constraint
    def c_hard(self):
        assert self.x >= 100


def _check_baseline(sol):
    assert 100 <= sol["x"] <= 255


def _check_with_soft(sol):
    assert 100 <= sol["x"] <= 255


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_soft_relaxation_baseline(solver, tmp_path):
    """Throughput without soft constraints (reference baseline)."""
    solver.bench(SoftRelaxBaseline, validate=_check_baseline, tmp_path=tmp_path)


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_soft_relaxation_with_conflict(solver, tmp_path):
    """Throughput with a conflicting soft constraint that must be relaxed."""
    solver.bench(SoftRelaxWithConflict, validate=_check_with_soft, tmp_path=tmp_path)
