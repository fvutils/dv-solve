"""Benchmark: 32-bit SoC address-map allocation (8 regions).

A stress-test variant of the SoCMemMap benchmark using 32-bit address
variables.  Wider bit-widths expose scaling differences between solver
backends: BDD/enumeration-based commercial simulators work exponentially
harder on 32-bit variables, while interval-based CP and native solvers
are largely insensitive to domain width.

Variables (24 total — 3 per region):
  r<N>_base  — start address         [0, 0x6F00_0000]
  r<N>_size  — region size           [16 MB, 128 MB]
  r<N>_end   — base + size helper    [16 MB, 0x7FFF_FFFF]

Constraints:
  c_ends  : r<N>_end == r<N>_base + r<N>_size  (8 arithmetic equalities)
  c_order : r0_end <= r1_base <= ... <= r7_end  (7 ordering constraints)
  c_fits  : r7_end <= 0x7FFF_FFFF              (1 upper-bound constraint)

Address space pressure:
  Min total size: 8 × 16 MB = 128 MB  (fits easily in 2 GB)
  Max total size: 8 × 128 MB = 1 GB   (still fits, but leaves < 1 GB of
  slack, so the solver must choose carefully when sizes are large).

All constraints are natively compiled (no SMT fallback).
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


# ── address-space parameters ──────────────────────────────────────────────────
_SZ_LO    = 0x0100_0000  # 16 MB  — minimum region size
_SZ_HI    = 0x0800_0000  # 128 MB — maximum region size
_BASE_MAX = 0x6F00_0000  # maximum valid base address
_ADDR_TOP = 0x7FFF_FFFF  # top of 2 GB window (inclusive)


@zdc.dataclass
class SoCAddrMap32:
    """Eight non-overlapping memory regions in a 32-bit (2 GB) address space."""

    r0_base: zdc.rand(domain=(0, _BASE_MAX), default=0)
    r0_size: zdc.rand(domain=(_SZ_LO, _SZ_HI), default=_SZ_LO)
    r0_end:  zdc.rand(domain=(_SZ_LO, _ADDR_TOP), default=_SZ_LO)

    r1_base: zdc.rand(domain=(0, _BASE_MAX), default=0)
    r1_size: zdc.rand(domain=(_SZ_LO, _SZ_HI), default=_SZ_LO)
    r1_end:  zdc.rand(domain=(_SZ_LO, _ADDR_TOP), default=_SZ_LO)

    r2_base: zdc.rand(domain=(0, _BASE_MAX), default=0)
    r2_size: zdc.rand(domain=(_SZ_LO, _SZ_HI), default=_SZ_LO)
    r2_end:  zdc.rand(domain=(_SZ_LO, _ADDR_TOP), default=_SZ_LO)

    r3_base: zdc.rand(domain=(0, _BASE_MAX), default=0)
    r3_size: zdc.rand(domain=(_SZ_LO, _SZ_HI), default=_SZ_LO)
    r3_end:  zdc.rand(domain=(_SZ_LO, _ADDR_TOP), default=_SZ_LO)

    r4_base: zdc.rand(domain=(0, _BASE_MAX), default=0)
    r4_size: zdc.rand(domain=(_SZ_LO, _SZ_HI), default=_SZ_LO)
    r4_end:  zdc.rand(domain=(_SZ_LO, _ADDR_TOP), default=_SZ_LO)

    r5_base: zdc.rand(domain=(0, _BASE_MAX), default=0)
    r5_size: zdc.rand(domain=(_SZ_LO, _SZ_HI), default=_SZ_LO)
    r5_end:  zdc.rand(domain=(_SZ_LO, _ADDR_TOP), default=_SZ_LO)

    r6_base: zdc.rand(domain=(0, _BASE_MAX), default=0)
    r6_size: zdc.rand(domain=(_SZ_LO, _SZ_HI), default=_SZ_LO)
    r6_end:  zdc.rand(domain=(_SZ_LO, _ADDR_TOP), default=_SZ_LO)

    r7_base: zdc.rand(domain=(0, _BASE_MAX), default=0)
    r7_size: zdc.rand(domain=(_SZ_LO, _SZ_HI), default=_SZ_LO)
    r7_end:  zdc.rand(domain=(_SZ_LO, _ADDR_TOP), default=_SZ_LO)

    @zdc.constraint
    def c_ends(self):
        assert self.r0_end == self.r0_base + self.r0_size
        assert self.r1_end == self.r1_base + self.r1_size
        assert self.r2_end == self.r2_base + self.r2_size
        assert self.r3_end == self.r3_base + self.r3_size
        assert self.r4_end == self.r4_base + self.r4_size
        assert self.r5_end == self.r5_base + self.r5_size
        assert self.r6_end == self.r6_base + self.r6_size
        assert self.r7_end == self.r7_base + self.r7_size

    @zdc.constraint
    def c_order(self):
        assert self.r0_end <= self.r1_base
        assert self.r1_end <= self.r2_base
        assert self.r2_end <= self.r3_base
        assert self.r3_end <= self.r4_base
        assert self.r4_end <= self.r5_base
        assert self.r5_end <= self.r6_base
        assert self.r6_end <= self.r7_base

    @zdc.constraint
    def c_fits(self):
        assert self.r7_end <= 0x7FFF_FFFF


def _check(sol):
    for i in range(8):
        b = sol[f"r{i}_base"]; s = sol[f"r{i}_size"]; e = sol[f"r{i}_end"]
        assert e == b + s, f"r{i}: end {e:#010x} != base {b:#010x} + size {s:#010x}"
        assert _SZ_LO <= s <= _SZ_HI, f"r{i}: size {s:#010x} out of range"
        assert e <= _ADDR_TOP, f"r{i}: end {e:#010x} exceeds 2 GB"
    for i in range(7):
        assert sol[f"r{i}_end"] <= sol[f"r{i+1}_base"], f"r{i} overlaps r{i+1}"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_soc_addr_map_32(solver, tmp_path):
    solver.bench(SoCAddrMap32, validate=_check, tmp_path=tmp_path)
