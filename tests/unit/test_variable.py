"""Unit tests for the Variable model and solver_compile (Phase 4).

Tests:
- solver_create: basic lifecycle
- solver_compile with 0 variables: no-op
- Tier-0 (≤32-bit) variable: correct lo/hi via var_lo32/var_hi32
- Tier-0 signed variable: negative bounds stored correctly
- Tier-1 (33–64-bit) variable: correct lo/hi via var_lo64/var_hi64
- Tier-2 (>64-bit) variable: created, correct flags
- var_lo64 on tier-0 widens correctly (signed & unsigned)
- Static pool grows after compile
- Multiple variables with different tiers in one problem
- solver_get_var returns correct Variable fields (width, flags)
"""
from __future__ import annotations

import ctypes
import pytest

# ------------------------------------------------------------------ #
# ctypes mirrors                                                       #
# ------------------------------------------------------------------ #

# Variable flags
VAR_SIGNED = 0x01
VAR_RANDC  = 0x02
VAR_STATE  = 0x04
VAR_TIER1  = 0x08
VAR_TIER2  = 0x10

EXPR_NULL = 0xFFFFFFFF

# BinOp (needed to build a SolveProblem for compile tests)
BIN_LT = 12


class Variable(ctypes.Structure):
    _fields_ = [
        ("lo",           ctypes.c_int32),
        ("hi",           ctypes.c_int32),
        ("holes_offset", ctypes.c_uint32),
        ("width",        ctypes.c_uint16),
        ("flags",        ctypes.c_uint8),
        ("_pad",         ctypes.c_uint8),
    ]


# ------------------------------------------------------------------ #
# Library setup                                                       #
# ------------------------------------------------------------------ #

_SP_BUF_SIZE  = 65536   # problem buffer
_CTX_BUF_SIZE = 131072  # context static pool (128 KiB)


def _setup(lib: ctypes.CDLL):
    # SolveProblem builders (already tested in test_problem.py)
    lib.solve_problem_init.restype  = ctypes.c_void_p
    lib.solve_problem_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    lib.problem_add_var.restype  = ctypes.c_uint32
    lib.problem_add_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_uint8, ctypes.c_uint8,
                                    ctypes.c_int64, ctypes.c_int64]
    # SolveCtx
    lib.solver_create.restype  = ctypes.c_void_p
    lib.solver_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p]

    lib.solver_destroy.restype  = None
    lib.solver_destroy.argtypes = [ctypes.c_void_p]

    lib.solver_compile.restype  = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.solver_get_var.restype  = ctypes.c_void_p
    lib.solver_get_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.zsp_var_lo32.restype  = ctypes.c_int32
    lib.zsp_var_lo32.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.zsp_var_hi32.restype  = ctypes.c_int32
    lib.zsp_var_hi32.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.zsp_var_lo64.restype  = ctypes.c_int64
    lib.zsp_var_lo64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.zsp_var_hi64.restype  = ctypes.c_int64
    lib.zsp_var_hi64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.zsp_ctx_pool_used.restype  = ctypes.c_uint32
    lib.zsp_ctx_pool_used.argtypes = [ctypes.c_void_p]


# ------------------------------------------------------------------ #
# Helpers                                                             #
# ------------------------------------------------------------------ #

def _make_sp(lib):
    buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(buf, _SP_BUF_SIZE)
    assert sp is not None
    return sp, buf


def _make_ctx(lib, block_alloc=None):
    buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ctx = lib.solver_create(buf, _CTX_BUF_SIZE, block_alloc)
    assert ctx is not None
    return ctx, buf


def _var(lib, ctx, var_id):
    ptr = lib.solver_get_var(ctx, var_id)
    assert ptr is not None
    return Variable.from_address(ptr)


# ------------------------------------------------------------------ #
# Tests                                                               #
# ------------------------------------------------------------------ #

