"""Tier 2 benchmark: 4-state FSM with one-hot encoding invariant.

Properties:
  - Assertion: exactly one of the low 4 bits of state is set (one-hot invariant).
  - Cover: state reaches DONE.

BMC depth: 10 (cover reachable at depth 4).
k-induction: assertion provable at k=1.
"""

import zuspec.dataclasses as zdc

IDLE = 1
WAIT = 2
BUSY = 4
DONE = 8


@zdc.dataclass
class FsmOnehot(zdc.Component):
    clk: zdc.bit = zdc.input()
    reset: zdc.bit = zdc.input()
    req: zdc.bit = zdc.input()
    ack: zdc.bit = zdc.input()
    state: zdc.bit8 = zdc.output()

    @zdc.sync(clock=lambda s: s.clk, reset=lambda s: s.reset)
    def fsm_step(self):
        if self.reset:
            self.state = IDLE
        else:
            if self.state == IDLE and self.req:
                self.state = WAIT
            elif self.state == WAIT and self.ack:
                self.state = BUSY
            elif self.state == BUSY:
                self.state = DONE
            elif self.state == DONE:
                self.state = IDLE

        # Assertion: exactly one of the low 4 bits is set (one-hot)
        assert self.state == IDLE or self.state == WAIT or self.state == BUSY or self.state == DONE

        # Cover: state reaches DONE
        zdc.cover(self.state == DONE)
