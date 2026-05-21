"""Tests for the backtrack bug fix (L1 + L2 + L3).

Exercises the C solver directly via ctypes to verify:
1. Bidirectional backtrack exclusion (L1)
2. Bounds shaving (L2)
3. Restart defaults (L3)

Each test builds a SolveProblem in the C layer, compiles it, and solves.
"""
from __future__ import annotations

import ctypes

import pytest


# ------------------------------------------------------------------ #
# Constants (must match C headers)                                     #
# ------------------------------------------------------------------ #
EXPR_NULL  = 0xFFFFFFFF
SOLVE_OK   = 0
SOLVE_UNSAT = 1

# BinOp codes (from zsp_problem.h)
BIN_ADD = 0
BIN_EQ  = 10
BIN_LTE = 13


# ------------------------------------------------------------------ #
# ctypes setup                                                         #
# ------------------------------------------------------------------ #

def _setup(lib):
    """Wire argtypes/restypes on the library handle."""
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
    lib.solver_solve.restype  = c.c_int
    lib.solver_solve.argtypes = [c.c_void_p, c.c_void_p]
    lib.solver_get_value.restype  = c.c_int64
    lib.solver_get_value.argtypes = [c.c_void_p, c.c_uint32]


# ------------------------------------------------------------------ #
# SolveOpts (must match zsp_search.h)                                  #
# ------------------------------------------------------------------ #

class SolveOpts(ctypes.Structure):
    _fields_ = [
        ("seed",            ctypes.c_uint64),
        ("max_conflicts",   ctypes.c_uint32),
        ("max_restarts",    ctypes.c_uint32),
        ("use_phase_save",  ctypes.c_uint8),
        ("_pad",            ctypes.c_uint8 * 3),
        ("max_shave_iters", ctypes.c_uint32),
    ]


# ------------------------------------------------------------------ #
# Helpers                                                              #
# ------------------------------------------------------------------ #

def _sp_buf(lib, buf_size=1 << 18):
    buf = (ctypes.c_uint8 * buf_size)()
    sp = lib.solve_problem_init(buf, buf_size)
    assert sp is not None, "solve_problem_init failed"
    return buf, sp


def _add_var(lib, sp, var_id, width, is_signed, lo, hi):
    ref = lib.problem_add_var(sp, var_id, width, is_signed, lo, hi)
    assert ref != EXPR_NULL, f"problem_add_var({var_id}) failed"


def _expr_var(lib, sp, var_id):
    ref = lib.expr_var(sp, var_id)
    assert ref != EXPR_NULL
    return ref


def _expr_const(lib, sp, value, is_signed=0):
    ref = lib.expr_const(sp, value, is_signed)
    assert ref != EXPR_NULL
    return ref


def _expr_binary(lib, sp, op, lhs, rhs):
    ref = lib.expr_binary(sp, op, lhs, rhs)
    assert ref != EXPR_NULL
    return ref


def _add_constraint(lib, sp, root):
    ref = lib.problem_add_constraint(sp, root)
    assert ref != EXPR_NULL


def _make_ctx(lib, sp, buf_size=1 << 20):
    ba = lib.zsp_block_alloc_create(None, buf_size)
    assert ba is not None
    ctx_buf = (ctypes.c_uint8 * buf_size)()
    ctx = lib.solver_create(ctx_buf, buf_size, ba)
    assert ctx is not None
    rc = lib.solver_compile(ctx, sp)
    return ctx_buf, ba, ctx, rc


def _build_reproducer(lib):
    """Build the minimal reproducer: p0<=p1, p0+p1==s01, s01==100."""
    sp_buf, sp = _sp_buf(lib)

    _add_var(lib, sp, 0, 32, 0, 10, 80)   # p0
    _add_var(lib, sp, 1, 32, 0, 10, 80)   # p1
    _add_var(lib, sp, 2, 32, 0, 20, 160)  # s01

    # p0 <= p1
    _add_constraint(lib, sp,
        _expr_binary(lib, sp, BIN_LTE,
                     _expr_var(lib, sp, 0),
                     _expr_var(lib, sp, 1)))

    # s01 == p0 + p1
    _add_constraint(lib, sp,
        _expr_binary(lib, sp, BIN_EQ,
                     _expr_var(lib, sp, 2),
                     _expr_binary(lib, sp, BIN_ADD,
                                  _expr_var(lib, sp, 0),
                                  _expr_var(lib, sp, 1))))

    # s01 == 100
    _add_constraint(lib, sp,
        _expr_binary(lib, sp, BIN_EQ,
                     _expr_var(lib, sp, 2),
                     _expr_const(lib, sp, 100)))

    return sp_buf, sp


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

