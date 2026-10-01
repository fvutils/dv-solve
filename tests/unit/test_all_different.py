"""Unit tests for the AllDifferent propagator (Phase S1).

Tests:
- 3 vars [0,2]: solve produces 3 distinct values
- 3 vars [0,3]: solve with 20 seeds all produce distinct values
- 3 vars [0,1] (pigeonhole): SOLVE_UNSAT
- AllDifferent with InSet
- Singleton exclusion (one var fixed, others avoid it)
- 8 vars [0,7]: produces a permutation
- AllDifferent via builder → compile → solve
"""
from __future__ import annotations

import ctypes
import pytest

# ------------------------------------------------------------------ #
# Constants                                                            #
# ------------------------------------------------------------------ #
EXPR_NULL     = 0xFFFF_FFFF
SOLVE_OK      = 0
SOLVE_UNSAT   = 1

_CTX_BUF_SIZE = 1 << 20   # 1 MiB


def _setup_lib(lib):
    """Wire argtypes/restypes needed by these tests."""
    c = ctypes

    lib.dvs_block_alloc_create.restype  = c.c_void_p
    lib.dvs_block_alloc_create.argtypes = [c.c_void_p, c.c_size_t]
    lib.dvs_block_alloc_destroy.restype  = None
    lib.dvs_block_alloc_destroy.argtypes = [c.c_void_p]

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
    lib.expr_in_set.restype  = c.c_uint32
    lib.expr_in_set.argtypes = [c.c_void_p, c.c_uint32,
                                c.c_uint32, c.c_void_p]

    lib.dvs_solver_create.restype  = c.c_void_p
    lib.dvs_solver_create.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p]
    lib.dvs_solver_destroy.restype  = None
    lib.dvs_solver_destroy.argtypes = [c.c_void_p]
    lib.dvs_solver_compile.restype  = c.c_int
    lib.dvs_solver_compile.argtypes = [c.c_void_p, c.c_void_p]
    lib.dvs_solver_solve.restype  = c.c_int
    lib.dvs_solver_solve.argtypes = [c.c_void_p, c.c_void_p]
    lib.dvs_solver_get_value.restype  = c.c_int64
    lib.dvs_solver_get_value.argtypes = [c.c_void_p, c.c_uint32]

    # Builder
    lib.dvs_builder_create.restype  = c.c_void_p
    lib.dvs_builder_create.argtypes = [c.c_uint32, c.c_void_p]
    lib.dvs_builder_destroy.restype  = None
    lib.dvs_builder_destroy.argtypes = [c.c_void_p]
    lib.dvs_builder_finalize.restype  = c.c_void_p
    lib.dvs_builder_finalize.argtypes = [c.c_void_p, c.POINTER(c.c_size_t)]
    lib.dvs_builder_free_problem.restype  = None
    lib.dvs_builder_free_problem.argtypes = [c.c_void_p, c.c_void_p, c.c_size_t]
    lib.dvs_builder_add_var.restype  = c.c_uint32
    lib.dvs_builder_add_var.argtypes = [c.c_void_p, c.c_uint32,
                                    c.c_uint8, c.c_uint8,
                                    c.c_int64, c.c_int64]
    lib.dvs_builder_add_constraint.restype  = c.c_uint32
    lib.dvs_builder_add_constraint.argtypes = [c.c_void_p, c.c_uint32]
    lib.dvs_builder_add_all_different.restype  = c.c_uint32
    lib.dvs_builder_add_all_different.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p]
    lib.dvs_builder_expr_const.restype  = c.c_uint32
    lib.dvs_builder_expr_const.argtypes = [c.c_void_p, c.c_int64, c.c_uint8]
    lib.dvs_builder_expr_var.restype  = c.c_uint32
    lib.dvs_builder_expr_var.argtypes = [c.c_void_p, c.c_uint32]
    lib.dvs_builder_expr_binary.restype  = c.c_uint32
    lib.dvs_builder_expr_binary.argtypes = [c.c_void_p, c.c_uint32,
                                        c.c_uint32, c.c_uint32]
    lib.dvs_builder_expr_in_set.restype  = c.c_uint32
    lib.dvs_builder_expr_in_set.argtypes = [c.c_void_p, c.c_uint32,
                                        c.c_uint32, c.c_void_p]


