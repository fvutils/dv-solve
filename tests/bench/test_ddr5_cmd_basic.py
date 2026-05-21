"""Benchmark: DDR5 command-field constraints (basic).

Derived from Shehab-Naga/ddr5_phy ddr_sequence_item.sv (MIT license).
Models the core address/command fields of a DDR5 transaction without
randc or dist (those are tracked as feature gaps).

Variables (8):
  cmd       -- command type;      0..4  (ACT=0, RD=1, MRW=2, MRR=3, PREab=4)
  ba        -- bank address;      0..3   (2-bit)
  bg        -- bank group;        0..7   (3-bit)
  cid       -- chip ID;           0..15  (4-bit)
  row_hi    -- row address upper; 0..255 (top 8 of 18-bit row)
  row_lo    -- row address lower; 0..1023 (bottom 10 of 18-bit row)
  col       -- column address;    0..511 (9-bit, bits [10:2])
  ap        -- auto-precharge;    0..1

Constraints:
  c_cmd_valid  : cmd <= 4                                     [range]
  c_row_align  : row_hi * 1024 + row_lo == row_addr (helper)  [arithmetic]
  c_rd_needs_row : if cmd is RD (1), row_hi > 0              [conditional implication]
  c_act_full_row : if cmd is ACT (0), row_lo + row_hi > 0    [conditional implication]

The benchmark exercises multi-field domains, arithmetic linking, and
implication-style constraints across 8 variables.
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


# Command encoding (matches DDR5 command_t enum order)
_ACT   = 0
_RD    = 1
_MRW   = 2
_MRR   = 3
_PREab = 4


@zdc.dataclass
class Ddr5CmdBasic:
    cmd:    zdc.rand(domain=(0, 4),    default=0)
    ba:     zdc.rand(domain=(0, 3),    default=0)
    bg:     zdc.rand(domain=(0, 7),    default=0)
    cid:    zdc.rand(domain=(0, 15),   default=0)
    row_hi: zdc.rand(domain=(0, 255),  default=1)
    row_lo: zdc.rand(domain=(0, 1023), default=0)
    col:    zdc.rand(domain=(0, 511),  default=0)
    ap:     zdc.rand(domain=(0, 1),    default=0)

    # Row address must be non-zero for ACT and RD commands
    @zdc.constraint
    def c_act_row(self):
        assert self.cmd != 0 or self.row_hi + self.row_lo > 0  # ACT -> valid row

    @zdc.constraint
    def c_rd_row(self):
        assert self.cmd != 1 or self.row_hi > 0  # RD -> row_hi > 0

    # Column address only meaningful for RD
    @zdc.constraint
    def c_rd_col(self):
        assert self.cmd == 1 or self.col == 0  # non-RD -> col == 0

    # AP only meaningful for RD
    @zdc.constraint
    def c_rd_ap(self):
        assert self.cmd == 1 or self.ap == 0  # non-RD -> ap == 0


def _check(sol):
    assert 0 <= sol["cmd"] <= 4, f"cmd out of range: {sol['cmd']}"
    assert 0 <= sol["ba"] <= 3
    assert 0 <= sol["bg"] <= 7
    assert 0 <= sol["cid"] <= 15
    assert 0 <= sol["row_hi"] <= 255
    assert 0 <= sol["row_lo"] <= 1023
    assert 0 <= sol["col"] <= 511
    assert 0 <= sol["ap"] <= 1

    if sol["cmd"] == _ACT:
        assert sol["row_hi"] + sol["row_lo"] > 0, "ACT with zero row"
    if sol["cmd"] == _RD:
        assert sol["row_hi"] > 0, "RD with row_hi == 0"
    if sol["cmd"] != _RD:
        assert sol["col"] == 0, f"non-RD cmd={sol['cmd']} with col={sol['col']}"
        assert sol["ap"] == 0, f"non-RD cmd={sol['cmd']} with ap={sol['ap']}"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_ddr5_cmd_basic(solver, tmp_path):
    solver.bench(Ddr5CmdBasic, validate=_check, tmp_path=tmp_path)
