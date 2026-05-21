"""Unit tests for ITE (if-then-else) value propagator (Sprint 2.2/2.3).

Tests the ITE value propagator: r = cond ? a : b
and the ITE constraint compilation.
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
    lib.expr_ite.restype  = ctypes.c_uint32
    lib.expr_ite.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
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

    lib.prop_add_ite_value_64.restype  = ctypes.c_uint32
    lib.prop_add_ite_value_64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                          ctypes.c_uint32, ctypes.c_uint32,
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


BIN_EQ  = 10
BIN_LTE = 13


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


# ------------------------------------------------------------------ #
# ITE value propagator tests (direct API)                             #
# ------------------------------------------------------------------ #

def test_ite_value_cond_true(libzsp):
    """r = (1 ? a : b); a=[5,5] -> r=[5,5]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 100),  # 0: r
        (1,  1, 1,   1),  # 1: cond = 1
        (33, 1, 5,   5),  # 2: a = 5
        (33, 1, 0, 100),  # 3: b
    ])

    ref = lib.prop_add_ite_value_64(ctx, 0, 1, 2, 3, 0)
    assert ref != EXPR_NULL

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert lib.zsp_var_lo64(ctx, 0) == 5
    assert lib.zsp_var_hi64(ctx, 0) == 5

    lib.zsp_block_alloc_destroy(ba)


def test_ite_value_cond_false(libzsp):
    """r = (0 ? a : b); b=[7,7] -> r=[7,7]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 100),  # 0: r
        (1,  1, 0,   0),  # 1: cond = 0
        (33, 1, 0, 100),  # 2: a
        (33, 1, 7,   7),  # 3: b = 7
    ])

    ref = lib.prop_add_ite_value_64(ctx, 0, 1, 2, 3, 0)
    assert ref != EXPR_NULL

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert lib.zsp_var_lo64(ctx, 0) == 7
    assert lib.zsp_var_hi64(ctx, 0) == 7

    lib.zsp_block_alloc_destroy(ba)


def test_ite_value_cond_undecided(libzsp):
    """r = (cond ? a : b); cond in [0,1], a=[3,3], b=[8,8] -> r in [3,8]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 100),  # 0: r
        (1,  1, 0,   1),  # 1: cond undecided
        (33, 1, 3,   3),  # 2: a = 3
        (33, 1, 8,   8),  # 3: b = 8
    ])

    ref = lib.prop_add_ite_value_64(ctx, 0, 1, 2, 3, 0)
    assert ref != EXPR_NULL

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    lo = lib.zsp_var_lo64(ctx, 0)
    hi = lib.zsp_var_hi64(ctx, 0)
    assert lo <= 3, f"Expected lo <= 3, got {lo}"
    assert hi >= 8, f"Expected hi >= 8, got {hi}"

    lib.zsp_block_alloc_destroy(ba)


def test_ite_value_backward_propagation(libzsp):
    """r = (1 ? a : b); fix r=[10,10] -> a=[10,10] (backward)."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (33, 1, 0, 100),  # 0: r
        (1,  1, 1,   1),  # 1: cond = 1
        (33, 1, 0, 100),  # 2: a
        (33, 1, 0, 100),  # 3: b
    ])

    ref = lib.prop_add_ite_value_64(ctx, 0, 1, 2, 3, 0)
    assert ref != EXPR_NULL

    lib.ctx_tighten_lb64(ctx, 0, 10)
    lib.ctx_tighten_ub64(ctx, 0, 10)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert lib.zsp_var_lo64(ctx, 2) == 10
    assert lib.zsp_var_hi64(ctx, 2) == 10

    lib.zsp_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# ITE constraint compilation tests (via problem builder)              #
# ------------------------------------------------------------------ #

def test_ite_compile_var_eq_ite(libzsp):
    """Compile r == (cond ? a : b) via problem builder, then solve."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # r=0, cond=1, a=2, b=3
    lib.problem_add_var(sp, 0, 33, 1, 0, 100)   # r
    lib.problem_add_var(sp, 1, 1,  1, 1, 1)     # cond = 1
    lib.problem_add_var(sp, 2, 33, 1, 42, 42)   # a = 42
    lib.problem_add_var(sp, 3, 33, 1, 0, 100)   # b

    # Build: r == ite(cond, a, b)
    v_r    = lib.expr_var(sp, 0)
    v_cond = lib.expr_var(sp, 1)
    v_a    = lib.expr_var(sp, 2)
    v_b    = lib.expr_var(sp, 3)
    ite_e  = lib.expr_ite(sp, v_cond, v_a, v_b)
    eq_e   = lib.expr_binary(sp, BIN_EQ, v_r, ite_e)
    lib.problem_add_constraint(sp, eq_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0, f"compile failed: {rc}"

    opts = lib._SolveOpts(seed=0x1234)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    r_val = lib.solver_get_value(ctx, 0)
    assert r_val == 42, f"Expected r=42, got {r_val}"

    lib.zsp_block_alloc_destroy(ba)


def test_ite_compile_static_true(libzsp):
    """ITE at constraint root with constant cond=1: only then-branch applies."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 32, 1, 0, 100)   # x

    # Build: ite(1, x <= 5, x <= 99)
    v_x    = lib.expr_var(sp, 0)
    c_true = lib.expr_const(sp, 1, 0)
    c_5    = lib.expr_const(sp, 5, 0)
    c_99   = lib.expr_const(sp, 99, 0)
    then_e = lib.expr_binary(sp, BIN_LTE, v_x, c_5)
    else_e = lib.expr_binary(sp, BIN_LTE, v_x, c_99)
    ite_e  = lib.expr_ite(sp, c_true, then_e, else_e)
    lib.problem_add_constraint(sp, ite_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x5678)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    x_val = lib.solver_get_value(ctx, 0)
    assert x_val <= 5, f"Expected x <= 5, got {x_val}"

    lib.zsp_block_alloc_destroy(ba)


def test_ite_compile_static_false(libzsp):
    """ITE at constraint root with constant cond=0: only else-branch applies."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 32, 1, 0, 100)   # x

    # Build: ite(0, x <= 5, x <= 50)
    v_x     = lib.expr_var(sp, 0)
    c_false = lib.expr_const(sp, 0, 0)
    c_5     = lib.expr_const(sp, 5, 0)
    c_50    = lib.expr_const(sp, 50, 0)
    then_e  = lib.expr_binary(sp, BIN_LTE, v_x, c_5)
    else_e  = lib.expr_binary(sp, BIN_LTE, v_x, c_50)
    ite_e   = lib.expr_ite(sp, c_false, then_e, else_e)
    lib.problem_add_constraint(sp, ite_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0xABCD)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    x_val = lib.solver_get_value(ctx, 0)
    assert x_val <= 50, f"Expected x <= 50, got {x_val}"

    lib.zsp_block_alloc_destroy(ba)
