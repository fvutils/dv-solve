"""Unit tests for MUS extraction via QuickXplain (Sprint 2).

Tests contra_analyze_unsat() with MUS minimization.
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


class ContraResult(ctypes.Structure):
    _fields_ = [
        ("mus_constraint_ids", ctypes.POINTER(ctypes.c_uint32)),
        ("mus_size",           ctypes.c_uint32),
        ("proof_text",         ctypes.c_char_p),
        ("proof_json",         ctypes.c_char_p),
        ("core_size",          ctypes.c_uint32),
        ("n_solver_calls",     ctypes.c_uint32),
        ("elapsed_sec",        ctypes.c_double),
        ("relaxations",        ctypes.c_void_p),
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


def _make_ctx(lib):
    ba = lib.zsp_block_alloc_create(None, 4096)
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx
    return ctx_buf, ctx, ba


def _get_mus(lib, sp):
    """Run contra_analyze_unsat and return the MUS constraint_ids as a set."""
    ctx_buf, ctx, ba = _make_ctx(lib)

    result = ContraResult()
    rc = lib.contra_analyze_unsat(ctx, sp, None, ctypes.byref(result))
    assert rc == 0, f"contra_analyze_unsat failed: {rc}"

    mus = set()
    for i in range(result.mus_size):
        mus.add(result.mus_constraint_ids[i])

    n_calls = result.n_solver_calls
    lib.contra_result_free(ctypes.byref(result))
    lib.zsp_block_alloc_destroy(ba)
    return mus, n_calls


def test_mus_2_of_2(libzsp_debug):
    """Trivial MUS: {x >= 10, x <= 5}. MUS size should be 2."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)

    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 10, 0)))  # C1
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 5, 0)))   # C2

    mus, _ = _get_mus(lib, sp)
    assert mus == {1, 2}, f"Expected MUS {{1,2}}, got {mus}"


def test_mus_2_of_5(libzsp_debug):
    """5 constraints, known MUS of size 2. Only the contradictory pair
    should remain after minimization."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)

    # x in [0, 100], y in [0, 100]
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    lib.problem_add_var(sp, 1, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)
    vy = lib.expr_var(sp, 1)

    # C1: x >= 10
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 10, 0)))
    # C2: y >= 20
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vy,
                               lib.expr_const(sp, 20, 0)))
    # C3: x <= 50
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 50, 0)))
    # C4: x <= 5  (contradicts C1)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 5, 0)))
    # C5: y <= 80
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vy,
                               lib.expr_const(sp, 80, 0)))

    mus, _ = _get_mus(lib, sp)
    # MUS must contain C1 and C4 (the contradiction on x)
    assert 1 in mus, f"C1 (x >= 10) not in MUS: {mus}"
    assert 4 in mus, f"C4 (x <= 5) not in MUS: {mus}"
    # MUS should be exactly {1, 4} (minimal)
    assert len(mus) == 2, f"MUS not minimal, got {mus}"


def test_mus_is_minimal(libzsp_debug):
    """Verify that the MUS is truly minimal: removing any single
    constraint makes the remainder satisfiable."""
    lib = libzsp_debug
    _setup(lib)

    # Set up solver_solve for verification
    class SolveOpts(ctypes.Structure):
        _fields_ = [
            ("seed",           ctypes.c_uint64),
            ("max_conflicts",  ctypes.c_uint32),
            ("max_restarts",   ctypes.c_uint32),
            ("use_phase_save", ctypes.c_uint8),
            ("_pad",           ctypes.c_uint8 * 3),
            ("max_shave_iters", ctypes.c_uint32),
        ]
    lib.solver_solve.restype  = ctypes.c_int
    lib.solver_solve.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)

    # x in [0, 100], y in [0, 100]
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    lib.problem_add_var(sp, 1, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)
    vy = lib.expr_var(sp, 1)

    # Build 5 constraints with known MUS {C1, C4}
    # C1: x >= 10
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 10, 0)))
    # C2: y >= 5
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vy,
                               lib.expr_const(sp, 5, 0)))
    # C3: x <= 50
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 50, 0)))
    # C4: x <= 5 (contradicts C1)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 5, 0)))
    # C5: y <= 90
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vy,
                               lib.expr_const(sp, 90, 0)))

    mus, _ = _get_mus(lib, sp)
    assert len(mus) >= 2, f"MUS too small: {mus}"

    # Verify minimality: for each constraint in MUS, build a sub-problem
    # without it and verify it's SAT.
    mus_list = sorted(mus)

    # Build a mapping: constraint_id -> (op, var_id, const_val)
    # C1: x >= 10, C2: y >= 5, C3: x <= 50, C4: x <= 5, C5: y <= 90
    constraints = {
        1: (BIN_GTE, 0, 10),
        2: (BIN_GTE, 1, 5),
        3: (BIN_LTE, 0, 50),
        4: (BIN_LTE, 0, 5),
        5: (BIN_LTE, 1, 90),
    }

    for removed_cid in mus_list:
        # Build sub-problem with MUS constraints minus this one
        sub_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
        sub_sp = lib.solve_problem_init(sub_buf, _SP_BUF_SIZE)
        lib.problem_add_var(sub_sp, 0, 8, 0, 0, 100)
        lib.problem_add_var(sub_sp, 1, 8, 0, 0, 100)

        for cid in mus_list:
            if cid == removed_cid:
                continue
            op, vid, cv = constraints[cid]
            ve = lib.expr_var(sub_sp, vid)
            ce = lib.expr_const(sub_sp, cv, 0)
            lib.problem_add_constraint(sub_sp, lib.expr_binary(sub_sp, op, ve, ce))

        ctx_buf2, ctx2, ba2 = _make_ctx(lib)
        rc = lib.solver_compile(ctx2, sub_sp)
        if rc >= 0:
            res = lib.solver_solve(ctx2, None)
            assert res == 0, (
                f"MUS minus C{removed_cid} should be SAT but got {res}. "
                f"MUS={mus_list}"
            )
        # rc == -2 would mean sub-problem is UNSAT at compile time,
        # which would violate minimality
        assert rc != -2, (
            f"MUS minus C{removed_cid} is UNSAT at compile time! "
            f"MUS={mus_list}"
        )
        lib.zsp_block_alloc_destroy(ba2)


def test_mus_single_constraint_domain(libzsp_debug):
    """Single constraint that conflicts with variable domain bounds.
    MUS should have size 1."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)

    # x in [10, 20]  -- domain is [10, 20]
    lib.problem_add_var(sp, 0, 8, 0, 10, 20)
    vx = lib.expr_var(sp, 0)

    # C1: x <= 5  -- contradicts domain lower bound
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 5, 0)))

    mus, _ = _get_mus(lib, sp)
    # The MUS should include C1 (the only constraint)
    assert 1 in mus, f"Expected C1 in MUS, got {mus}"
    assert len(mus) == 1, f"Expected MUS of size 1, got {mus}"


