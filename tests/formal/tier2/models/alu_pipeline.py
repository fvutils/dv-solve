"""Tier 2 benchmark: 3-stage pipelined ALU (fetch, execute, writeback).

Stages:
  Stage 1 (Fetch): Capture inputs a_in, b_in, op_in into pipeline registers.
  Stage 2 (Execute): Compute result based on op (0=add, 1=sub, 2=and, 3=or).
  Stage 3 (Writeback): Copy result to output.

Properties:
  - Assertion: when out_vld is 0 (e.g. after reset), result must be 0.
  - Cover: out_vld == 1 (pipeline produces a valid result).

BMC depth: 10 (cover reachable at depth 4).
k-induction: assertion provable at k=1.
"""

import zuspec.dataclasses as zdc


@zdc.dataclass
class AluPipeline(zdc.Component):
    clk: zdc.bit = zdc.input()
    reset: zdc.bit = zdc.input()

    # Primary inputs
    a_in: zdc.bit8 = zdc.input()
    b_in: zdc.bit8 = zdc.input()
    op_in: zdc.bit8 = zdc.input()

    # Stage 1 pipeline registers
    s1_a: zdc.bit8 = zdc.output()
    s1_b: zdc.bit8 = zdc.output()
    s1_op: zdc.bit8 = zdc.output()
    s1_vld: zdc.bit = zdc.output()

    # Stage 2 pipeline registers
    s2_res: zdc.bit8 = zdc.output()
    s2_vld: zdc.bit = zdc.output()

    # Stage 3 output
    result: zdc.bit8 = zdc.output()
    out_vld: zdc.bit = zdc.output()

    @zdc.sync(clock=lambda s: s.clk, reset=lambda s: s.reset)
    def pipeline(self):
        if self.reset:
            self.s1_a = 0
            self.s1_b = 0
            self.s1_op = 0
            self.s1_vld = 0
            self.s2_res = 0
            self.s2_vld = 0
            self.result = 0
            self.out_vld = 0
        else:
            # Stage 1: Fetch - capture inputs
            self.s1_a = self.a_in
            self.s1_b = self.b_in
            self.s1_op = self.op_in
            self.s1_vld = 1

            # Stage 2: Execute - compute based on op (low 2 bits)
            if (self.s1_op & 3) == 0:
                self.s2_res = (self.s1_a + self.s1_b) & 255
            elif (self.s1_op & 3) == 1:
                self.s2_res = (self.s1_a - self.s1_b) & 255
            elif (self.s1_op & 3) == 2:
                self.s2_res = self.s1_a & self.s1_b
            else:
                self.s2_res = self.s1_a | self.s1_b
            self.s2_vld = self.s1_vld

            # Stage 3: Writeback
            self.result = self.s2_res
            self.out_vld = self.s2_vld

        # Assertion: when out_vld is 0, result must be 0
        assert self.out_vld == 1 or self.result == 0

        # Cover: pipeline produces a valid output
        zdc.cover(self.out_vld == 1)
