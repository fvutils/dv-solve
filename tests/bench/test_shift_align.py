"""Benchmark: shift-based alignment constraint.

Exercises shift propagators with alignment patterns.

base_addr  -- base address
shift_amt  -- shift amount (power-of-2 alignment)
aligned    -- aligned result (base_addr >> shift_amt) << shift_amt

Constraints:
  aligned == (base_addr >> 2) << 2   (4-byte alignment)
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class ShiftAligned:
    base_addr:  zdc.rand(domain=(0, 1023), default=0)
    aligned:    zdc.rand(domain=(0, 1023), default=0)

    @zdc.constraint
    def c_aligned(self):
        assert self.aligned == (self.base_addr // 4) * 4


def _check(sol):
    assert 0 <= sol["base_addr"] <= 1023
    assert sol["aligned"] == (sol["base_addr"] // 4) * 4, (
        f"aligned={sol['aligned']} != floor(base/4)*4={sol['base_addr']//4*4}"
    )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_shift_align(solver, tmp_path):
    solver.bench(ShiftAligned, validate=_check, tmp_path=tmp_path)
