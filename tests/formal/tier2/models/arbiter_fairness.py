"""Tier 2 benchmark: 4-port fixed-priority arbiter with mutual exclusion.

Architecture:
  - 4 request lines (req, low 4 bits) and a one-hot grant output.
  - Fixed priority: port 0 > port 1 > port 2 > port 3.
  - Grant is one-hot or zero (no grant when no request).

Properties:
  - Assertion: grant is always one-hot or zero (mutual exclusion).
  - Cover: grant == 8 (port 3 is granted).

BMC depth: 10 (cover reachable at depth 2).
k-induction: assertion provable at k=1.
"""

import zuspec.dataclasses as zdc


@zdc.dataclass
class ArbiterFairness(zdc.Component):
    clk: zdc.bit = zdc.input()
    reset: zdc.bit = zdc.input()
    req: zdc.bit8 = zdc.input()
    grant: zdc.bit8 = zdc.output()
    last_grant: zdc.bit8 = zdc.output()

    @zdc.sync(clock=lambda s: s.clk, reset=lambda s: s.reset)
    def arbiter(self):
        if self.reset:
            self.grant = 0
            self.last_grant = 0
        else:
            if (self.req & 1) == 1:
                self.grant = 1
                self.last_grant = 0
            elif (self.req & 2) == 2:
                self.grant = 2
                self.last_grant = 1
            elif (self.req & 4) == 4:
                self.grant = 4
                self.last_grant = 2
            elif (self.req & 8) == 8:
                self.grant = 8
                self.last_grant = 3
            else:
                self.grant = 0

        # Assertion: mutual exclusion -- grant is one-hot or zero
        assert (self.grant == 0) or (self.grant == 1) or (self.grant == 2) or (self.grant == 4) or (self.grant == 8)

        # Cover: port 3 granted
        zdc.cover(self.grant == 8)
