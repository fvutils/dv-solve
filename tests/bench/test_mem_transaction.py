"""Benchmark: word-addressed memory read/write transaction.

addr     — 8-bit byte address; 0..252
length   — transfer length in bytes; 1..16
end_addr — addr + length (helper variable for native arithmetic constraint)

Constraints (all natively compiled):
  c_end:          end_addr == addr + length   [var == var + var]
  c_no_overflow:  end_addr <= 256             [var <= const]
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class MemTransaction:
    addr:     zdc.rand(domain=(0, 252), default=0)
    length:   zdc.rand(domain=(1, 16),  default=1)
    end_addr: zdc.rand(domain=(1, 268), default=1)   # addr + length

    @zdc.constraint
    def c_end(self):
        assert self.end_addr == self.addr + self.length

    @zdc.constraint
    def c_no_overflow(self):
        assert self.end_addr <= 256


def _check(sol):
    assert 0 <= sol["addr"] <= 252, f"addr out of range: {sol['addr']}"
    assert 1 <= sol["length"] <= 16, f"length out of range: {sol['length']}"
    assert sol["end_addr"] == sol["addr"] + sol["length"], (
        f"end_addr mismatch: {sol['end_addr']} != {sol['addr']} + {sol['length']}"
    )
    assert sol["end_addr"] <= 256, (
        f"overflow: addr={sol['addr']} length={sol['length']} end_addr={sol['end_addr']}"
    )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_mem_transaction(solver, tmp_path):
    solver.bench(MemTransaction, validate=_check, tmp_path=tmp_path)