class TestBacktrackFix:

    @pytest.fixture(autouse=True)
    def setup_lib(self, libzsp):
        _setup(libzsp)
        self.lib = libzsp

    def test_reproducer_le_add_const(self):
        """Minimal reproducer: p0<=p1, p0+p1==100.
        Must produce p0 <= p1 AND p0 + p1 == 100."""
        lib = self.lib
        sp_buf, sp = _build_reproducer(lib)

        for seed in [42, 1, 999, 0xDEAD, 7777, 12345, 65536]:
            ctx_buf, ba, ctx, rc = _make_ctx(lib, sp)
            assert rc == 0, f"seed={seed}: {rc} constraints uncompiled"

            opts = SolveOpts(seed=seed)
            result = lib.solver_solve(ctx, ctypes.byref(opts))
            assert result == SOLVE_OK, f"seed={seed}: solve returned {result}"

            p0 = lib.solver_get_value(ctx, 0)
            p1 = lib.solver_get_value(ctx, 1)
            s01 = lib.solver_get_value(ctx, 2)

            assert p0 <= p1, f"seed={seed}: p0={p0} > p1={p1}"
            assert p0 + p1 == 100, f"seed={seed}: p0+p1={p0+p1} != 100"
            assert s01 == 100, f"seed={seed}: s01={s01}"
            assert 10 <= p0 <= 80, f"seed={seed}: p0={p0} out of range"
            assert 10 <= p1 <= 80, f"seed={seed}: p1={p1} out of range"

            lib.zsp_block_alloc_destroy(ba)

    def test_middle_value_lower_half(self):
        """Valid values all in lower half -- bidirectional backtrack
        must find them."""
        lib = self.lib
        sp_buf, sp = _sp_buf(lib)

        _add_var(lib, sp, 0, 32, 0, 0, 100)  # x
        _add_var(lib, sp, 1, 32, 0, 0, 100)  # y

        # x <= y
        _add_constraint(lib, sp,
            _expr_binary(lib, sp, BIN_LTE,
                         _expr_var(lib, sp, 0),
                         _expr_var(lib, sp, 1)))
        # y == 20
        _add_constraint(lib, sp,
            _expr_binary(lib, sp, BIN_EQ,
                         _expr_var(lib, sp, 1),
                         _expr_const(lib, sp, 20)))

        for seed in [42, 1, 999, 0xBEEF]:
            ctx_buf, ba, ctx, rc = _make_ctx(lib, sp)
            opts = SolveOpts(seed=seed)
            result = lib.solver_solve(ctx, ctypes.byref(opts))
            assert result == SOLVE_OK, f"seed={seed}: {result}"

            x = lib.solver_get_value(ctx, 0)
            y = lib.solver_get_value(ctx, 1)
            assert y == 20, f"seed={seed}: y={y}"
            assert x <= 20, f"seed={seed}: x={x} > 20"

            lib.zsp_block_alloc_destroy(ba)

    def test_middle_value_upper_half(self):
        """Valid values all in upper half -- bidirectional backtrack
        must find them."""
        lib = self.lib
        sp_buf, sp = _sp_buf(lib)

        _add_var(lib, sp, 0, 32, 0, 0, 100)  # x
        _add_var(lib, sp, 1, 32, 0, 0, 100)  # y

        # y <= x
        _add_constraint(lib, sp,
            _expr_binary(lib, sp, BIN_LTE,
                         _expr_var(lib, sp, 1),
                         _expr_var(lib, sp, 0)))
        # y == 80
        _add_constraint(lib, sp,
            _expr_binary(lib, sp, BIN_EQ,
                         _expr_var(lib, sp, 1),
                         _expr_const(lib, sp, 80)))

        for seed in [42, 1, 999]:
            ctx_buf, ba, ctx, rc = _make_ctx(lib, sp)
            opts = SolveOpts(seed=seed)
            result = lib.solver_solve(ctx, ctypes.byref(opts))
            assert result == SOLVE_OK, f"seed={seed}: {result}"

            x = lib.solver_get_value(ctx, 0)
            y = lib.solver_get_value(ctx, 1)
            assert y == 80, f"seed={seed}: y={y}"
            assert x >= 80, f"seed={seed}: x={x} < 80"

            lib.zsp_block_alloc_destroy(ba)

    def test_shaving_tightens_bounds(self):
        """After solving with shaving, p0 must be <= 50."""
        lib = self.lib
        sp_buf, sp = _build_reproducer(lib)

        ctx_buf, ba, ctx, rc = _make_ctx(lib, sp)
        assert rc == 0

        opts = SolveOpts(seed=42, max_shave_iters=1000)
        result = lib.solver_solve(ctx, ctypes.byref(opts))
        assert result == SOLVE_OK

        p0 = lib.solver_get_value(ctx, 0)
        p1 = lib.solver_get_value(ctx, 1)
        assert p0 <= p1
        assert p0 + p1 == 100
        assert p0 <= 50, f"p0={p0} should be <= 50 after shaving"

        lib.zsp_block_alloc_destroy(ba)

    def test_restart_finds_solution(self):
        """With aggressive restarts (max_conflicts=1), the solver should
        still find a valid solution."""
        lib = self.lib
        sp_buf, sp = _build_reproducer(lib)

        ctx_buf, ba, ctx, rc = _make_ctx(lib, sp)
        opts = SolveOpts(seed=42, max_conflicts=1, max_restarts=1000)
        result = lib.solver_solve(ctx, ctypes.byref(opts))
        assert result == SOLVE_OK

        p0 = lib.solver_get_value(ctx, 0)
        p1 = lib.solver_get_value(ctx, 1)
        assert p0 <= p1, f"p0={p0} > p1={p1}"
        assert p0 + p1 == 100, f"p0+p1={p0+p1} != 100"

        lib.zsp_block_alloc_destroy(ba)

    def test_simple_no_regression(self):
        """Simple unconstrained problem still solves instantly."""
        lib = self.lib
        sp_buf, sp = _sp_buf(lib)

        _add_var(lib, sp, 0, 32, 0, 0, 255)
        _add_var(lib, sp, 1, 32, 0, 0, 255)

        ctx_buf, ba, ctx, rc = _make_ctx(lib, sp)
        opts = SolveOpts(seed=42)
        result = lib.solver_solve(ctx, ctypes.byref(opts))
        assert result == SOLVE_OK

        a = lib.solver_get_value(ctx, 0)
        b = lib.solver_get_value(ctx, 1)
        assert 0 <= a <= 255
        assert 0 <= b <= 255

        lib.zsp_block_alloc_destroy(ba)

    def test_unsat_detected(self):
        """Unsatisfiable problem: x in [0,10], x == 20."""
        lib = self.lib
        sp_buf, sp = _sp_buf(lib)

        _add_var(lib, sp, 0, 32, 0, 0, 10)
        _add_constraint(lib, sp,
            _expr_binary(lib, sp, BIN_EQ,
                         _expr_var(lib, sp, 0),
                         _expr_const(lib, sp, 20)))

        ba = lib.zsp_block_alloc_create(None, 1 << 20)
        ctx_buf = (ctypes.c_uint8 * (1 << 20))()
        ctx = lib.solver_create(ctx_buf, 1 << 20, ba)
        rc = lib.solver_compile(ctx, sp)

        if rc == -2:
            # Caught at compile time -- correct
            lib.zsp_block_alloc_destroy(ba)
            return

        opts = SolveOpts(seed=42)
        result = lib.solver_solve(ctx, ctypes.byref(opts))
        assert result == SOLVE_UNSAT, f"Expected UNSAT, got {result}"
        lib.zsp_block_alloc_destroy(ba)
