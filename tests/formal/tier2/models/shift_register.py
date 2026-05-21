"""Tier 2 benchmark: 8-bit serial-in/parallel-out shift register.

Properties:
  - Cover: dout == 255 (all ones shifted in).
  - Cover: dout == 170 (0xAA alternating pattern).

BMC depth: 20 (cover goals reachable within 8 shifts).
"""

import zuspec.dataclasses as zdc


@zdc.dataclass
class ShiftRegister(zdc.Component):
    clk: zdc.bit = zdc.input()
    reset: zdc.bit = zdc.input()
    din: zdc.bit = zdc.input()
    shift_en: zdc.bit = zdc.input()
    dout: zdc.bit8 = zdc.output()

    @zdc.sync(clock=lambda s: s.clk, reset=lambda s: s.reset)
    def shift(self):
        if self.reset:
            self.dout = 0
        else:
            if self.shift_en:
                self.dout = self.dout * 2 + self.din

        # Cover: all ones shifted in
        zdc.cover(self.dout == 255)

        # Cover: alternating 0xAA pattern
        zdc.cover(self.dout == 170)