class TestVariable:
    @pytest.fixture(autouse=True)
    def setup_lib(self, libzsp):
        _setup(libzsp)
        self.lib = libzsp

    # -- lifecycle -------------------------------------------------- #

    def test_solver_create_returns_non_null(self):
        ctx, buf = _make_ctx(self.lib)
        self.lib.solver_destroy(ctx)

    def test_solver_create_too_small(self):
        tiny = (ctypes.c_uint8 * 32)()
        ctx = self.lib.solver_create(tiny, 32, None)
        assert ctx is None

    # -- zero-variable compile -------------------------------------- #

    def test_compile_zero_vars(self):
        sp, sp_buf = _make_sp(self.lib)
        ctx, ctx_buf = _make_ctx(self.lib)
        rc = self.lib.solver_compile(ctx, sp)
        assert rc == 0
        self.lib.solver_destroy(ctx)

    # -- tier-0: unsigned 8-bit ------------------------------------ #

    def test_tier0_unsigned_8bit(self):
        sp, sp_buf = _make_sp(self.lib)
        self.lib.problem_add_var(sp, 0, 8, 0, 0, 255)  # u8 [0, 255]
        ctx, ctx_buf = _make_ctx(self.lib)
        assert self.lib.solver_compile(ctx, sp) == 0

        v = _var(self.lib, ctx, 0)
        assert v.width == 8
        assert not (v.flags & VAR_TIER1)
        assert not (v.flags & VAR_TIER2)
        assert not (v.flags & VAR_SIGNED)

        assert self.lib.zsp_var_lo32(ctx, 0) == 0
        assert self.lib.zsp_var_hi32(ctx, 0) == 255
        self.lib.solver_destroy(ctx)

    # -- tier-0: signed 32-bit ------------------------------------- #

    def test_tier0_signed_32bit(self):
        sp, sp_buf = _make_sp(self.lib)
        self.lib.problem_add_var(sp, 0, 32, 1, -2**31, 2**31 - 1)
        ctx, ctx_buf = _make_ctx(self.lib)
        assert self.lib.solver_compile(ctx, sp) == 0

        v = _var(self.lib, ctx, 0)
        assert v.width == 32
        assert v.flags & VAR_SIGNED
        assert not (v.flags & VAR_TIER1)

        assert self.lib.zsp_var_lo32(ctx, 0) == -(2**31)
        assert self.lib.zsp_var_hi32(ctx, 0) == 2**31 - 1
        self.lib.solver_destroy(ctx)

    # -- tier-0: negative bounds ----------------------------------- #

    def test_tier0_negative_bounds(self):
        sp, sp_buf = _make_sp(self.lib)
        self.lib.problem_add_var(sp, 0, 16, 1, -100, 100)  # s16 [-100, 100]
        ctx, ctx_buf = _make_ctx(self.lib)
        assert self.lib.solver_compile(ctx, sp) == 0

        assert self.lib.zsp_var_lo32(ctx, 0) == -100
        assert self.lib.zsp_var_hi32(ctx, 0) ==  100
        self.lib.solver_destroy(ctx)

    # -- tier-0: var_lo64 widens correctly  ------------------------ #

    def test_unsigned_32_lo64(self):
        """var_lo64 on an unsigned 32-bit var (promoted to tier-1)."""
        sp, sp_buf = _make_sp(self.lib)
        self.lib.problem_add_var(sp, 0, 32, 0, 0, 2**32 - 1)
        ctx, ctx_buf = _make_ctx(self.lib)
        assert self.lib.solver_compile(ctx, sp) == 0

        # lo is 0 → zero-extended should still be 0
        assert self.lib.zsp_var_lo64(ctx, 0) == 0
        self.lib.solver_destroy(ctx)

    def test_tier0_lo64_signed_widening(self):
        """var_lo64 on a signed tier-0 var should sign-extend."""
        sp, sp_buf = _make_sp(self.lib)
        self.lib.problem_add_var(sp, 0, 8, 1, -128, 127)
        ctx, ctx_buf = _make_ctx(self.lib)
        assert self.lib.solver_compile(ctx, sp) == 0

        assert self.lib.zsp_var_lo64(ctx, 0) == -128
        assert self.lib.zsp_var_hi64(ctx, 0) ==  127
        self.lib.solver_destroy(ctx)

    # -- tier-1: 64-bit -------------------------------------------- #

    def test_tier1_unsigned_64bit(self):
        """64-bit unsigned variable: bounds in WideBounds64."""
        sp, sp_buf = _make_sp(self.lib)
        lo = 0
        hi = 2**63 - 1   # max positive int64_t (stored as signed int64)
        self.lib.problem_add_var(sp, 0, 64, 0, lo, hi)
        ctx, ctx_buf = _make_ctx(self.lib)
        assert self.lib.solver_compile(ctx, sp) == 0

        v = _var(self.lib, ctx, 0)
        assert v.width == 64
        assert v.flags & VAR_TIER1
        assert not (v.flags & VAR_TIER2)
        assert v.holes_offset != 0

        assert self.lib.zsp_var_lo64(ctx, 0) == lo
        assert self.lib.zsp_var_hi64(ctx, 0) == hi
        self.lib.solver_destroy(ctx)

    def test_tier1_signed_64bit(self):
        sp, sp_buf = _make_sp(self.lib)
        lo = -(2**63)
        hi = 2**63 - 1
        self.lib.problem_add_var(sp, 0, 64, 1, lo, hi)
        ctx, ctx_buf = _make_ctx(self.lib)
        assert self.lib.solver_compile(ctx, sp) == 0

        v = _var(self.lib, ctx, 0)
        assert v.flags & VAR_TIER1
        assert v.flags & VAR_SIGNED

        assert self.lib.zsp_var_lo64(ctx, 0) == lo
        assert self.lib.zsp_var_hi64(ctx, 0) == hi
        self.lib.solver_destroy(ctx)

    def test_tier1_33bit_boundary(self):
        """33-bit variable is the smallest tier-1."""
        sp, sp_buf = _make_sp(self.lib)
        self.lib.problem_add_var(sp, 0, 33, 0, 0, 2**33 - 1)
        ctx, ctx_buf = _make_ctx(self.lib)
        assert self.lib.solver_compile(ctx, sp) == 0

        v = _var(self.lib, ctx, 0)
        assert v.width == 33
        assert v.flags & VAR_TIER1
        self.lib.solver_destroy(ctx)

    # -- tier-2: > 64-bit ------------------------------------------ #

    def test_tier2_128bit(self):
        """128-bit variable is a tier-2."""
        sp, sp_buf = _make_sp(self.lib)
        self.lib.problem_add_var(sp, 0, 128, 0, 0, 1000)
        ctx, ctx_buf = _make_ctx(self.lib)
        assert self.lib.solver_compile(ctx, sp) == 0

        v = _var(self.lib, ctx, 0)
        assert v.width == 128
        assert v.flags & VAR_TIER2
        assert not (v.flags & VAR_TIER1)
        assert v.holes_offset != 0
        self.lib.solver_destroy(ctx)

    # -- multiple variables of mixed tiers ------------------------- #

    def test_mixed_tiers(self):
        sp, sp_buf = _make_sp(self.lib)
        self.lib.problem_add_var(sp, 0,   8, 0,       0,     255)  # tier-0
        self.lib.problem_add_var(sp, 1,  32, 1, -2**31,  2**31-1)  # tier-0
        self.lib.problem_add_var(sp, 2,  64, 0,       0, 2**63-1)  # tier-1
        self.lib.problem_add_var(sp, 3, 128, 0,       0,    1000)  # tier-2

        ctx, ctx_buf = _make_ctx(self.lib)
        assert self.lib.solver_compile(ctx, sp) == 0

        v0 = _var(self.lib, ctx, 0)
        v1 = _var(self.lib, ctx, 1)
        v2 = _var(self.lib, ctx, 2)
        v3 = _var(self.lib, ctx, 3)

        assert not (v0.flags & (VAR_TIER1 | VAR_TIER2))
        assert not (v1.flags & (VAR_TIER1 | VAR_TIER2))
        assert v2.flags & VAR_TIER1
        assert v3.flags & VAR_TIER2

        assert self.lib.zsp_var_lo32(ctx, 0) == 0
        assert self.lib.zsp_var_hi32(ctx, 0) == 255
        assert self.lib.zsp_var_lo32(ctx, 1) == -(2**31)
        assert self.lib.zsp_var_lo64(ctx, 2) == 0
        assert self.lib.zsp_var_hi64(ctx, 2) == 2**63 - 1
        self.lib.solver_destroy(ctx)

    # -- static pool usage ----------------------------------------- #

    def test_pool_used_grows_after_compile(self):
        """Compiling a non-empty problem should consume static pool bytes."""
        sp, sp_buf = _make_sp(self.lib)
        self.lib.problem_add_var(sp, 0, 8, 0, 0, 255)
        ctx, ctx_buf = _make_ctx(self.lib)

        used_before = self.lib.zsp_ctx_pool_used(ctx)
        assert self.lib.solver_compile(ctx, sp) == 0
        used_after = self.lib.zsp_ctx_pool_used(ctx)

        assert used_after > used_before, (
            f"Pool should have grown: {used_before} → {used_after}"
        )
        self.lib.solver_destroy(ctx)

    def test_pool_used_larger_for_tier1(self):
        """Tier-1 variables need extra pool space for WideBounds64."""
        sp0, buf0 = _make_sp(self.lib)
        sp1, buf1 = _make_sp(self.lib)
        self.lib.problem_add_var(sp0, 0, 32, 1, 0, 100)  # tier-0 (signed)
        self.lib.problem_add_var(sp1, 0, 64, 0, 0, 100)  # tier-1

        ctx0, cbuf0 = _make_ctx(self.lib)
        ctx1, cbuf1 = _make_ctx(self.lib)
        assert self.lib.solver_compile(ctx0, sp0) == 0
        assert self.lib.solver_compile(ctx1, sp1) == 0

        used0 = self.lib.zsp_ctx_pool_used(ctx0)
        used1 = self.lib.zsp_ctx_pool_used(ctx1)

        assert used1 > used0, (
            f"Tier-1 should use more pool than tier-0: {used0} vs {used1}"
        )
        self.lib.solver_destroy(ctx0)
        self.lib.solver_destroy(ctx1)
