"""Benchmark: array sum constraint.

8-element array, sum constrained to 1000. Exercises the SumEq propagator.
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class ArraySum8:
    a0: zdc.rand(domain=(0, 255), default=0)
    a1: zdc.rand(domain=(0, 255), default=0)
    a2: zdc.rand(domain=(0, 255), default=0)
    a3: zdc.rand(domain=(0, 255), default=0)
    a4: zdc.rand(domain=(0, 255), default=0)
    a5: zdc.rand(domain=(0, 255), default=0)
    a6: zdc.rand(domain=(0, 255), default=0)
    a7: zdc.rand(domain=(0, 255), default=0)
    s:  zdc.rand(domain=(0, 2040), default=0)

    @zdc.constraint
    def c_sum(self):
        assert self.s == self.a0 + self.a1 + self.a2 + self.a3 + \
                          self.a4 + self.a5 + self.a6 + self.a7

    @zdc.constraint
    def c_target(self):
        assert self.s == 1000


def _check(sol):
    total = sum(sol[f"a{i}"] for i in range(8))
    assert total == 1000, f"sum={total}, expected 1000"
    for i in range(8):
        assert 0 <= sol[f"a{i}"] <= 255


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_array_sum(solver, tmp_path):
    solver.bench(ArraySum8, validate=_check, tmp_path=tmp_path)
