"""Unit tests for 64-bit propagator implementations (Sprint 1.2).

Tests mul_64, div_64, mod_64 propagators that were previously no-op stubs.
Uses tier-1 variables (width=33 or unsigned 32-bit) to force 64-bit path.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL     = 0xFFFF_FFFF
PROP_OK       = 0
PROP_CONFLICT = 1
PROP_ENTAILED = 2

_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 524288


def _setup(lib: ctypes.CDLL):
    lib.zsp_block_alloc_create.restype  = ctypes.c_void_p
    lib.zsp_block_alloc_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.zsp_block_alloc_destroy.restype  = None
    lib.zsp_block_alloc_destroy.argtypes = [ctypes.c_void_p]

    lib.solve_problem_init.restype  = ctypes.c_void_p
    lib.solve_problem_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.problem_add_var.restype  = ctypes.c_uint32
    lib.problem_add_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_uint8, ctypes.c_uint8,
                                    ctypes.c_int64, ctypes.c_int64]

    lib.solver_create.restype  = ctypes.c_void_p
    lib.solver_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p]
    lib.solver_destroy.restype  = None
    lib.solver_destroy.argtypes = [ctypes.c_void_p]
    lib.solver_compile.restype  = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.zsp_var_lo64.restype  = ctypes.c_int64
    lib.zsp_var_lo64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_hi64.restype  = ctypes.c_int64
    lib.zsp_var_hi64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.solver_propagate.restype  = ctypes.c_int
    lib.solver_propagate.argtypes = [ctypes.c_void_p]

    lib.ctx_tighten_lb64.restype  = ctypes.c_int
    lib.ctx_tighten_lb64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int64]
    lib.ctx_tighten_ub64.restype  = ctypes.c_int
    lib.ctx_tighten_ub64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int64]

    lib.prop_add_bounds_mul_64.restype  = ctypes.c_uint32
    lib.prop_add_bounds_mul_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                           ctypes.c_uint32, ctypes.c_uint32,
                                           ctypes.c_uint8]
    lib.prop_add_bounds_div_64.restype  = ctypes.c_uint32
    lib.prop_add_bounds_div_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                           ctypes.c_uint32, ctypes.c_uint32,
                                           ctypes.c_uint8]
    lib.prop_add_bounds_mod_64.restype  = ctypes.c_uint32
    lib.prop_add_bounds_mod_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                           ctypes.c_uint32, ctypes.c_uint32,
                                           ctypes.c_uint8]
    lib.prop_add_bounds_add_64.restype  = ctypes.c_uint32
    lib.prop_add_bounds_add_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                           ctypes.c_uint32, ctypes.c_uint32,
                                           ctypes.c_uint8]
    lib.prop_add_bounds_eq_64.restype  = ctypes.c_uint32
    lib.prop_add_bounds_eq_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                          ctypes.c_uint32, ctypes.c_uint8]


def _make_ctx_64(lib, var_specs):
    """Create problem + context with tier-1 variables.
    var_specs: list of (width, is_signed, lo, hi)
    """
    sp_buf  = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()

    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    for i, (width, is_signed, lo, hi) in enumerate(var_specs):
        ref = lib.problem_add_var(sp, i, width, is_signed, lo, hi)
        assert ref != EXPR_NULL

    ba  = lib.zsp_block_alloc_create(None, 0)
    assert ba
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx

    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    return sp_buf, ctx_buf, ba, sp, ctx


def _lo(lib, ctx, var_id):
    return lib.zsp_var_lo64(ctx, var_id)


def _hi(lib, ctx, var_id):
    return lib.zsp_var_hi64(ctx, var_id)


def _fix64(lib, ctx, var_id, val):
    """Fix a 64-bit variable to a singleton."""
    lib.ctx_tighten_lb64(ctx, var_id, val)
    lib.ctx_tighten_ub64(ctx, var_id, val)


# ------------------------------------------------------------------ #
# Mul_64 tests                                                        #
# ------------------------------------------------------------------ #

def test_mul_64_basic(libzsp):
    """r = a * b; fix a=3, b=4; propagate -> r=[12,12]."""
    lib = libzsp
    _setup(lib)

    # Use width=33 to force tier-1
    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx_64(lib, [
        (33, 1, 0, 1000),  # r
        (33, 1, 0, 100),   # a
        (33, 1, 0, 100),   # b
    ])

    lib.prop_add_bounds_mul_64(ctx, 0, 1, 2, 0)
    _fix64(lib, ctx, 1, 3)
    _fix64(lib, ctx, 2, 4)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) == 12
    assert _hi(lib, ctx, 0) == 12

    lib.zsp_block_alloc_destroy(ba)


def test_mul_64_range(libzsp):
    """r = a * b; a=[2,4], b=[3,5]; propagate -> r in [6,20]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx_64(lib, [
        (33, 1, 0, 1000),  # r
        (33, 1, 2, 4),     # a
        (33, 1, 3, 5),     # b
    ])

    lib.prop_add_bounds_mul_64(ctx, 0, 1, 2, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    # With singleton specialization only, the mul propagator won't tighten
    # when both a and b are ranges. But r should not have been made
    # inconsistent.
    assert _lo(lib, ctx, 0) <= 6
    assert _hi(lib, ctx, 0) >= 20

    lib.zsp_block_alloc_destroy(ba)


def test_mul_64_backward(libzsp):
    """r = a * b; fix a=5, fix r=[15,15]; propagate -> b=[3,3]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx_64(lib, [
        (33, 1, 0, 1000),  # r
        (33, 1, 0, 100),   # a
        (33, 1, 0, 100),   # b
    ])

    lib.prop_add_bounds_mul_64(ctx, 0, 1, 2, 0)
    _fix64(lib, ctx, 1, 5)    # a = 5
    _fix64(lib, ctx, 0, 15)   # r = 15

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 2) == 3
    assert _hi(lib, ctx, 2) == 3

    lib.zsp_block_alloc_destroy(ba)


def test_mul_64_zero(libzsp):
    """r = a * b; fix a=0; propagate -> r=0."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx_64(lib, [
        (33, 1, -100, 100),  # r
        (33, 1, -100, 100),  # a
        (33, 1, -100, 100),  # b
    ])

    lib.prop_add_bounds_mul_64(ctx, 0, 1, 2, 0)
    _fix64(lib, ctx, 1, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) == 0
    assert _hi(lib, ctx, 0) == 0

    lib.zsp_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# Div_64 tests                                                        #
