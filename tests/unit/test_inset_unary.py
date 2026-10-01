"""Unit tests for EXPR_IN_SET, EXPR_IN_RANGE, and EXPR_UNARY compilation.

Tests:
- InSet: x in {10, 20, 30}
- InRange: x in [50, 100]
- Unary NOT of comparison: !(x < 5) -> x >= 5
- Unary NEG in EQ: r == -a
- Unary INVERT in EQ: r == ~a
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL = 0xFFFF_FFFF
SOLVE_OK = 0
BIN_EQ = 10; BIN_LT = 12; BIN_GTE = 15
UN_NEG = 0; UN_NOT = 1; UN_INVERT = 2

_SP = 65536
_CTX = 1 << 20


class SolveOpts(ctypes.Structure):
    _fields_ = [
        ("seed", ctypes.c_uint64),
        ("max_conflicts", ctypes.c_uint32),
        ("max_restarts", ctypes.c_uint32),
        ("use_phase_save", ctypes.c_uint8),
        ("_pad", ctypes.c_uint8 * 3),
        ("max_shave_iters", ctypes.c_uint32),
    ]


def _wire(lib):
    c = ctypes
    lib.dvs_block_alloc_create.restype = c.c_void_p
    lib.dvs_block_alloc_create.argtypes = [c.c_void_p, c.c_size_t]
    lib.dvs_block_alloc_destroy.restype = None
    lib.dvs_block_alloc_destroy.argtypes = [c.c_void_p]
    lib.solve_problem_init.restype = c.c_void_p
    lib.solve_problem_init.argtypes = [c.c_void_p, c.c_size_t]
    lib.problem_add_var.restype = c.c_uint32
    lib.problem_add_var.argtypes = [c.c_void_p, c.c_uint32,
                                    c.c_uint8, c.c_uint8, c.c_int64, c.c_int64]
    lib.problem_add_constraint.restype = c.c_uint32
    lib.problem_add_constraint.argtypes = [c.c_void_p, c.c_uint32]
    lib.expr_var.restype = c.c_uint32
    lib.expr_var.argtypes = [c.c_void_p, c.c_uint32]
    lib.expr_const.restype = c.c_uint32
    lib.expr_const.argtypes = [c.c_void_p, c.c_int64, c.c_uint8]
    lib.expr_binary.restype = c.c_uint32
    lib.expr_binary.argtypes = [c.c_void_p, c.c_int32, c.c_uint32, c.c_uint32]
    lib.expr_unary.restype = c.c_uint32
    lib.expr_unary.argtypes = [c.c_void_p, c.c_int32, c.c_uint32]
    lib.expr_in_set.restype = c.c_uint32
    lib.expr_in_set.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32, c.c_void_p]
    lib.expr_in_range.restype = c.c_uint32
    lib.expr_in_range.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32, c.c_uint32]
    lib.dvs_solver_create.restype = c.c_void_p
    lib.dvs_solver_create.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p]
    lib.dvs_solver_destroy.restype = None
    lib.dvs_solver_destroy.argtypes = [c.c_void_p]
    lib.dvs_solver_compile.restype = c.c_int
    lib.dvs_solver_compile.argtypes = [c.c_void_p, c.c_void_p]
    lib.dvs_solver_solve.restype = c.c_int
    lib.dvs_solver_solve.argtypes = [c.c_void_p, c.c_void_p]
    lib.dvs_solver_get_value.restype = c.c_int64
    lib.dvs_solver_get_value.argtypes = [c.c_void_p, c.c_uint32]


def _solve(lib, sp, n_vars, seed=42):
    ctx_buf = (ctypes.c_uint8 * _CTX)()
    ba = lib.dvs_block_alloc_create(None, _CTX)
    ctx = lib.dvs_solver_create(ctx_buf, _CTX, ba)
    assert ctx
    crc = lib.dvs_solver_compile(ctx, sp)
    assert crc == 0, f"compile returned {crc} (uncompiled constraints)"
    opts = SolveOpts(seed=seed)
    rc = lib.dvs_solver_solve(ctx, ctypes.byref(opts))
    assert rc == SOLVE_OK, f"solve returned {rc}"
    vals = [lib.dvs_solver_get_value(ctx, i) for i in range(n_vars)]
    lib.dvs_solver_destroy(ctx)
    lib.dvs_block_alloc_destroy(ba)
    return vals


# ------------------------------------------------------------------ #
# EXPR_IN_SET                                                         #
# ------------------------------------------------------------------ #

def test_in_set_basic(libdvs):
    """x in {10, 20, 30}. Verify result is one of these values."""
    _wire(libdvs)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libdvs.solve_problem_init(sp_buf, _SP)

    libdvs.problem_add_var(sp, 0, 8, 0, 0, 255)
    elem_refs = [libdvs.expr_const(sp, v, 0) for v in [10, 20, 30]]
    arr = (ctypes.c_uint32 * 3)(*elem_refs)
    eis = libdvs.expr_in_set(sp, libdvs.expr_var(sp, 0), 3,
                              ctypes.cast(arr, ctypes.c_void_p))
    libdvs.problem_add_constraint(sp, eis)

    for seed in range(1, 21):
        vals = _solve(libdvs, sp, 1, seed=seed)
        assert vals[0] in (10, 20, 30), f"x={vals[0]} not in {{10, 20, 30}}"


def test_in_set_singleton(libdvs):
    """x in {42}. Verify x == 42."""
    _wire(libdvs)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libdvs.solve_problem_init(sp_buf, _SP)

    libdvs.problem_add_var(sp, 0, 32, 1, 0, 1000)
    elem_refs = [libdvs.expr_const(sp, 42, 0)]
    arr = (ctypes.c_uint32 * 1)(*elem_refs)
    eis = libdvs.expr_in_set(sp, libdvs.expr_var(sp, 0), 1,
                              ctypes.cast(arr, ctypes.c_void_p))
    libdvs.problem_add_constraint(sp, eis)

    vals = _solve(libdvs, sp, 1)
    assert vals[0] == 42


# ------------------------------------------------------------------ #
# EXPR_IN_RANGE                                                       #
# ------------------------------------------------------------------ #

def test_in_range_basic(libdvs):
    """x in [50, 100]. Verify 50 <= x <= 100."""
    _wire(libdvs)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libdvs.solve_problem_init(sp_buf, _SP)

    libdvs.problem_add_var(sp, 0, 8, 0, 0, 255)
    eir = libdvs.expr_in_range(sp, libdvs.expr_var(sp, 0),
                                libdvs.expr_const(sp, 50, 0),
                                libdvs.expr_const(sp, 100, 0))
    libdvs.problem_add_constraint(sp, eir)

    for seed in range(1, 21):
        vals = _solve(libdvs, sp, 1, seed=seed)
        assert 50 <= vals[0] <= 100, f"x={vals[0]}"


# ------------------------------------------------------------------ #
# EXPR_UNARY                                                           #
# ------------------------------------------------------------------ #

def test_not_comparison(libdvs):
    """!(x < 5) compiles as x >= 5."""
    _wire(libdvs)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libdvs.solve_problem_init(sp_buf, _SP)

    libdvs.problem_add_var(sp, 0, 8, 0, 0, 255)
    # !(x < 5) -> x >= 5
    inner = libdvs.expr_binary(sp, BIN_LT,
                                libdvs.expr_var(sp, 0),
                                libdvs.expr_const(sp, 5, 0))
    neg = libdvs.expr_unary(sp, UN_NOT, inner)
    libdvs.problem_add_constraint(sp, neg)

    for seed in range(1, 21):
        vals = _solve(libdvs, sp, 1, seed=seed)
        assert vals[0] >= 5, f"x={vals[0]}, expected >= 5"


def test_neg_in_eq(libdvs):
    """r == -a. With a pinned to 7, r should be -7."""
    _wire(libdvs)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libdvs.solve_problem_init(sp_buf, _SP)

    libdvs.problem_add_var(sp, 0, 32, 1, -100, 100)   # r
    libdvs.problem_add_var(sp, 1, 32, 1, 7, 7)        # a = 7
    # r == -a
    libdvs.problem_add_constraint(sp,
        libdvs.expr_binary(sp, BIN_EQ,
                           libdvs.expr_var(sp, 0),
                           libdvs.expr_unary(sp, UN_NEG, libdvs.expr_var(sp, 1))))

    vals = _solve(libdvs, sp, 2)
    assert vals[0] == -7, f"r={vals[0]}, expected -7"


def test_invert_in_eq(libdvs):
    """r == ~a. With a = 0, r should be 0xFFFFFFFF (as signed: -1)."""
    _wire(libdvs)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libdvs.solve_problem_init(sp_buf, _SP)

    libdvs.problem_add_var(sp, 0, 32, 1, -2**31, 2**31 - 1)  # r
    libdvs.problem_add_var(sp, 1, 32, 1, 0, 0)                # a = 0
    # r == ~a
    libdvs.problem_add_constraint(sp,
        libdvs.expr_binary(sp, BIN_EQ,
                           libdvs.expr_var(sp, 0),
                           libdvs.expr_unary(sp, UN_INVERT, libdvs.expr_var(sp, 1))))

    vals = _solve(libdvs, sp, 2)
    assert vals[0] == -1, f"r={vals[0]}, expected -1 (~0)"
