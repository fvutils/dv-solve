"""Benchmark: enum-driven conditional field selection.

Re-implementation inspired by DDR5 command-type dispatch and interview
patterns where an enum selector determines which fields are active.

Models a simplified bus transaction where the operation type determines
which address/data fields are meaningful -- a pattern common in protocol
verification (AXI, AHB, SPI, DDR).

Variables (7):
  op         -- operation type; 0..2  (READ=0, WRITE=1, CONFIG=2)
  addr       -- address;        0..4095
  wdata      -- write data;     0..255
  rdata_exp  -- expected read;  0..255
  cfg_reg    -- config register; 0..15
  cfg_val    -- config value;    0..63
  status     -- status bits;     0..3

Constraints:
  READ  (0) -> wdata == 0, cfg_reg == 0, cfg_val == 0
  WRITE (1) -> rdata_exp == 0, cfg_reg == 0, cfg_val == 0
  CONFIG(2) -> addr == 0, wdata == 0, rdata_exp == 0, addr range narrowed

Each operation zeros out irrelevant fields, creating a 3-way fan-out
of implication constraints.
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers

_READ   = 0
_WRITE  = 1
_CONFIG = 2


@zdc.dataclass
class EnumCond:
    op:        zdc.rand(domain=(0, 2),    default=0)
    addr:      zdc.rand(domain=(0, 4095), default=0)
    wdata:     zdc.rand(domain=(0, 255),  default=0)
    rdata_exp: zdc.rand(domain=(0, 255),  default=0)
    cfg_reg:   zdc.rand(domain=(0, 15),   default=0)
    cfg_val:   zdc.rand(domain=(0, 63),   default=0)
    status:    zdc.rand(domain=(0, 3),    default=0)

    # READ: zero write and config fields
    @zdc.constraint
    def c_read(self):
        assert self.op != 0 or self.wdata == 0   # READ -> zero write/config
        assert self.op != 0 or self.cfg_reg == 0
        assert self.op != 0 or self.cfg_val == 0

    # WRITE: zero read and config fields
    @zdc.constraint
    def c_write(self):
        assert self.op != 1 or self.rdata_exp == 0  # WRITE -> zero read/config
        assert self.op != 1 or self.cfg_reg == 0
        assert self.op != 1 or self.cfg_val == 0

    # CONFIG: zero addr, write, and read fields
    @zdc.constraint
    def c_config(self):
        assert self.op != 2 or self.addr == 0  # CONFIG -> zero addr/data
        assert self.op != 2 or self.wdata == 0
        assert self.op != 2 or self.rdata_exp == 0

    # At least one data-carrying field is non-zero
    @zdc.constraint
    def c_not_empty(self):
        assert self.addr + self.wdata + self.rdata_exp + self.cfg_reg + self.cfg_val > 0


def _check(sol):
    op = sol["op"]
    assert 0 <= op <= 2, f"op out of range: {op}"
    assert 0 <= sol["status"] <= 3

    if op == _READ:
        assert sol["wdata"] == 0, "READ: wdata not zero"
        assert sol["cfg_reg"] == 0, "READ: cfg_reg not zero"
        assert sol["cfg_val"] == 0, "READ: cfg_val not zero"
    elif op == _WRITE:
        assert sol["rdata_exp"] == 0, "WRITE: rdata_exp not zero"
        assert sol["cfg_reg"] == 0, "WRITE: cfg_reg not zero"
        assert sol["cfg_val"] == 0, "WRITE: cfg_val not zero"
    elif op == _CONFIG:
        assert sol["addr"] == 0, "CONFIG: addr not zero"
        assert sol["wdata"] == 0, "CONFIG: wdata not zero"
        assert sol["rdata_exp"] == 0, "CONFIG: rdata_exp not zero"

    total = sol["addr"] + sol["wdata"] + sol["rdata_exp"] + sol["cfg_reg"] + sol["cfg_val"]
    assert total > 0, "all data fields are zero"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_enum_cond(solver, tmp_path):
    solver.bench(EnumCond, validate=_check, tmp_path=tmp_path)
