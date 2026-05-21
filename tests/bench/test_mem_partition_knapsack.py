"""Benchmark: knapsack-style memory partitioning.

8 partitions whose sizes are drawn from a discrete set must sum to an
exact target.  Start addresses are derived from cumulative sums.

This is a constrained partition / subset-sum problem that causes BDD
intermediate blowup in commercial solvers.

Derived from Ibrahiiiiim/memory-using-sv-constraints (MIT license).

Variables (16):
  s0..s7   -- partition sizes from {64, 128, 256, 512}
  a0..a7   -- start addresses (cumulative sums)

Constraints:
  c_sum   : s0 + s1 + ... + s7 == 2048
  c_start : a0 == 0; a_i == a_{i-1} + s_{i-1}  for i > 0
  c_vals  : each s_i in {64, 128, 256, 512}  (via disjunction)
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers

_TARGET = 2048


@zdc.dataclass
class MemPartitionKnapsack:
    s0: zdc.rand(domain=(64, 512), default=256)
    s1: zdc.rand(domain=(64, 512), default=256)
    s2: zdc.rand(domain=(64, 512), default=256)
    s3: zdc.rand(domain=(64, 512), default=256)
    s4: zdc.rand(domain=(64, 512), default=256)
    s5: zdc.rand(domain=(64, 512), default=256)
    s6: zdc.rand(domain=(64, 512), default=256)
    s7: zdc.rand(domain=(64, 512), default=256)

    a0: zdc.rand(domain=(0, 0),              default=0)
    a1: zdc.rand(domain=(64, _TARGET - 64),  default=256)
    a2: zdc.rand(domain=(128, _TARGET - 64), default=512)
    a3: zdc.rand(domain=(192, _TARGET - 64), default=768)
    a4: zdc.rand(domain=(256, _TARGET - 64), default=1024)
    a5: zdc.rand(domain=(320, _TARGET - 64), default=1280)
    a6: zdc.rand(domain=(384, _TARGET - 64), default=1536)
    a7: zdc.rand(domain=(448, _TARGET - 64), default=1792)

    total: zdc.rand(domain=(_TARGET, _TARGET), default=_TARGET)

    # Sizes from discrete set
    @zdc.constraint
    def c_s0_vals(self):
        assert self.s0 == 64 or self.s0 == 128 or self.s0 == 256 or self.s0 == 512

    @zdc.constraint
    def c_s1_vals(self):
        assert self.s1 == 64 or self.s1 == 128 or self.s1 == 256 or self.s1 == 512

    @zdc.constraint
    def c_s2_vals(self):
        assert self.s2 == 64 or self.s2 == 128 or self.s2 == 256 or self.s2 == 512

    @zdc.constraint
    def c_s3_vals(self):
        assert self.s3 == 64 or self.s3 == 128 or self.s3 == 256 or self.s3 == 512

    @zdc.constraint
    def c_s4_vals(self):
        assert self.s4 == 64 or self.s4 == 128 or self.s4 == 256 or self.s4 == 512

    @zdc.constraint
    def c_s5_vals(self):
        assert self.s5 == 64 or self.s5 == 128 or self.s5 == 256 or self.s5 == 512

    @zdc.constraint
    def c_s6_vals(self):
        assert self.s6 == 64 or self.s6 == 128 or self.s6 == 256 or self.s6 == 512

    @zdc.constraint
    def c_s7_vals(self):
        assert self.s7 == 64 or self.s7 == 128 or self.s7 == 256 or self.s7 == 512

    # Sum to target
    @zdc.constraint
    def c_sum(self):
        assert self.total == self.s0 + self.s1 + self.s2 + self.s3 + \
                              self.s4 + self.s5 + self.s6 + self.s7

    # Cumulative start addresses
    @zdc.constraint
    def c_a0(self):
        assert self.a0 == 0

    @zdc.constraint
    def c_a1(self):
        assert self.a1 == self.a0 + self.s0

    @zdc.constraint
    def c_a2(self):
        assert self.a2 == self.a1 + self.s1

    @zdc.constraint
    def c_a3(self):
        assert self.a3 == self.a2 + self.s2

    @zdc.constraint
    def c_a4(self):
        assert self.a4 == self.a3 + self.s3

    @zdc.constraint
    def c_a5(self):
        assert self.a5 == self.a4 + self.s4

    @zdc.constraint
    def c_a6(self):
        assert self.a6 == self.a5 + self.s5

    @zdc.constraint
    def c_a7(self):
        assert self.a7 == self.a6 + self.s6


def _check(sol):
    sizes  = [sol[f"s{i}"] for i in range(8)]
    starts = [sol[f"a{i}"] for i in range(8)]
    valid_sizes = {64, 128, 256, 512}

    for i, s in enumerate(sizes):
        assert s in valid_sizes, f"s{i}={s} not in {valid_sizes}"
    assert sum(sizes) == _TARGET, f"sum={sum(sizes)} != {_TARGET}"
    assert starts[0] == 0
    for i in range(1, 8):
        assert starts[i] == starts[i-1] + sizes[i-1], (
            f"a{i}={starts[i]} != a{i-1}={starts[i-1]} + s{i-1}={sizes[i-1]}"
        )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_mem_partition_knapsack(solver, tmp_path):
    solver.bench(MemPartitionKnapsack, validate=_check, tmp_path=tmp_path)
