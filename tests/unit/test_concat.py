"""Unit tests for concat propagators (Sprint 4.2).

Tests bit concatenation: r = {hi, lo}.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL     = 0xFFFF_FFFF
PROP_OK       = 0
PROP_CONFLICT = 1
SOLVE_OK      = 0

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
    lib.problem_add_constraint.restype  = ctypes.c_uint32
    lib.problem_add_constraint.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.expr_var.restype  = ctypes.c_uint32
    lib.expr_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.expr_binary.restype  = ctypes.c_uint32
    lib.expr_binary.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                ctypes.c_uint32, ctypes.c_uint32]
    lib.expr_concat.restype  = ctypes.c_uint32
    lib.expr_concat.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                ctypes.c_uint32, ctypes.c_uint8]

    lib.solver_create.restype  = ctypes.c_void_p
    lib.solver_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p]
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

    lib.prop_add_bounds_concat_64.restype  = ctypes.c_uint32
    lib.prop_add_bounds_concat_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                              ctypes.c_uint32, ctypes.c_uint32,
                                              ctypes.c_uint8, ctypes.c_uint8]

    class SolveOpts(ctypes.Structure):
        _fields_ = [
            ("seed",           ctypes.c_uint64),
            ("max_conflicts",  ctypes.c_uint32),
            ("max_restarts",   ctypes.c_uint32),
            ("use_phase_save", ctypes.c_uint8),
            ("_pad",           ctypes.c_uint8 * 3),
            ("max_shave_iters", ctypes.c_uint32),
        ]
    lib._SolveOpts = SolveOpts
    lib.solver_solve.restype  = ctypes.c_int
    lib.solver_solve.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.solver_get_value.restype  = ctypes.c_int64
    lib.solver_get_value.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


BIN_EQ  = 10
BIN_NEQ = 11


def _make_ctx(lib, var_specs):
    sp_buf  = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp
    for i, (width, is_signed, lo, hi) in enumerate(var_specs):
        ref = lib.problem_add_var(sp, i, width, is_signed, lo, hi)
        assert ref != EXPR_NULL
    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0
    return sp_buf, ctx_buf, ba, sp, ctx


def _lo(lib, ctx, i):
    return lib.zsp_var_lo64(ctx, i)


def _hi(lib, ctx, i):
    return lib.zsp_var_hi64(ctx, i)


def _fix(lib, ctx, i, v):
    lib.ctx_tighten_lb64(ctx, i, v)
    lib.ctx_tighten_ub64(ctx, i, v)


# ------------------------------------------------------------------ #
# Direct propagator tests                                             #
# ------------------------------------------------------------------ #

def test_concat_basic(libzsp):
    """r = {a, b}; a=8-bit, b=8-bit, r=16-bit.
    Fix a=0xAB, b=0xCD. Verify r=0xABCD."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 0, 0, 0xFFFF),  # 0: r (16-bit range)
        (33, 0, 0, 0xFF),    # 1: hi (a)
        (33, 0, 0, 0xFF),    # 2: lo (b)
    ])

    lib.prop_add_bounds_concat_64(ctx, 0, 1, 2, 8, 0)

    _fix(lib, ctx, 1, 0xAB)
    _fix(lib, ctx, 2, 0xCD)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) == 0xABCD
    assert _hi(lib, ctx, 0) == 0xABCD

    lib.zsp_block_alloc_destroy(ba)


def test_concat_backward(libzsp):
    """r = {a, b}; fix r=0x1234 -> a=0x12, b=0x34."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 0, 0, 0xFFFF),  # 0: r
        (33, 0, 0, 0xFF),    # 1: hi
        (33, 0, 0, 0xFF),    # 2: lo
    ])

    lib.prop_add_bounds_concat_64(ctx, 0, 1, 2, 8, 0)

    _fix(lib, ctx, 0, 0x1234)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 1) == 0x12
    assert _hi(lib, ctx, 1) == 0x12
    assert _lo(lib, ctx, 2) == 0x34
    assert _hi(lib, ctx, 2) == 0x34

    lib.zsp_block_alloc_destroy(ba)


def test_concat_forward_bounds(libzsp):
    """r = {a, b}; a=[1,3], b=[0,255] -> r in [0x100, 0x3FF]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 0, 0, 0xFFFF),  # 0: r
        (33, 0, 1, 3),       # 1: hi
        (33, 0, 0, 0xFF),    # 2: lo
    ])

    lib.prop_add_bounds_concat_64(ctx, 0, 1, 2, 8, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) >= 0x100   # (1 << 8) | 0
    assert _hi(lib, ctx, 0) <= 0x3FF   # (3 << 8) | 0xFF

    lib.zsp_block_alloc_destroy(ba)


def test_concat_lo_constrained(libzsp):
    """lo_width=8, so lo must be in [0, 255]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 0, 0, 0xFFFF),  # 0: r
        (33, 0, 0, 0xFF),    # 1: hi
        (33, 0, 0, 0xFFF),   # 2: lo (initially too wide)
    ])

    lib.prop_add_bounds_concat_64(ctx, 0, 1, 2, 8, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    # lo should be tightened to [0, 255]
    assert _hi(lib, ctx, 2) <= 0xFF

    lib.zsp_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# Compilation / end-to-end tests                                      #
# ------------------------------------------------------------------ #

def test_concat_compile_e2e(libzsp):
    """r == concat(a, b) via problem builder, then solve."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 33, 0, 0, 0xFFFF)  # r (16-bit range)
    lib.problem_add_var(sp, 1, 8, 0, 0, 0xFF)     # hi
    lib.problem_add_var(sp, 2, 8, 0, 0, 0xFF)     # lo

    v_r  = lib.expr_var(sp, 0)
    v_hi = lib.expr_var(sp, 1)
    v_lo = lib.expr_var(sp, 2)
    cat_e = lib.expr_concat(sp, v_hi, v_lo, 8)
    eq_e  = lib.expr_binary(sp, BIN_EQ, v_r, cat_e)
    lib.problem_add_constraint(sp, eq_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0xBEEF)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    r  = lib.solver_get_value(ctx, 0)
    hi = lib.solver_get_value(ctx, 1)
    lo = lib.solver_get_value(ctx, 2)
    expected = (hi << 8) | lo
    assert r == expected, f"r={r:#x} != {{hi,lo}} = {expected:#x} (hi={hi:#x}, lo={lo:#x})"

    lib.zsp_block_alloc_destroy(ba)


def test_concat_compile_fixed(libzsp):
    """r == concat(0xAB, 0xCD) with fixed operands. Verify r=0xABCD."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 33, 0, 0, 0xFFFF)       # r
    lib.problem_add_var(sp, 1, 8, 0, 0xAB, 0xAB)        # hi = 0xAB
    lib.problem_add_var(sp, 2, 8, 0, 0xCD, 0xCD)        # lo = 0xCD

    v_r  = lib.expr_var(sp, 0)
    v_hi = lib.expr_var(sp, 1)
    v_lo = lib.expr_var(sp, 2)
    cat_e = lib.expr_concat(sp, v_hi, v_lo, 8)
    eq_e  = lib.expr_binary(sp, BIN_EQ, v_r, cat_e)
    lib.problem_add_constraint(sp, eq_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x1111)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    r = lib.solver_get_value(ctx, 0)
    assert r == 0xABCD, f"r={r:#x}, expected 0xABCD"

    lib.zsp_block_alloc_destroy(ba)
