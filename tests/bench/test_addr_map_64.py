"""Benchmark: 64-bit SoC address-map allocation (8 regions).

Scales the existing 32/40-bit address-map benchmarks to full 64-bit.
BDD-based simulators must build BDDs over 24 x 64-bit variables with
ordering constraints -- exponentially harder than 32-bit.  Interval-
propagation solvers are insensitive to variable width.

Variables (24 total -- 3 per region):
  r<N>_base  -- start address         [0, max_base]
  r<N>_size  -- region size           [4 GB, 32 GB]
  r<N>_end   -- base + size helper    [4 GB, max_end]

Constraints:
  c_ends  : r<N>_end == r<N>_base + r<N>_size   (8 arithmetic equalities)
  c_order : r0_end <= r1_base <= ... <= r7_end   (7 ordering constraints)
  c_fits  : r7_end <= 0x0000_FFFF_FFFF_FFFF      (48-bit ceiling)
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers

# Use a 48-bit window (256 TB) -- large enough to be a true 64-bit problem
# for the solver, but small enough that sizes are meaningful.
_SZ_LO    = 0x1_0000_0000          #  4 GB minimum region size
_SZ_HI    = 0x8_0000_0000          # 32 GB maximum region size
_ADDR_TOP = 0x0000_FFFF_FFFF_FFFF  # 48-bit ceiling
_BASE_MAX = _ADDR_TOP - _SZ_HI     # max valid base


@zdc.dataclass
class AddrMap64:
    """Eight non-overlapping memory regions in a 64-bit address space."""

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
        assert self.r7_end <= _ADDR_TOP


def _check(sol):
    for i in range(8):
        b = sol[f"r{i}_base"]; s = sol[f"r{i}_size"]; e = sol[f"r{i}_end"]
        assert e == b + s, f"r{i}: end {e:#018x} != base {b:#018x} + size {s:#018x}"
        assert _SZ_LO <= s <= _SZ_HI, f"r{i}: size {s:#018x} out of range"
        assert e <= _ADDR_TOP, f"r{i}: end {e:#018x} exceeds ceiling"
    for i in range(7):
        assert sol[f"r{i}_end"] <= sol[f"r{i+1}_base"], (
            f"r{i} overlaps r{i+1}: end={sol[f'r{i}_end']:#018x} "
            f"> base={sol[f'r{i+1}_base']:#018x}"
        )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_addr_map_64(solver, tmp_path):
    solver.bench(AddrMap64, validate=_check, tmp_path=tmp_path)
