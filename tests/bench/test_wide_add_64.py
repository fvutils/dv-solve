"""Benchmark: 64-bit unsigned addition with relational bound.

Exercises 64-bit arithmetic -- a regime where BDD-based solvers (VCS,
Questa) suffer exponential node explosion, while interval-propagation
solvers handle it in constant time.

Variables (3):
  a, b   -- 64-bit unsigned operands
  sum_ab -- 64-bit unsigned result

Constraints:
  c_sum    : sum_ab == a + b                          [64-bit addition]
  c_order  : a < b                                    [relational]
  c_bound  : sum_ab <= 0xFFFF_FFFF_FFFF_0000          [upper bound]
  c_min    : a >= 0x0000_0001_0000_0000               [lower bound on a]
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers

_MAX_SUM = 0xFFFF_FFFF_FFFF_0000
_MIN_A   = 0x0000_0001_0000_0000
_MAX_OP  = 0x7FFF_FFFF_FFFF_8000  # ensures a + b can't overflow past _MAX_SUM


@zdc.dataclass
class WideAdd64:
    a:      zdc.rand(domain=(_MIN_A, _MAX_OP),  default=_MIN_A)
    b:      zdc.rand(domain=(_MIN_A, _MAX_OP),  default=_MIN_A + 1)
    sum_ab: zdc.rand(domain=(2 * _MIN_A, _MAX_SUM), default=2 * _MIN_A + 1)

    @zdc.constraint
    def c_sum(self):
        assert self.sum_ab == self.a + self.b

    @zdc.constraint
    def c_order(self):
        assert self.a < self.b

    @zdc.constraint
    def c_bound(self):
        assert self.sum_ab <= _MAX_SUM


def _check(sol):
    a, b, s = sol["a"], sol["b"], sol["sum_ab"]
    assert s == a + b, f"sum mismatch: {s:#018x} != {a:#018x} + {b:#018x}"
    assert a < b, f"ordering violated: a={a:#018x} >= b={b:#018x}"
    assert s <= _MAX_SUM, f"sum exceeds bound: {s:#018x}"
    assert a >= _MIN_A, f"a below minimum: {a:#018x}"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_wide_add_64(solver, tmp_path):
    solver.bench(WideAdd64, validate=_check, tmp_path=tmp_path)
