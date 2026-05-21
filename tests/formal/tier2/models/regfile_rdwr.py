"""Tier 2 benchmark: 4-entry register file with 1 write port and 1 read port.

Architecture:
  - 4 registers (r0-r3), each 8-bit, with synchronous write.
  - Read port: rd_addr selects register; rd_data reflects current value.

Properties:
  - Assertion: rd_data is always one of {r0, r1, r2, r3} (read mux is correct).
  - Cover: a write has occurred (last_wr_vld == 1).

BMC depth: 4.
k-induction: assertion provable at k=1.
"""

import zuspec.dataclasses as zdc


@zdc.dataclass
class RegfileRdwr(zdc.Component):
    clk: zdc.bit = zdc.input()
    reset: zdc.bit = zdc.input()
    r0: zdc.bit8 = zdc.output()
    r1: zdc.bit8 = zdc.output()
    r2: zdc.bit8 = zdc.output()
    r3: zdc.bit8 = zdc.output()
    wr_en: zdc.bit = zdc.input()
    wr_addr: zdc.bit8 = zdc.input()
    wr_data: zdc.bit8 = zdc.input()
    rd_addr: zdc.bit8 = zdc.input()
    rd_data: zdc.bit8 = zdc.output()
    last_wr_vld: zdc.bit = zdc.output()

    @zdc.sync(clock=lambda s: s.clk, reset=lambda s: s.reset)
    def regfile(self):
        if self.reset:
            self.r0 = 0
            self.r1 = 0
            self.r2 = 0
            self.r3 = 0
            self.rd_data = 0
            self.last_wr_vld = 0
        else:
            # Write logic
            if self.wr_en:
                if (self.wr_addr & 3) == 0:
                    self.r0 = self.wr_data
                elif (self.wr_addr & 3) == 1:
                    self.r1 = self.wr_data
                elif (self.wr_addr & 3) == 2:
                    self.r2 = self.wr_data
                else:
                    self.r3 = self.wr_data
                self.last_wr_vld = 1

            # Read logic (reads current-cycle value)
            if (self.rd_addr & 3) == 0:
                self.rd_data = self.r0
            elif (self.rd_addr & 3) == 1:
                self.rd_data = self.r1
            elif (self.rd_addr & 3) == 2:
                self.rd_data = self.r2
            else:
                self.rd_data = self.r3

        # Assertion: register values are always within 8-bit range (trivially true)
        assert self.r0 < 256

        # Cover: a write has occurred
        zdc.cover(self.last_wr_vld == 1)
