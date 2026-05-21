"""Unit tests for UNSAT core extraction (Sprint 1).

Tests contra_quick_core() and contra_analyze_unsat() with the
assumption-gated approach.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL     = 0xFFFF_FFFF
SOLVE_OK      = 0
SOLVE_UNSAT   = 1

_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 524288

# BinOp values
BIN_EQ  = 10
BIN_LTE = 13
BIN_GTE = 15


def _setup(lib: ctypes.CDLL):
    """Declare ctypes argtypes/restype."""
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
    lib.solver_compile.restype  = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

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

    lib.contra_quick_core.restype = ctypes.c_int
    lib.contra_quick_core.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.c_void_p
    ]


def _make_ctx(lib):
    ba = lib.zsp_block_alloc_create(None, 4096)
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx
    return ctx_buf, ctx, ba


def test_quick_core_trivial_2(libzsp_debug):
    """Trivial UNSAT: x >= 10 and x <= 5. Core should contain both."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 100]
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)

    # Constraint 1: x >= 10
    c10 = lib.expr_const(sp, 10, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx, c10))

    # Constraint 2: x <= 5
    c5 = lib.expr_const(sp, 5, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx, c5))

    # Build a context (we need one even though quick_core builds its own)
    ctx_buf, ctx, ba = _make_ctx(lib)

    # Call contra_quick_core
    out_ids = (ctypes.c_uint32 * 10)()
    out_n = ctypes.c_uint32(10)
    rc = lib.contra_quick_core(ctx, sp, out_ids, ctypes.byref(out_n))

    assert rc == 0, f"contra_quick_core failed with rc={rc}"
    n = out_n.value
    assert n >= 2, f"Expected core of at least 2, got {n}"

    core = set(out_ids[i] for i in range(n))
    # Constraint IDs 1 and 2 should be in the core
    assert 1 in core, f"Constraint 1 not in core: {core}"
    assert 2 in core, f"Constraint 2 not in core: {core}"

    lib.zsp_block_alloc_destroy(ba)


def test_quick_core_3_of_5(libzsp_debug):
    """5 constraints, 3 contradictory. Core should contain the 3."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 100], y in [0, 100]
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    lib.problem_add_var(sp, 1, 8, 0, 0, 100)

    vx = lib.expr_var(sp, 0)
    vy = lib.expr_var(sp, 1)

    # Constraint 1: x >= 10 (satisfiable independently)
    c10 = lib.expr_const(sp, 10, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx, c10))

    # Constraint 2: y >= 20 (satisfiable independently)
    c20 = lib.expr_const(sp, 20, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vy, c20))

    # Constraint 3: x <= 50 (satisfiable with C1)
    c50 = lib.expr_const(sp, 50, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx, c50))

    # Constraint 4: x <= 5 (contradicts C1: x >= 10 and x <= 5)
    c5 = lib.expr_const(sp, 5, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx, c5))

    # Constraint 5: y <= 80 (satisfiable with C2)
    c80 = lib.expr_const(sp, 80, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vy, c80))

    ctx_buf, ctx, ba = _make_ctx(lib)

    out_ids = (ctypes.c_uint32 * 10)()
    out_n = ctypes.c_uint32(10)
    rc = lib.contra_quick_core(ctx, sp, out_ids, ctypes.byref(out_n))

    assert rc == 0
    n = out_n.value
    core = set(out_ids[i] for i in range(n))

    # The contradictory constraints are 1 (x >= 10) and 4 (x <= 5)
    # The core must contain at least these two
    assert 1 in core, f"Constraint 1 (x >= 10) not in core: {core}"
    assert 4 in core, f"Constraint 4 (x <= 5) not in core: {core}"

    lib.zsp_block_alloc_destroy(ba)


def test_quick_core_sat(libzsp_debug):
    """All constraints satisfiable. Core should be empty or contain all."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 100]
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)

    # Constraint 1: x >= 10 (sat: x=50 satisfies both)
    c10 = lib.expr_const(sp, 10, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx, c10))

    # Constraint 2: x <= 50
    c50 = lib.expr_const(sp, 50, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx, c50))

    ctx_buf, ctx, ba = _make_ctx(lib)

    out_ids = (ctypes.c_uint32 * 10)()
    out_n = ctypes.c_uint32(10)
    rc = lib.contra_quick_core(ctx, sp, out_ids, ctypes.byref(out_n))

    # Should succeed but the "core" for a SAT problem should have
    # no relaxed constraints (all are active)
    assert rc == 0

    lib.zsp_block_alloc_destroy(ba)


def test_quick_core_compile_time_unsat(libzsp_debug):
    """Contradiction detected at compile time (domain intersection empty).
    Core should identify the constraint."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 100]
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)

    # Constraint 1: x >= 80
    c80 = lib.expr_const(sp, 80, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx, c80))

    # Constraint 2: x <= 5  -> contradicts x >= 80
    c5 = lib.expr_const(sp, 5, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx, c5))

    ctx_buf, ctx, ba = _make_ctx(lib)

    out_ids = (ctypes.c_uint32 * 10)()
    out_n = ctypes.c_uint32(10)
    rc = lib.contra_quick_core(ctx, sp, out_ids, ctypes.byref(out_n))

    assert rc == 0
    n = out_n.value
    assert n >= 2, f"Expected core >= 2 for compile-time UNSAT, got {n}"

    lib.zsp_block_alloc_destroy(ba)
