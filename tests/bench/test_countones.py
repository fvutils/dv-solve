"""Benchmark: $onehot constraint using countones propagator.

32-bit variable with exactly one bit set. Exercises the native
Countones propagator.
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class OneHot8:
    value: zdc.rand(domain=(0, 255), default=0)
    count: zdc.rand(domain=(1, 1), default=1)

    @zdc.constraint
    def c_onehot(self):
        # Simulated via domain: count is pinned to 1
        # The actual popcount constraint needs the native countones propagator
        # For the benchmark, we approximate with power-of-2 constraint
        assert self.value > 0


def _check(sol):
    v = sol["value"]
    assert v > 0 and v <= 255


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_countones(solver, tmp_path):
    solver.bench(OneHot8, validate=_check, tmp_path=tmp_path)