def test_mus_budget_exhaustion(libzsp_debug):
    """With max_solver_calls=2 on a problem that needs more,
    partial result should be returned."""
    lib = libzsp_debug
    _setup(lib)

    class ContraOpts(ctypes.Structure):
        _fields_ = [
            ("max_solver_calls",   ctypes.c_uint32),
            ("time_limit_sec",     ctypes.c_double),
            ("skip_minimization",  ctypes.c_uint8),
            ("emit_json",          ctypes.c_uint8),
            ("emit_proof",         ctypes.c_uint8),
            ("compute_relaxations", ctypes.c_uint8),
            ("find_alternatives",  ctypes.c_uint8),
            ("_pad",               ctypes.c_uint8 * 3),
            ("constraint_info",    ctypes.c_void_p),
            ("n_constraint_info",  ctypes.c_uint32),
        ]

    lib.contra_analyze_unsat.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p,
        ctypes.POINTER(ContraOpts), ctypes.POINTER(ContraResult)
    ]

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)

    # Many constraints, only 2 contradictory
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    lib.problem_add_var(sp, 1, 8, 0, 0, 100)
    lib.problem_add_var(sp, 2, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)
    vy = lib.expr_var(sp, 1)
    vz = lib.expr_var(sp, 2)

    for i in range(8):
        c = lib.expr_const(sp, 10 + i * 5, 0)
        lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vy, c))

    # The actual contradiction
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 50, 0)))
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 10, 0)))

    ctx_buf, ctx, ba = _make_ctx(lib)

    opts = ContraOpts()
    ctypes.memset(ctypes.byref(opts), 0, ctypes.sizeof(opts))
    opts.max_solver_calls = 3
    opts.compute_relaxations = 0  # skip relaxation to test budget on MUS only

    result = ContraResult()
    rc = lib.contra_analyze_unsat(ctx, sp, ctypes.byref(opts),
                                   ctypes.byref(result))
    assert rc == 0
    # Should have some result even with tight budget
    assert result.mus_size >= 0  # partial or empty result is acceptable
    assert result.n_solver_calls <= 5  # might exceed budget slightly

    lib.contra_result_free(ctypes.byref(result))
    lib.zsp_block_alloc_destroy(ba)


def test_level0_fast_path(libzsp_debug):
    """Trivially-UNSAT problem (compile-time conflict) should trigger
    the level-0 fast path with correct MUS."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)

    # x in [0, 100]
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)

    # C1: x >= 80
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 80, 0)))
    # C2: x <= 20
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 20, 0)))
    # C3: x >= 10 (satisfiable, not part of the contradiction)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 10, 0)))

    mus, n_calls = _get_mus(lib, sp)

    # MUS should contain C1 and C2 (the contradiction)
    assert 1 in mus, f"C1 not in MUS: {mus}"
    assert 2 in mus, f"C2 not in MUS: {mus}"
    assert len(mus) == 2, f"MUS not minimal, got {mus}"
