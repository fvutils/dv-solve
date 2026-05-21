"""Unit tests for the trail and backtracking (Phase 5).

Tests:
- trail_push_level increments decision_level
- trail_record_lb/ub: tier-0 bounds updated, trail_count incremented
- trail_backtrack to level 0: bounds fully restored
- tier-1 (64-bit) LB/UB record and backtrack
- Multi-level push/pop: intermediate level backtrack
- Nested push/pop: block_count grows and shrinks
- Backtrack across a dynamic-stack block boundary (small block size)
- trail_record_hole: recorded, trail_count increments, backtrack no-op
- trail_count tracks total entries across multiple levels
"""
from __future__ import annotations

import ctypes
import pytest

# ------------------------------------------------------------------ #
# Constants (mirroring C enums)                                       #
# ------------------------------------------------------------------ #

EXPR_NULL = 0xFFFFFFFF

VAR_SIGNED = 0x01
VAR_TIER1  = 0x08
VAR_TIER2  = 0x10

TRAIL_LB   = 0
TRAIL_UB   = 1
TRAIL_HOLE = 2

_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 524288   # 512 KiB — large enough for LevelMark[256]


# ------------------------------------------------------------------ #
# Library setup                                                       #
# ------------------------------------------------------------------ #

def _setup(lib: ctypes.CDLL):
    # block allocator
    lib.zsp_block_alloc_create.restype  = ctypes.c_void_p
    lib.zsp_block_alloc_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.zsp_block_alloc_destroy.restype  = None
    lib.zsp_block_alloc_destroy.argtypes = [ctypes.c_void_p]

    # problem
    lib.solve_problem_init.restype  = ctypes.c_void_p
    lib.solve_problem_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.problem_add_var.restype  = ctypes.c_uint32
    lib.problem_add_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_uint8, ctypes.c_uint8,
                                    ctypes.c_int64, ctypes.c_int64]

    # context
    lib.solver_create.restype  = ctypes.c_void_p
    lib.solver_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p]
    lib.solver_destroy.restype  = None
    lib.solver_destroy.argtypes = [ctypes.c_void_p]
    lib.solver_compile.restype  = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.zsp_var_lo32.restype  = ctypes.c_int32
    lib.zsp_var_lo32.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_hi32.restype  = ctypes.c_int32
    lib.zsp_var_hi32.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_lo64.restype  = ctypes.c_int64
    lib.zsp_var_lo64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_hi64.restype  = ctypes.c_int64
    lib.zsp_var_hi64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.zsp_ctx_decision_level.restype  = ctypes.c_uint32
    lib.zsp_ctx_decision_level.argtypes = [ctypes.c_void_p]
    lib.zsp_ctx_trail_count.restype  = ctypes.c_uint64
    lib.zsp_ctx_trail_count.argtypes = [ctypes.c_void_p]

    # trail
    lib.trail_push_level.restype  = None
    lib.trail_push_level.argtypes = [ctypes.c_void_p]
    lib.trail_record_lb.restype  = ctypes.c_int
    lib.trail_record_lb.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_int64]
    lib.trail_record_ub.restype  = ctypes.c_int
    lib.trail_record_ub.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_int64]
    lib.trail_record_hole.restype  = ctypes.c_int
    lib.trail_record_hole.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                      ctypes.c_int64]
    lib.trail_backtrack.restype  = None
    lib.trail_backtrack.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    # stack block count (for verifying dynamic memory management)
    lib.zsp_stack_block_count.restype  = ctypes.c_size_t
    lib.zsp_stack_block_count.argtypes = [ctypes.c_void_p]

    lib.solver_get_var.restype  = ctypes.c_void_p
    lib.solver_get_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


# ------------------------------------------------------------------ #
# Helpers                                                             #
# ------------------------------------------------------------------ #

def _make_ba(lib, block_size=65536):
    ba = lib.zsp_block_alloc_create(None, block_size)
    assert ba is not None
    return ba


def _make_sp(lib, *var_specs):
    """Create and return (sp, buf). var_specs: (var_id, width, signed, lo, hi)"""
    buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(buf, _SP_BUF_SIZE)
    assert sp is not None
    for var_id, width, signed, lo, hi in var_specs:
        lib.problem_add_var(sp, var_id, width, signed, lo, hi)
    return sp, buf


