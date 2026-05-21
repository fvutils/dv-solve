"""Unit tests for extend propagators (Sprint 4.1).

Tests zero-extend and sign-extend compilation via the constraint
r == extend(a, from_bits, to_bits) pattern.
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
    lib.expr_extend.restype  = ctypes.c_uint32
    lib.expr_extend.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]

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


BIN_EQ = 10


def _lo(lib, ctx, i):
    return lib.zsp_var_lo64(ctx, i)


def _hi(lib, ctx, i):
    return lib.zsp_var_hi64(ctx, i)


# ------------------------------------------------------------------ #
# Zero-extend tests                                                   #
# ------------------------------------------------------------------ #

def test_zero_extend_8_to_32(libzsp):
    """a=[0,255] (8-bit), r = zext(a, 8, 32). r in [0, 255].
    Tighten a to [100,200], verify r in [100,200]."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # var 0: r (32-bit result of zero-extend)
    lib.problem_add_var(sp, 0, 33, 0, 0, 0xFFFFFFFF)
    # var 1: a (8-bit source)
    lib.problem_add_var(sp, 1, 8, 0, 0, 255)

    v_r = lib.expr_var(sp, 0)
    v_a = lib.expr_var(sp, 1)
    ext_e = lib.expr_extend(sp, v_a, 8, 32, 0)  # zero-extend
    eq_e = lib.expr_binary(sp, BIN_EQ, v_r, ext_e)
    lib.problem_add_constraint(sp, eq_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    # After compilation, r should be tightened to [0, 255]
    assert _lo(lib, ctx, 0) >= 0
    assert _hi(lib, ctx, 0) <= 255

    # Tighten a to [100, 200]
    lib.ctx_tighten_lb64(ctx, 1, 100)
    lib.ctx_tighten_ub64(ctx, 1, 200)
    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    # r should follow a
    assert _lo(lib, ctx, 0) >= 100
    assert _hi(lib, ctx, 0) <= 200

    lib.zsp_block_alloc_destroy(ba)


def test_zero_extend_e2e(libzsp):
    """r = zext(a, 8, 32), solve and verify r == a."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 33, 0, 0, 0xFFFFFFFF)  # r
    lib.problem_add_var(sp, 1, 8, 0, 0, 255)           # a

    v_r = lib.expr_var(sp, 0)
    v_a = lib.expr_var(sp, 1)
    ext_e = lib.expr_extend(sp, v_a, 8, 32, 0)
    eq_e = lib.expr_binary(sp, BIN_EQ, v_r, ext_e)
    lib.problem_add_constraint(sp, eq_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0xABCD)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    r = lib.solver_get_value(ctx, 0)
    a = lib.solver_get_value(ctx, 1)
    assert r == a, f"r={r} != a={a}"
    assert 0 <= r <= 255

    lib.zsp_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# Sign-extend tests                                                   #
# ------------------------------------------------------------------ #

def test_sign_extend_8_to_32(libzsp):
    """a=[-128,127] (8-bit signed), r = sext(a, 8, 32).
    r in [-128, 127] (as 32-bit signed)."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 33, 1, -0x80000000, 0x7FFFFFFF)  # r (32-bit signed)
    lib.problem_add_var(sp, 1, 8, 1, -128, 127)                  # a (8-bit signed)

    v_r = lib.expr_var(sp, 0)
    v_a = lib.expr_var(sp, 1)
    ext_e = lib.expr_extend(sp, v_a, 8, 32, 1)  # sign-extend
    eq_e = lib.expr_binary(sp, BIN_EQ, v_r, ext_e)
    lib.problem_add_constraint(sp, eq_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    # After compilation, r should be tightened to [-128, 127]
    assert _lo(lib, ctx, 0) >= -128
    assert _hi(lib, ctx, 0) <= 127

    lib.zsp_block_alloc_destroy(ba)


def test_sign_extend_e2e(libzsp):
    """r = sext(a, 8, 32), solve and verify r == a."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 33, 1, -0x80000000, 0x7FFFFFFF)
    lib.problem_add_var(sp, 1, 8, 1, -128, 127)

    v_r = lib.expr_var(sp, 0)
    v_a = lib.expr_var(sp, 1)
    ext_e = lib.expr_extend(sp, v_a, 8, 32, 1)
    eq_e = lib.expr_binary(sp, BIN_EQ, v_r, ext_e)
    lib.problem_add_constraint(sp, eq_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x4567)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    r = lib.solver_get_value(ctx, 0)
    a = lib.solver_get_value(ctx, 1)
    assert r == a, f"r={r} != a={a}"
    assert -128 <= r <= 127

    lib.zsp_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# Mixed-width constraint test                                         #
# ------------------------------------------------------------------ #

def test_mixed_width_constraint(libzsp):
    """Verilator pattern: s64'(x) != u64'(tiny).
    8-bit x, 1-bit tiny, compare after extension."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # var 0: r_sext = sext(x, 8, 64)
    lib.problem_add_var(sp, 0, 33, 1, -0x200000000, 0x1FFFFFFFF)
    # var 1: x (8-bit signed)
    lib.problem_add_var(sp, 1, 8, 1, -128, 127)
    # var 2: r_zext = zext(tiny, 1, 64)
    lib.problem_add_var(sp, 2, 33, 0, 0, 0x1FFFFFFFF)
    # var 3: tiny (1-bit unsigned)
    lib.problem_add_var(sp, 3, 1, 0, 0, 1)

    # r_sext == sext(x)
    v_r0 = lib.expr_var(sp, 0)
    v_x  = lib.expr_var(sp, 1)
    sext_e = lib.expr_extend(sp, v_x, 8, 64, 1)
    eq_sext = lib.expr_binary(sp, BIN_EQ, v_r0, sext_e)
    lib.problem_add_constraint(sp, eq_sext)

    # r_zext == zext(tiny)
    v_r2 = lib.expr_var(sp, 2)
    v_t  = lib.expr_var(sp, 3)
    zext_e = lib.expr_extend(sp, v_t, 1, 64, 0)
    eq_zext = lib.expr_binary(sp, BIN_EQ, v_r2, zext_e)
    lib.problem_add_constraint(sp, eq_zext)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    # r_sext should be tightened to [-128, 127]
    assert _lo(lib, ctx, 0) >= -128
    assert _hi(lib, ctx, 0) <= 127

    # r_zext should be tightened to [0, 1]
    assert _lo(lib, ctx, 2) >= 0
    assert _hi(lib, ctx, 2) <= 1

    opts = lib._SolveOpts(seed=0x9999)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    r_sext = lib.solver_get_value(ctx, 0)
    x = lib.solver_get_value(ctx, 1)
    r_zext = lib.solver_get_value(ctx, 2)
    tiny = lib.solver_get_value(ctx, 3)

    assert r_sext == x
    assert r_zext == tiny

    lib.zsp_block_alloc_destroy(ba)
