"""Unit tests for shift propagators (Sprint 4.3).

Tests left shift (SHL) and logical right shift (LSHR) propagators.
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

    lib.prop_add_bounds_shl_64.restype  = ctypes.c_uint32
    lib.prop_add_bounds_shl_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                           ctypes.c_uint32, ctypes.c_uint32,
                                           ctypes.c_uint8]
    lib.prop_add_bounds_lshr_64.restype  = ctypes.c_uint32
    lib.prop_add_bounds_lshr_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                            ctypes.c_uint32, ctypes.c_uint32,
                                            ctypes.c_uint8]

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


BIN_EQ     = 10
BIN_LSHIFT = 8
BIN_RSHIFT = 9


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


# ------------------------------------------------------------------ #
# SHL tests                                                           #
# ------------------------------------------------------------------ #

def test_shl_const(libzsp):
    """r = a << 3; a=[1,4] -> r in [8, 32]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 10000),  # 0: r
        (33, 1, 1, 4),      # 1: a
        (33, 1, 3, 3),      # 2: b = 3
    ])

    lib.prop_add_bounds_shl_64(ctx, 0, 1, 2, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) >= 8    # 1 << 3
    assert _hi(lib, ctx, 0) <= 32   # 4 << 3

    lib.zsp_block_alloc_destroy(ba)


def test_shl_both_singletons(libzsp):
    """r = 5 << 2 -> r = 20."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 10000),  # 0: r
        (33, 1, 5, 5),      # 1: a = 5
        (33, 1, 2, 2),      # 2: b = 2
    ])

    lib.prop_add_bounds_shl_64(ctx, 0, 1, 2, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) == 20
    assert _hi(lib, ctx, 0) == 20

    lib.zsp_block_alloc_destroy(ba)


def test_shl_backward(libzsp):
    """r = a << 3; fix r=[24,24] -> a=[3,3]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 10000),  # 0: r
        (33, 1, 0, 100),    # 1: a
        (33, 1, 3, 3),      # 2: b = 3
    ])

    lib.prop_add_bounds_shl_64(ctx, 0, 1, 2, 0)
    lib.ctx_tighten_lb64(ctx, 0, 24)
    lib.ctx_tighten_ub64(ctx, 0, 24)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 1) == 3
    assert _hi(lib, ctx, 1) == 3

    lib.zsp_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# LSHR tests                                                          #
# ------------------------------------------------------------------ #

def test_lshr_const(libzsp):
    """r = a >> 2; a=[8,15] -> r in [2,3]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 10000),  # 0: r
        (33, 1, 8, 15),     # 1: a
        (33, 1, 2, 2),      # 2: b = 2
    ])

    lib.prop_add_bounds_lshr_64(ctx, 0, 1, 2, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) >= 2    # 8 >> 2
    assert _hi(lib, ctx, 0) <= 3    # 15 >> 2

    lib.zsp_block_alloc_destroy(ba)


def test_lshr_both_singletons(libzsp):
    """r = 100 >> 3 -> r = 12."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 10000),  # 0: r
        (33, 1, 100, 100),  # 1: a = 100
        (33, 1, 3, 3),      # 2: b = 3
    ])

    lib.prop_add_bounds_lshr_64(ctx, 0, 1, 2, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) == 12
    assert _hi(lib, ctx, 0) == 12

    lib.zsp_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# Compilation tests                                                    #
# ------------------------------------------------------------------ #

def test_shl_compile_e2e(libzsp):
    """r == (a << b) via problem builder, then solve."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 33, 1, 0, 10000)  # r
    lib.problem_add_var(sp, 1, 33, 1, 1, 10)     # a
    lib.problem_add_var(sp, 2, 33, 1, 2, 2)      # b = 2

    v_r = lib.expr_var(sp, 0)
    v_a = lib.expr_var(sp, 1)
    v_b = lib.expr_var(sp, 2)
    shl_e = lib.expr_binary(sp, BIN_LSHIFT, v_a, v_b)
    eq_e  = lib.expr_binary(sp, BIN_EQ, v_r, shl_e)
    lib.problem_add_constraint(sp, eq_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x9876)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    r = lib.solver_get_value(ctx, 0)
    a = lib.solver_get_value(ctx, 1)
    b = lib.solver_get_value(ctx, 2)
    assert r == (a << b), f"r={r} != a<<b = {a}<<{b} = {a<<b}"

    lib.zsp_block_alloc_destroy(ba)
