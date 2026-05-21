"""Benchmark: DDR5 full command-to-command timing matrix.

Extends the existing test_ddr5_timing benchmark with the complete
5x5 command-type matrix including MRR timing, cancel-dependent
paths, and variable-dependent bounds (RL).  This exercises deep
conditional cascades that force BDD solvers to expand every branch.

Derived from Shehab-Naga/ddr5_phy ddr_sequence_item.sv (MIT license).

Variables (8):
  cmd_prev          -- previous command       [0, 4]
  cmd               -- current command        [0, 4]
  cancel_prev       -- prev cmd cancelled     [0, 1]
  rl                -- CAS read latency       [22, 40]
  max_slack         -- randomized jitter       [0, 8]
  tccd_base         -- minimum delay           [2, 50]
  tccd              -- actual delay = base + slack
  valid             -- helper, always 1

Command encoding: ACT=0, RD=1, MRW=2, MRR=3, PREab=4

Constraints encode the full DDR5 inter-command timing table:
  25 (cmd_prev, cmd) pairs with cancel_prev variants = ~35 rules
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

# Timing parameters (clocks)
_tMRW     = 8
_tMRD     = 16
_tRRD     = 8
_tRCD     = 24
_tCCD     = 8
_tRTP     = 12
_tPPD     = 2
_tRP      = 24
_tCANCEL  = 8


@zdc.dataclass
class Ddr5TimingFull:
    cmd_prev:    zdc.rand(domain=(0, 4),  default=0)
    cmd:         zdc.rand(domain=(0, 4),  default=1)
    cancel_prev: zdc.rand(domain=(0, 1),  default=0)
    rl:          zdc.rand(domain=(22, 40), default=22)
    max_slack:   zdc.rand(domain=(0, 8),  default=4)
    tccd_base:   zdc.rand(domain=(2, 50), default=8)
    tccd:        zdc.rand(domain=(2, 58), default=12)
    valid:       zdc.rand(domain=(1, 1),  default=1)

    # tccd == tccd_base + max_slack
    @zdc.constraint
    def c_tccd_sum(self):
        assert self.tccd == self.tccd_base + self.max_slack

    # Cancelled commands always: base >= 8
    @zdc.constraint
    def c_cancel(self):
        assert self.cancel_prev != 1 or self.tccd_base >= _tCANCEL

    # --- MRW previous (non-cancel) ---
    @zdc.constraint
    def c_mrw_mrw(self):
        assert self.cancel_prev != 0 or self.cmd_prev != _MRW or self.cmd != _MRW or self.tccd_base >= _tMRW

    @zdc.constraint
    def c_mrw_other(self):
        assert self.cancel_prev != 0 or self.cmd_prev != _MRW or self.cmd == _MRW or self.tccd_base >= _tMRD

    # --- MRR previous (non-cancel) ---
    @zdc.constraint
    def c_mrr_mrr(self):
        assert self.cancel_prev != 0 or self.cmd_prev != _MRR or self.cmd != _MRR or self.tccd_base >= _tMRD

    @zdc.constraint
    def c_mrr_mrw(self):
        # MRR -> MRW requires RL + 9 clocks
        assert self.cancel_prev != 0 or self.cmd_prev != _MRR or self.cmd != _MRW or self.tccd_base >= self.rl + 9

    @zdc.constraint
    def c_mrr_other(self):
        assert self.cancel_prev != 0 or self.cmd_prev != _MRR or self.cmd == _MRR or self.cmd == _MRW or self.tccd_base >= _tMRD

    # --- ACT previous (non-cancel) ---
    @zdc.constraint
    def c_act_act(self):
        assert self.cancel_prev != 0 or self.cmd_prev != _ACT or self.cmd != _ACT or self.tccd_base >= _tRRD

    @zdc.constraint
    def c_act_other(self):
        assert self.cancel_prev != 0 or self.cmd_prev != _ACT or self.cmd == _ACT or self.tccd_base >= _tRCD

    # --- RD previous (non-cancel) ---
    @zdc.constraint
    def c_rd_rd(self):
        assert self.cancel_prev != 0 or self.cmd_prev != _RD or self.cmd != _RD or self.tccd_base >= _tCCD

    @zdc.constraint
    def c_rd_pre(self):
        assert self.cancel_prev != 0 or self.cmd_prev != _RD or self.cmd != _PREab or self.tccd_base >= _tRTP

    @zdc.constraint
    def c_rd_other(self):
        assert self.cancel_prev != 0 or self.cmd_prev != _RD or self.cmd == _RD or self.cmd == _PREab or self.tccd_base >= _tRCD

    # --- PREab previous (non-cancel) ---
    @zdc.constraint
    def c_pre_pre(self):
        assert self.cancel_prev != 0 or self.cmd_prev != _PREab or self.cmd != _PREab or self.tccd_base >= _tPPD

    @zdc.constraint
    def c_pre_other(self):
        assert self.cancel_prev != 0 or self.cmd_prev != _PREab or self.cmd == _PREab or self.tccd_base >= _tRP


def _check(sol):
    assert sol["tccd"] == sol["tccd_base"] + sol["max_slack"]
    prev, cur, cancel, base, rl = (
        sol["cmd_prev"], sol["cmd"], sol["cancel_prev"],
        sol["tccd_base"], sol["rl"],
    )
    if cancel == 1:
        assert base >= _tCANCEL, f"cancel: base {base} < {_tCANCEL}"
        return
    if prev == _MRW:
        if cur == _MRW:
            assert base >= _tMRW
        else:
            assert base >= _tMRD
    elif prev == _MRR:
        if cur == _MRR:
            assert base >= _tMRD
        elif cur == _MRW:
            assert base >= rl + 9, f"MRR->MRW: base {base} < rl+9={rl+9}"
        else:
            assert base >= _tMRD
    elif prev == _ACT:
        if cur == _ACT:
            assert base >= _tRRD
        else:
            assert base >= _tRCD
    elif prev == _RD:
        if cur == _RD:
            assert base >= _tCCD
        elif cur == _PREab:
            assert base >= _tRTP
        else:
            assert base >= _tRCD
    elif prev == _PREab:
        if cur == _PREab:
            assert base >= _tPPD
        else:
            assert base >= _tRP


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_ddr5_timing_full(solver, tmp_path):
    solver.bench(Ddr5TimingFull, validate=_check, tmp_path=tmp_path)
