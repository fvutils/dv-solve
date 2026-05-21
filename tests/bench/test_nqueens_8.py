"""Benchmark: 8-Queens puzzle via constraints.

Classic constraint satisfaction problem: place 8 queens on an 8x8 board
so that no two attack each other.  Row uniqueness is implicit (one queen
per row); column and diagonal uniqueness must be constrained.

Variables (8):
  q0..q7 -- column position of queen in row 0..7, domain [0, 7]

Constraints:
  Column: all q_i unique  (28 pairwise !=)
  Diag+:  q_i + i unique  (28 pairwise !=)
  Diag-:  q_i - i unique  (28 pairwise !=)

Total: 84 constraints over 8 variables.
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class NQueens8:
    q0: zdc.rand(domain=(0, 7), default=0)
    q1: zdc.rand(domain=(0, 7), default=1)
    q2: zdc.rand(domain=(0, 7), default=2)
    q3: zdc.rand(domain=(0, 7), default=3)
    q4: zdc.rand(domain=(0, 7), default=4)
    q5: zdc.rand(domain=(0, 7), default=5)
    q6: zdc.rand(domain=(0, 7), default=6)
    q7: zdc.rand(domain=(0, 7), default=7)

    # Helper variables for diagonal checks: d+_i = q_i + i, d-_i = q_i - i + 7
    dp0: zdc.rand(domain=(0, 14), default=0)
    dp1: zdc.rand(domain=(0, 14), default=2)
    dp2: zdc.rand(domain=(0, 14), default=4)
    dp3: zdc.rand(domain=(0, 14), default=6)
    dp4: zdc.rand(domain=(0, 14), default=8)
    dp5: zdc.rand(domain=(0, 14), default=10)
    dp6: zdc.rand(domain=(0, 14), default=12)
    dp7: zdc.rand(domain=(0, 14), default=14)

    dm0: zdc.rand(domain=(0, 14), default=7)
    dm1: zdc.rand(domain=(0, 14), default=7)
    dm2: zdc.rand(domain=(0, 14), default=7)
    dm3: zdc.rand(domain=(0, 14), default=7)
    dm4: zdc.rand(domain=(0, 14), default=7)
    dm5: zdc.rand(domain=(0, 14), default=7)
    dm6: zdc.rand(domain=(0, 14), default=7)
    dm7: zdc.rand(domain=(0, 14), default=7)

    # Diagonal-plus definitions: dp_i == q_i + i
    @zdc.constraint
    def c_dp(self):
        assert self.dp0 == self.q0 + 0
        assert self.dp1 == self.q1 + 1
        assert self.dp2 == self.q2 + 2
        assert self.dp3 == self.q3 + 3
        assert self.dp4 == self.q4 + 4
        assert self.dp5 == self.q5 + 5
        assert self.dp6 == self.q6 + 6
        assert self.dp7 == self.q7 + 7

    # Diagonal-minus definitions: dm_i == q_i - i + 7
    @zdc.constraint
    def c_dm(self):
        assert self.dm0 == self.q0 + 7
        assert self.dm1 == self.q1 + 6
        assert self.dm2 == self.q2 + 5
        assert self.dm3 == self.q3 + 4
        assert self.dm4 == self.q4 + 3
        assert self.dm5 == self.q5 + 2
        assert self.dm6 == self.q6 + 1
        assert self.dm7 == self.q7 + 0

    # Column uniqueness
    @zdc.constraint
    def c_col_0(self):
        assert self.q0 != self.q1
        assert self.q0 != self.q2
        assert self.q0 != self.q3
        assert self.q0 != self.q4
        assert self.q0 != self.q5
        assert self.q0 != self.q6
        assert self.q0 != self.q7

    @zdc.constraint
    def c_col_1(self):
        assert self.q1 != self.q2
        assert self.q1 != self.q3
        assert self.q1 != self.q4
        assert self.q1 != self.q5
        assert self.q1 != self.q6
        assert self.q1 != self.q7

    @zdc.constraint
    def c_col_2(self):
        assert self.q2 != self.q3
        assert self.q2 != self.q4
        assert self.q2 != self.q5
        assert self.q2 != self.q6
        assert self.q2 != self.q7

    @zdc.constraint
    def c_col_3(self):
        assert self.q3 != self.q4
        assert self.q3 != self.q5
        assert self.q3 != self.q6
        assert self.q3 != self.q7

    @zdc.constraint
    def c_col_4(self):
        assert self.q4 != self.q5
        assert self.q4 != self.q6
        assert self.q4 != self.q7

    @zdc.constraint
    def c_col_5(self):
        assert self.q5 != self.q6
        assert self.q5 != self.q7

    @zdc.constraint
    def c_col_6(self):
        assert self.q6 != self.q7

    # Diagonal-plus uniqueness
    @zdc.constraint
    def c_dp_0(self):
        assert self.dp0 != self.dp1
        assert self.dp0 != self.dp2
        assert self.dp0 != self.dp3
        assert self.dp0 != self.dp4
        assert self.dp0 != self.dp5
        assert self.dp0 != self.dp6
        assert self.dp0 != self.dp7

    @zdc.constraint
    def c_dp_1(self):
        assert self.dp1 != self.dp2
        assert self.dp1 != self.dp3
        assert self.dp1 != self.dp4
        assert self.dp1 != self.dp5
        assert self.dp1 != self.dp6
        assert self.dp1 != self.dp7

    @zdc.constraint
    def c_dp_2(self):
        assert self.dp2 != self.dp3
        assert self.dp2 != self.dp4
        assert self.dp2 != self.dp5
        assert self.dp2 != self.dp6
        assert self.dp2 != self.dp7

    @zdc.constraint
    def c_dp_3(self):
        assert self.dp3 != self.dp4
        assert self.dp3 != self.dp5
        assert self.dp3 != self.dp6
        assert self.dp3 != self.dp7

    @zdc.constraint
    def c_dp_4(self):
        assert self.dp4 != self.dp5
        assert self.dp4 != self.dp6
        assert self.dp4 != self.dp7

    @zdc.constraint
    def c_dp_5(self):
        assert self.dp5 != self.dp6
        assert self.dp5 != self.dp7

    @zdc.constraint
    def c_dp_6(self):
        assert self.dp6 != self.dp7

    # Diagonal-minus uniqueness
    @zdc.constraint
    def c_dm_0(self):
        assert self.dm0 != self.dm1
        assert self.dm0 != self.dm2
        assert self.dm0 != self.dm3
        assert self.dm0 != self.dm4
        assert self.dm0 != self.dm5
        assert self.dm0 != self.dm6
        assert self.dm0 != self.dm7

    @zdc.constraint
    def c_dm_1(self):
        assert self.dm1 != self.dm2
        assert self.dm1 != self.dm3
        assert self.dm1 != self.dm4
        assert self.dm1 != self.dm5
        assert self.dm1 != self.dm6
        assert self.dm1 != self.dm7

    @zdc.constraint
    def c_dm_2(self):
        assert self.dm2 != self.dm3
        assert self.dm2 != self.dm4
        assert self.dm2 != self.dm5
        assert self.dm2 != self.dm6
        assert self.dm2 != self.dm7

    @zdc.constraint
    def c_dm_3(self):
        assert self.dm3 != self.dm4
        assert self.dm3 != self.dm5
        assert self.dm3 != self.dm6
        assert self.dm3 != self.dm7

    @zdc.constraint
    def c_dm_4(self):
        assert self.dm4 != self.dm5
        assert self.dm4 != self.dm6
        assert self.dm4 != self.dm7

    @zdc.constraint
    def c_dm_5(self):
        assert self.dm5 != self.dm6
        assert self.dm5 != self.dm7

    @zdc.constraint
    def c_dm_6(self):
        assert self.dm6 != self.dm7


def _check(sol):
    queens = [sol[f"q{i}"] for i in range(8)]
    for v in queens:
        assert 0 <= v <= 7
    # Column uniqueness
    assert len(set(queens)) == 8, f"column conflict: {queens}"
    # Diagonal uniqueness
    dp = [queens[i] + i for i in range(8)]
    dm = [queens[i] - i for i in range(8)]
    assert len(set(dp)) == 8, f"diag+ conflict: {queens}"
    assert len(set(dm)) == 8, f"diag- conflict: {queens}"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_nqueens_8(solver, tmp_path):
    solver.bench(NQueens8, validate=_check, tmp_path=tmp_path)
