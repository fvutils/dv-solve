"""Benchmark: SoC address-map allocation.

A realistic scenario from SoC verification: allocate four non-overlapping
memory regions (BOOT ROM, SRAM, PERIPH, DMA buffer) within a 64 KB address
space.

Variables (12 total — 3 per region):
  <region>_base     — start address
  <region>_size     — size in bytes
  <region>_end_addr — base + size  (arithmetic helper)

Constraints:
  c_<r>_end  : end_addr == base + size            [var == var + var]
  c_boot_sram: boot_end_addr <= sram_base         [var <= var]
  c_sram_per : sram_end_addr <= periph_base       [var <= var]
  c_per_dma  : periph_end_addr <= dma_base        [var <= var]
  c_fits     : dma_end_addr <= 0xFFFF             [var <= const]

Address space pressure:
  Each size in [0x1000 .. 0x6000] (4 KB – 24 KB).
  Maximum total: 4 × 24 KB = 96 KB > 64 KB, so the solver must sometimes
  backtrack when it picks large sizes early.

All constraints are natively compiled (no SMT fallback).
"""
import pytest
import zuspec.dataclasses as zdc
from solvers import solvers


@zdc.dataclass
class SoCMemMap:
    # BOOT ROM — typically at the bottom of the address space
    # base_max (0x9FFF) + size_max (0x6000) = 0xFFFF: no 16-bit overflow in any backend
    boot_base:     zdc.rand(domain=(0x0000, 0x9FFF), default=0x0000)
    boot_size:     zdc.rand(domain=(0x1000, 0x6000), default=0x2000)
    boot_end_addr: zdc.rand(domain=(0x1000, 0xFFFF), default=0x2000)

    # SRAM — general-purpose RAM above BOOT ROM
    sram_base:     zdc.rand(domain=(0x0000, 0x9FFF), default=0x2000)
    sram_size:     zdc.rand(domain=(0x1000, 0x6000), default=0x4000)
    sram_end_addr: zdc.rand(domain=(0x1000, 0xFFFF), default=0x6000)

    # PERIPH — memory-mapped peripherals
    periph_base:     zdc.rand(domain=(0x0000, 0x9FFF), default=0x8000)
    periph_size:     zdc.rand(domain=(0x1000, 0x6000), default=0x2000)
    periph_end_addr: zdc.rand(domain=(0x1000, 0xFFFF), default=0xA000)

    # DMA buffer — at the top of the address space
    dma_base:     zdc.rand(domain=(0x0000, 0x9FFF), default=0xA000)
    dma_size:     zdc.rand(domain=(0x1000, 0x6000), default=0x2000)
    dma_end_addr: zdc.rand(domain=(0x1000, 0xFFFF), default=0xC000)

    # ── end_addr helpers ──────────────────────────────────────────────
    @zdc.constraint
    def c_boot_end(self):
        assert self.boot_end_addr == self.boot_base + self.boot_size

    @zdc.constraint
    def c_sram_end(self):
        assert self.sram_end_addr == self.sram_base + self.sram_size

    @zdc.constraint
    def c_periph_end(self):
        assert self.periph_end_addr == self.periph_base + self.periph_size

    @zdc.constraint
    def c_dma_end(self):
        assert self.dma_end_addr == self.dma_base + self.dma_size

    # ── non-overlap (total ordering of regions) ───────────────────────
    @zdc.constraint
    def c_boot_before_sram(self):
        assert self.boot_end_addr <= self.sram_base

    @zdc.constraint
    def c_sram_before_periph(self):
        assert self.sram_end_addr <= self.periph_base

    @zdc.constraint
    def c_periph_before_dma(self):
        assert self.periph_end_addr <= self.dma_base

    # ── fits in 64 KB ────────────────────────────────────────────────
    @zdc.constraint
    def c_fits_in_64k(self):
        assert self.dma_end_addr <= 0xFFFF


def _check(sol):
    # Each end_addr matches base + size
    for region in ("boot", "sram", "periph", "dma"):
        base = sol[f"{region}_base"]
        size = sol[f"{region}_size"]
        end  = sol[f"{region}_end_addr"]
        assert end == base + size, (
            f"{region}: end_addr {end:#x} != base {base:#x} + size {size:#x}"
        )
        assert 0x1000 <= size <= 0x6000, f"{region}: size {size:#x} out of range"

    # Non-overlap
    assert sol["boot_end_addr"]   <= sol["sram_base"],   "BOOT overlaps SRAM"
    assert sol["sram_end_addr"]   <= sol["periph_base"], "SRAM overlaps PERIPH"
    assert sol["periph_end_addr"] <= sol["dma_base"],    "PERIPH overlaps DMA"

    # Fits in 64 KB
    assert sol["dma_end_addr"] <= 0xFFFF, (
        f"DMA region overflows 64 KB: dma_end_addr={sol['dma_end_addr']:#x}"
    )


@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_soc_mem_map(solver, tmp_path):
    solver.bench(SoCMemMap, validate=_check, tmp_path=tmp_path)
