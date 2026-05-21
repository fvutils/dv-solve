"""Unit tests for soft constraint diagnostics (Sprint 6).

Tests contra_explain_soft() for diagnosing why softs were relaxed.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL = 0xFFFF_FFFF
_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 524288

BIN_EQ  = 10
BIN_LTE = 13
BIN_GTE = 15
SOLVE_OK = 0


class ContraRelaxSuggestion(ctypes.Structure):
    _fields_ = [
        ("constraint_id",     ctypes.c_uint32),
        ("original_constant", ctypes.c_int64),
        ("relaxed_constant",  ctypes.c_int64),
        ("delta",             ctypes.c_int64),
        ("is_relaxable",      ctypes.c_uint8),
        ("relax_direction",   ctypes.c_uint8),
        ("_rpad",             ctypes.c_uint8 * 6),
    ]


class ContraSoftDiagEntry(ctypes.Structure):
    _fields_ = [
        ("soft_constraint_id",  ctypes.c_uint32),
        ("soft_priority",       ctypes.c_uint32),
        ("conflict_hard_ids",   ctypes.POINTER(ctypes.c_uint32)),
        ("n_conflict_hard",     ctypes.c_uint32),
        ("proof_text",          ctypes.c_char_p),
        ("hard_relax",          ctypes.POINTER(ContraRelaxSuggestion)),
        ("n_hard_relax",        ctypes.c_uint32),
        ("alternative_soft_ids", ctypes.POINTER(ctypes.c_uint32)),
        ("n_alternatives",      ctypes.c_uint32),
    ]


class ContraSoftDiagResult(ctypes.Structure):
    _fields_ = [
        ("entries",     ctypes.POINTER(ContraSoftDiagEntry)),
        ("n_entries",   ctypes.c_uint32),
        ("elapsed_sec", ctypes.c_double),
    ]


class SolveOpts(ctypes.Structure):
    _fields_ = [
        ("seed",           ctypes.c_uint64),
        ("max_conflicts",  ctypes.c_uint32),
        ("max_restarts",   ctypes.c_uint32),
        ("use_phase_save", ctypes.c_uint8),
        ("_pad",           ctypes.c_uint8 * 3),
        ("max_shave_iters", ctypes.c_uint32),
    ]


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

    lib.solver_solve.restype  = ctypes.c_int
    lib.solver_solve.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.solver_soft_active.restype  = ctypes.c_int
    lib.solver_soft_active.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.contra_explain_soft.restype  = ctypes.c_int
    lib.contra_explain_soft.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.POINTER(ContraSoftDiagResult)
    ]
    lib.contra_soft_diag_free.restype  = None
    lib.contra_soft_diag_free.argtypes = [ctypes.POINTER(ContraSoftDiagResult)]


def _make_ctx(lib):
    ba = lib.zsp_block_alloc_create(None, 4096)
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx
    return ctx_buf, ctx, ba


def test_soft_diag_single_relaxed(libzsp_debug):
    """One soft relaxed due to conflict with hard constraint."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)

    # x in [0, 100]
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)

    # Hard: x >= 20
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 20, 0)))

    # Soft (priority 5): x <= 10  -- conflicts with hard x >= 20
    lib.problem_add_soft_constraint(sp,
        lib.expr_binary(sp, BIN_LTE, vx, lib.expr_const(sp, 10, 0)), 5)

    ctx_buf, ctx, ba = _make_ctx(lib)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = SolveOpts()
    res = lib.solver_solve(ctx, ctypes.byref(opts))
    assert res == SOLVE_OK

    # The soft should be relaxed
    assert lib.solver_soft_active(ctx, 0) == 0, \
        "Soft constraint should be relaxed"

    # Run diagnostic
    diag = ContraSoftDiagResult()
    rc = lib.contra_explain_soft(ctx, sp, None, ctypes.byref(diag))
    assert rc == 0

    assert diag.n_entries == 1, f"Expected 1 entry, got {diag.n_entries}"
    entry = diag.entries[0]
    assert entry.n_conflict_hard > 0, "Should have conflicting hard constraints"

    lib.contra_soft_diag_free(ctypes.byref(diag))
    lib.zsp_block_alloc_destroy(ba)


def test_soft_diag_all_kept(libzsp_debug):
    """No softs relaxed -> n_entries should be 0."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)

    # x in [0, 100]
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)

    # Hard: x >= 10
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 10, 0)))

    # Soft: x <= 50 -- compatible with hard x >= 10
    lib.problem_add_soft_constraint(sp,
        lib.expr_binary(sp, BIN_LTE, vx, lib.expr_const(sp, 50, 0)), 5)

    ctx_buf, ctx, ba = _make_ctx(lib)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = SolveOpts()
    res = lib.solver_solve(ctx, ctypes.byref(opts))
    assert res == SOLVE_OK
    assert lib.solver_soft_active(ctx, 0) == 1

    diag = ContraSoftDiagResult()
    rc = lib.contra_explain_soft(ctx, sp, None, ctypes.byref(diag))
    assert rc == 0
    assert diag.n_entries == 0

    lib.contra_soft_diag_free(ctypes.byref(diag))
    lib.zsp_block_alloc_destroy(ba)


def test_soft_diag_two_relaxed(libzsp_debug):
    """Two softs relaxed due to independent conflicts."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)

    # x in [0, 100], y in [0, 100]
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    lib.problem_add_var(sp, 1, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)
    vy = lib.expr_var(sp, 1)

    # Hard: x >= 30
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 30, 0)))
    # Hard: y >= 40
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vy,
                               lib.expr_const(sp, 40, 0)))

    # Soft (priority 5): x <= 10 -- conflicts with x >= 30
    lib.problem_add_soft_constraint(sp,
        lib.expr_binary(sp, BIN_LTE, vx, lib.expr_const(sp, 10, 0)), 5)
    # Soft (priority 5): y <= 20 -- conflicts with y >= 40
    lib.problem_add_soft_constraint(sp,
        lib.expr_binary(sp, BIN_LTE, vy, lib.expr_const(sp, 20, 0)), 5)

    ctx_buf, ctx, ba = _make_ctx(lib)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = SolveOpts()
    res = lib.solver_solve(ctx, ctypes.byref(opts))
    assert res == SOLVE_OK

    diag = ContraSoftDiagResult()
    rc = lib.contra_explain_soft(ctx, sp, None, ctypes.byref(diag))
    assert rc == 0

    assert diag.n_entries == 2, f"Expected 2 entries, got {diag.n_entries}"

    for i in range(diag.n_entries):
        entry = diag.entries[i]
        assert entry.n_conflict_hard > 0

    lib.contra_soft_diag_free(ctypes.byref(diag))
    lib.zsp_block_alloc_destroy(ba)