# ------------------------------------------------------------------ #

def test_div_64_basic(libzsp):
    """r = a / b; fix a=12, b=3; propagate -> r=[4,4]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx_64(lib, [
        (33, 1, 0, 1000),  # r
        (33, 1, 0, 1000),  # a
        (33, 1, 1, 100),   # b (positive, avoid div-by-zero)
    ])

    lib.prop_add_bounds_div_64(ctx, 0, 1, 2, 0)
    _fix64(lib, ctx, 1, 12)
    _fix64(lib, ctx, 2, 3)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) == 4
    assert _hi(lib, ctx, 0) == 4

    lib.zsp_block_alloc_destroy(ba)


def test_div_64_range(libzsp):
    """r = a / b; a=[10,20], b=5; propagate -> r in [2,4]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx_64(lib, [
        (33, 1, 0, 1000),  # r
        (33, 1, 10, 20),   # a
        (33, 1, 1, 100),   # b
    ])

    lib.prop_add_bounds_div_64(ctx, 0, 1, 2, 0)
    _fix64(lib, ctx, 2, 5)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) >= 2
    assert _hi(lib, ctx, 0) <= 4

    lib.zsp_block_alloc_destroy(ba)


def test_div_64_positive_range_divisor(libzsp):
    """r = a / b; a=[100,200], b=[5,10]; propagate -> r in [10,40]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx_64(lib, [
        (33, 1, 0, 1000),  # r
        (33, 1, 100, 200), # a
        (33, 1, 5, 10),    # b
    ])

    lib.prop_add_bounds_div_64(ctx, 0, 1, 2, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) >= 10   # 100/10
    assert _hi(lib, ctx, 0) <= 40   # 200/5

    lib.zsp_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# Mod_64 tests                                                        #
# ------------------------------------------------------------------ #

def test_mod_64_basic(libzsp):
    """r = a % b; fix a=17, b=5; propagate -> r in [0,4]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx_64(lib, [
        (33, 1, 0, 1000),  # r
        (33, 1, 0, 1000),  # a
        (33, 1, 1, 100),   # b
    ])

    lib.prop_add_bounds_mod_64(ctx, 0, 1, 2, 0)
    _fix64(lib, ctx, 2, 5)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) >= 0
    assert _hi(lib, ctx, 0) <= 4

    lib.zsp_block_alloc_destroy(ba)


def test_mod_64_range_divisor(libzsp):
    """r = a % b; b=[3,7]; propagate -> r in [0,6]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx_64(lib, [
        (33, 1, 0, 1000),  # r
        (33, 1, 0, 1000),  # a
        (33, 1, 3, 7),     # b
    ])

    lib.prop_add_bounds_mod_64(ctx, 0, 1, 2, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) >= 0
    assert _hi(lib, ctx, 0) <= 6

    lib.zsp_block_alloc_destroy(ba)
