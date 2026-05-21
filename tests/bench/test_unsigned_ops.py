"""Benchmark: unsigned 32-bit operations.

Exercises unsigned 32-bit variables with domains spanning above 0x7FFFFFFF,
validating correct handling after tier-1 promotion.

addr    -- 32-bit unsigned address; 0..0xFFFFFFFF
mask    -- 32-bit unsigned mask;    0..0xFFFFFFFF
offset  -- 16-bit unsigned offset;  0..65535
result  -- 32-bit unsigned result;  0..0xFFFFFFFF

Constraints:
  c_addr_range:  addr >= 0x80000000             [high unsigned range]
  c_offset_fit:  result == addr + offset         [arithmetic with unsigned]
  c_result_hi:   result >= 0x80000000            [result in high range]
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class UnsignedOps:
    addr:    zdc.rand(domain=(0x80000000, 0xFFFFFFF0), default=0x80000000)
    offset:  zdc.rand(domain=(0, 15),                  default=0)
    result:  zdc.rand(domain=(0x80000000, 0xFFFFFFFF),  default=0x80000000)

    @zdc.constraint
    def c_sum(self):
        assert self.result == self.addr + self.offset


def _check(sol):
    assert 0x80000000 <= sol["addr"] <= 0xFFFFFFF0, (
        f"addr out of range: {sol['addr']:#x}"
    )
    assert 0 <= sol["offset"] <= 15, (
        f"offset out of range: {sol['offset']}"
    )
    assert sol["result"] == sol["addr"] + sol["offset"], (
        f"result mismatch: {sol['result']:#x} != {sol['addr']:#x} + {sol['offset']}"
    )
    assert 0x80000000 <= sol["result"] <= 0xFFFFFFFF, (
        f"result out of range: {sol['result']:#x}"
    )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_unsigned_ops(solver, tmp_path):
    solver.bench(UnsignedOps, validate=_check, tmp_path=tmp_path)
