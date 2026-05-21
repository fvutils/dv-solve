"""Benchmark: AXI4 burst legality constraints.

Models real AXI4 burst rules: address alignment, burst-type dependent
length rules, no 4 KB boundary crossing, and end-address computation.

Derived from pulp-platform/axi axi_test.sv (Solderpad Hardware License).

To avoid expensive modular-arithmetic constraints, alignment is enforced
by decomposing the intra-page address into a word index (0..1023) that
is scaled by 4 to produce the byte offset.  The 4 KB page boundary is
enforced as end_off <= 1024 (in words).

Variables (5):
  addr_word  -- word index within a 4 KB page  [0, 1023]
  addr_page  -- page index                     [0, 15]
  len_m1     -- burst length minus 1           [0, 255]
  burst      -- burst type: 0=FIXED, 1=INCR, 2=WRAP
  end_word   -- addr_word + len_m1 + 1 (must stay <= 1024)

Constraints:
  c_end_word : end_word == addr_word + len_m1 + 1
  c_no_cross : end_word <= 1024
  c_wrap_len : WRAP -> len_m1 in {1, 3, 7, 15}
  c_fixed_len: FIXED -> len_m1 <= 15
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class Axi4Burst:
    addr_word: zdc.rand(domain=(0, 1023),  default=0)
    addr_page: zdc.rand(domain=(0, 15),    default=0)
    len_m1:    zdc.rand(domain=(0, 255),   default=0)
    burst:     zdc.rand(domain=(0, 2),     default=1)
    end_word:  zdc.rand(domain=(1, 1024),  default=1)

    @zdc.constraint
    def c_end_word(self):
        assert self.end_word == self.addr_word + self.len_m1 + 1

    @zdc.constraint
    def c_no_cross(self):
        assert self.end_word <= 1024

    # WRAP bursts: len must be 2, 4, 8, or 16 beats
    @zdc.constraint
    def c_wrap_len(self):
        assert self.burst != 2 or self.len_m1 == 1 or self.len_m1 == 3 or \
               self.len_m1 == 7 or self.len_m1 == 15

    # FIXED bursts: max 16 beats
    @zdc.constraint
    def c_fixed_len(self):
        assert self.burst != 0 or self.len_m1 <= 15


def _check(sol):
    addr_word = sol["addr_word"]
    len_m1    = sol["len_m1"]
    burst     = sol["burst"]
    end_word  = sol["end_word"]

    assert 0 <= burst <= 2
    assert 0 <= len_m1 <= 255
    assert end_word == addr_word + len_m1 + 1
    # 4 KB boundary: all beats within the same page
    assert end_word <= 1024, f"4KB cross: end_word={end_word}"
    if burst == 2:
        assert len_m1 in (1, 3, 7, 15), f"WRAP len_m1={len_m1}"
    if burst == 0:
        assert len_m1 <= 15, f"FIXED len_m1={len_m1}"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_axi4_burst(solver, tmp_path):
    solver.bench(Axi4Burst, validate=_check, tmp_path=tmp_path)
