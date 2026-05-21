"""Unit tests for zsp_pool_t (Phase 2b).

Tests:
- basic alloc + ptr conversion
- alignment is respected
- overflow returns EXPR_NULL
- overflow is sticky
- reset clears allocations and overflow flag
- used() tracking
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL = 0xFFFFFFFF


def _setup(lib: ctypes.CDLL):
    lib.zsp_pool_init.restype  = ctypes.c_void_p
    lib.zsp_pool_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    lib.zsp_pool_alloc.restype  = ctypes.c_uint32
    lib.zsp_pool_alloc.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]

    lib.zsp_pool_ptr.restype  = ctypes.c_void_p
    lib.zsp_pool_ptr.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.zsp_pool_reset.restype  = None
    lib.zsp_pool_reset.argtypes = [ctypes.c_void_p]

    lib.zsp_pool_used.restype  = ctypes.c_uint32
    lib.zsp_pool_used.argtypes = [ctypes.c_void_p]


_HEADER_SIZE = 16   # sizeof(zsp_pool_t) — 4 × uint32_t


class TestPool:
    @pytest.fixture(autouse=True)
    def setup_lib(self, libzsp):
        _setup(libzsp)
        self.lib = libzsp

    def _make_pool(self, capacity_bytes: int):
        buf_size = _HEADER_SIZE + capacity_bytes
        buf = (ctypes.c_uint8 * buf_size)()
        pool = self.lib.zsp_pool_init(buf, buf_size)
        assert pool is not None, "zsp_pool_init returned NULL"
        return pool, buf

    def test_init_returns_non_null(self):
        pool, buf = self._make_pool(256)
        assert pool is not None

    def test_init_too_small(self):
        # Buffer smaller than the pool header must return NULL
        buf = (ctypes.c_uint8 * (_HEADER_SIZE - 1))()
        result = self.lib.zsp_pool_init(buf, _HEADER_SIZE - 1)
        assert result is None

    def test_alloc_basic(self):
        pool, buf = self._make_pool(256)
        off = self.lib.zsp_pool_alloc(pool, 32, 1)
        assert off != EXPR_NULL
        assert off >= _HEADER_SIZE, "Offset must point past the pool header"

    def test_alloc_ptr_roundtrip(self):
        pool, buf = self._make_pool(256)
        off = self.lib.zsp_pool_alloc(pool, 4, 4)
        assert off != EXPR_NULL
        ptr = self.lib.zsp_pool_ptr(pool, off)
        assert ptr is not None

    def test_null_ptr_for_expr_null(self):
        pool, buf = self._make_pool(256)
        ptr = self.lib.zsp_pool_ptr(pool, EXPR_NULL)
        assert ptr is None

    def test_alignment_respected(self):
        pool, buf = self._make_pool(512)
        for align in (1, 2, 4, 8, 16):
            off = self.lib.zsp_pool_alloc(pool, 1, align)
            assert off != EXPR_NULL
            ptr = self.lib.zsp_pool_ptr(pool, off)
            assert ptr % align == 0, f"align={align}: ptr {ptr:#x} not aligned"

    def test_used_tracking(self):
        pool, buf = self._make_pool(256)
        assert self.lib.zsp_pool_used(pool) == 0
        self.lib.zsp_pool_alloc(pool, 32, 1)
        used = self.lib.zsp_pool_used(pool)
        assert used >= 32

    def test_overflow_returns_expr_null(self):
        pool, buf = self._make_pool(32)
        # Exhaust the pool
        while True:
            off = self.lib.zsp_pool_alloc(pool, 8, 1)
            if off == EXPR_NULL:
                break

    def test_overflow_is_sticky(self):
        pool, buf = self._make_pool(16)
        # Force overflow
        self.lib.zsp_pool_alloc(pool, 16, 1)   # may or may not overflow
        self.lib.zsp_pool_alloc(pool, 16, 1)   # this will overflow
        # Once overflow, all further allocs must also return EXPR_NULL
        for _ in range(3):
            off = self.lib.zsp_pool_alloc(pool, 1, 1)
            assert off == EXPR_NULL, "Overflow not sticky"

    def test_reset_clears_allocations(self):
        pool, buf = self._make_pool(64)
        off1 = self.lib.zsp_pool_alloc(pool, 16, 1)
        assert off1 != EXPR_NULL
        self.lib.zsp_pool_reset(pool)
        assert self.lib.zsp_pool_used(pool) == 0
        # After reset, a new alloc should return the same base offset
        off2 = self.lib.zsp_pool_alloc(pool, 16, 1)
        assert off2 == off1, "Expected same base offset after reset"

    def test_reset_clears_overflow(self):
        pool, buf = self._make_pool(8)
        # Force overflow
        self.lib.zsp_pool_alloc(pool, 64, 1)
        assert self.lib.zsp_pool_alloc(pool, 1, 1) == EXPR_NULL
        self.lib.zsp_pool_reset(pool)
        # Should be able to allocate again
        off = self.lib.zsp_pool_alloc(pool, 4, 1)
        assert off != EXPR_NULL
