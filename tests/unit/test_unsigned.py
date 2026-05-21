"""Unit tests for unsigned 32-bit variable handling (Sprint 1.1).

Verifies that unsigned 32-bit variables are correctly promoted to tier-1
storage, allowing values above 0x7FFFFFFF to be represented without
int32 overflow.
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

    lib.solver_get_var.restype  = ctypes.c_void_p
    lib.solver_get_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.solver_propagate.restype  = ctypes.c_int
    lib.solver_propagate.argtypes = [ctypes.c_void_p]

    lib.ctx_tighten_lb64.restype  = ctypes.c_int
    lib.ctx_tighten_lb64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int64]
    lib.ctx_tighten_ub64.restype  = ctypes.c_int
    lib.ctx_tighten_ub64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int64]

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


# BinOp enum values
BIN_EQ  = 10
BIN_LT  = 12
BIN_LTE = 13
BIN_GT  = 14
BIN_GTE = 15


def _make_ctx(lib, var_specs):
    """Create a compiled SolveCtx with constraints from the problem.
    var_specs: list of (width, is_signed, lo, hi)
    Returns (sp_buf, ctx_buf, ba, sp, ctx)
    """
    sp_buf  = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()

    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    for i, (width, is_signed, lo, hi) in enumerate(var_specs):
        ref = lib.problem_add_var(sp, i, width, is_signed, lo, hi)
        assert ref != EXPR_NULL

    return sp_buf, ctx_buf, sp


def _compile_and_solve(lib, sp_buf, ctx_buf, sp, seed=0x1234):
    ba  = lib.zsp_block_alloc_create(None, 0)
    assert ba
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx

    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0, f"solver_compile returned {rc}"

    opts = lib._SolveOpts(seed=seed)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    return ba, ctx, result


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

def test_unsigned_32_full_range(libzsp):
    """Unsigned 32-bit var with full [0, 0xFFFFFFFF] domain.
    Constraint: x > 0x80000000. Solution must be > 2^31."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, sp = _make_ctx(lib, [
        (32, 0, 0, 0xFFFFFFFF),  # x: unsigned 32-bit, full range
    ])

    # Add constraint: x > 0x80000000
    v0 = lib.expr_var(sp, 0)
    c0 = lib.expr_const(sp, 0x80000000, 0)
    gt_expr = lib.expr_binary(sp, BIN_GT, v0, c0)
    lib.problem_add_constraint(sp, gt_expr)

    ba, ctx, result = _compile_and_solve(lib, sp_buf, ctx_buf, sp)
    assert result == SOLVE_OK

    val = lib.solver_get_value(ctx, 0)
    assert val > 0x80000000, f"Expected > 0x80000000, got {val:#x}"
    assert val <= 0xFFFFFFFF, f"Expected <= 0xFFFFFFFF, got {val:#x}"

    lib.zsp_block_alloc_destroy(ba)


def test_unsigned_32_comparison(libzsp):
    """Two unsigned 32-bit vars: x < y, both spanning the full range.
    Verify the solution satisfies x < y using unsigned comparison."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, sp = _make_ctx(lib, [
        (32, 0, 0, 0xFFFFFFFE),  # x
        (32, 0, 1, 0xFFFFFFFF),  # y
    ])

    # x < y
    v0 = lib.expr_var(sp, 0)
    v1 = lib.expr_var(sp, 1)
    lt_expr = lib.expr_binary(sp, BIN_LT, v0, v1)
    lib.problem_add_constraint(sp, lt_expr)

    ba, ctx, result = _compile_and_solve(lib, sp_buf, ctx_buf, sp)
    assert result == SOLVE_OK

    x = lib.solver_get_value(ctx, 0)
    y = lib.solver_get_value(ctx, 1)
    assert x < y, f"Expected x < y, got x={x:#x}, y={y:#x}"

    lib.zsp_block_alloc_destroy(ba)


def test_unsigned_32_eq_high_value(libzsp):
    """Unsigned 32-bit var, constrained x == 0xDEADBEEF."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, sp = _make_ctx(lib, [
        (32, 0, 0, 0xFFFFFFFF),  # x
    ])

    v0 = lib.expr_var(sp, 0)
    c0 = lib.expr_const(sp, 0xDEADBEEF, 0)
    eq_expr = lib.expr_binary(sp, BIN_EQ, v0, c0)
    lib.problem_add_constraint(sp, eq_expr)

    ba, ctx, result = _compile_and_solve(lib, sp_buf, ctx_buf, sp)
    assert result == SOLVE_OK

    val = lib.solver_get_value(ctx, 0)
    assert val == 0xDEADBEEF, f"Expected 0xDEADBEEF, got {val:#x}"

    lib.zsp_block_alloc_destroy(ba)


