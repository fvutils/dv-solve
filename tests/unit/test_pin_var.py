"""Unit tests for solver_pin_var (Sprint 5.2).

Tests:
- Pin a variable and verify dependent constraints are satisfied
- Pin that causes a conflict returns -1
- Pin + checkpoint/restore pattern
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL   = 0xFFFF_FFFF
SOLVE_OK    = 0

_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 1048576


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
    lib.solver_pin_var.restype  = ctypes.c_int
    lib.solver_pin_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                   ctypes.c_int64]
    lib.solver_reset.restype  = None
    lib.solver_reset.argtypes = [ctypes.c_void_p]
    lib.solver_checkpoint.restype  = ctypes.c_int
    lib.solver_checkpoint.argtypes = [ctypes.c_void_p]
    lib.solver_restore.restype  = None
    lib.solver_restore.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


BIN_LT  = 12
BIN_LTE = 13
BIN_GT  = 14
BIN_EQ  = 10


def test_pin_var_basic(libzsp):
    """Pin x=5, constraint y > x. Verify y > 5."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 32, 1, 0, 100)  # x
    lib.problem_add_var(sp, 1, 32, 1, 0, 100)  # y

    # y > x
    v_x = lib.expr_var(sp, 0)
    v_y = lib.expr_var(sp, 1)
    gt_e = lib.expr_binary(sp, BIN_GT, v_y, v_x)
    lib.problem_add_constraint(sp, gt_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    # Pin x = 5
    rc = lib.solver_pin_var(ctx, 0, 5)
    assert rc == 0

    opts = lib._SolveOpts(seed=0x1234)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    x = lib.solver_get_value(ctx, 0)
    y = lib.solver_get_value(ctx, 1)
    assert x == 5
    assert y > 5, f"Expected y > 5, got {y}"

    lib.zsp_block_alloc_destroy(ba)


def test_pin_var_conflict(libzsp):
    """Pin x=5, then pin x=3 via constraint: conflict."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 32, 1, 0, 100)  # x

    # x < 3
    v_x = lib.expr_var(sp, 0)
    c_3 = lib.expr_const(sp, 3, 1)
    lt_e = lib.expr_binary(sp, BIN_LT, v_x, c_3)
    lib.problem_add_constraint(sp, lt_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    # Pin x = 5, but x < 3 => conflict
    rc = lib.solver_pin_var(ctx, 0, 5)
    assert rc == -1, "Expected conflict from pin_var"

    lib.zsp_block_alloc_destroy(ba)


def test_pin_var_with_checkpoint(libzsp):
    """Checkpoint, pin, solve, restore, pin different, solve again."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 32, 1, 0, 100)  # x
    lib.problem_add_var(sp, 1, 32, 1, 0, 100)  # y

    # y == x (EQ)
    v_x = lib.expr_var(sp, 0)
    v_y = lib.expr_var(sp, 1)
    eq_e = lib.expr_binary(sp, BIN_EQ, v_x, v_y)
    lib.problem_add_constraint(sp, eq_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    # Checkpoint
    cp = lib.solver_checkpoint(ctx)
    assert cp >= 0

    # Pin x=10, solve
    rc = lib.solver_pin_var(ctx, 0, 10)
    assert rc == 0
    opts = lib._SolveOpts(seed=0x1111)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK
    assert lib.solver_get_value(ctx, 1) == 10  # y == x == 10

    # Restore
    lib.solver_restore(ctx, cp)

    # Pin x=42, solve
    rc = lib.solver_pin_var(ctx, 0, 42)
    assert rc == 0
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK
    assert lib.solver_get_value(ctx, 1) == 42  # y == x == 42

    lib.zsp_block_alloc_destroy(ba)
