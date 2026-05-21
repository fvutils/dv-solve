"""Benchmark: 16-variable all-different (uniqueness) constraint.

Scales the existing 3-variable unique benchmark to 16 variables,
producing 120 pairwise inequality constraints.  BDD solvers build
intermediate product BDDs whose size is exponential in N; domain-
filtering solvers with an all-different propagator handle it in
O(N log N).

Variables (16):
  v0..v15 -- domain [0, 31]

Constraints:
  c_unique: all pairs v_i != v_j for i != j  (120 inequalities)
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class Unique16:
    v0:  zdc.rand(domain=(0, 31), default=0)
    v1:  zdc.rand(domain=(0, 31), default=1)
    v2:  zdc.rand(domain=(0, 31), default=2)
    v3:  zdc.rand(domain=(0, 31), default=3)
    v4:  zdc.rand(domain=(0, 31), default=4)
    v5:  zdc.rand(domain=(0, 31), default=5)
    v6:  zdc.rand(domain=(0, 31), default=6)
    v7:  zdc.rand(domain=(0, 31), default=7)
    v8:  zdc.rand(domain=(0, 31), default=8)
    v9:  zdc.rand(domain=(0, 31), default=9)
    v10: zdc.rand(domain=(0, 31), default=10)
    v11: zdc.rand(domain=(0, 31), default=11)
    v12: zdc.rand(domain=(0, 31), default=12)
    v13: zdc.rand(domain=(0, 31), default=13)
    v14: zdc.rand(domain=(0, 31), default=14)
    v15: zdc.rand(domain=(0, 31), default=15)

    @zdc.constraint
    def c_unique_0(self):
        assert self.v0 != self.v1
        assert self.v0 != self.v2
        assert self.v0 != self.v3
        assert self.v0 != self.v4
        assert self.v0 != self.v5
        assert self.v0 != self.v6
        assert self.v0 != self.v7
        assert self.v0 != self.v8
        assert self.v0 != self.v9
        assert self.v0 != self.v10
        assert self.v0 != self.v11
        assert self.v0 != self.v12
        assert self.v0 != self.v13
        assert self.v0 != self.v14
        assert self.v0 != self.v15

    @zdc.constraint
    def c_unique_1(self):
        assert self.v1 != self.v2
        assert self.v1 != self.v3
        assert self.v1 != self.v4
        assert self.v1 != self.v5
        assert self.v1 != self.v6
        assert self.v1 != self.v7
        assert self.v1 != self.v8
        assert self.v1 != self.v9
        assert self.v1 != self.v10
        assert self.v1 != self.v11
        assert self.v1 != self.v12
        assert self.v1 != self.v13
        assert self.v1 != self.v14
        assert self.v1 != self.v15

    @zdc.constraint
    def c_unique_2(self):
        assert self.v2 != self.v3
        assert self.v2 != self.v4
        assert self.v2 != self.v5
        assert self.v2 != self.v6
        assert self.v2 != self.v7
        assert self.v2 != self.v8
        assert self.v2 != self.v9
        assert self.v2 != self.v10
        assert self.v2 != self.v11
        assert self.v2 != self.v12
        assert self.v2 != self.v13
        assert self.v2 != self.v14
        assert self.v2 != self.v15

    @zdc.constraint
    def c_unique_3(self):
        assert self.v3 != self.v4
        assert self.v3 != self.v5
        assert self.v3 != self.v6
        assert self.v3 != self.v7
        assert self.v3 != self.v8
        assert self.v3 != self.v9
        assert self.v3 != self.v10
        assert self.v3 != self.v11
        assert self.v3 != self.v12
        assert self.v3 != self.v13
        assert self.v3 != self.v14
        assert self.v3 != self.v15

    @zdc.constraint
    def c_unique_4(self):
        assert self.v4 != self.v5
        assert self.v4 != self.v6
        assert self.v4 != self.v7
        assert self.v4 != self.v8
        assert self.v4 != self.v9
        assert self.v4 != self.v10
        assert self.v4 != self.v11
        assert self.v4 != self.v12
        assert self.v4 != self.v13
        assert self.v4 != self.v14
        assert self.v4 != self.v15

    @zdc.constraint
    def c_unique_5(self):
        assert self.v5 != self.v6
        assert self.v5 != self.v7
        assert self.v5 != self.v8
        assert self.v5 != self.v9
        assert self.v5 != self.v10
        assert self.v5 != self.v11
        assert self.v5 != self.v12
        assert self.v5 != self.v13
        assert self.v5 != self.v14
        assert self.v5 != self.v15

    @zdc.constraint
    def c_unique_6(self):
        assert self.v6 != self.v7
        assert self.v6 != self.v8
        assert self.v6 != self.v9
        assert self.v6 != self.v10
        assert self.v6 != self.v11
        assert self.v6 != self.v12
        assert self.v6 != self.v13
        assert self.v6 != self.v14
        assert self.v6 != self.v15

    @zdc.constraint
    def c_unique_7(self):
        assert self.v7 != self.v8
        assert self.v7 != self.v9
        assert self.v7 != self.v10
        assert self.v7 != self.v11
        assert self.v7 != self.v12
        assert self.v7 != self.v13
        assert self.v7 != self.v14
        assert self.v7 != self.v15

    @zdc.constraint
    def c_unique_8(self):
        assert self.v8 != self.v9
        assert self.v8 != self.v10
        assert self.v8 != self.v11
        assert self.v8 != self.v12
        assert self.v8 != self.v13
        assert self.v8 != self.v14
        assert self.v8 != self.v15

    @zdc.constraint
    def c_unique_9(self):
        assert self.v9 != self.v10
        assert self.v9 != self.v11
        assert self.v9 != self.v12
        assert self.v9 != self.v13
        assert self.v9 != self.v14
        assert self.v9 != self.v15

    @zdc.constraint
    def c_unique_10(self):
        assert self.v10 != self.v11
        assert self.v10 != self.v12
        assert self.v10 != self.v13
        assert self.v10 != self.v14
        assert self.v10 != self.v15

    @zdc.constraint
    def c_unique_11(self):
        assert self.v11 != self.v12
        assert self.v11 != self.v13
        assert self.v11 != self.v14
        assert self.v11 != self.v15

    @zdc.constraint
    def c_unique_12(self):
        assert self.v12 != self.v13
        assert self.v12 != self.v14
        assert self.v12 != self.v15

    @zdc.constraint
    def c_unique_13(self):
        assert self.v13 != self.v14
        assert self.v13 != self.v15

    @zdc.constraint
    def c_unique_14(self):
        assert self.v14 != self.v15


def _check(sol):
    vals = [sol[f"v{i}"] for i in range(16)]
    for v in vals:
        assert 0 <= v <= 31, f"value {v} out of domain [0,31]"
    assert len(set(vals)) == 16, f"duplicate values found: {vals}"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_unique_16(solver, tmp_path):
    solver.bench(Unique16, validate=_check, tmp_path=tmp_path)
