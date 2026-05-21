"""Unit tests for bitwise propagators (Sprint 3).

Tests AND, OR, XOR, NOT propagators at both the direct API level
and via constraint compilation.
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
    lib.expr_const.restype  = ctypes.c_uint32
    lib.expr_const.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_uint8]
    lib.expr_binary.restype  = ctypes.c_uint32
    lib.expr_binary.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                ctypes.c_uint32, ctypes.c_uint32]

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

    lib.prop_add_bounds_band_64.restype  = ctypes.c_uint32
    lib.prop_add_bounds_band_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                            ctypes.c_uint32, ctypes.c_uint32,
                                            ctypes.c_uint8]
    lib.prop_add_bounds_bor_64.restype  = ctypes.c_uint32
    lib.prop_add_bounds_bor_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                           ctypes.c_uint32, ctypes.c_uint32,
                                           ctypes.c_uint8]
    lib.prop_add_bounds_bxor_64.restype  = ctypes.c_uint32
    lib.prop_add_bounds_bxor_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                            ctypes.c_uint32, ctypes.c_uint32,
                                            ctypes.c_uint8]
    lib.prop_add_bounds_bnot_64.restype  = ctypes.c_uint32
    lib.prop_add_bounds_bnot_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                            ctypes.c_uint32, ctypes.c_uint8]

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


BIN_EQ   = 10
BIN_BAND = 5
BIN_BOR  = 6
BIN_BXOR = 7


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
# AND tests                                                            #
# ------------------------------------------------------------------ #

def test_band_singleton_mask(libzsp):
    """r = x & 0xF0; x=[0,255] -> r in [0,0xF0], low nibble clear."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 255),   # 0: r
        (33, 1, 0, 255),   # 1: x
        (33, 1, 0xF0, 0xF0),  # 2: mask = 0xF0
    ])

    lib.prop_add_bounds_band_64(ctx, 0, 1, 2, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _hi(lib, ctx, 0) <= 0xF0
    assert _lo(lib, ctx, 0) >= 0

    lib.zsp_block_alloc_destroy(ba)


def test_band_both_singletons(libzsp):
    """r = a & b; a=0xFF, b=0x0F -> r=0x0F."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 255),   # 0: r
        (33, 1, 0xFF, 0xFF),  # 1: a
        (33, 1, 0x0F, 0x0F),  # 2: b
    ])

    lib.prop_add_bounds_band_64(ctx, 0, 1, 2, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) == 0x0F
    assert _hi(lib, ctx, 0) == 0x0F

    lib.zsp_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# OR tests                                                             #
# ------------------------------------------------------------------ #

def test_bor_singleton(libzsp):
    """r = x | 0x80; x=[0,127] -> r in [0x80, ...]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 255),     # 0: r
        (33, 1, 0, 127),     # 1: x
        (33, 1, 0x80, 0x80), # 2: const 0x80
    ])

    lib.prop_add_bounds_bor_64(ctx, 0, 1, 2, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) >= 0x80

    lib.zsp_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# XOR tests                                                            #
# ------------------------------------------------------------------ #

def test_bxor_singleton(libzsp):
    """r = x ^ 0xFF; x=[0,0] -> r=[0xFF,0xFF]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 0xFFFF),  # 0: r
        (33, 1, 0, 0),        # 1: x = 0
        (33, 1, 0xFF, 0xFF),  # 2: const 0xFF
    ])

    lib.prop_add_bounds_bxor_64(ctx, 0, 1, 2, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) == 0xFF
    assert _hi(lib, ctx, 0) == 0xFF

    lib.zsp_block_alloc_destroy(ba)


def test_bxor_backward(libzsp):
    """r = x ^ 0xFF; fix r=0xAA -> x=0x55."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 0xFFFF),  # 0: r
        (33, 1, 0, 0xFFFF),  # 1: x
        (33, 1, 0xFF, 0xFF), # 2: const 0xFF
    ])

    lib.prop_add_bounds_bxor_64(ctx, 0, 1, 2, 0)
    _fix(lib, ctx, 0, 0xAA)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 1) == 0x55
    assert _hi(lib, ctx, 1) == 0x55

    lib.zsp_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# NOT tests                                                            #
# ------------------------------------------------------------------ #

def test_bnot_zero(libzsp):
    """r = ~x; x=[0,0] -> r=[-1,-1] (all bits set, signed)."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, -0x200000000, 0x1FFFFFFFF),  # 0: r (wide signed)
        (33, 1, 0, 0),                        # 1: x = 0
    ])

    lib.prop_add_bounds_bnot_64(ctx, 0, 1, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) == -1
    assert _hi(lib, ctx, 0) == -1

    lib.zsp_block_alloc_destroy(ba)


def test_bnot_backward(libzsp):
    """r = ~x; fix r=-1 -> x=0."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, -0x200000000, 0x1FFFFFFFF),  # 0: r
        (33, 1, -0x200000000, 0x1FFFFFFFF),  # 1: x
    ])

    lib.prop_add_bounds_bnot_64(ctx, 0, 1, 0)
    _fix(lib, ctx, 0, -1)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 1) == 0
    assert _hi(lib, ctx, 1) == 0

    lib.zsp_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# Compilation / end-to-end tests                                      #
# ------------------------------------------------------------------ #

def test_band_compile_e2e(libzsp):
    """r == (x & mask) via problem builder, then solve."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 33, 1, 0, 0xFF)   # r
    lib.problem_add_var(sp, 1, 33, 1, 0, 0xFF)   # x
    lib.problem_add_var(sp, 2, 33, 1, 0xF0, 0xF0) # mask

    v_r  = lib.expr_var(sp, 0)
    v_x  = lib.expr_var(sp, 1)
    v_m  = lib.expr_var(sp, 2)
    and_e = lib.expr_binary(sp, BIN_BAND, v_x, v_m)
    eq_e  = lib.expr_binary(sp, BIN_EQ, v_r, and_e)
    lib.problem_add_constraint(sp, eq_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x1234)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    r = lib.solver_get_value(ctx, 0)
    x = lib.solver_get_value(ctx, 1)
    assert r == (x & 0xF0), f"r={r:#x} != x&0xF0 = {x&0xF0:#x}"

    lib.zsp_block_alloc_destroy(ba)


def test_bor_compile_e2e(libzsp):
    """r == (x | 0x80) via problem builder, then solve."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 33, 1, 0, 0xFF)     # r
    lib.problem_add_var(sp, 1, 33, 1, 0, 0x7F)     # x
    lib.problem_add_var(sp, 2, 33, 1, 0x80, 0x80)  # const

    v_r = lib.expr_var(sp, 0)
    v_x = lib.expr_var(sp, 1)
    v_c = lib.expr_var(sp, 2)
    or_e = lib.expr_binary(sp, BIN_BOR, v_x, v_c)
    eq_e = lib.expr_binary(sp, BIN_EQ, v_r, or_e)
    lib.problem_add_constraint(sp, eq_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x5678)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    r = lib.solver_get_value(ctx, 0)
    x = lib.solver_get_value(ctx, 1)
    assert r == (x | 0x80), f"r={r:#x} != x|0x80 = {x|0x80:#x}"

    lib.zsp_block_alloc_destroy(ba)
