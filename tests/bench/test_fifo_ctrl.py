"""Benchmark: FIFO control registers.

wr_ptr        — write pointer; 0..255
rd_ptr        — read pointer; 0..255
watermark_lo  — low watermark; 0..63
watermark_hi  — high watermark; 192..255

Constraints (all natively compiled):
  c_ptrs_differ:  wr_ptr != rd_ptr              [var != var]
  c_watermark:    watermark_lo < watermark_hi   [var < var]
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class FifoCtrl:
    wr_ptr:       zdc.rand(domain=(0, 255),  default=1)
    rd_ptr:       zdc.rand(domain=(0, 255),  default=0)
    watermark_lo: zdc.rand(domain=(0, 63),   default=16)
    watermark_hi: zdc.rand(domain=(192, 255), default=240)

    @zdc.constraint
    def c_ptrs_differ(self):
        assert self.wr_ptr != self.rd_ptr

    @zdc.constraint
    def c_watermark(self):
        assert self.watermark_lo < self.watermark_hi


def _check(sol):
    assert 0 <= sol["wr_ptr"] <= 255, f"wr_ptr out of range: {sol['wr_ptr']}"
    assert 0 <= sol["rd_ptr"] <= 255, f"rd_ptr out of range: {sol['rd_ptr']}"
    assert sol["wr_ptr"] != sol["rd_ptr"], (
        f"wr_ptr == rd_ptr == {sol['wr_ptr']}"
    )
    assert 0 <= sol["watermark_lo"] <= 63, (
        f"watermark_lo out of range: {sol['watermark_lo']}"
    )
    assert 192 <= sol["watermark_hi"] <= 255, (
        f"watermark_hi out of range: {sol['watermark_hi']}"
    )
    assert sol["watermark_lo"] < sol["watermark_hi"], (
        f"watermark ordering violated: lo={sol['watermark_lo']} hi={sol['watermark_hi']}"
    )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_fifo_ctrl(solver, tmp_path):
    solver.bench(FifoCtrl, validate=_check, tmp_path=tmp_path)
