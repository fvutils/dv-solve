"""Unit tests for constraint relaxation suggestions (Sprint 5).

Tests contra_compute_relaxations() and relaxation output in
contra_analyze_unsat().
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


class ContraResult(ctypes.Structure):
    _fields_ = [
        ("mus_constraint_ids", ctypes.POINTER(ctypes.c_uint32)),
        ("mus_size",           ctypes.c_uint32),
        ("proof_text",         ctypes.c_char_p),
        ("proof_json",         ctypes.c_char_p),
        ("core_size",          ctypes.c_uint32),
        ("n_solver_calls",     ctypes.c_uint32),
        ("elapsed_sec",        ctypes.c_double),
        ("relaxations",        ctypes.POINTER(ContraRelaxSuggestion)),
        ("n_relaxations",      ctypes.c_uint32),
        ("unconfirmed",        ctypes.c_uint8),
        ("_res_pad",           ctypes.c_uint8 * 7),
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

    lib.contra_analyze_unsat.restype  = ctypes.c_int
    lib.contra_analyze_unsat.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.POINTER(ContraResult)
    ]
    lib.contra_result_free.restype  = None
    lib.contra_result_free.argtypes = [ctypes.POINTER(ContraResult)]

    lib.contra_compute_relaxations.restype  = ctypes.c_int
    lib.contra_compute_relaxations.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.c_uint32,
        ctypes.c_void_p, ctypes.POINTER(ContraRelaxSuggestion)
    ]


def _make_ctx(lib):
    ba = lib.zsp_block_alloc_create(None, 4096)
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx
    return ctx_buf, ctx, ba


def test_relax_le_simple(libzsp_debug):
    """MUS {x >= 8, x <= 5}. Relaxation of x <= 5 -> x <= 8, delta = +3."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)

    # C1: x >= 8
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 8, 0)))
    # C2: x <= 5
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 5, 0)))

    ctx_buf, ctx, ba = _make_ctx(lib)
    result = ContraResult()
    rc = lib.contra_analyze_unsat(ctx, sp, None, ctypes.byref(result))
    assert rc == 0

    assert result.mus_size == 2
    assert result.n_relaxations == 2

    # Find the relaxation suggestion for C2 (x <= 5)
    for i in range(result.n_relaxations):
        r = result.relaxations[i]
        if r.constraint_id == 2:  # C2: x <= 5
            assert r.is_relaxable == 1
            assert r.original_constant == 5
            assert r.relaxed_constant >= 8, \
                f"Expected relaxed >= 8, got {r.relaxed_constant}"
            assert r.delta >= 3, f"Expected delta >= 3, got {r.delta}"
            break
    else:
        pytest.fail("No relaxation found for C2")

    lib.contra_result_free(ctypes.byref(result))
    lib.zsp_block_alloc_destroy(ba)


def test_relax_ge_simple(libzsp_debug):
    """MUS {x >= 8, x <= 5}. Relaxation of x >= 8 -> x >= 5, delta = -3."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)

    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 8, 0)))  # C1
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 5, 0)))  # C2

    ctx_buf, ctx, ba = _make_ctx(lib)
    result = ContraResult()
    rc = lib.contra_analyze_unsat(ctx, sp, None, ctypes.byref(result))
    assert rc == 0

    # Find relaxation for C1 (x >= 8)
    for i in range(result.n_relaxations):
        r = result.relaxations[i]
        if r.constraint_id == 1:  # C1: x >= 8
            assert r.is_relaxable == 1
            assert r.original_constant == 8
            assert r.relaxed_constant <= 5, \
                f"Expected relaxed <= 5, got {r.relaxed_constant}"
            assert r.delta <= -3, f"Expected delta <= -3, got {r.delta}"
            break
    else:
        pytest.fail("No relaxation found for C1")

    lib.contra_result_free(ctypes.byref(result))
    lib.zsp_block_alloc_destroy(ba)


def test_relax_all_mus_constraints(libzsp_debug):
    """Every relaxable MUS constraint should have a relaxation suggestion."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)

    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 20, 0)))  # C1
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 10, 0)))  # C2

    ctx_buf, ctx, ba = _make_ctx(lib)
    result = ContraResult()
    rc = lib.contra_analyze_unsat(ctx, sp, None, ctypes.byref(result))
    assert rc == 0

    assert result.n_relaxations == result.mus_size
    for i in range(result.n_relaxations):
        r = result.relaxations[i]
        assert r.constraint_id > 0, f"Relaxation {i} has no constraint_id"
        assert r.is_relaxable == 1, \
            f"Constraint {r.constraint_id} should be relaxable"
        assert r.delta != 0, \
            f"Constraint {r.constraint_id} has zero delta"

    lib.contra_result_free(ctypes.byref(result))
    lib.zsp_block_alloc_destroy(ba)


def test_relax_in_result(libzsp_debug):
    """contra_analyze_unsat with default opts includes relaxations."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)

    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 15, 0)))
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 3, 0)))

    ctx_buf, ctx, ba = _make_ctx(lib)
    result = ContraResult()
    rc = lib.contra_analyze_unsat(ctx, sp, None, ctypes.byref(result))
    assert rc == 0

    # With default opts (NULL), relaxations should be computed
    assert result.n_relaxations > 0, "Expected relaxations in result"
    assert result.relaxations is not None

    lib.contra_result_free(ctypes.byref(result))
    lib.zsp_block_alloc_destroy(ba)