class _SolveOpts(ctypes.Structure):
    _fields_ = [
        ("seed",           ctypes.c_uint64),
        ("max_conflicts",  ctypes.c_uint32),
        ("max_restarts",   ctypes.c_uint32),
        ("use_phase_save", ctypes.c_uint8),
        ("_pad",           ctypes.c_uint8 * 3),
        ("max_shave_iters", ctypes.c_uint32),
    ]


def _make_problem(lib, buf_size=65536):
    buf = (ctypes.c_uint8 * buf_size)()
    sp = lib.solve_problem_init(buf, buf_size)
    assert sp is not None
    return buf, sp


def _solve(lib, sp, seed=42):
    """Compile + solve a SolveProblem; return (result, ctx, ctx_buf, ba)."""
    ba = lib.dvs_block_alloc_create(None, _CTX_BUF_SIZE)
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ctx = lib.dvs_solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx is not None

    rc = lib.dvs_solver_compile(ctx, sp)
    assert rc >= 0, f"dvs_solver_compile failed with rc={rc}"

    opts = _SolveOpts(seed=seed)
    result = lib.dvs_solver_solve(ctx, ctypes.byref(opts))
    return result, ctx, ctx_buf, ba


def _cleanup(lib, ba):
    lib.dvs_block_alloc_destroy(ba)


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

