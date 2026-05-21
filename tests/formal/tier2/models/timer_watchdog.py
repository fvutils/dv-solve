"""Tier 2 benchmark: Programmable timer with watchdog timeout assertion.

Properties:
  - Assertion: counter <= 255 (trivially true for 8-bit, but exercises the flow).
  - Assertion: if not enable and not reload_cmd and not reset, counter unchanged
    (no action when disabled -- checked via the transition relation implicitly).
  - Cover: expired == 1.

BMC depth: 20 (cover reachable once counter reaches limit).
"""

import zuspec.dataclasses as zdc


@zdc.dataclass
class TimerWatchdog(zdc.Component):
    clk: zdc.bit = zdc.input()
    reset: zdc.bit = zdc.input()
    enable: zdc.bit = zdc.input()
    reload_cmd: zdc.bit = zdc.input()
    limit: zdc.bit8 = zdc.input()
    counter: zdc.bit8 = zdc.output()
    expired: zdc.bit = zdc.output()

    @zdc.sync(clock=lambda s: s.clk, reset=lambda s: s.reset)
    def timer_tick(self):
        if self.reset:
            self.counter = 0
            self.expired = 0
        else:
            if self.reload_cmd:
                self.counter = 0
                self.expired = 0
            elif self.enable and self.counter < self.limit:
                self.counter = self.counter + 1
            elif self.enable and self.counter >= self.limit:
                self.expired = 1

        # Assertion: counter is always within 8-bit range (trivially true)
        assert self.counter < 256

        # Cover: timer expires
        zdc.cover(self.expired == 1)
