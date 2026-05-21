"""Tier 2 benchmark: N-bit up-counter with overflow assertion.

Properties:
  - Assertion: count is always within N-bit range (trivially true, count wraps).
  - Cover: count reaches max value (2^N - 1).

BMC depth: 20 (cover reachable at depth 15 for N=4).
k-induction: assertion provable at k=1.
"""

import zuspec.dataclasses as zdc


@zdc.dataclass
class CounterOverflow(zdc.Component):
    clk: zdc.bit = zdc.input()
    reset: zdc.bit = zdc.input()
    count: zdc.bit8 = zdc.output()

    @zdc.sync(clock=lambda s: s.clk, reset=lambda s: s.reset)
    def tick(self):
        if self.reset:
            self.count = 0
        else:
            self.count = self.count + 1

        # Assertion: count is always < 256 (trivially true for 8-bit)
        assert self.count < 256

        # Cover: count reaches max value
        zdc.cover(self.count == 255)
