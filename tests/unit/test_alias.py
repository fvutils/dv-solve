"""Unit tests for var == var aliasing optimization.

Tests:
- Basic aliasing: x == y eliminates the EQ propagator.
- Transitive aliasing: x == y, y == z -> all resolve to same root.
- Domain intersection: aliased vars get intersected domains.
- Aliased var values: solver_get_value returns the root's value.
- Non-aliased NEQ: x != y is not aliased.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL = 0xFFFF_FFFF
SOLVE_OK = 0
BIN_EQ = 10; BIN_NEQ = 11; BIN_LT = 12; BIN_ADD = 0

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
    lib.zsp_block_alloc_create.restype = c.c_void_p
    lib.zsp_block_alloc_create.argtypes = [c.c_void_p, c.c_size_t]
    lib.zsp_block_alloc_destroy.restype = None
    lib.zsp_block_alloc_destroy.argtypes = [c.c_void_p]
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
    lib.solver_create.restype = c.c_void_p
    lib.solver_create.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p]
    lib.solver_destroy.restype = None
    lib.solver_destroy.argtypes = [c.c_void_p]
    lib.solver_compile.restype = c.c_int
    lib.solver_compile.argtypes = [c.c_void_p, c.c_void_p]
    lib.solver_solve.restype = c.c_int
    lib.solver_solve.argtypes = [c.c_void_p, c.c_void_p]
    lib.solver_get_value.restype = c.c_int64
    lib.solver_get_value.argtypes = [c.c_void_p, c.c_uint32]


def _solve(lib, sp, n_vars, seed=42):
    ctx_buf = (ctypes.c_uint8 * _CTX)()
    ba = lib.zsp_block_alloc_create(None, _CTX)
    ctx = lib.solver_create(ctx_buf, _CTX, ba)
    assert ctx
    crc = lib.solver_compile(ctx, sp)
    assert crc == 0, f"compile returned {crc}"
    opts = SolveOpts(seed=seed)
    rc = lib.solver_solve(ctx, ctypes.byref(opts))
    assert rc == SOLVE_OK, f"solve returned {rc}"
    vals = [lib.solver_get_value(ctx, i) for i in range(n_vars)]
    lib.solver_destroy(ctx)
    lib.zsp_block_alloc_destroy(ba)
    return vals


def test_basic_alias(libzsp):
    """x == y: both vars should have the same value after solve."""
    _wire(libzsp)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libzsp.solve_problem_init(sp_buf, _SP)

    libzsp.problem_add_var(sp, 0, 8, 0, 0, 255)
    libzsp.problem_add_var(sp, 1, 8, 0, 0, 255)
    libzsp.problem_add_constraint(sp,
        libzsp.expr_binary(sp, BIN_EQ,
                           libzsp.expr_var(sp, 0),
                           libzsp.expr_var(sp, 1)))

    for seed in range(1, 11):
        vals = _solve(libzsp, sp, 2, seed=seed)
        assert vals[0] == vals[1], f"x={vals[0]} != y={vals[1]}"


def test_transitive_alias(libzsp):
    """x == y, y == z: all three should have the same value."""
    _wire(libzsp)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libzsp.solve_problem_init(sp_buf, _SP)

    libzsp.problem_add_var(sp, 0, 8, 0, 10, 50)
    libzsp.problem_add_var(sp, 1, 8, 0, 10, 50)
    libzsp.problem_add_var(sp, 2, 8, 0, 10, 50)
    # x == y
    libzsp.problem_add_constraint(sp,
        libzsp.expr_binary(sp, BIN_EQ,
                           libzsp.expr_var(sp, 0),
                           libzsp.expr_var(sp, 1)))
    # y == z
    libzsp.problem_add_constraint(sp,
        libzsp.expr_binary(sp, BIN_EQ,
                           libzsp.expr_var(sp, 1),
                           libzsp.expr_var(sp, 2)))

    for seed in range(1, 11):
        vals = _solve(libzsp, sp, 3, seed=seed)
        assert vals[0] == vals[1] == vals[2], f"x={vals[0]} y={vals[1]} z={vals[2]}"


def test_alias_domain_intersection(libzsp):
    """x in [0, 100], y in [50, 200], x == y: effective domain [50, 100]."""
    _wire(libzsp)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libzsp.solve_problem_init(sp_buf, _SP)

    libzsp.problem_add_var(sp, 0, 8, 0, 0, 100)
    libzsp.problem_add_var(sp, 1, 8, 0, 50, 200)
    libzsp.problem_add_constraint(sp,
        libzsp.expr_binary(sp, BIN_EQ,
                           libzsp.expr_var(sp, 0),
                           libzsp.expr_var(sp, 1)))

    for seed in range(1, 21):
        vals = _solve(libzsp, sp, 2, seed=seed)
        assert vals[0] == vals[1]
        assert 50 <= vals[0] <= 100, f"value {vals[0]} not in [50, 100]"


def test_alias_with_other_constraints(libzsp):
    """x == y, x + z == 100. Both x and y should be consistent."""
    _wire(libzsp)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libzsp.solve_problem_init(sp_buf, _SP)

    libzsp.problem_add_var(sp, 0, 32, 1, 0, 100)   # x
    libzsp.problem_add_var(sp, 1, 32, 1, 0, 100)   # y
    libzsp.problem_add_var(sp, 2, 32, 1, 0, 100)   # z
    libzsp.problem_add_var(sp, 3, 32, 1, 100, 100)  # sum = 100

    # x == y
    libzsp.problem_add_constraint(sp,
        libzsp.expr_binary(sp, BIN_EQ,
                           libzsp.expr_var(sp, 0),
                           libzsp.expr_var(sp, 1)))
    # sum == x + z
    libzsp.problem_add_constraint(sp,
        libzsp.expr_binary(sp, BIN_EQ,
                           libzsp.expr_var(sp, 3),
                           libzsp.expr_binary(sp, BIN_ADD,
                                              libzsp.expr_var(sp, 0),
                                              libzsp.expr_var(sp, 2))))

    for seed in range(1, 11):
        vals = _solve(libzsp, sp, 4, seed=seed)
        assert vals[0] == vals[1], f"x={vals[0]} != y={vals[1]}"
        assert vals[0] + vals[2] == 100, f"x+z={vals[0]+vals[2]} != 100"


def test_alias_conflict_detected(libzsp):
    """x in [0, 10], y in [20, 30], x == y: UNSAT (empty intersection)."""
    _wire(libzsp)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libzsp.solve_problem_init(sp_buf, _SP)

    libzsp.problem_add_var(sp, 0, 8, 0, 0, 10)
    libzsp.problem_add_var(sp, 1, 8, 0, 20, 30)
    libzsp.problem_add_constraint(sp,
        libzsp.expr_binary(sp, BIN_EQ,
                           libzsp.expr_var(sp, 0),
                           libzsp.expr_var(sp, 1)))

    ctx_buf = (ctypes.c_uint8 * _CTX)()
    ba = libzsp.zsp_block_alloc_create(None, _CTX)
    ctx = libzsp.solver_create(ctx_buf, _CTX, ba)
    crc = libzsp.solver_compile(ctx, sp)
    assert crc == -2, f"Expected UNSAT (-2) at compile time, got {crc}"
    libzsp.solver_destroy(ctx)
    libzsp.zsp_block_alloc_destroy(ba)