class TestAllDifferent:
    """Tests for the AllDifferent propagator."""

    def test_alldiff_3_vars_disjoint(self, libdvs):
        """3 vars [0,2]; solve produces 3 distinct values."""
        lib = libdvs
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        for i in range(3):
            lib.problem_add_var(sp, i, 8, 0, 0, 2)

        vids = (ctypes.c_uint32 * 3)(0, 1, 2)
        ref = lib.problem_add_all_different(sp, 3, vids)
        assert ref != EXPR_NULL

        result, ctx, _, ba = _solve(lib, sp, seed=1)
        assert result == SOLVE_OK

        vals = [lib.dvs_solver_get_value(ctx, i) for i in range(3)]
        assert len(set(vals)) == 3, f"Not all distinct: {vals}"
        assert all(0 <= v <= 2 for v in vals)
        _cleanup(lib, ba)

    def test_alldiff_3_vars_domain_4(self, libdvs):
        """3 vars [0,3]; 20 seeds all produce distinct values."""
        lib = libdvs
        _setup_lib(lib)

        for seed in range(1, 21):
            buf, sp = _make_problem(lib)
            for i in range(3):
                lib.problem_add_var(sp, i, 8, 0, 0, 3)

            vids = (ctypes.c_uint32 * 3)(0, 1, 2)
            lib.problem_add_all_different(sp, 3, vids)

            result, ctx, _, ba = _solve(lib, sp, seed=seed)
            assert result == SOLVE_OK

            vals = [lib.dvs_solver_get_value(ctx, i) for i in range(3)]
            assert len(set(vals)) == 3, f"seed={seed}: not all distinct: {vals}"
            _cleanup(lib, ba)

    def test_alldiff_conflict_pigeonhole(self, libdvs):
        """3 vars [0,1] (only 2 values for 3 vars); SOLVE_UNSAT."""
        lib = libdvs
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        for i in range(3):
            lib.problem_add_var(sp, i, 8, 0, 0, 1)

        vids = (ctypes.c_uint32 * 3)(0, 1, 2)
        lib.problem_add_all_different(sp, 3, vids)

        result, ctx, _, ba = _solve(lib, sp)
        assert result == SOLVE_UNSAT
        _cleanup(lib, ba)

    def test_alldiff_with_bounds(self, libdvs):
        """2 vars with tight domains [1,3]; AllDifferent; distinct values."""
        lib = libdvs
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        # Two vars with the same domain [1,3]
        lib.problem_add_var(sp, 0, 8, 0, 1, 3)
        lib.problem_add_var(sp, 1, 8, 0, 1, 3)

        vids = (ctypes.c_uint32 * 2)(0, 1)
        lib.problem_add_all_different(sp, 2, vids)

        result, ctx, _, ba = _solve(lib, sp, seed=7)
        assert result == SOLVE_OK

        v0 = lib.dvs_solver_get_value(ctx, 0)
        v1 = lib.dvs_solver_get_value(ctx, 1)
        assert v0 != v1, f"Values are equal: {v0}"
        assert 1 <= v0 <= 3
        assert 1 <= v1 <= 3
        _cleanup(lib, ba)

    def test_alldiff_singleton_exclusion(self, libdvs):
        """3 vars; first fixed to 5; other two must avoid 5."""
        lib = libdvs
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        lib.problem_add_var(sp, 0, 8, 0, 5, 5)   # fixed to 5
        lib.problem_add_var(sp, 1, 8, 0, 3, 7)
        lib.problem_add_var(sp, 2, 8, 0, 3, 7)

        vids = (ctypes.c_uint32 * 3)(0, 1, 2)
        lib.problem_add_all_different(sp, 3, vids)

        result, ctx, _, ba = _solve(lib, sp, seed=42)
        assert result == SOLVE_OK

        v0 = lib.dvs_solver_get_value(ctx, 0)
        v1 = lib.dvs_solver_get_value(ctx, 1)
        v2 = lib.dvs_solver_get_value(ctx, 2)
        assert v0 == 5
        assert v1 != 5 and v2 != 5, f"Singleton exclusion failed: v1={v1}, v2={v2}"
        assert v1 != v2, f"v1 == v2 == {v1}"
        _cleanup(lib, ba)

    def test_alldiff_8_vars(self, libdvs):
        """8 vars [0,7]; all distinct; verify permutation."""
        lib = libdvs
        _setup_lib(lib)

        buf, sp = _make_problem(lib)
        for i in range(8):
            lib.problem_add_var(sp, i, 8, 0, 0, 7)

        vids = (ctypes.c_uint32 * 8)(*range(8))
        lib.problem_add_all_different(sp, 8, vids)

        result, ctx, _, ba = _solve(lib, sp, seed=123)
        assert result == SOLVE_OK

        vals = [lib.dvs_solver_get_value(ctx, i) for i in range(8)]
        assert sorted(vals) == list(range(8)), f"Not a permutation: {vals}"
        _cleanup(lib, ba)

    def test_alldiff_via_builder(self, libdvs):
        """Build via SolveProblemBuilder, finalize, compile, solve."""
        lib = libdvs
        _setup_lib(lib)

        b = lib.dvs_builder_create(4096, None)
        assert b is not None

        for i in range(4):
            lib.dvs_builder_add_var(b, i, 8, 0, 0, 3)

        vids = (ctypes.c_uint32 * 4)(*range(4))
        ref = lib.dvs_builder_add_all_different(b, 4, vids)
        assert ref != EXPR_NULL

        sz = ctypes.c_size_t(0)
        sp_ptr = lib.dvs_builder_finalize(b, ctypes.byref(sz))
        assert sp_ptr is not None

        # Copy to Python-owned buffer
        buf = (ctypes.c_uint8 * sz.value)()
        ctypes.memmove(buf, sp_ptr, sz.value)
        lib.dvs_builder_free_problem(b, sp_ptr, sz.value)
        lib.dvs_builder_destroy(b)

        sp = ctypes.cast(buf, ctypes.c_void_p).value
        result, ctx, _, ba = _solve(lib, sp, seed=55)
        assert result == SOLVE_OK

        vals = [lib.dvs_solver_get_value(ctx, i) for i in range(4)]
        assert sorted(vals) == list(range(4)), f"Not a permutation: {vals}"
        _cleanup(lib, ba)
