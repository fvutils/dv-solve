"""Benchmark: array with ascending/descending ordering and modular arithmetic.

Inspired by Karan-nevage/Systemverilog-Constraints-Interview-Questions
Moderate/q01.sv (MIT license).

Original SV:
  10-element array, first 5 ascending, next 5 descending.
  All elements in [50,100], all multiples of 5.

Since zdc does not support arrays or foreach directly, this is unrolled
into 10 scalar fields with explicit ordering and divisibility constraints.

Variables (10):
  v0..v9 -- array elements; domain [50, 100]

Constraints:
  c_ascending  : v0 < v1 < v2 < v3 < v4             [strict ordering]
  c_descending : v5 > v6 > v7 > v8 > v9             [strict ordering]
  c_div5_*     : each vi % 5 == 0 (approximated via  [modular arithmetic]
                 vi == 5*qi helper variables)

This is a 20-variable problem (10 values + 10 quotient helpers) with
20 constraints -- a medium-complexity benchmark.
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class ArrayOrdering:
    # Array values
    v0: zdc.rand(domain=(50, 100), default=50)
    v1: zdc.rand(domain=(50, 100), default=55)
    v2: zdc.rand(domain=(50, 100), default=60)
    v3: zdc.rand(domain=(50, 100), default=65)
    v4: zdc.rand(domain=(50, 100), default=70)
    v5: zdc.rand(domain=(50, 100), default=100)
    v6: zdc.rand(domain=(50, 100), default=95)
    v7: zdc.rand(domain=(50, 100), default=90)
    v8: zdc.rand(domain=(50, 100), default=85)
    v9: zdc.rand(domain=(50, 100), default=80)

    # Quotient helpers to enforce divisibility by 5: vi == 5 * qi
    q0: zdc.rand(domain=(10, 20), default=10)
    q1: zdc.rand(domain=(10, 20), default=11)
    q2: zdc.rand(domain=(10, 20), default=12)
    q3: zdc.rand(domain=(10, 20), default=13)
    q4: zdc.rand(domain=(10, 20), default=14)
    q5: zdc.rand(domain=(10, 20), default=20)
    q6: zdc.rand(domain=(10, 20), default=19)
    q7: zdc.rand(domain=(10, 20), default=18)
    q8: zdc.rand(domain=(10, 20), default=17)
    q9: zdc.rand(domain=(10, 20), default=16)

    # Divisibility: vi == 5 * qi
    @zdc.constraint
    def c_div5(self):
        assert self.v0 == 5 * self.q0
        assert self.v1 == 5 * self.q1
        assert self.v2 == 5 * self.q2
        assert self.v3 == 5 * self.q3
        assert self.v4 == 5 * self.q4
        assert self.v5 == 5 * self.q5
        assert self.v6 == 5 * self.q6
        assert self.v7 == 5 * self.q7
        assert self.v8 == 5 * self.q8
        assert self.v9 == 5 * self.q9

    # First 5 elements ascending
    @zdc.constraint
    def c_ascending(self):
        assert self.v0 < self.v1
        assert self.v1 < self.v2
        assert self.v2 < self.v3
        assert self.v3 < self.v4

    # Last 5 elements descending
    @zdc.constraint
    def c_descending(self):
        assert self.v5 > self.v6
        assert self.v6 > self.v7
        assert self.v7 > self.v8
        assert self.v8 > self.v9


def _check(sol):
    vals = [sol[f"v{i}"] for i in range(10)]

    for i, v in enumerate(vals):
        assert 50 <= v <= 100, f"v{i}={v} out of range"
        assert v % 5 == 0, f"v{i}={v} not divisible by 5"
        assert v == 5 * sol[f"q{i}"], f"v{i}={v} != 5*q{i}={5*sol[f'q{i}']}"

    # Ascending: v0 < v1 < v2 < v3 < v4
    for i in range(4):
        assert vals[i] < vals[i + 1], (
            f"ascending violated: v{i}={vals[i]} >= v{i+1}={vals[i+1]}"
        )

    # Descending: v5 > v6 > v7 > v8 > v9
    for i in range(5, 9):
        assert vals[i] > vals[i + 1], (
            f"descending violated: v{i}={vals[i]} <= v{i+1}={vals[i+1]}"
        )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_array_ordering(solver, tmp_path):
    solver.bench(ArrayOrdering, validate=_check, tmp_path=tmp_path)
