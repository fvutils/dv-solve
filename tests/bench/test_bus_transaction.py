"""Benchmark: AXI-style bus transaction.

word_addr   — 16-bit word address;       0..65535
burst_len   — number of beats minus one; 0..255
beat_size   — log2(bytes-per-beat);      0..3  (1/2/4/8 bytes)
end_addr    — word_addr + burst_len (helper variable for native arithmetic constraint)

Constraints (all natively compiled):
  c_end_addr:   end_addr == word_addr + burst_len   [var == var + var]
  c_in_range:   end_addr <= 65535                   [var <= const]
  c_beat_fits:  beat_size <= burst_len               [var <= var]
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class BusTransaction:
    word_addr:  zdc.rand(domain=(0, 65535),      default=0)
    burst_len:  zdc.rand(domain=(0, 255),        default=0)
    beat_size:  zdc.rand(domain=(0, 3),          default=0)
    end_addr:   zdc.rand(domain=(0, 65535 + 255), default=0)

    @zdc.constraint
    def c_end_addr(self):
        assert self.end_addr == self.word_addr + self.burst_len

    @zdc.constraint
    def c_in_range(self):
        assert self.end_addr <= 65535

    @zdc.constraint
    def c_beat_fits(self):
        assert self.beat_size <= self.burst_len


def _check(sol):
    assert 0 <= sol["word_addr"] <= 65535, (
        f"word_addr out of range: {sol['word_addr']}"
    )
    assert 0 <= sol["burst_len"] <= 255, (
        f"burst_len out of range: {sol['burst_len']}"
    )
    assert 0 <= sol["beat_size"] <= 3, (
        f"beat_size out of range: {sol['beat_size']}"
    )
    assert sol["end_addr"] == sol["word_addr"] + sol["burst_len"], (
        f"end_addr mismatch: {sol['end_addr']} != {sol['word_addr']} + {sol['burst_len']}"
    )
    assert sol["end_addr"] <= 65535, (
        f"end_addr out of range: {sol['end_addr']}"
    )
    assert sol["beat_size"] <= sol["burst_len"], (
        f"beat_size {sol['beat_size']} > burst_len {sol['burst_len']}"
    )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_bus_transaction(solver, tmp_path):
    solver.bench(BusTransaction, validate=_check, tmp_path=tmp_path)
