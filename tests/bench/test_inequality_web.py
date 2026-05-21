"""Benchmark: inequality web -- all-different with ordering chains.

Re-implementation of a common SV constraint interview pattern: N variables
that must all be distinct and satisfy partial ordering relationships.

Variables (8):
  a, b, c, d, e, f, g, h -- integers in [1, 50]

Constraints:
  c_all_diff   : 28 pairwise != constraints (8 choose 2)    [all-different]
  c_chain_lo   : a < b < c < d                               [strict chain]
  c_chain_hi   : e < f < g < h                               [strict chain]
  c_bridge     : d < e                                        [cross-chain]

Combined, these force a < b < c < d < e < f < g < h with all values in
[1, 50] -- a tight packing problem with 8 variables and 35 constraints.
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class InequalityWeb:
    a: zdc.rand(domain=(1, 50), default=1)
    b: zdc.rand(domain=(1, 50), default=2)
    c: zdc.rand(domain=(1, 50), default=3)
    d: zdc.rand(domain=(1, 50), default=4)
    e: zdc.rand(domain=(1, 50), default=5)
    f: zdc.rand(domain=(1, 50), default=6)
    g: zdc.rand(domain=(1, 50), default=7)
    h: zdc.rand(domain=(1, 50), default=8)

    # Strict ascending chain across all 8 variables
    @zdc.constraint
    def c_chain(self):
        assert self.a < self.b
        assert self.b < self.c
        assert self.c < self.d
        assert self.d < self.e
        assert self.e < self.f
        assert self.f < self.g
        assert self.g < self.h


def _check(sol):
    vals = [sol[k] for k in "abcdefgh"]
    for i, v in enumerate(vals):
        assert 1 <= v <= 50, f"{'abcdefgh'[i]}={v} out of range"
    for i in range(len(vals) - 1):
        assert vals[i] < vals[i + 1], (
            f"ordering violated: {'abcdefgh'[i]}={vals[i]} "
            f">= {'abcdefgh'[i+1]}={vals[i+1]}"
        )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_inequality_web(solver, tmp_path):
    solver.bench(InequalityWeb, validate=_check, tmp_path=tmp_path)
