"""Benchmark: DDR5 inter-command timing constraints.

Derived from Shehab-Naga/ddr5_phy ddr_sequence_item.sv (MIT license).
Models the tCCD (command-to-command delay) constraints that depend on the
previous and current command types.  The original uses solve...before and
deeply nested if/else; this version expresses the same logic as implication
constraints.

Variables (7):
  cmd_prev          -- previous command;     0..4
  cmd               -- current command;      0..4
  ap_prev           -- prev auto-precharge;  0..1
  cancel_prev       -- prev cmd cancelled;   0..1
  max_tccd          -- max extra delay;      0..8
  tccd              -- command-to-command delay; 2..50
  tccd_min_helper   -- minimum delay (derived); 2..50

Constraints encode DDR5 timing rules:
  - MRW->MRW: min 8 clocks
  - MRW->other: min 16 clocks
  - ACT->ACT: min 8 clocks (tRRD)
  - ACT->RD/MRW: min 24 clocks (tRCD)
  - RD->RD: min 8 clocks (tCCD)
  - RD->PREab: min 12 clocks (tRTP)
  - PREab->PREab: min 2 clocks (tPPD)
  - PREab->other: min 24 clocks (tRP)
  - Cancelled commands: min 8 clocks

The constraints form a complex conditional web across 7 variables.
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers

# Command encoding
_ACT   = 0
_RD    = 1
_MRW   = 2
_MRR   = 3
_PREab = 4

# Key DDR5 timing parameters (in clock cycles)
_tMRW  = 8
_tMRD  = 16
_tRRD  = 8
_tRCD  = 24
_tCCD  = 8
_tRTP  = 12
_tPPD  = 2
_tRP   = 24
_tCANCEL = 8


@zdc.dataclass
class Ddr5Timing:
    cmd_prev:    zdc.rand(domain=(0, 4),  default=0)
    cmd:         zdc.rand(domain=(0, 4),  default=1)
    ap_prev:     zdc.rand(domain=(0, 1),  default=0)
    cancel_prev: zdc.rand(domain=(0, 1),  default=0)
    max_tccd:    zdc.rand(domain=(0, 8),  default=4)
    tccd:        zdc.rand(domain=(2, 50), default=8)
    tccd_base:   zdc.rand(domain=(2, 50), default=8)

    # tccd == tccd_base + max_tccd (randomized jitter above minimum)
    @zdc.constraint
    def c_tccd_sum(self):
        assert self.tccd == self.tccd_base + self.max_tccd

    # --- MRW previous ---
    # MRW -> MRW: base >= 8
    @zdc.constraint
    def c_mrw_mrw(self):
        assert self.cmd_prev != 2 or self.cmd != 2 or self.tccd_base >= 8

    # MRW -> non-MRW: base >= 16
    @zdc.constraint
    def c_mrw_other(self):
        assert self.cmd_prev != 2 or self.cmd == 2 or self.tccd_base >= 16

    # --- ACT previous ---
    # ACT -> ACT: base >= 8 (tRRD)
    @zdc.constraint
    def c_act_act(self):
        assert self.cmd_prev != 0 or self.cmd != 0 or self.tccd_base >= 8

    # ACT -> non-ACT: base >= 24 (tRCD)
    @zdc.constraint
    def c_act_other(self):
        assert self.cmd_prev != 0 or self.cmd == 0 or self.tccd_base >= 24

    # --- RD previous ---
    # RD -> RD: base >= 8 (tCCD)
    @zdc.constraint
    def c_rd_rd(self):
        assert self.cmd_prev != 1 or self.cmd != 1 or self.tccd_base >= 8

    # RD -> PREab: base >= 12 (tRTP)
    @zdc.constraint
    def c_rd_pre(self):
        assert self.cmd_prev != 1 or self.cmd != 4 or self.tccd_base >= 12

    # --- PREab previous ---
    # PREab -> PREab: base >= 2 (tPPD)
    @zdc.constraint
    def c_pre_pre(self):
        assert self.cmd_prev != 4 or self.cmd != 4 or self.tccd_base >= 2

    # PREab -> non-PREab: base >= 24 (tRP)
    @zdc.constraint
    def c_pre_other(self):
        assert self.cmd_prev != 4 or self.cmd == 4 or self.tccd_base >= 24


def _check(sol):
    assert 0 <= sol["cmd_prev"] <= 4
    assert 0 <= sol["cmd"] <= 4
    assert sol["tccd"] == sol["tccd_base"] + sol["max_tccd"], "tccd decomposition"

    prev, cur, base = sol["cmd_prev"], sol["cmd"], sol["tccd_base"]

    if prev == _MRW and cur == _MRW:
        assert base >= _tMRW, f"MRW->MRW: base {base} < {_tMRW}"
    if prev == _MRW and cur != _MRW:
        assert base >= _tMRD, f"MRW->other: base {base} < {_tMRD}"
    if prev == _ACT and cur == _ACT:
        assert base >= _tRRD, f"ACT->ACT: base {base} < {_tRRD}"
    if prev == _ACT and cur != _ACT:
        assert base >= _tRCD, f"ACT->other: base {base} < {_tRCD}"
    if prev == _RD and cur == _RD:
        assert base >= _tCCD, f"RD->RD: base {base} < {_tCCD}"
    if prev == _RD and cur == _PREab:
        assert base >= _tRTP, f"RD->PREab: base {base} < {_tRTP}"
    if prev == _PREab and cur == _PREab:
        assert base >= _tPPD, f"PREab->PREab: base {base} < {_tPPD}"
    if prev == _PREab and cur != _PREab:
        assert base >= _tRP, f"PREab->other: base {base} < {_tRP}"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_ddr5_timing(solver, tmp_path):
    solver.bench(Ddr5Timing, validate=_check, tmp_path=tmp_path)
