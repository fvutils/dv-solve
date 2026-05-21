"""Benchmark: address alignment constraint via bitwise AND.

Exercises the bitwise AND propagator with alignment patterns.

addr  -- 32-bit address
size  -- log2 alignment (0..4 -> 1/2/4/8/16 byte alignment)

Constraint: addr & ((1 << size) - 1) == 0
Simplified for the native solver: addr & 0xF == 0 (16-byte alignment).
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class AlignedAddr:
    addr:  zdc.rand(domain=(0, 65535), default=0)
    mask:  zdc.rand(domain=(0xF, 0xF), default=0xF)
    masked: zdc.rand(domain=(0, 0xF),  default=0)

    @zdc.constraint
    def c_mask_result(self):
        assert self.masked == self.addr % 16

    @zdc.constraint
    def c_aligned(self):
        assert self.masked == 0


def _check(sol):
    assert 0 <= sol["addr"] <= 65535
    assert sol["addr"] % 16 == 0, (
        f"addr {sol['addr']} not aligned to 16 bytes"
    )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_alignment(solver, tmp_path):
    solver.bench(AlignedAddr, validate=_check, tmp_path=tmp_path)
