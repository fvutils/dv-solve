"""Tier 2 benchmark: Circular FIFO with read/write pointers and empty/full tracking.

Properties:
  - Assertion: count <= DEPTH always holds.
  - Cover: count == DEPTH (full state reachable).

BMC depth: 20 (cover reachable for DEPTH=16).
k-induction: assertion provable.
"""

import zuspec.dataclasses as zdc

DEPTH = 16


@zdc.dataclass
class FifoPtrValid(zdc.Component):
    clk: zdc.bit = zdc.input()
    reset: zdc.bit = zdc.input()
    wr_en: zdc.bit = zdc.input()
    rd_en: zdc.bit = zdc.input()
    wr_ptr: zdc.bv[5] = zdc.output()
    rd_ptr: zdc.bv[5] = zdc.output()
    count: zdc.bv[5] = zdc.output()

    @zdc.sync(clock=lambda s: s.clk, reset=lambda s: s.reset)
    def fifo_ctrl(self):
        if self.reset:
            self.wr_ptr = 0
            self.rd_ptr = 0
            self.count = 0
        else:
            if self.wr_en and self.count < DEPTH:
                self.wr_ptr = self.wr_ptr + 1
                self.count = self.count + 1
            if self.rd_en and self.count > 0:
                self.rd_ptr = self.rd_ptr + 1
                self.count = self.count - 1

        # Assertion: count never exceeds DEPTH
        assert self.count <= DEPTH

        # Cover: full state reachable
        zdc.cover(self.count == DEPTH)
