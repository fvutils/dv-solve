"""Benchmark: array sum partition constraint.

Inspired by Karan-nevage/Systemverilog-Constraints-Interview-Questions
Moderate/q05.sv (MIT license) and Ibrahiiiiim/memory-using-sv-constraints
memory_n_var_partitions.sv (MIT license).

Original SV:
  arr[20], arr.sum() == 300, arr[i] inside {[1:30]}, sorted ascending.

Since zdc does not support arrays or .sum() directly, this is expressed
as 8 scalar fields with explicit sum and ordering constraints -- a
simplified but structurally equivalent problem.

Variables (9):
  p0..p6 -- partition sizes; domain [10, 80]
  total  -- sum helper;      domain [70, 560]
  target -- fixed target;    domain [200, 200]  (state, not random)

Constraints:
  c_sum      : p0 + p1 + p2 + p3 + p4 + p5 + p6 == target  [sum == const]
  c_ordered  : p0 <= p1 <= ... <= p6                          [weak ordering]

The sum constraint with ordering creates a constrained partition problem
that requires the solver to coordinate across all 7 variables.
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers

_TARGET = 200


@zdc.dataclass
class SumPartition:
    p0: zdc.rand(domain=(10, 80), default=28)
    p1: zdc.rand(domain=(10, 80), default=28)
    p2: zdc.rand(domain=(10, 80), default=28)
    p3: zdc.rand(domain=(10, 80), default=29)
    p4: zdc.rand(domain=(10, 80), default=29)
    p5: zdc.rand(domain=(10, 80), default=29)
    p6: zdc.rand(domain=(10, 80), default=29)

    # Running-sum helpers to decompose the 7-way sum into pairwise arithmetic
    s01:  zdc.rand(domain=(20, 160),  default=56)
    s012: zdc.rand(domain=(30, 240),  default=84)
    s0123: zdc.rand(domain=(40, 320), default=113)
    s01234: zdc.rand(domain=(50, 400), default=142)
    s012345: zdc.rand(domain=(60, 480), default=171)

    @zdc.constraint
    def c_sum_chain(self):
        assert self.s01 == self.p0 + self.p1
        assert self.s012 == self.s01 + self.p2
        assert self.s0123 == self.s012 + self.p3
        assert self.s01234 == self.s0123 + self.p4
        assert self.s012345 == self.s01234 + self.p5

    @zdc.constraint
    def c_total(self):
        assert self.s012345 + self.p6 == 200

    @zdc.constraint
    def c_ordered(self):
        assert self.p0 <= self.p1
        assert self.p1 <= self.p2
        assert self.p2 <= self.p3
        assert self.p3 <= self.p4
        assert self.p4 <= self.p5
        assert self.p5 <= self.p6


def _check(sol):
    parts = [sol[f"p{i}"] for i in range(7)]
    for i, p in enumerate(parts):
        assert 10 <= p <= 80, f"p{i}={p} out of range"

    assert sum(parts) == _TARGET, f"sum {sum(parts)} != {_TARGET}"

    for i in range(6):
        assert parts[i] <= parts[i + 1], (
            f"ordering violated: p{i}={parts[i]} > p{i+1}={parts[i+1]}"
        )

    # Verify running-sum helpers
    assert sol["s01"] == parts[0] + parts[1]
    assert sol["s012"] == sol["s01"] + parts[2]
    assert sol["s0123"] == sol["s012"] + parts[3]
    assert sol["s01234"] == sol["s0123"] + parts[4]
    assert sol["s012345"] == sol["s01234"] + parts[5]


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_sum_partition(solver, tmp_path):
    solver.bench(SumPartition, validate=_check, tmp_path=tmp_path)
