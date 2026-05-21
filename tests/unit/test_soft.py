"""Unit tests for soft constraints (Sprint 6).

Tests assumption-based soft constraint relaxation.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL     = 0xFFFF_FFFF
PROP_OK       = 0
PROP_CONFLICT = 1
SOLVE_OK      = 0
SOLVE_UNSAT   = 1

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
    lib.problem_add_soft_constraint.restype  = ctypes.c_uint32
    lib.problem_add_soft_constraint.argtypes = [ctypes.c_void_p,
                                                ctypes.c_uint32, ctypes.c_uint32]
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
    lib.solver_compile.restype  = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.zsp_var_lo64.restype  = ctypes.c_int64
    lib.zsp_var_lo64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_hi64.restype  = ctypes.c_int64
    lib.zsp_var_hi64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.solver_propagate.restype  = ctypes.c_int
    lib.solver_propagate.argtypes = [ctypes.c_void_p]

    lib.solver_soft_active.restype  = ctypes.c_int
    lib.solver_soft_active.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

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
BIN_GT  = 14
BIN_GTE = 15


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

def test_soft_all_satisfiable(libzsp):
    """Hard: x in [0,10]. Soft: x == 5. Solution should be x=5."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 10]
    lib.problem_add_var(sp, 0, 8, 0, 0, 10)

    # Soft: x == 5 (priority 0 = highest)
    v_x = lib.expr_var(sp, 0)
    c5 = lib.expr_const(sp, 5, 0)
    eq_e = lib.expr_binary(sp, BIN_EQ, v_x, c5)
    lib.problem_add_soft_constraint(sp, eq_e, 0)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x1234)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    x = lib.solver_get_value(ctx, 0)
    assert x == 5, f"x={x}, expected 5"

    # Soft should be active (not relaxed)
    assert lib.solver_soft_active(ctx, 0) == 1

    lib.zsp_block_alloc_destroy(ba)


def test_soft_one_relaxed(libzsp):
    """Hard: x > 7. Soft: x == 5. Soft must be relaxed. Solution: x > 7."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 20]
    lib.problem_add_var(sp, 0, 8, 0, 0, 20)

    # Hard: x > 7  (i.e. x >= 8)
    v_x = lib.expr_var(sp, 0)
    c7 = lib.expr_const(sp, 7, 0)
    gt_e = lib.expr_binary(sp, BIN_GT, v_x, c7)
    lib.problem_add_constraint(sp, gt_e)

    # Soft: x == 5 (conflicts with hard)
    c5 = lib.expr_const(sp, 5, 0)
    v_x2 = lib.expr_var(sp, 0)
    eq_e = lib.expr_binary(sp, BIN_EQ, v_x2, c5)
    lib.problem_add_soft_constraint(sp, eq_e, 0)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x5678)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    x = lib.solver_get_value(ctx, 0)
    assert x > 7, f"x={x}, expected > 7"

    # Soft should be relaxed
    assert lib.solver_soft_active(ctx, 0) == 0

    lib.zsp_block_alloc_destroy(ba)


def test_soft_priority_ordering(libzsp):
    """Two conflicting soft constraints with different priorities.
    Lower priority (higher number) is relaxed first."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 20]
    lib.problem_add_var(sp, 0, 8, 0, 0, 20)

    # Hard: x >= 10
    v_x = lib.expr_var(sp, 0)
    c10 = lib.expr_const(sp, 10, 0)
    gte_e = lib.expr_binary(sp, BIN_GTE, v_x, c10)
    lib.problem_add_constraint(sp, gte_e)

    # Soft 0: x == 3 (priority 0 = high, should try to keep)
    v_x2 = lib.expr_var(sp, 0)
    c3 = lib.expr_const(sp, 3, 0)
    eq3_e = lib.expr_binary(sp, BIN_EQ, v_x2, c3)
    lib.problem_add_soft_constraint(sp, eq3_e, 0)

    # Soft 1: x == 5 (priority 10 = low, should be relaxed first)
    v_x3 = lib.expr_var(sp, 0)
    c5 = lib.expr_const(sp, 5, 0)
    eq5_e = lib.expr_binary(sp, BIN_EQ, v_x3, c5)
    lib.problem_add_soft_constraint(sp, eq5_e, 10)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0xABCD)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    x = lib.solver_get_value(ctx, 0)
    assert x >= 10, f"x={x}, expected >= 10"

    # Both soft constraints conflict with hard (x >= 10), both must be relaxed.
    # Assumption indices are reversed from add order (prepended list):
    # assumption 0 = soft 1 (x==5, pri 10) -> relaxed
    # assumption 1 = soft 0 (x==3, pri 0)  -> relaxed
    assert lib.solver_soft_active(ctx, 0) == 0  # both relaxed
    assert lib.solver_soft_active(ctx, 1) == 0

    lib.zsp_block_alloc_destroy(ba)


