"""Benchmark: conditional range constraint.

Inspired by Karan-nevage/Systemverilog-Constraints-Interview-Questions
Easy/q37.sv (MIT license).

Original SV:
  if (a < 20) b inside {[10:30]};
  if (a > 50) b inside {[30:50]};

Variables (2):
  a -- control variable; 0..100
  b -- dependent variable; 0..100

Constraints (implication style):
  c_lo: a < 20  -> 10 <= b <= 30   [conditional range]
  c_hi: a > 50  -> 30 <= b <= 50   [conditional range]

Tests conditional implication with range constraints.
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class CondInside:
    a: zdc.rand(domain=(0, 100), default=25)
    b: zdc.rand(domain=(0, 100), default=20)

    @zdc.constraint
    def c_lo(self):
        # a < 20 implies b in [10, 30]
        assert self.a >= 20 or self.b >= 10
        assert self.a >= 20 or self.b <= 30

    @zdc.constraint
    def c_hi(self):
        # a > 50 implies b in [30, 50]
        assert self.a <= 50 or self.b >= 30
        assert self.a <= 50 or self.b <= 50


def _check(sol):
    a, b = sol["a"], sol["b"]
    assert 0 <= a <= 100
    assert 0 <= b <= 100
    if a < 20:
        assert 10 <= b <= 30, f"a={a} < 20 but b={b} not in [10,30]"
    if a > 50:
        assert 30 <= b <= 50, f"a={a} > 50 but b={b} not in [30,50]"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_cond_inside(solver, tmp_path):
    solver.bench(CondInside, validate=_check, tmp_path=tmp_path)
