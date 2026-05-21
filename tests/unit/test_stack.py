"""Unit tests for zsp_stack_t (Phase 2c).

Tests:
- create/destroy
- alloc + write-back
- push/alloc/pop restores block count
- multiple nested push/pop levels
- alloc spanning multiple blocks
- overflow (request larger than block) returns NULL
"""
from __future__ import annotations

import ctypes
import pytest


def _setup(lib: ctypes.CDLL):
    # block allocator
    lib.zsp_block_alloc_create.restype  = ctypes.c_void_p
    lib.zsp_block_alloc_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.zsp_block_alloc_destroy.restype  = None
    lib.zsp_block_alloc_destroy.argtypes = [ctypes.c_void_p]

    # stack
    lib.zsp_stack_create.restype  = ctypes.c_void_p
    lib.zsp_stack_create.argtypes = [ctypes.c_void_p]

    lib.zsp_stack_destroy.restype  = None
    lib.zsp_stack_destroy.argtypes = [ctypes.c_void_p]

    lib.zsp_stack_alloc.restype  = ctypes.c_void_p
    lib.zsp_stack_alloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t]

    lib.zsp_stack_block_count.restype  = ctypes.c_size_t
    lib.zsp_stack_block_count.argtypes = [ctypes.c_void_p]

    # push returns a struct by value — easier to call through a helper
    # We'll use a thin pointer-based wrapper strategy:
    # push/pop are called via ctypes structures.
    lib.zsp_stack_push.restype  = _StackMark
    lib.zsp_stack_push.argtypes = [ctypes.c_void_p]

    lib.zsp_stack_pop.restype  = None
    lib.zsp_stack_pop.argtypes = [ctypes.c_void_p, _StackMark]


class _StackMark(ctypes.Structure):
    _fields_ = [
        ("block", ctypes.c_void_p),
        ("intra", ctypes.c_uint32),
    ]


# Override after class definition
def _setup_final(lib):
    lib.zsp_stack_push.restype  = _StackMark
    lib.zsp_stack_push.argtypes = [ctypes.c_void_p]
    lib.zsp_stack_pop.restype  = None
    lib.zsp_stack_pop.argtypes = [ctypes.c_void_p, _StackMark]


_BLOCK_SIZE = 256


class TestStack:
    @pytest.fixture(autouse=True)
    def setup_lib(self, libzsp):
        _setup(libzsp)
        _setup_final(libzsp)
        self.lib = libzsp
        ba = libzsp.zsp_block_alloc_create(None, _BLOCK_SIZE)
        assert ba is not None
        self.ba = ba
        stk = libzsp.zsp_stack_create(ba)
        assert stk is not None
        self.stk = stk
        yield
        libzsp.zsp_stack_destroy(stk)
        libzsp.zsp_block_alloc_destroy(ba)

    def test_create_destroy(self):
        # Created in fixture; just verifying no crash
        pass

    def test_initial_block_count(self):
        assert self.lib.zsp_stack_block_count(self.stk) == 0

    def test_alloc_returns_non_null(self):
        ptr = self.lib.zsp_stack_alloc(self.stk, 16, 8)
        assert ptr is not None

    def test_alloc_allocates_block(self):
        self.lib.zsp_stack_alloc(self.stk, 16, 1)
        assert self.lib.zsp_stack_block_count(self.stk) == 1

    def test_push_pop_same_block(self):
        mark = self.lib.zsp_stack_push(self.stk)
        assert self.lib.zsp_stack_block_count(self.stk) == 0
        self.lib.zsp_stack_alloc(self.stk, 16, 1)
        assert self.lib.zsp_stack_block_count(self.stk) == 1
        self.lib.zsp_stack_pop(self.stk, mark)
        assert self.lib.zsp_stack_block_count(self.stk) == 0

    def test_nested_push_pop(self):
        self.lib.zsp_stack_alloc(self.stk, 16, 1)  # depth 0 alloc
        mark1 = self.lib.zsp_stack_push(self.stk)
        self.lib.zsp_stack_alloc(self.stk, 16, 1)
        mark2 = self.lib.zsp_stack_push(self.stk)
        self.lib.zsp_stack_alloc(self.stk, 16, 1)
        bc3 = self.lib.zsp_stack_block_count(self.stk)

        self.lib.zsp_stack_pop(self.stk, mark2)
        bc2 = self.lib.zsp_stack_block_count(self.stk)
        assert bc2 <= bc3

        self.lib.zsp_stack_pop(self.stk, mark1)
        bc1 = self.lib.zsp_stack_block_count(self.stk)
        assert bc1 <= bc2

    def test_alloc_fills_multiple_blocks(self):
        """Allocating more than one block's capacity triggers a new block."""
        # Allocate enough to fill the first block and spill into a second
        slot_size = _BLOCK_SIZE // 4  # 64 bytes
        count = 0
        for _ in range(8):
            ptr = self.lib.zsp_stack_alloc(self.stk, slot_size, 1)
            assert ptr is not None
            count += 1
        assert self.lib.zsp_stack_block_count(self.stk) >= 2

    def test_pop_returns_extra_blocks(self):
        mark = self.lib.zsp_stack_push(self.stk)
        slot_size = _BLOCK_SIZE // 4
        for _ in range(8):
            self.lib.zsp_stack_alloc(self.stk, slot_size, 1)
        assert self.lib.zsp_stack_block_count(self.stk) >= 2
        self.lib.zsp_stack_pop(self.stk, mark)
        assert self.lib.zsp_stack_block_count(self.stk) == 0

    def test_alloc_too_large_returns_null(self):
        """A request larger than one block must return NULL."""
        ptr = self.lib.zsp_stack_alloc(self.stk, _BLOCK_SIZE * 2, 1)
        assert ptr is None

    def test_write_to_allocated_memory(self):
        """Actually writing to allocated memory should not crash."""
        size = 64
        ptr = self.lib.zsp_stack_alloc(self.stk, size, 8)
        assert ptr is not None
        buf = (ctypes.c_uint8 * size).from_address(ptr)
        for i in range(size):
            buf[i] = i % 256
        for i in range(size):
            assert buf[i] == i % 256