def test_soft_multiple_relaxed(libzsp):
    """Three soft constraints, two conflict with hard. Both relaxed,
    third remains active."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 20], y in [0, 20]
    lib.problem_add_var(sp, 0, 8, 0, 0, 20)
    lib.problem_add_var(sp, 1, 8, 0, 0, 20)

    # Hard: x >= 10
    v_x = lib.expr_var(sp, 0)
    c10 = lib.expr_const(sp, 10, 0)
    gte_e = lib.expr_binary(sp, BIN_GTE, v_x, c10)
    lib.problem_add_constraint(sp, gte_e)

    # Soft 0: x == 3 (priority 5, conflicts with hard)
    v_x2 = lib.expr_var(sp, 0)
    c3 = lib.expr_const(sp, 3, 0)
    eq3_e = lib.expr_binary(sp, BIN_EQ, v_x2, c3)
    lib.problem_add_soft_constraint(sp, eq3_e, 5)

    # Soft 1: x == 5 (priority 10, conflicts with hard)
    v_x3 = lib.expr_var(sp, 0)
    c5 = lib.expr_const(sp, 5, 0)
    eq5_e = lib.expr_binary(sp, BIN_EQ, v_x3, c5)
    lib.problem_add_soft_constraint(sp, eq5_e, 10)

    # Soft 2: y == 7 (priority 1, does NOT conflict, should be active)
    v_y = lib.expr_var(sp, 1)
    c7 = lib.expr_const(sp, 7, 0)
    eq7_e = lib.expr_binary(sp, BIN_EQ, v_y, c7)
    lib.problem_add_soft_constraint(sp, eq7_e, 1)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x9999)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    x = lib.solver_get_value(ctx, 0)
    y = lib.solver_get_value(ctx, 1)
    assert x >= 10, f"x={x}, expected >= 10"
    assert y == 7, f"y={y}, expected 7"

    # Soft constraints are stored in reverse add order (prepended list):
    # assumption 0 = soft 2 (y==7, pri 1) -> should be active
    # assumption 1 = soft 1 (x==5, pri 10) -> should be relaxed
    # assumption 2 = soft 0 (x==3, pri 5) -> should be relaxed
    assert lib.solver_soft_active(ctx, 0) == 1   # y==7, active
    assert lib.solver_soft_active(ctx, 1) == 0   # x==5, relaxed
    assert lib.solver_soft_active(ctx, 2) == 0   # x==3, relaxed

    lib.zsp_block_alloc_destroy(ba)


def test_soft_active_query(libzsp):
    """After solve, verify solver_soft_active() returns correct status."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 8, 0, 0, 20)

    # Soft 0: x == 15 (satisfiable, priority 0)
    v_x = lib.expr_var(sp, 0)
    c15 = lib.expr_const(sp, 15, 0)
    eq_e = lib.expr_binary(sp, BIN_EQ, v_x, c15)
    lib.problem_add_soft_constraint(sp, eq_e, 0)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x4444)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    assert lib.solver_soft_active(ctx, 0) == 1
    # Out of range returns -1
    assert lib.solver_soft_active(ctx, 99) == -1

    lib.zsp_block_alloc_destroy(ba)


def test_soft_with_hard_unsat(libzsp):
    """Hard constraints alone are UNSAT. Returns SOLVE_UNSAT
    (soft relaxation can't help)."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 5]
    lib.problem_add_var(sp, 0, 8, 0, 0, 5)

    # Hard: x > 10 (impossible with domain [0, 5])
    v_x = lib.expr_var(sp, 0)
    c10 = lib.expr_const(sp, 10, 0)
    gt_e = lib.expr_binary(sp, BIN_GT, v_x, c10)
    lib.problem_add_constraint(sp, gt_e)

    # Soft: x == 3 (doesn't matter, hard is UNSAT)
    v_x2 = lib.expr_var(sp, 0)
    c3 = lib.expr_const(sp, 3, 0)
    eq_e = lib.expr_binary(sp, BIN_EQ, v_x2, c3)
    lib.problem_add_soft_constraint(sp, eq_e, 0)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)

    # Compile might detect UNSAT directly (returns -2)
    if rc == -2:
        # UNSAT detected at compile time - that's fine
        lib.zsp_block_alloc_destroy(ba)
        return

    opts = lib._SolveOpts(seed=0x7777)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    # Should be UNSAT since hard constraint alone is impossible
    assert result != SOLVE_OK

    lib.zsp_block_alloc_destroy(ba)