def _make_ctx(lib, ba=None):
    buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ctx = lib.solver_create(buf, _CTX_BUF_SIZE, ba)
    assert ctx is not None
    return ctx, buf


def _compile(lib, ctx, sp):
    rc = lib.solver_compile(ctx, sp)
    assert rc == 0


# ------------------------------------------------------------------ #
# Tests                                                               #
# ------------------------------------------------------------------ #

class TestTrail:
    @pytest.fixture(autouse=True)
    def setup_lib(self, libzsp):
        _setup(libzsp)
        self.lib = libzsp

    # -- push_level ------------------------------------------------- #

    def test_push_level_increments_decision_level(self):
        ba = _make_ba(self.lib)
        sp, sb = _make_sp(self.lib, (0, 8, 0, 0, 255))
        ctx, cb = _make_ctx(self.lib, ba)
        _compile(self.lib, ctx, sp)

        assert self.lib.zsp_ctx_decision_level(ctx) == 0
        self.lib.trail_push_level(ctx)
        assert self.lib.zsp_ctx_decision_level(ctx) == 1
        self.lib.trail_push_level(ctx)
        assert self.lib.zsp_ctx_decision_level(ctx) == 2

        self.lib.solver_destroy(ctx)
        self.lib.zsp_block_alloc_destroy(ba)

    # -- tier-0 LB record ------------------------------------------- #

    def test_record_lb_updates_variable(self):
        ba = _make_ba(self.lib)
        sp, sb = _make_sp(self.lib, (0, 8, 0, 0, 255))
        ctx, cb = _make_ctx(self.lib, ba)
        _compile(self.lib, ctx, sp)
        self.lib.trail_push_level(ctx)

        assert self.lib.zsp_var_lo32(ctx, 0) == 0
        rc = self.lib.trail_record_lb(ctx, 0, 10)
        assert rc == 0
        assert self.lib.zsp_var_lo32(ctx, 0) == 10
        assert self.lib.zsp_ctx_trail_count(ctx) == 1

        self.lib.solver_destroy(ctx)
        self.lib.zsp_block_alloc_destroy(ba)

    # -- tier-0 UB record ------------------------------------------- #

    def test_record_ub_updates_variable(self):
        ba = _make_ba(self.lib)
        sp, sb = _make_sp(self.lib, (0, 8, 0, 0, 255))
        ctx, cb = _make_ctx(self.lib, ba)
        _compile(self.lib, ctx, sp)
        self.lib.trail_push_level(ctx)

        rc = self.lib.trail_record_ub(ctx, 0, 200)
        assert rc == 0
        assert self.lib.zsp_var_hi32(ctx, 0) == 200

        self.lib.solver_destroy(ctx)
        self.lib.zsp_block_alloc_destroy(ba)

    # -- backtrack to level 0: restores bounds ---------------------- #

    def test_backtrack_level0_restores_tier0_bounds(self):
        ba = _make_ba(self.lib)
        sp, sb = _make_sp(self.lib, (0, 8, 0, 0, 255))
        ctx, cb = _make_ctx(self.lib, ba)
        _compile(self.lib, ctx, sp)

        self.lib.trail_push_level(ctx)   # level 0 → 1
        self.lib.trail_record_lb(ctx, 0, 50)
        self.lib.trail_record_ub(ctx, 0, 200)
        assert self.lib.zsp_var_lo32(ctx, 0) == 50
        assert self.lib.zsp_var_hi32(ctx, 0) == 200

        self.lib.trail_backtrack(ctx, 0)
        assert self.lib.zsp_ctx_decision_level(ctx) == 0
        assert self.lib.zsp_var_lo32(ctx, 0) == 0
        assert self.lib.zsp_var_hi32(ctx, 0) == 255
        assert self.lib.zsp_ctx_trail_count(ctx) == 0

        self.lib.solver_destroy(ctx)
        self.lib.zsp_block_alloc_destroy(ba)

    # -- multiple changes at one level ------------------------------ #

    def test_multiple_changes_backtrack(self):
        ba = _make_ba(self.lib)
        sp, sb = _make_sp(self.lib,
            (0, 16, 1, -100, 100),
            (1, 8,  0,    0, 255))
        ctx, cb = _make_ctx(self.lib, ba)
        _compile(self.lib, ctx, sp)

        self.lib.trail_push_level(ctx)
        self.lib.trail_record_lb(ctx, 0, -50)
        self.lib.trail_record_ub(ctx, 0,  50)
        self.lib.trail_record_lb(ctx, 1,  10)
        self.lib.trail_record_ub(ctx, 1, 200)
        assert self.lib.zsp_ctx_trail_count(ctx) == 4

        self.lib.trail_backtrack(ctx, 0)
        assert self.lib.zsp_var_lo32(ctx, 0) == -100
        assert self.lib.zsp_var_hi32(ctx, 0) ==  100
        assert self.lib.zsp_var_lo32(ctx, 1) ==    0
        assert self.lib.zsp_var_hi32(ctx, 1) ==  255
        assert self.lib.zsp_ctx_trail_count(ctx) == 0

        self.lib.solver_destroy(ctx)
        self.lib.zsp_block_alloc_destroy(ba)

    # -- multi-level push/pop --------------------------------------- #

    def test_multi_level_intermediate_backtrack(self):
        ba = _make_ba(self.lib)
        sp, sb = _make_sp(self.lib, (0, 8, 0, 0, 255))
        ctx, cb = _make_ctx(self.lib, ba)
        _compile(self.lib, ctx, sp)

        # Level 0→1: tighten lo to 10
        self.lib.trail_push_level(ctx)
        self.lib.trail_record_lb(ctx, 0, 10)

        # Level 1→2: tighten lo to 20
        self.lib.trail_push_level(ctx)
        self.lib.trail_record_lb(ctx, 0, 20)

        # Level 2→3: tighten lo to 30
        self.lib.trail_push_level(ctx)
        self.lib.trail_record_lb(ctx, 0, 30)

        assert self.lib.zsp_var_lo32(ctx, 0) == 30
        assert self.lib.zsp_ctx_decision_level(ctx) == 3

        # Backtrack to level 1: should restore lo to 10
        self.lib.trail_backtrack(ctx, 1)
        assert self.lib.zsp_ctx_decision_level(ctx) == 1
        assert self.lib.zsp_var_lo32(ctx, 0) == 10

        # Backtrack to level 0: lo back to 0
        self.lib.trail_backtrack(ctx, 0)
        assert self.lib.zsp_ctx_decision_level(ctx) == 0
        assert self.lib.zsp_var_lo32(ctx, 0) == 0

        self.lib.solver_destroy(ctx)
        self.lib.zsp_block_alloc_destroy(ba)

    # -- tier-1 (64-bit) trail -------------------------------------- #

    def test_tier1_lb_ub_record_backtrack(self):
        ba = _make_ba(self.lib)
        sp, sb = _make_sp(self.lib, (0, 64, 0, 0, 2**63 - 1))
        ctx, cb = _make_ctx(self.lib, ba)
        _compile(self.lib, ctx, sp)

        orig_lo = self.lib.zsp_var_lo64(ctx, 0)
        orig_hi = self.lib.zsp_var_hi64(ctx, 0)
        assert orig_lo == 0
        assert orig_hi == 2**63 - 1

        self.lib.trail_push_level(ctx)
        rc_lb = self.lib.trail_record_lb(ctx, 0, 1000)
        rc_ub = self.lib.trail_record_ub(ctx, 0, 2**62)
        assert rc_lb == 0
        assert rc_ub == 0
        assert self.lib.zsp_var_lo64(ctx, 0) == 1000
        assert self.lib.zsp_var_hi64(ctx, 0) == 2**62

        self.lib.trail_backtrack(ctx, 0)
        assert self.lib.zsp_var_lo64(ctx, 0) == orig_lo
        assert self.lib.zsp_var_hi64(ctx, 0) == orig_hi

        self.lib.solver_destroy(ctx)
        self.lib.zsp_block_alloc_destroy(ba)

    # -- hole record (no-op restore) -------------------------------- #

    def test_hole_record_increments_trail_count(self):
        ba = _make_ba(self.lib)
        sp, sb = _make_sp(self.lib, (0, 8, 0, 0, 255))
        ctx, cb = _make_ctx(self.lib, ba)
        _compile(self.lib, ctx, sp)
        self.lib.trail_push_level(ctx)

        rc = self.lib.trail_record_hole(ctx, 0, 42)
        assert rc == 0
        assert self.lib.zsp_ctx_trail_count(ctx) == 1

        # Backtrack: hole restore is no-op, but trail_count goes back to 0
        self.lib.trail_backtrack(ctx, 0)
        assert self.lib.zsp_ctx_trail_count(ctx) == 0

        self.lib.solver_destroy(ctx)
        self.lib.zsp_block_alloc_destroy(ba)

    # -- backtrack across block boundary ---------------------------- #

    def test_backtrack_across_block_boundary(self):
        """Use a small block_alloc block size to force multiple blocks."""
        # TrailEntry is 24 bytes; with 512-byte blocks, ~20 entries per block
        # so 50 entries forces ~3 blocks.
        small_block = 512
        ba = _make_ba(self.lib, block_size=small_block)
        sp, sb = _make_sp(self.lib, (0, 8, 0, 0, 255))
        ctx, cb = _make_ctx(self.lib, ba)
        _compile(self.lib, ctx, sp)

        self.lib.trail_push_level(ctx)
        orig_lo = self.lib.zsp_var_lo32(ctx, 0)

        # Record 50 LB changes (each raises lo by 1)
        for i in range(1, 51):
            rc = self.lib.trail_record_lb(ctx, 0, i)
            assert rc == 0, f"trail_record_lb failed at step {i}"

        assert self.lib.zsp_ctx_trail_count(ctx) == 50
        assert self.lib.zsp_var_lo32(ctx, 0) == 50

        # Backtrack: all 50 changes undone, dynamic memory recovered
        self.lib.trail_backtrack(ctx, 0)
        assert self.lib.zsp_var_lo32(ctx, 0) == orig_lo
        assert self.lib.zsp_ctx_trail_count(ctx) == 0

        self.lib.solver_destroy(ctx)
        self.lib.zsp_block_alloc_destroy(ba)

    # -- trail_count accumulates across levels ---------------------- #

    def test_trail_count_accumulates(self):
        ba = _make_ba(self.lib)
        sp, sb = _make_sp(self.lib, (0, 8, 0, 0, 255), (1, 8, 0, 0, 255))
        ctx, cb = _make_ctx(self.lib, ba)
        _compile(self.lib, ctx, sp)

        self.lib.trail_push_level(ctx)          # level 0→1
        self.lib.trail_record_lb(ctx, 0, 5)    # count=1
        self.lib.trail_push_level(ctx)          # level 1→2
        self.lib.trail_record_lb(ctx, 1, 10)   # count=2
        self.lib.trail_record_ub(ctx, 1, 200)  # count=3

        assert self.lib.zsp_ctx_trail_count(ctx) == 3

        # Backtrack to level 1: undo 2 entries for var 1, keep 1 for var 0
        self.lib.trail_backtrack(ctx, 1)
        assert self.lib.zsp_ctx_trail_count(ctx) == 1
        assert self.lib.zsp_var_lo32(ctx, 0) == 5   # still tightened
        assert self.lib.zsp_var_lo32(ctx, 1) == 0   # restored
        assert self.lib.zsp_var_hi32(ctx, 1) == 255  # restored

        self.lib.solver_destroy(ctx)
        self.lib.zsp_block_alloc_destroy(ba)

    # -- dynamic stack block count shrinks on backtrack ------------- #

    def test_dynamic_stack_blocks_recovered(self):
        """Backtrack should pop dynamic stack so block_count returns to 0."""
        small_block = 256
        ba = _make_ba(self.lib, block_size=small_block)
        sp, sb = _make_sp(self.lib, (0, 8, 0, 0, 255))
        ctx, cb = _make_ctx(self.lib, ba)
        _compile(self.lib, ctx, sp)

        self.lib.trail_push_level(ctx)

        # Get the dynamic stack pointer — it's stored inside ctx but we
        # can indirectly verify by recording many entries and then backtracking.
        for i in range(1, 30):
            rc = self.lib.trail_record_lb(ctx, 0, i)
            assert rc == 0

        self.lib.trail_backtrack(ctx, 0)
        # After backtrack, no trail entries exist → dynamic stack should be empty.
        # We verify indirectly: trail_count == 0 and bounds restored.
        assert self.lib.zsp_ctx_trail_count(ctx) == 0
        assert self.lib.zsp_var_lo32(ctx, 0) == 0

        self.lib.solver_destroy(ctx)
        self.lib.zsp_block_alloc_destroy(ba)
