"""Unit tests for incremental constraint addition (Phase S2).

Tests:
- Add a new variable after compile
- Add a constraint that tightens a domain
- Add a constraint that causes UNSAT
- Add multiple constraints incrementally
- Add AllDifferent after compile
- Variable capacity overflow detection
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL     = 0xFFFF_FFFF
SOLVE_OK      = 0
SOLVE_UNSAT   = 1
BIN_LTE       = 13
BIN_GTE       = 15
BIN_EQ        = 10

_CTX_BUF_SIZE = 1 << 20


class _SolveOpts(ctypes.Structure):
    _fields_ = [
        ("seed",           ctypes.c_uint64),
        ("max_conflicts",  ctypes.c_uint32),
        ("max_restarts",   ctypes.c_uint32),
        ("use_phase_save", ctypes.c_uint8),
        ("_pad",           ctypes.c_uint8 * 3),
        ("max_shave_iters", ctypes.c_uint32),
    ]


def _setup_lib(lib):
    c = ctypes
    lib.zsp_block_alloc_create.restype  = c.c_void_p
    lib.zsp_block_alloc_create.argtypes = [c.c_void_p, c.c_size_t]
    lib.zsp_block_alloc_destroy.restype  = None
    lib.zsp_block_alloc_destroy.argtypes = [c.c_void_p]

    lib.solve_problem_init.restype  = c.c_void_p
    lib.solve_problem_init.argtypes = [c.c_void_p, c.c_size_t]
    lib.solve_problem_reset.restype  = None
    lib.solve_problem_reset.argtypes = [c.c_void_p]
    lib.problem_add_var.restype  = c.c_uint32
    lib.problem_add_var.argtypes = [c.c_void_p, c.c_uint32,
                                    c.c_uint8, c.c_uint8,
                                    c.c_int64, c.c_int64]
    lib.problem_add_constraint.restype  = c.c_uint32
    lib.problem_add_constraint.argtypes = [c.c_void_p, c.c_uint32]
    lib.problem_add_all_different.restype  = c.c_uint32
    lib.problem_add_all_different.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p]

    lib.expr_const.restype  = c.c_uint32
    lib.expr_const.argtypes = [c.c_void_p, c.c_int64, c.c_uint8]
    lib.expr_var.restype  = c.c_uint32
    lib.expr_var.argtypes = [c.c_void_p, c.c_uint32]
    lib.expr_binary.restype  = c.c_uint32
    lib.expr_binary.argtypes = [c.c_void_p, c.c_int32, c.c_uint32, c.c_uint32]

    lib.solver_create.restype  = c.c_void_p
    lib.solver_create.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p]
    lib.solver_destroy.restype  = None
    lib.solver_destroy.argtypes = [c.c_void_p]
    lib.solver_compile.restype  = c.c_int
    lib.solver_compile.argtypes = [c.c_void_p, c.c_void_p]
    lib.solver_add_constraint.restype  = c.c_int
    lib.solver_add_constraint.argtypes = [c.c_void_p, c.c_void_p]
    lib.solver_solve.restype  = c.c_int
    lib.solver_solve.argtypes = [c.c_void_p, c.c_void_p]
    lib.solver_get_value.restype  = c.c_int64
    lib.solver_get_value.argtypes = [c.c_void_p, c.c_uint32]
    lib.zsp_var_lo32.restype  = c.c_int32
    lib.zsp_var_lo32.argtypes = [c.c_void_p, c.c_uint32]
    lib.zsp_var_hi32.restype  = c.c_int32
    lib.zsp_var_hi32.argtypes = [c.c_void_p, c.c_uint32]


def _make_problem(lib, buf_size=65536):
    buf = (ctypes.c_uint8 * buf_size)()
    sp = lib.solve_problem_init(buf, buf_size)
    assert sp is not None
    return buf, sp


def _compile(lib, sp):
    """Compile a SolveProblem; return (ctx, ctx_buf, ba)."""
    ba = lib.zsp_block_alloc_create(None, _CTX_BUF_SIZE)
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx is not None
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0, f"solver_compile failed with rc={rc}"
    return ctx, ctx_buf, ba


def _solve(lib, ctx, seed=42):
    opts = _SolveOpts(seed=seed)
    return lib.solver_solve(ctx, ctypes.byref(opts))


class TestIncremental:

    def test_add_constraint_tightens_domain(self, libzsp):
        """Compile x in [0,100]; add x <= 10; solve gives x in [0,10]."""
        lib = libzsp
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        lib.problem_add_var(sp, 0, 8, 0, 0, 100)

        ctx, _, ba = _compile(lib, sp)

        # Build aux problem with constraint: x <= 10
        aux_buf, aux_sp = _make_problem(lib)
        vref = lib.expr_var(aux_sp, 0)
        cref = lib.expr_const(aux_sp, 10, 0)
        lib.problem_add_constraint(aux_sp, lib.expr_binary(aux_sp, BIN_LTE, vref, cref))

        rc = lib.solver_add_constraint(ctx, aux_sp)
        assert rc >= 0, f"solver_add_constraint failed: {rc}"

        # Verify domain tightened
        hi = lib.zsp_var_hi32(ctx, 0)
        assert hi <= 10, f"Expected hi <= 10, got {hi}"

        result = _solve(lib, ctx)
        assert result == SOLVE_OK
        val = lib.solver_get_value(ctx, 0)
        assert 0 <= val <= 10, f"Expected value in [0,10], got {val}"
        lib.zsp_block_alloc_destroy(ba)

    def test_add_constraint_unsat(self, libzsp):
        """Compile x in [0,5]; add x >= 10; returns -2 (UNSAT)."""
        lib = libzsp
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        lib.problem_add_var(sp, 0, 8, 0, 0, 5)

        ctx, _, ba = _compile(lib, sp)

        aux_buf, aux_sp = _make_problem(lib)
        vref = lib.expr_var(aux_sp, 0)
        cref = lib.expr_const(aux_sp, 10, 0)
        lib.problem_add_constraint(aux_sp, lib.expr_binary(aux_sp, BIN_GTE, vref, cref))

        rc = lib.solver_add_constraint(ctx, aux_sp)
        assert rc == -2, f"Expected -2 (UNSAT), got {rc}"
        lib.zsp_block_alloc_destroy(ba)

    def test_add_var_after_compile(self, libzsp):
        """Compile with 2 vars; add 3rd var via aux_sp; solve uses all 3."""
        lib = libzsp
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        lib.problem_add_var(sp, 0, 8, 0, 0, 10)
        lib.problem_add_var(sp, 1, 8, 0, 0, 10)

        ctx, _, ba = _compile(lib, sp)

        # Add var 2 with constraint: v2 == 7
        aux_buf, aux_sp = _make_problem(lib)
        lib.problem_add_var(aux_sp, 2, 8, 0, 0, 20)
        vref = lib.expr_var(aux_sp, 2)
        cref = lib.expr_const(aux_sp, 7, 0)
        lib.problem_add_constraint(aux_sp, lib.expr_binary(aux_sp, BIN_EQ, vref, cref))

        rc = lib.solver_add_constraint(ctx, aux_sp)
        assert rc >= 0, f"solver_add_constraint failed: {rc}"

        result = _solve(lib, ctx)
        assert result == SOLVE_OK
        val = lib.solver_get_value(ctx, 2)
        assert val == 7, f"Expected 7, got {val}"
        lib.zsp_block_alloc_destroy(ba)

    def test_add_multiple_constraints(self, libzsp):
        """Compile base; add 3 constraints incrementally; final solve correct."""
        lib = libzsp
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        lib.problem_add_var(sp, 0, 8, 0, 0, 100)

        ctx, _, ba = _compile(lib, sp)

        # Add: x >= 20
        aux1_buf, aux1 = _make_problem(lib)
        lib.problem_add_constraint(aux1, lib.expr_binary(aux1, BIN_GTE,
            lib.expr_var(aux1, 0), lib.expr_const(aux1, 20, 0)))
        rc = lib.solver_add_constraint(ctx, aux1)
        assert rc >= 0

        # Add: x <= 50
        aux2_buf, aux2 = _make_problem(lib)
        lib.problem_add_constraint(aux2, lib.expr_binary(aux2, BIN_LTE,
            lib.expr_var(aux2, 0), lib.expr_const(aux2, 50, 0)))
        rc = lib.solver_add_constraint(ctx, aux2)
        assert rc >= 0

        result = _solve(lib, ctx)
        assert result == SOLVE_OK
        val = lib.solver_get_value(ctx, 0)
        assert 20 <= val <= 50, f"Expected [20,50], got {val}"
        lib.zsp_block_alloc_destroy(ba)

    def test_add_alldiff_after_compile(self, libzsp):
        """Compile 3 vars; add AllDifferent incrementally; solve distinct."""
        lib = libzsp
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        for i in range(3):
            lib.problem_add_var(sp, i, 8, 0, 0, 2)

        ctx, _, ba = _compile(lib, sp)

        # Add AllDifferent
        aux_buf, aux_sp = _make_problem(lib)
        vids = (ctypes.c_uint32 * 3)(0, 1, 2)
        lib.problem_add_all_different(aux_sp, 3, vids)

        rc = lib.solver_add_constraint(ctx, aux_sp)
        assert rc >= 0

        result = _solve(lib, ctx)
        assert result == SOLVE_OK
        vals = [lib.solver_get_value(ctx, i) for i in range(3)]
        assert len(set(vals)) == 3, f"Not all distinct: {vals}"
        lib.zsp_block_alloc_destroy(ba)

    def test_var_capacity_overflow(self, libzsp):
        """Try to add var_id >= capacity; returns -1."""
        lib = libzsp
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        lib.problem_add_var(sp, 0, 8, 0, 0, 10)

        ctx, _, ba = _compile(lib, sp)

        # Add var with id way beyond capacity (1 + 64 slack = 65 max)
        aux_buf, aux_sp = _make_problem(lib)
        lib.problem_add_var(aux_sp, 200, 8, 0, 0, 10)

        rc = lib.solver_add_constraint(ctx, aux_sp)
        assert rc == -1, f"Expected -1 (capacity overflow), got {rc}"
        lib.zsp_block_alloc_destroy(ba)
