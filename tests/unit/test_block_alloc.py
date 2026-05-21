"""Unit tests for zsp_block_alloc_t (Phase 2a).

Tests:
- get/put round-trip
- cached block is reused without a new malloc
- destroy releases all cached blocks
"""
from __future__ import annotations

import ctypes
import pytest


# ------------------------------------------------------------------ #
# ctypes helpers                                                       #
# ------------------------------------------------------------------ #

def _setup(lib: ctypes.CDLL):
    """Declare function signatures on the library handle."""
    # zsp_block_alloc_create(alloc*, block_size) -> ba*
    lib.zsp_block_alloc_create.restype  = ctypes.c_void_p
    lib.zsp_block_alloc_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    # zsp_block_alloc_get(ba*) -> void*
    lib.zsp_block_alloc_get.restype  = ctypes.c_void_p
    lib.zsp_block_alloc_get.argtypes = [ctypes.c_void_p]

    # zsp_block_alloc_put(ba*, block*)
    lib.zsp_block_alloc_put.restype  = None
    lib.zsp_block_alloc_put.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    # zsp_block_alloc_destroy(ba*)
    lib.zsp_block_alloc_destroy.restype  = None
    lib.zsp_block_alloc_destroy.argtypes = [ctypes.c_void_p]

    # zsp_block_alloc_block_size(ba*) -> size_t
    lib.zsp_block_alloc_block_size.restype  = ctypes.c_size_t
    lib.zsp_block_alloc_block_size.argtypes = [ctypes.c_void_p]


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

class TestBlockAlloc:
    @pytest.fixture(autouse=True)
    def setup_lib(self, libzsp):
        _setup(libzsp)
        self.lib = libzsp

    def _create(self, block_size=4096):
        ba = self.lib.zsp_block_alloc_create(None, block_size)
        assert ba is not None, "zsp_block_alloc_create returned NULL"
        return ba

    def test_create_and_destroy(self):
        ba = self._create()
        self.lib.zsp_block_alloc_destroy(ba)

    def test_block_size_preserved(self):
        ba = self._create(block_size=1024)
        assert self.lib.zsp_block_alloc_block_size(ba) == 1024
        self.lib.zsp_block_alloc_destroy(ba)

    def test_get_returns_non_null(self):
        ba = self._create()
        blk = self.lib.zsp_block_alloc_get(ba)
        assert blk is not None
        self.lib.zsp_block_alloc_put(ba, blk)
        self.lib.zsp_block_alloc_destroy(ba)

    def test_put_get_reuses_block(self):
        """A put-then-get must return the same block (cache hit)."""
        ba = self._create()
        blk1 = self.lib.zsp_block_alloc_get(ba)
        assert blk1 is not None
        self.lib.zsp_block_alloc_put(ba, blk1)
        blk2 = self.lib.zsp_block_alloc_get(ba)
        assert blk2 == blk1, (
            f"Expected cached block {blk1:#x} but got {blk2:#x}"
        )
        self.lib.zsp_block_alloc_put(ba, blk2)
        self.lib.zsp_block_alloc_destroy(ba)

    def test_multiple_blocks(self):
        """Allocate several blocks; put them all back; get them all again."""
        ba = self._create(block_size=256)
        blocks = [self.lib.zsp_block_alloc_get(ba) for _ in range(5)]
        assert all(b is not None for b in blocks)
        assert len(set(blocks)) == 5, "Expected distinct blocks"

        # Put all back
        for b in blocks:
            self.lib.zsp_block_alloc_put(ba, b)

        # Get same 5 back (LIFO order from the free list)
        regot = [self.lib.zsp_block_alloc_get(ba) for _ in range(5)]
        assert sorted(regot) == sorted(blocks), (
            "Not all cached blocks were returned"
        )
        for b in regot:
            self.lib.zsp_block_alloc_put(ba, b)
        self.lib.zsp_block_alloc_destroy(ba)

    def test_destroy_with_cached_blocks(self):
        """destroy() must not leak when blocks are in the cache."""
        ba = self._create()
        blk = self.lib.zsp_block_alloc_get(ba)
        assert blk is not None
        self.lib.zsp_block_alloc_put(ba, blk)
        # destroy frees the cached block — no ASan error expected
        self.lib.zsp_block_alloc_destroy(ba)

    def test_minimum_block_size(self):
        """Even a block_size of 1 should work (rounded up to pointer size)."""
        ba = self._create(block_size=1)
        blk = self.lib.zsp_block_alloc_get(ba)
        assert blk is not None
        self.lib.zsp_block_alloc_put(ba, blk)
        self.lib.zsp_block_alloc_destroy(ba)
