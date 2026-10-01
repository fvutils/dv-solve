"""Unit tests for the EXPR_ARRAY_SELECT lowering.

Tests:
- Constant index: r == arr[2] with 4 elements.
- Variable index: r == arr[idx] with idx in [0,3].
- Index with constraint on result: r == arr[idx], r > 10.
- Combined with SumEq: sum of array + array select.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL = 0xFFFF_FFFF
SOLVE_OK = 0
SOLVE_UNSAT = 1
BIN_EQ = 10
BIN_GT = 14

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
        ("time_limit_ms",   ctypes.c_uint32),
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
    lib.expr_array_select.restype = c.c_uint32
    lib.expr_array_select.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32,
                                      c.c_uint32, c.c_uint32]
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
    """Compile and solve, return list of values."""
    ctx_buf = (ctypes.c_uint8 * _CTX)()
    ba = lib.dvs_block_alloc_create(None, _CTX)
    ctx = lib.dvs_solver_create(ctx_buf, _CTX, ba)
    assert ctx
    crc = lib.dvs_solver_compile(ctx, sp)
    assert crc == 0, f"compile returned {crc}"
    opts = SolveOpts(seed=seed)
    rc = lib.dvs_solver_solve(ctx, ctypes.byref(opts))
    assert rc == SOLVE_OK, f"solve returned {rc}"
    vals = [lib.dvs_solver_get_value(ctx, i) for i in range(n_vars)]
    lib.dvs_solver_destroy(ctx)
    lib.dvs_block_alloc_destroy(ba)
    return vals


def test_select_const_index(libdvs):
    """r == arr[2] where arr has 4 elements pinned to distinct values."""
    _wire(libdvs)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libdvs.solve_problem_init(sp_buf, _SP)

    # var 0: result, var 1: index, vars 2-5: array elements
    libdvs.problem_add_var(sp, 0, 32, 1, 0, 1000)   # result
    libdvs.problem_add_var(sp, 1, 32, 1, 2, 2)       # index pinned to 2
    libdvs.problem_add_var(sp, 2, 32, 1, 10, 10)     # arr[0] = 10
    libdvs.problem_add_var(sp, 3, 32, 1, 20, 20)     # arr[1] = 20
    libdvs.problem_add_var(sp, 4, 32, 1, 30, 30)     # arr[2] = 30
    libdvs.problem_add_var(sp, 5, 32, 1, 40, 40)     # arr[3] = 40

    # r = arr[idx], base=2, n_elems=4
    e = libdvs.expr_array_select(sp, 2, 4,
                                  libdvs.expr_var(sp, 0),
                                  libdvs.expr_var(sp, 1))
    libdvs.problem_add_constraint(sp, e)

    vals = _solve(libdvs, sp, 6)
    assert vals[0] == 30, f"result={vals[0]}, expected 30 (arr[2])"


def test_select_var_index(libdvs):
    """r == arr[idx], idx in [0,3]. Result must match arr[solved_idx]."""
    _wire(libdvs)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libdvs.solve_problem_init(sp_buf, _SP)

    libdvs.problem_add_var(sp, 0, 32, 1, 0, 1000)   # result
    libdvs.problem_add_var(sp, 1, 32, 1, 0, 3)       # index [0,3]
    libdvs.problem_add_var(sp, 2, 32, 1, 100, 100)   # arr[0] = 100
    libdvs.problem_add_var(sp, 3, 32, 1, 200, 200)   # arr[1] = 200
    libdvs.problem_add_var(sp, 4, 32, 1, 300, 300)   # arr[2] = 300
    libdvs.problem_add_var(sp, 5, 32, 1, 400, 400)   # arr[3] = 400

    e = libdvs.expr_array_select(sp, 2, 4,
                                  libdvs.expr_var(sp, 0),
                                  libdvs.expr_var(sp, 1))
    libdvs.problem_add_constraint(sp, e)

    expected = {0: 100, 1: 200, 2: 300, 3: 400}
    for seed in range(1, 11):
        vals = _solve(libdvs, sp, 6, seed=seed)
        idx = vals[1]
        assert idx in expected, f"idx={idx} out of range"
        assert vals[0] == expected[idx], \
            f"seed={seed}: result={vals[0]}, idx={idx}, expected {expected[idx]}"


def test_select_with_result_constraint(libdvs):
    """r == arr[idx], r > 250. Forces idx to select arr[2] or arr[3]."""
    _wire(libdvs)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libdvs.solve_problem_init(sp_buf, _SP)

    libdvs.problem_add_var(sp, 0, 32, 1, 0, 1000)   # result
    libdvs.problem_add_var(sp, 1, 32, 1, 0, 3)       # index
    libdvs.problem_add_var(sp, 2, 32, 1, 100, 100)   # arr[0]
    libdvs.problem_add_var(sp, 3, 32, 1, 200, 200)   # arr[1]
    libdvs.problem_add_var(sp, 4, 32, 1, 300, 300)   # arr[2]
    libdvs.problem_add_var(sp, 5, 32, 1, 400, 400)   # arr[3]

    e = libdvs.expr_array_select(sp, 2, 4,
                                  libdvs.expr_var(sp, 0),
                                  libdvs.expr_var(sp, 1))
    libdvs.problem_add_constraint(sp, e)

    # result > 250
    er = libdvs.expr_var(sp, 0)
    ec = libdvs.expr_const(sp, 250, 0)
    libdvs.problem_add_constraint(sp, libdvs.expr_binary(sp, BIN_GT, er, ec))

    for seed in range(1, 11):
        vals = _solve(libdvs, sp, 6, seed=seed)
        assert vals[0] > 250
        assert vals[1] in (2, 3)
        expected = {2: 300, 3: 400}
        assert vals[0] == expected[vals[1]]


def test_select_with_random_elements(libdvs):
    """r == arr[idx] where arr elements are random (not singletons)."""
    _wire(libdvs)
    sp_buf = (ctypes.c_uint8 * _SP)()
    sp = libdvs.solve_problem_init(sp_buf, _SP)

    libdvs.problem_add_var(sp, 0, 32, 1, 0, 1000)   # result
    libdvs.problem_add_var(sp, 1, 32, 1, 0, 3)       # index
    libdvs.problem_add_var(sp, 2, 32, 1, 10, 50)     # arr[0]
    libdvs.problem_add_var(sp, 3, 32, 1, 10, 50)     # arr[1]
    libdvs.problem_add_var(sp, 4, 32, 1, 10, 50)     # arr[2]
    libdvs.problem_add_var(sp, 5, 32, 1, 10, 50)     # arr[3]

    e = libdvs.expr_array_select(sp, 2, 4,
                                  libdvs.expr_var(sp, 0),
                                  libdvs.expr_var(sp, 1))
    libdvs.problem_add_constraint(sp, e)

    for seed in range(1, 11):
        vals = _solve(libdvs, sp, 6, seed=seed)
        idx = vals[1]
        assert 0 <= idx <= 3
        elem_val = vals[2 + idx]
        assert vals[0] == elem_val, \
            f"seed={seed}: result={vals[0]}, arr[{idx}]={elem_val}"