def test_unsigned_32_bounds_read(libzsp):
    """Verify zsp_var_lo64/hi64 read correct bounds for unsigned 32-bit."""
    lib = libzsp
    _setup(lib)

    sp_buf  = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp
    lib.problem_add_var(sp, 0, 32, 0, 0, 0xFFFFFFFF)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert lib.solver_compile(ctx, sp) >= 0

    lo = lib.zsp_var_lo64(ctx, 0)
    hi = lib.zsp_var_hi64(ctx, 0)
    assert lo == 0, f"Expected lo=0, got {lo}"
    assert hi == 0xFFFFFFFF, f"Expected hi=0xFFFFFFFF, got {hi:#x}"

    lib.zsp_block_alloc_destroy(ba)


def test_signed_32_unchanged(libzsp):
    """Signed 32-bit variables should still use tier-0 and work correctly.
    Regression guard for the unsigned promotion change."""
    lib = libzsp
    _setup(lib)

    sp_buf  = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp
    lib.problem_add_var(sp, 0, 32, 1, -100, 100)  # signed 32-bit

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    lo = lib.zsp_var_lo64(ctx, 0)
    hi = lib.zsp_var_hi64(ctx, 0)
    assert lo == -100
    assert hi == 100

    # Verify variable is still tier-0 by checking that the Variable struct
    # has lo/hi set directly (tier-0 characteristic)
    var_ptr = lib.solver_get_var(ctx, 0)
    # Variable struct: lo(4) hi(4) holes_offset(4) width(2) flags(1) _pad(1)
    lo32 = ctypes.c_int32.from_address(var_ptr).value
    hi32 = ctypes.c_int32.from_address(var_ptr + 4).value
    assert lo32 == -100, f"Tier-0 lo should be -100, got {lo32}"
    assert hi32 == 100, f"Tier-0 hi should be 100, got {hi32}"

    lib.zsp_block_alloc_destroy(ba)


def test_unsigned_32_domain_tighten(libzsp):
    """Tightening unsigned 32-bit domain with 64-bit functions works."""
    lib = libzsp
    _setup(lib)

    sp_buf  = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp
    lib.problem_add_var(sp, 0, 32, 0, 0, 0xFFFFFFFF)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert lib.solver_compile(ctx, sp) >= 0

    # Tighten to [0x80000000, 0xFFFFFFFF]
    rc = lib.ctx_tighten_lb64(ctx, 0, 0x80000000)
    assert rc == PROP_OK

    lo = lib.zsp_var_lo64(ctx, 0)
    hi = lib.zsp_var_hi64(ctx, 0)
    assert lo == 0x80000000
    assert hi == 0xFFFFFFFF

    # Further tighten to [0xA0000000, 0xC0000000]
    rc = lib.ctx_tighten_lb64(ctx, 0, 0xA0000000)
    assert rc == PROP_OK
    rc = lib.ctx_tighten_ub64(ctx, 0, 0xC0000000)
    assert rc == PROP_OK

    lo = lib.zsp_var_lo64(ctx, 0)
    hi = lib.zsp_var_hi64(ctx, 0)
    assert lo == 0xA0000000
    assert hi == 0xC0000000

    lib.zsp_block_alloc_destroy(ba)
