"""Benchmark: DDR5 mode-register write constraints.

Derived from Shehab-Naga/ddr5_phy ddr_sequence_item.sv (MIT license).
Models the MRA/OP (mode register address / operand) constraints where the
valid OP bits depend on which mode register is selected.

The original SV uses bit-slice constraints (OP[1:0] inside {...}) and dist.
This version decomposes OP into named sub-fields and uses implication
constraints to encode the per-register rules.

Variables (7):
  mra          -- mode register address; 0..2  (index into {MR0, MR8, MR50})
  op_burst_len -- OP[1:0] burst length;  0..2  (MR0: exclude 3 = BL32 OTF)
  op_cas_lat   -- OP[6:2] CAS latency;   0..22 (MR0: valid range)
  op_rfu_7     -- OP[7] reserved;         0..0  (MR0: must be 0)
  op_rd_pre    -- OP[2:0] read preamble;  0..4  (MR8: valid range)
  op_rfu_5     -- OP[5] reserved;         0..0  (MR8: must be 0)
  op_crc_en    -- OP[0] CRC enable;       0..1  (MR50)

Constraints:
  MR0 selected -> burst_len <= 2, cas_lat <= 22, rfu_7 == 0
  MR8 selected -> rd_pre <= 4, rfu_5 == 0
  MR50 selected -> (no additional range constraint beyond domain)
  Cross-register: unused fields are zeroed when their register is not selected.

This exercises implication constraints with 3-way conditional fan-out.
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers

# MRA indices
_MR0  = 0
_MR8  = 1
_MR50 = 2


@zdc.dataclass
class Ddr5ModeRegister:
    mra:          zdc.rand(domain=(0, 2),  default=0)

    # MR0 sub-fields
    op_burst_len: zdc.rand(domain=(0, 3),  default=0)
    op_cas_lat:   zdc.rand(domain=(0, 31), default=0)
    op_rfu_7:     zdc.rand(domain=(0, 1),  default=0)

    # MR8 sub-fields
    op_rd_pre:    zdc.rand(domain=(0, 7),  default=0)
    op_rfu_5:     zdc.rand(domain=(0, 1),  default=0)

    # MR50 sub-fields
    op_crc_en:    zdc.rand(domain=(0, 1),  default=0)

    # --- MR0 constraints ---
    @zdc.constraint
    def c_mr0_burst(self):
        assert self.mra != 0 or self.op_burst_len <= 2  # MR0 -> burst_len <= 2

    @zdc.constraint
    def c_mr0_cas(self):
        assert self.mra != 0 or self.op_cas_lat <= 22  # MR0 -> cas_lat <= 22

    @zdc.constraint
    def c_mr0_rfu(self):
        assert self.mra != 0 or self.op_rfu_7 == 0  # MR0 -> rfu_7 == 0

    # --- MR8 constraints ---
    @zdc.constraint
    def c_mr8_pre(self):
        assert self.mra != 1 or self.op_rd_pre <= 4  # MR8 -> rd_pre <= 4

    @zdc.constraint
    def c_mr8_rfu(self):
        assert self.mra != 1 or self.op_rfu_5 == 0  # MR8 -> rfu_5 == 0

    # --- Zero unused sub-fields when register not selected ---
    @zdc.constraint
    def c_zero_mr0_fields(self):
        assert self.mra == 0 or self.op_burst_len == 0  # non-MR0 -> zero
        assert self.mra == 0 or self.op_cas_lat == 0
        assert self.mra == 0 or self.op_rfu_7 == 0

    @zdc.constraint
    def c_zero_mr8_fields(self):
        assert self.mra == 1 or self.op_rd_pre == 0  # non-MR8 -> zero
        assert self.mra == 1 or self.op_rfu_5 == 0

    @zdc.constraint
    def c_zero_mr50_fields(self):
        assert self.mra == 2 or self.op_crc_en == 0  # non-MR50 -> zero


def _check(sol):
    mra = sol["mra"]
    assert 0 <= mra <= 2, f"mra out of range: {mra}"

    if mra == _MR0:
        assert sol["op_burst_len"] <= 2, f"MR0 burst_len={sol['op_burst_len']}"
        assert sol["op_cas_lat"] <= 22, f"MR0 cas_lat={sol['op_cas_lat']}"
        assert sol["op_rfu_7"] == 0, "MR0 rfu_7 not zero"
        assert sol["op_rd_pre"] == 0, "MR0: MR8 field not zeroed"
        assert sol["op_rfu_5"] == 0, "MR0: MR8 field not zeroed"
        assert sol["op_crc_en"] == 0, "MR0: MR50 field not zeroed"
    elif mra == _MR8:
        assert sol["op_rd_pre"] <= 4, f"MR8 rd_pre={sol['op_rd_pre']}"
        assert sol["op_rfu_5"] == 0, "MR8 rfu_5 not zero"
        assert sol["op_burst_len"] == 0, "MR8: MR0 field not zeroed"
        assert sol["op_cas_lat"] == 0, "MR8: MR0 field not zeroed"
        assert sol["op_crc_en"] == 0, "MR8: MR50 field not zeroed"
    elif mra == _MR50:
        assert sol["op_burst_len"] == 0, "MR50: MR0 field not zeroed"
        assert sol["op_cas_lat"] == 0, "MR50: MR0 field not zeroed"
        assert sol["op_rd_pre"] == 0, "MR50: MR8 field not zeroed"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_ddr5_mode_register(solver, tmp_path):
    solver.bench(Ddr5ModeRegister, validate=_check, tmp_path=tmp_path)
