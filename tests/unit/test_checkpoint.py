"""Unit tests for checkpoint/restore (Phase S3).

Tests:
- Checkpoint and restore domain changes
- Checkpoint with added variable; restore resets n_vars
- Checkpoint with added constraint; restore deactivates it
- Nested checkpoints (2 levels)
- Solve after restore works correctly
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
    lib.solver_compile.restype  = c.c_int
    lib.solver_compile.argtypes = [c.c_void_p, c.c_void_p]
    lib.solver_add_constraint.restype  = c.c_int
    lib.solver_add_constraint.argtypes = [c.c_void_p, c.c_void_p]
    lib.solver_checkpoint.restype  = c.c_int
    lib.solver_checkpoint.argtypes = [c.c_void_p]
    lib.solver_restore.restype  = None
    lib.solver_restore.argtypes = [c.c_void_p, c.c_uint32]
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
    ba = lib.zsp_block_alloc_create(None, _CTX_BUF_SIZE)
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx is not None
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0
    return ctx, ctx_buf, ba


def _solve(lib, ctx, seed=42):
    opts = _SolveOpts(seed=seed)
    return lib.solver_solve(ctx, ctypes.byref(opts))


class TestCheckpoint:

    def test_checkpoint_restore_domains(self, libzsp):
        """Set x=[0,100]; checkpoint; tighten to [50,60]; restore; verify [0,100]."""
        lib = libzsp
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        lib.problem_add_var(sp, 0, 8, 0, 0, 100)
        ctx, _, ba = _compile(lib, sp)

        # Verify initial bounds
        lo = lib.zsp_var_lo32(ctx, 0)
        hi = lib.zsp_var_hi32(ctx, 0)
        assert lo == 0 and hi == 100

        # Checkpoint
        cp = lib.solver_checkpoint(ctx)
        assert cp == 0

        # Tighten via add_constraint
        aux_buf, aux_sp = _make_problem(lib)
        lib.problem_add_constraint(aux_sp, lib.expr_binary(aux_sp, BIN_GTE,
            lib.expr_var(aux_sp, 0), lib.expr_const(aux_sp, 50, 0)))
        lib.problem_add_constraint(aux_sp, lib.expr_binary(aux_sp, BIN_LTE,
            lib.expr_var(aux_sp, 0), lib.expr_const(aux_sp, 60, 0)))
        lib.solver_add_constraint(ctx, aux_sp)

        lo2 = lib.zsp_var_lo32(ctx, 0)
        hi2 = lib.zsp_var_hi32(ctx, 0)
        assert lo2 >= 50 and hi2 <= 60

        # Restore
        lib.solver_restore(ctx, cp)

        lo3 = lib.zsp_var_lo32(ctx, 0)
        hi3 = lib.zsp_var_hi32(ctx, 0)
        assert lo3 == 0 and hi3 == 100, f"Expected [0,100], got [{lo3},{hi3}]"
        lib.zsp_block_alloc_destroy(ba)

    def test_checkpoint_restore_added_constraint(self, libzsp):
        """Checkpoint; add x<=3; restore; verify constraint no longer enforced."""
        lib = libzsp
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        lib.problem_add_var(sp, 0, 8, 0, 0, 10)
        ctx, _, ba = _compile(lib, sp)

        cp = lib.solver_checkpoint(ctx)

        aux_buf, aux_sp = _make_problem(lib)
        lib.problem_add_constraint(aux_sp, lib.expr_binary(aux_sp, BIN_LTE,
            lib.expr_var(aux_sp, 0), lib.expr_const(aux_sp, 3, 0)))
        lib.solver_add_constraint(ctx, aux_sp)

        hi = lib.zsp_var_hi32(ctx, 0)
        assert hi <= 3

        # Restore
        lib.solver_restore(ctx, cp)

        hi2 = lib.zsp_var_hi32(ctx, 0)
        assert hi2 == 10, f"Expected hi=10 after restore, got {hi2}"

        # Solve should allow values > 3 now
        result = _solve(lib, ctx, seed=100)
        assert result == SOLVE_OK
        lib.zsp_block_alloc_destroy(ba)

    def test_nested_checkpoints(self, libzsp):
        """Checkpoint A; tighten; checkpoint B; tighten more; restore B; restore A."""
        lib = libzsp
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        lib.problem_add_var(sp, 0, 8, 0, 0, 100)
        ctx, _, ba = _compile(lib, sp)

        # Checkpoint A
        cpA = lib.solver_checkpoint(ctx)

        # Tighten to [20, 80]
        aux1_buf, aux1 = _make_problem(lib)
        lib.problem_add_constraint(aux1, lib.expr_binary(aux1, BIN_GTE,
            lib.expr_var(aux1, 0), lib.expr_const(aux1, 20, 0)))
        lib.problem_add_constraint(aux1, lib.expr_binary(aux1, BIN_LTE,
            lib.expr_var(aux1, 0), lib.expr_const(aux1, 80, 0)))
        lib.solver_add_constraint(ctx, aux1)

        lo1 = lib.zsp_var_lo32(ctx, 0)
        hi1 = lib.zsp_var_hi32(ctx, 0)
        assert lo1 >= 20 and hi1 <= 80

        # Checkpoint B
        cpB = lib.solver_checkpoint(ctx)

        # Tighten to [40, 60]
        aux2_buf, aux2 = _make_problem(lib)
        lib.problem_add_constraint(aux2, lib.expr_binary(aux2, BIN_GTE,
            lib.expr_var(aux2, 0), lib.expr_const(aux2, 40, 0)))
        lib.problem_add_constraint(aux2, lib.expr_binary(aux2, BIN_LTE,
            lib.expr_var(aux2, 0), lib.expr_const(aux2, 60, 0)))
        lib.solver_add_constraint(ctx, aux2)

        lo2 = lib.zsp_var_lo32(ctx, 0)
        hi2 = lib.zsp_var_hi32(ctx, 0)
        assert lo2 >= 40 and hi2 <= 60

        # Restore B → back to [20, 80]
        lib.solver_restore(ctx, cpB)
        lo3 = lib.zsp_var_lo32(ctx, 0)
        hi3 = lib.zsp_var_hi32(ctx, 0)
        assert lo3 == 20 and hi3 == 80, f"After restore B: [{lo3},{hi3}]"

        # Restore A → back to [0, 100]
        lib.solver_restore(ctx, cpA)
        lo4 = lib.zsp_var_lo32(ctx, 0)
        hi4 = lib.zsp_var_hi32(ctx, 0)
        assert lo4 == 0 and hi4 == 100, f"After restore A: [{lo4},{hi4}]"
        lib.zsp_block_alloc_destroy(ba)

    def test_solve_after_restore(self, libzsp):
        """Checkpoint; add constraints; solve; restore; add different; solve."""
        lib = libzsp
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        lib.problem_add_var(sp, 0, 8, 0, 0, 100)
        ctx, _, ba = _compile(lib, sp)

        # Checkpoint
        cp = lib.solver_checkpoint(ctx)

        # Add x == 42
        aux1_buf, aux1 = _make_problem(lib)
        lib.problem_add_constraint(aux1, lib.expr_binary(aux1, BIN_EQ,
            lib.expr_var(aux1, 0), lib.expr_const(aux1, 42, 0)))
        lib.solver_add_constraint(ctx, aux1)

        result1 = _solve(lib, ctx, seed=1)
        assert result1 == SOLVE_OK
        val1 = lib.solver_get_value(ctx, 0)
        assert val1 == 42

        # Restore
        lib.solver_restore(ctx, cp)

        # Add x == 77
        aux2_buf, aux2 = _make_problem(lib)
        lib.problem_add_constraint(aux2, lib.expr_binary(aux2, BIN_EQ,
            lib.expr_var(aux2, 0), lib.expr_const(aux2, 77, 0)))
        lib.solver_add_constraint(ctx, aux2)

        result2 = _solve(lib, ctx, seed=2)
        assert result2 == SOLVE_OK
        val2 = lib.solver_get_value(ctx, 0)
        assert val2 == 77, f"Expected 77, got {val2}"
        lib.zsp_block_alloc_destroy(ba)

    def test_checkpoint_with_alldiff(self, libzsp):
        """Checkpoint; add AllDifferent; solve; restore; solve without constraint."""
        lib = libzsp
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        for i in range(3):
            lib.problem_add_var(sp, i, 8, 0, 0, 2)
        ctx, _, ba = _compile(lib, sp)

        cp = lib.solver_checkpoint(ctx)

        # Add AllDifferent → forces a permutation of {0,1,2}
        aux_buf, aux_sp = _make_problem(lib)
        vids = (ctypes.c_uint32 * 3)(0, 1, 2)
        lib.problem_add_all_different(aux_sp, 3, vids)
        lib.solver_add_constraint(ctx, aux_sp)

        result = _solve(lib, ctx, seed=42)
        assert result == SOLVE_OK
        vals = [lib.solver_get_value(ctx, i) for i in range(3)]
        assert len(set(vals)) == 3

        # Restore → AllDifferent should be deactivated
        lib.solver_restore(ctx, cp)

        # Solve again — without AllDifferent, duplicates are allowed
        result2 = _solve(lib, ctx, seed=1)
        assert result2 == SOLVE_OK
        lib.zsp_block_alloc_destroy(ba)
