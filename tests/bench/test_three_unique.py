"""Benchmark: three unique values.

Inspired by Karan-nevage/Systemverilog-Constraints-Interview-Questions
Easy/q38.sv (MIT license).

Original SV:
  a inside {[0:100]}; b inside {[0:100]}; c inside {[0:100]};
  a != b; b != c; c != a;

Variables (3):
  a, b, c -- three integers in [0, 100]

Constraints:
  c_all_different: a != b, b != c, c != a   [pairwise inequality]

Simple but exercises the != constraint across all pairs -- the manual
equivalent of SV's `unique {a, b, c}`.
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class ThreeUnique:
    a: zdc.rand(domain=(0, 100), default=0)
    b: zdc.rand(domain=(0, 100), default=50)
    c: zdc.rand(domain=(0, 100), default=100)

    @zdc.constraint
    def c_all_different(self):
        assert self.a != self.b
        assert self.b != self.c
        assert self.c != self.a


def _check(sol):
    a, b, c = sol["a"], sol["b"], sol["c"]
    assert 0 <= a <= 100
    assert 0 <= b <= 100
    assert 0 <= c <= 100
    assert a != b, f"a == b == {a}"
    assert b != c, f"b == c == {b}"
    assert c != a, f"c == a == {c}"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_three_unique(solver, tmp_path):
    solver.bench(ThreeUnique, validate=_check, tmp_path=tmp_path)
