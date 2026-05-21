"""Unit tests for the SumEq propagator and solver_add_array_vars.

Tests:
- SumEq: result == sum of N summands, forward + backward propagation.
- solver_add_array_vars: bulk element variable creation.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL = 0xFFFF_FFFF
SOLVE_OK = 0
SOLVE_UNSAT = 1
BIN_EQ = 10

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
    lib.expr_sum.restype = c.c_uint32
    lib.expr_sum.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32, c.c_void_p]
    lib.expr_countones.restype = c.c_uint32
    lib.expr_countones.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]
    lib.expr_clog2.restype = c.c_uint32
    lib.expr_clog2.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]
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
    lib.solver_reset.restype = None
    lib.solver_reset.argtypes = [c.c_void_p]
    lib.solver_add_array_vars.restype = c.c_int
    lib.solver_add_array_vars.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32,
                                          c.c_uint8, c.c_uint8,
                                          c.c_int64, c.c_int64]
    lib.solver_add_constraint.restype = c.c_int
    lib.solver_add_constraint.argtypes = [c.c_void_p, c.c_void_p]


def _build_and_solve(lib, sp_setup, n_vars, seed=42):
    """Helper: create problem, compile, solve, return (ctx, ba, values)."""
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = lib.solve_problem_init(sp_buf, _SP)
    assert sp

    sp_setup(lib, sp)

    ctx_buf = (ctypes.c_uint8 * _CTX)()
    ba = lib.zsp_block_alloc_create(None, _CTX)
    ctx = lib.solver_create(ctx_buf, _CTX, ba)
    assert ctx

    crc = lib.solver_compile(ctx, sp)
    assert crc == 0, f"solver_compile returned {crc}"

    opts = SolveOpts(seed=seed)
    rc = lib.solver_solve(ctx, ctypes.byref(opts))
    assert rc == SOLVE_OK, f"solver_solve returned {rc}"

    values = [lib.solver_get_value(ctx, i) for i in range(n_vars)]
    lib.solver_destroy(ctx)
    lib.zsp_block_alloc_destroy(ba)
    return values


# ------------------------------------------------------------------ #
# SumEq tests                                                         #
# ------------------------------------------------------------------ #

def test_sum_4_vars_exact(libzsp):
    """4 summands each in [0,100], sum == 100. Verify sum is correct."""
    _wire(libzsp)

    def setup(lib, sp):
        # var 0: result [0, 400]
        lib.problem_add_var(sp, 0, 32, 1, 0, 400)
        # vars 1-4: summands [0, 100]
        for i in range(1, 5):
            lib.problem_add_var(sp, i, 32, 1, 0, 100)

        # result == 100
        er = lib.expr_var(sp, 0)
        ec = lib.expr_const(sp, 100, 0)
        lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_EQ, er, ec))

        # EXPR_SUM: result == sum(v1, v2, v3, v4)
        refs = [lib.expr_var(sp, i) for i in range(1, 5)]
        arr = (ctypes.c_uint32 * 4)(*refs)
        esum = lib.expr_sum(sp, lib.expr_var(sp, 0), 4,
                            ctypes.cast(arr, ctypes.c_void_p))
        lib.problem_add_constraint(sp, esum)

    values = _build_and_solve(libzsp, setup, 5)
    assert values[0] == 100
    assert sum(values[1:5]) == 100
    for v in values[1:5]:
        assert 0 <= v <= 100


def test_sum_backward_tighten(libzsp):
    """Sum == 10, 4 vars each [0, 10]. Backward propagation tightens each var."""
    _wire(libzsp)

    def setup(lib, sp):
        lib.problem_add_var(sp, 0, 32, 1, 10, 10)  # result pinned to 10
        for i in range(1, 5):
            lib.problem_add_var(sp, i, 32, 1, 0, 10)

        refs = [lib.expr_var(sp, i) for i in range(1, 5)]
        arr = (ctypes.c_uint32 * 4)(*refs)
        esum = lib.expr_sum(sp, lib.expr_var(sp, 0), 4,
                            ctypes.cast(arr, ctypes.c_void_p))
        lib.problem_add_constraint(sp, esum)

    values = _build_and_solve(libzsp, setup, 5)
    assert values[0] == 10
    assert sum(values[1:5]) == 10


def test_sum_conflict(libzsp):
    """Sum == 100, 4 vars [0, 10]. Max sum = 40. Should be UNSAT."""
    _wire(libzsp)

    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libzsp.solve_problem_init(sp_buf, _SP)

    libzsp.problem_add_var(sp, 0, 32, 1, 100, 100)
    for i in range(1, 5):
        libzsp.problem_add_var(sp, i, 32, 1, 0, 10)

    refs = [libzsp.expr_var(sp, i) for i in range(1, 5)]
    arr = (ctypes.c_uint32 * 4)(*refs)
    esum = libzsp.expr_sum(sp, libzsp.expr_var(sp, 0), 4,
                           ctypes.cast(arr, ctypes.c_void_p))
    libzsp.problem_add_constraint(sp, esum)

    ctx_buf = (ctypes.c_uint8 * _CTX)()
    ba = libzsp.zsp_block_alloc_create(None, _CTX)
    ctx = libzsp.solver_create(ctx_buf, _CTX, ba)

    crc = libzsp.solver_compile(ctx, sp)
    if crc == -2:
        # Detected at compile time -- acceptable
        pass
    else:
        assert crc == 0
        # Should detect UNSAT at solve time
        opts = SolveOpts(seed=42)
        rc = libzsp.solver_solve(ctx, ctypes.byref(opts))
        assert rc == SOLVE_UNSAT, f"Expected UNSAT, got {rc}"

    libzsp.solver_destroy(ctx)
    libzsp.zsp_block_alloc_destroy(ba)


def test_sum_boolean_counting(libzsp):
    """8 boolean [0,1] vars, sum == 3. Exactly 3 must be 1."""
    _wire(libzsp)

    def setup(lib, sp):
        lib.problem_add_var(sp, 0, 32, 1, 3, 3)  # result pinned to 3
        for i in range(1, 9):
            lib.problem_add_var(sp, i, 1, 0, 0, 1)

        refs = [lib.expr_var(sp, i) for i in range(1, 9)]
        arr = (ctypes.c_uint32 * 8)(*refs)
        esum = lib.expr_sum(sp, lib.expr_var(sp, 0), 8,
                            ctypes.cast(arr, ctypes.c_void_p))
        lib.problem_add_constraint(sp, esum)

    values = _build_and_solve(libzsp, setup, 9)
    assert values[0] == 3
    assert sum(values[1:9]) == 3
    for v in values[1:9]:
        assert v in (0, 1)


def test_sum_large_n(libzsp):
    """20 summands, each [0, 5], sum == 50. Verify."""
    _wire(libzsp)
    N = 20

    def setup(lib, sp):
        lib.problem_add_var(sp, 0, 32, 1, 50, 50)
        for i in range(1, N + 1):
            lib.problem_add_var(sp, i, 32, 1, 0, 5)

        refs = [lib.expr_var(sp, i) for i in range(1, N + 1)]
        arr = (ctypes.c_uint32 * N)(*refs)
        esum = lib.expr_sum(sp, lib.expr_var(sp, 0), N,
                            ctypes.cast(arr, ctypes.c_void_p))
        lib.problem_add_constraint(sp, esum)

    values = _build_and_solve(libzsp, setup, N + 1)
    assert values[0] == 50
    assert sum(values[1:N + 1]) == 50


# ------------------------------------------------------------------ #
# Countones tests                                                     #
# ------------------------------------------------------------------ #

def test_countones_exact_1(libzsp):
    """8-bit var, countones == 1. Result must be a power of 2."""
    _wire(libzsp)

    def setup(lib, sp):
        lib.problem_add_var(sp, 0, 32, 1, 1, 1)     # result pinned to 1
        lib.problem_add_var(sp, 1, 8, 0, 0, 255)     # operand

        ecnt = lib.expr_countones(sp, lib.expr_var(sp, 0), lib.expr_var(sp, 1))
        lib.problem_add_constraint(sp, ecnt)

    for seed in range(1, 11):
        values = _build_and_solve(libzsp, setup, 2, seed=seed)
        x = values[1]
        assert x > 0 and (x & (x - 1)) == 0, f"x={x} is not a power of 2"


def test_countones_exact_3(libzsp):
    """8-bit var, countones == 3. Verify popcount."""
    _wire(libzsp)

    def setup(lib, sp):
        lib.problem_add_var(sp, 0, 32, 1, 3, 3)
        lib.problem_add_var(sp, 1, 8, 0, 0, 255)

        ecnt = lib.expr_countones(sp, lib.expr_var(sp, 0), lib.expr_var(sp, 1))
        lib.problem_add_constraint(sp, ecnt)

    for seed in range(1, 11):
        values = _build_and_solve(libzsp, setup, 2, seed=seed)
        x = values[1]
        assert bin(x).count('1') == 3, f"x={x} (0b{x:08b}) has {bin(x).count('1')} ones, expected 3"


def test_countones_zero(libzsp):
    """countones == 0 -> x must be 0."""
    _wire(libzsp)

    def setup(lib, sp):
        lib.problem_add_var(sp, 0, 32, 1, 0, 0)  # result = 0
        lib.problem_add_var(sp, 1, 8, 0, 0, 255)

        ecnt = lib.expr_countones(sp, lib.expr_var(sp, 0), lib.expr_var(sp, 1))
        lib.problem_add_constraint(sp, ecnt)

    values = _build_and_solve(libzsp, setup, 2)
    assert values[1] == 0


def test_countones_max(libzsp):
    """countones == 8 -> x must be 0xFF."""
    _wire(libzsp)

    def setup(lib, sp):
        lib.problem_add_var(sp, 0, 32, 1, 8, 8)
        lib.problem_add_var(sp, 1, 8, 0, 0, 255)

        ecnt = lib.expr_countones(sp, lib.expr_var(sp, 0), lib.expr_var(sp, 1))
        lib.problem_add_constraint(sp, ecnt)

    values = _build_and_solve(libzsp, setup, 2)
    assert values[1] == 255


# ------------------------------------------------------------------ #
# Clog2 tests                                                         #
# ------------------------------------------------------------------ #

def test_clog2_singleton(libzsp):
    """clog2(x) == 3 -> x in [5, 8]."""
    _wire(libzsp)

    def setup(lib, sp):
        lib.problem_add_var(sp, 0, 32, 1, 3, 3)      # result = 3
        lib.problem_add_var(sp, 1, 8, 0, 1, 255)      # operand

        eclog = lib.expr_clog2(sp, lib.expr_var(sp, 0), lib.expr_var(sp, 1))
        lib.problem_add_constraint(sp, eclog)

    import math
    for seed in range(1, 11):
        values = _build_and_solve(libzsp, setup, 2, seed=seed)
        x = values[1]
        assert 5 <= x <= 8, f"x={x}, expected in [5, 8]"
        assert math.ceil(math.log2(x)) == 3 if x > 1 else 0 == 3


def test_clog2_one(libzsp):
    """clog2(1) == 0."""
    _wire(libzsp)

    def setup(lib, sp):
        lib.problem_add_var(sp, 0, 32, 1, 0, 0)  # result = 0
        lib.problem_add_var(sp, 1, 8, 0, 1, 255)

        eclog = lib.expr_clog2(sp, lib.expr_var(sp, 0), lib.expr_var(sp, 1))
        lib.problem_add_constraint(sp, eclog)

    values = _build_and_solve(libzsp, setup, 2)
    assert values[1] == 1


def test_clog2_with_equality(libzsp):
    """r == clog2(x), x in [1, 255]. Solve and verify."""
    _wire(libzsp)
    import math

    def setup(lib, sp):
        lib.problem_add_var(sp, 0, 32, 1, 0, 8)      # result [0, 8]
        lib.problem_add_var(sp, 1, 8, 0, 1, 255)      # operand

        eclog = lib.expr_clog2(sp, lib.expr_var(sp, 0), lib.expr_var(sp, 1))
        lib.problem_add_constraint(sp, eclog)

    for seed in range(1, 11):
        values = _build_and_solve(libzsp, setup, 2, seed=seed)
        r, x = values[0], values[1]
        expected = math.ceil(math.log2(x)) if x > 1 else 0
        assert r == expected, f"r={r}, x={x}, expected clog2={expected}"


# ------------------------------------------------------------------ #
# solver_add_array_vars tests                                         #
# ------------------------------------------------------------------ #

def test_add_array_vars_basic(libzsp):
    """Add 8 element variables and verify they exist after solve."""
    _wire(libzsp)

    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libzsp.solve_problem_init(sp_buf, _SP)
    # One scalar var
    libzsp.problem_add_var(sp, 0, 32, 1, 0, 100)

    ctx_buf = (ctypes.c_uint8 * _CTX)()
    ba = libzsp.zsp_block_alloc_create(None, _CTX)
    ctx = libzsp.solver_create(ctx_buf, _CTX, ba)

    crc = libzsp.solver_compile(ctx, sp)
    assert crc == 0

    # Add 8 element vars (IDs 1..8) with domain [10, 50]
    rc = libzsp.solver_add_array_vars(ctx, 1, 8, 8, 0, 10, 50)
    assert rc == 0

    # Solve
    opts = SolveOpts(seed=42)
    rc = libzsp.solver_solve(ctx, ctypes.byref(opts))
    assert rc == SOLVE_OK

    # Read values
    for i in range(1, 9):
        v = libzsp.solver_get_value(ctx, i)
        assert 10 <= v <= 50, f"var[{i}]={v} out of [10, 50]"

    libzsp.solver_destroy(ctx)
    libzsp.zsp_block_alloc_destroy(ba)


def test_add_array_vars_with_sum_constraint(libzsp):
    """Add array vars, then add sum constraint via aux problem, solve."""
    _wire(libzsp)

    # Phase 1: scalar problem with a size variable
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libzsp.solve_problem_init(sp_buf, _SP)
    libzsp.problem_add_var(sp, 0, 32, 1, 0, 400)  # sum result

    ctx_buf = (ctypes.c_uint8 * _CTX)()
    ba = libzsp.zsp_block_alloc_create(None, _CTX)
    ctx = libzsp.solver_create(ctx_buf, _CTX, ba)

    crc = libzsp.solver_compile(ctx, sp)
    assert crc == 0

    # Phase 2: add element vars and sum constraint
    N = 4
    rc = libzsp.solver_add_array_vars(ctx, 1, N, 32, 1, 0, 100)
    assert rc == 0

    # Build aux problem with sum constraint
    aux_buf = (ctypes.c_uint8 * _SP)()
    aux = libzsp.solve_problem_init(aux_buf, _SP)
    # Re-declare all vars in aux (existing vars are skipped)
    libzsp.problem_add_var(aux, 0, 32, 1, 0, 400)
    for i in range(1, N + 1):
        libzsp.problem_add_var(aux, i, 32, 1, 0, 100)

    # sum constraint: var0 == var1 + var2 + var3 + var4
    refs = [libzsp.expr_var(aux, i) for i in range(1, N + 1)]
    arr = (ctypes.c_uint32 * N)(*refs)
    esum = libzsp.expr_sum(aux, libzsp.expr_var(aux, 0), N,
                           ctypes.cast(arr, ctypes.c_void_p))
    libzsp.problem_add_constraint(aux, esum)

    # Pin result to 200
    er = libzsp.expr_var(aux, 0)
    ec = libzsp.expr_const(aux, 200, 0)
    libzsp.problem_add_constraint(aux, libzsp.expr_binary(aux, BIN_EQ, er, ec))

    arc = libzsp.solver_add_constraint(ctx, aux)
    assert arc >= 0, f"solver_add_constraint returned {arc}"

    opts = SolveOpts(seed=42)
    rc = libzsp.solver_solve(ctx, ctypes.byref(opts))
    assert rc == SOLVE_OK

    values = [libzsp.solver_get_value(ctx, i) for i in range(N + 1)]
    assert values[0] == 200
    assert sum(values[1:N + 1]) == 200

    libzsp.solver_destroy(ctx)
    libzsp.zsp_block_alloc_destroy(ba)
