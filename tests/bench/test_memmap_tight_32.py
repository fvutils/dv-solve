"""Benchmark: tight 32-bit memory map with 12 packed regions + sum budget.

Inspired by chipsalliance/riscv-dv riscv_instr_gen_config.sv (Apache-2.0)
memory-region allocation, and Ibrahiiiiim/memory-using-sv-constraints
memory_n_var_partitions.sv (MIT).

Models allocating 12 non-overlapping memory regions in a 2 GB window
where sizes are ordered ascending and the total allocated size must
equal a fixed budget (192 MB).

Variables (47):  12x(base,size,end) + 10 running-sum helpers + budget var
Constraints (59): 12 end=base+size, 11 ordering, 11 size-order,
                   10 sum-chain + 1 sum==budget, 1 fits-in-window
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers

# Window and size parameters chosen so base + max_size < 0x7FFF_FFFF
# to avoid SV bit-width overflow.
# Size: [1 MB, 32 MB]  Base: [0, 0x6000_0000]  End: [0, 0x7FFF_FFFF]
# Max base+size = 0x6000_0000 + 0x200_0000 = 0x6200_0000 < 0x7FFF_FFFF
# Budget: 192 MB = 201326592 = 0x0C00_0000


@zdc.dataclass
class MemMapTight32:
    """12 non-overlapping regions with ascending sizes summing to 192 MB."""

    r0_base:  zdc.rand(domain=(0, 1610612736), default=0)
    r0_size:  zdc.rand(domain=(1048576, 33554432), default=1048576)
    r0_end:   zdc.rand(domain=(0, 2147483647), default=1048576)

    r1_base:  zdc.rand(domain=(0, 1610612736), default=1048576)
    r1_size:  zdc.rand(domain=(1048576, 33554432), default=1048576)
    r1_end:   zdc.rand(domain=(0, 2147483647), default=2097152)

    r2_base:  zdc.rand(domain=(0, 1610612736), default=2097152)
    r2_size:  zdc.rand(domain=(1048576, 33554432), default=1048576)
    r2_end:   zdc.rand(domain=(0, 2147483647), default=3145728)

    r3_base:  zdc.rand(domain=(0, 1610612736), default=3145728)
    r3_size:  zdc.rand(domain=(1048576, 33554432), default=1048576)
    r3_end:   zdc.rand(domain=(0, 2147483647), default=4194304)

    r4_base:  zdc.rand(domain=(0, 1610612736), default=4194304)
    r4_size:  zdc.rand(domain=(1048576, 33554432), default=1048576)
    r4_end:   zdc.rand(domain=(0, 2147483647), default=5242880)

    r5_base:  zdc.rand(domain=(0, 1610612736), default=5242880)
    r5_size:  zdc.rand(domain=(1048576, 33554432), default=1048576)
    r5_end:   zdc.rand(domain=(0, 2147483647), default=6291456)

    r6_base:  zdc.rand(domain=(0, 1610612736), default=6291456)
    r6_size:  zdc.rand(domain=(1048576, 33554432), default=1048576)
    r6_end:   zdc.rand(domain=(0, 2147483647), default=7340032)

    r7_base:  zdc.rand(domain=(0, 1610612736), default=7340032)
    r7_size:  zdc.rand(domain=(1048576, 33554432), default=1048576)
    r7_end:   zdc.rand(domain=(0, 2147483647), default=8388608)

    r8_base:  zdc.rand(domain=(0, 1610612736), default=8388608)
    r8_size:  zdc.rand(domain=(1048576, 33554432), default=1048576)
    r8_end:   zdc.rand(domain=(0, 2147483647), default=9437184)

    r9_base:  zdc.rand(domain=(0, 1610612736), default=9437184)
    r9_size:  zdc.rand(domain=(1048576, 33554432), default=1048576)
    r9_end:   zdc.rand(domain=(0, 2147483647), default=10485760)

    r10_base: zdc.rand(domain=(0, 1610612736), default=10485760)
    r10_size: zdc.rand(domain=(1048576, 33554432), default=1048576)
    r10_end:  zdc.rand(domain=(0, 2147483647), default=11534336)

    r11_base: zdc.rand(domain=(0, 1610612736), default=11534336)
    r11_size: zdc.rand(domain=(1048576, 33554432), default=1048576)
    r11_end:  zdc.rand(domain=(0, 2147483647), default=12582912)

    # Running-sum helpers (domain 0..0x7FFFFFFF to match end bit-width)
    s01:     zdc.rand(domain=(0, 2147483647), default=2097152)
    s012:    zdc.rand(domain=(0, 2147483647), default=3145728)
    s0123:   zdc.rand(domain=(0, 2147483647), default=4194304)
    s01234:  zdc.rand(domain=(0, 2147483647), default=5242880)
    s012345: zdc.rand(domain=(0, 2147483647), default=6291456)
    s0to6:   zdc.rand(domain=(0, 2147483647), default=7340032)
    s0to7:   zdc.rand(domain=(0, 2147483647), default=8388608)
    s0to8:   zdc.rand(domain=(0, 2147483647), default=9437184)
    s0to9:   zdc.rand(domain=(0, 2147483647), default=10485760)
    s0to10:  zdc.rand(domain=(0, 2147483647), default=11534336)

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
        assert self.r8_end == self.r8_base + self.r8_size
        assert self.r9_end == self.r9_base + self.r9_size
        assert self.r10_end == self.r10_base + self.r10_size
        assert self.r11_end == self.r11_base + self.r11_size

    @zdc.constraint
    def c_order(self):
        assert self.r0_end <= self.r1_base
        assert self.r1_end <= self.r2_base
        assert self.r2_end <= self.r3_base
        assert self.r3_end <= self.r4_base
        assert self.r4_end <= self.r5_base
        assert self.r5_end <= self.r6_base
        assert self.r6_end <= self.r7_base
        assert self.r7_end <= self.r8_base
        assert self.r8_end <= self.r9_base
        assert self.r9_end <= self.r10_base
        assert self.r10_end <= self.r11_base

    @zdc.constraint
    def c_fits(self):
        assert self.r11_end <= 2147483647

    @zdc.constraint
    def c_size_order(self):
        assert self.r0_size <= self.r1_size
        assert self.r1_size <= self.r2_size
        assert self.r2_size <= self.r3_size
        assert self.r3_size <= self.r4_size
        assert self.r4_size <= self.r5_size
        assert self.r5_size <= self.r6_size
        assert self.r6_size <= self.r7_size
        assert self.r7_size <= self.r8_size
        assert self.r8_size <= self.r9_size
        assert self.r9_size <= self.r10_size
        assert self.r10_size <= self.r11_size

    @zdc.constraint
    def c_sum_chain(self):
        assert self.s01 == self.r0_size + self.r1_size
        assert self.s012 == self.s01 + self.r2_size
        assert self.s0123 == self.s012 + self.r3_size
        assert self.s01234 == self.s0123 + self.r4_size
        assert self.s012345 == self.s01234 + self.r5_size
        assert self.s0to6 == self.s012345 + self.r6_size
        assert self.s0to7 == self.s0to6 + self.r7_size
        assert self.s0to8 == self.s0to7 + self.r8_size
        assert self.s0to9 == self.s0to8 + self.r9_size
        assert self.s0to10 == self.s0to9 + self.r10_size

    @zdc.constraint
    def c_total(self):
        assert self.s0to10 + self.r11_size == 201326592


def _check(sol):
    n = 12
    sizes = []
    for i in range(n):
        b = sol[f"r{i}_base"]
        s = sol[f"r{i}_size"]
        e = sol[f"r{i}_end"]
        assert e == b + s, f"r{i}: end {e} != base {b} + size {s}"
        assert 1048576 <= s <= 33554432, f"r{i}: size {s} out of range"
        assert e <= 2147483647, f"r{i}: end {e} exceeds window"
        sizes.append(s)

    for i in range(n - 1):
        assert sol[f"r{i}_end"] <= sol[f"r{i+1}_base"], (
            f"r{i} overlaps r{i+1}")
        assert sizes[i] <= sizes[i+1], (
            f"size order: r{i}={sizes[i]} > r{i+1}={sizes[i+1]}")

    assert sum(sizes) == 201326592, f"total size {sum(sizes)} != 201326592"


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_memmap_tight_32(solver, tmp_path):
    solver.bench(MemMapTight32, validate=_check, tmp_path=tmp_path)
