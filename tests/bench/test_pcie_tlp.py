"""Benchmark: PCIe TLP field-validity constraints.

Models PCIe Transaction Layer Packet constraints where field validity
depends on the TLP type.  Memory reads/writes require DW-aligned
addresses; config accesses have restricted address ranges; byte-enable
rules depend on length.

Derived from VishalAyyappan/pcie-endpoint-uvm (MIT license).

Variables (7):
  tlp_type   -- 0=MRd32, 1=MWr32, 2=CfgRd0, 3=CfgWr0
  length     -- DW count [1, 16]
  address    -- 16-bit address (simplified from 32/64-bit)
  first_be   -- first byte enable [1, 15]
  last_be    -- last byte enable [0, 15]
  tag        -- transaction tag [0, 255]
  fmt        -- header format derived from type [0, 3]

Constraints:
  c_addr_align : memory TLPs -> address[1:0] == 0
  c_cfg_range  : config TLPs -> address <= 1023
  c_be_single  : length == 1 -> last_be == 0
  c_be_multi   : length > 1  -> last_be > 0
  c_first_be   : first_be > 0  (always valid)
  c_fmt_type   : fmt matches tlp_type
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers

# TLP type encoding
_MRd32  = 0
_MWr32  = 1
_CfgRd0 = 2
_CfgWr0 = 3


@zdc.dataclass
class PcieTlp:
    tlp_type:  zdc.rand(domain=(0, 3),     default=0)
    length:    zdc.rand(domain=(1, 16),    default=1)
    address:   zdc.rand(domain=(0, 65535), default=0)
    first_be:  zdc.rand(domain=(1, 15),    default=0xF)
    last_be:   zdc.rand(domain=(0, 15),    default=0)
    tag:       zdc.rand(domain=(0, 255),   default=0)
    fmt:       zdc.rand(domain=(0, 3),     default=0)

    # Memory TLPs: DW-aligned address
    @zdc.constraint
    def c_addr_align(self):
        assert self.tlp_type >= 2 or self.address % 4 == 0

    # Config TLPs: restricted address range
    @zdc.constraint
    def c_cfg_range(self):
        assert self.tlp_type <= 1 or self.address <= 1023

    # Single-DW: last_be must be 0
    @zdc.constraint
    def c_be_single(self):
        assert self.length != 1 or self.last_be == 0

    # Multi-DW: last_be must be nonzero
    @zdc.constraint
    def c_be_multi(self):
        assert self.length <= 1 or self.last_be >= 1

    # fmt mirrors tlp_type (simplified: write types have data)
    @zdc.constraint
    def c_fmt_type(self):
        assert self.fmt == self.tlp_type


def _check(sol):
    tt  = sol["tlp_type"]
    ln  = sol["length"]
    adr = sol["address"]
    fbe = sol["first_be"]
    lbe = sol["last_be"]

    assert 0 <= tt <= 3
    assert 1 <= ln <= 16
    assert 1 <= fbe <= 15

    if tt in (_MRd32, _MWr32):
        assert adr % 4 == 0, f"mem TLP addr not aligned: {adr:#06x}"
    if tt in (_CfgRd0, _CfgWr0):
        assert adr <= 1023, f"cfg TLP addr out of range: {adr}"
    if ln == 1:
        assert lbe == 0, f"single-DW last_be != 0: {lbe}"
    if ln > 1:
        assert lbe >= 1, f"multi-DW last_be == 0"
    assert sol["fmt"] == tt


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_pcie_tlp(solver, tmp_path):
    solver.bench(PcieTlp, validate=_check, tmp_path=tmp_path)
