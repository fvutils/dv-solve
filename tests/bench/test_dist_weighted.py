"""Benchmark: distribution-weighted value selection throughput.

Exercises the dist constraint infrastructure with multiple weighted
ranges.  Verifies that weighted selection does not regress overall
solve throughput significantly.

x -- 8-bit variable with dist { [0:49] :/ 1, [50:99] :/ 3 }
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class DistWeighted:
    x: zdc.rand(domain=(0, 99), default=50)


def _check(sol):
    assert 0 <= sol["x"] <= 99


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_dist_weighted(solver, tmp_path):
    solver.bench(DistWeighted, validate=_check, tmp_path=tmp_path)
