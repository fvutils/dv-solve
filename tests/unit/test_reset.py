"""Unit tests for solver_reset, solver_set_seed, solver_get_values (Sprint 5).

Tests:
- solver_reset restores domains after solve
- solver_reset + re-solve produces valid solutions
- Repeated reset+solve yields diverse solutions
- solver_set_seed produces deterministic results
- solver_get_values batch read
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL   = 0xFFFF_FFFF
SOLVE_OK    = 0
SOLVE_UNSAT = 1

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
    lib.solver_reset.restype  = None
    lib.solver_reset.argtypes = [ctypes.c_void_p]
    lib.solver_set_seed.restype  = None
    lib.solver_set_seed.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    lib.solver_get_values.restype  = None
    lib.solver_get_values.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                      ctypes.POINTER(ctypes.c_uint32),
                                      ctypes.POINTER(ctypes.c_int64)]


BIN_LTE = 13
BIN_ADD = 0
BIN_EQ  = 10


def _build_constrained(lib):
    """Build: x in [0,99], y in [0,99], x + y <= 50.
    Returns (sp_buf, ctx_buf, ba, ctx, sp).
    """
    sp_buf  = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 32, 1, 0, 99)   # x
    lib.problem_add_var(sp, 1, 32, 1, 0, 99)   # y
    lib.problem_add_var(sp, 2, 32, 1, 0, 198)  # sum = x+y

    # sum == x + y
    v_x   = lib.expr_var(sp, 0)
    v_y   = lib.expr_var(sp, 1)
    v_sum = lib.expr_var(sp, 2)
    add_e = lib.expr_binary(sp, BIN_ADD, v_x, v_y)
    eq_e  = lib.expr_binary(sp, BIN_EQ, v_sum, add_e)
    lib.problem_add_constraint(sp, eq_e)

    # sum <= 50
    c_50  = lib.expr_const(sp, 50, 1)
    le_e  = lib.expr_binary(sp, BIN_LTE, v_sum, c_50)
    lib.problem_add_constraint(sp, le_e)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0
    return sp_buf, ctx_buf, ba, ctx, sp


def _solve(lib, ctx, seed=0x1234):
    opts = lib._SolveOpts(seed=seed)
    return lib.solver_solve(ctx, ctypes.byref(opts))


def test_reset_restores_domains(libzsp):
    """After solve + reset, variable domains match post-compile state."""
    lib = libzsp
    _setup(lib)
    sp_buf, ctx_buf, ba, ctx, sp = _build_constrained(lib)

    # Record post-compile bounds
    pre_lo = [lib.zsp_var_lo64(ctx, i) for i in range(3)]
    pre_hi = [lib.zsp_var_hi64(ctx, i) for i in range(3)]

    # Solve (mutates bounds)
    rc = _solve(lib, ctx)
    assert rc == SOLVE_OK

    # Reset
    lib.solver_reset(ctx)

    # Verify bounds restored
    for i in range(3):
        lo = lib.zsp_var_lo64(ctx, i)
        hi = lib.zsp_var_hi64(ctx, i)
        assert lo == pre_lo[i], f"var {i} lo: {lo} != {pre_lo[i]}"
        assert hi == pre_hi[i], f"var {i} hi: {hi} != {pre_hi[i]}"

    lib.zsp_block_alloc_destroy(ba)


def test_reset_then_solve_again(libzsp):
    """Solve, reset, solve again with different seed: both valid."""
    lib = libzsp
    _setup(lib)
    sp_buf, ctx_buf, ba, ctx, sp = _build_constrained(lib)

    rc = _solve(lib, ctx, seed=0xAAAA)
    assert rc == SOLVE_OK
    x1 = lib.solver_get_value(ctx, 0)
    y1 = lib.solver_get_value(ctx, 1)
    assert x1 + y1 <= 50

    lib.solver_reset(ctx)

    rc = _solve(lib, ctx, seed=0xBBBB)
    assert rc == SOLVE_OK
    x2 = lib.solver_get_value(ctx, 0)
    y2 = lib.solver_get_value(ctx, 1)
    assert x2 + y2 <= 50

    lib.zsp_block_alloc_destroy(ba)


def test_reset_repeated(libzsp):
    """100 iterations of reset + solve: all valid and diverse."""
    lib = libzsp
    _setup(lib)
    sp_buf, ctx_buf, ba, ctx, sp = _build_constrained(lib)

    seen = set()
    for i in range(100):
        rc = _solve(lib, ctx, seed=(i + 1) * 0x9E3779B97F4A7C15)
        assert rc == SOLVE_OK, f"Solve failed on iteration {i}"
        x = lib.solver_get_value(ctx, 0)
        y = lib.solver_get_value(ctx, 1)
        assert 0 <= x <= 99
        assert 0 <= y <= 99
        assert x + y <= 50, f"Constraint violated: {x} + {y} = {x+y}"
        seen.add((x, y))
        lib.solver_reset(ctx)

    # With 100 solves over a domain with ~1300 valid pairs, expect diversity
    assert len(seen) >= 10, f"Only {len(seen)} distinct solutions in 100 runs"

    lib.zsp_block_alloc_destroy(ba)


def test_deterministic_seed(libzsp):
    """Same seed after reset produces same solution."""
    lib = libzsp
    _setup(lib)
    sp_buf, ctx_buf, ba, ctx, sp = _build_constrained(lib)

    SEED = 0xDEADBEEF

    rc = _solve(lib, ctx, seed=SEED)
    assert rc == SOLVE_OK
    x1 = lib.solver_get_value(ctx, 0)
    y1 = lib.solver_get_value(ctx, 1)

    lib.solver_reset(ctx)

    rc = _solve(lib, ctx, seed=SEED)
    assert rc == SOLVE_OK
    x2 = lib.solver_get_value(ctx, 0)
    y2 = lib.solver_get_value(ctx, 1)

    assert x1 == x2 and y1 == y2, (
        f"Same seed different results: ({x1},{y1}) vs ({x2},{y2})"
    )

    lib.zsp_block_alloc_destroy(ba)


def test_solver_set_seed(libzsp):
    """solver_set_seed changes the RNG state."""
    lib = libzsp
    _setup(lib)
    sp_buf, ctx_buf, ba, ctx, sp = _build_constrained(lib)

    SEED = 0xCAFEBABE
    lib.solver_set_seed(ctx, SEED)

    # Solve with opts.seed=0 so it uses ctx->rng_state set by solver_set_seed
    opts = lib._SolveOpts(seed=0)
    rc = lib.solver_solve(ctx, ctypes.byref(opts))
    assert rc == SOLVE_OK
    x1 = lib.solver_get_value(ctx, 0)

    lib.solver_reset(ctx)
    lib.solver_set_seed(ctx, SEED)
    rc = lib.solver_solve(ctx, ctypes.byref(opts))
    assert rc == SOLVE_OK
    x2 = lib.solver_get_value(ctx, 0)

    assert x1 == x2, f"solver_set_seed not deterministic: {x1} vs {x2}"

    lib.zsp_block_alloc_destroy(ba)


def test_batch_get_values(libzsp):
    """solver_get_values reads multiple variables."""
    lib = libzsp
    _setup(lib)
    sp_buf, ctx_buf, ba, ctx, sp = _build_constrained(lib)

    rc = _solve(lib, ctx)
    assert rc == SOLVE_OK

    var_ids = (ctypes.c_uint32 * 3)(0, 1, 2)
    out     = (ctypes.c_int64 * 3)()

    lib.solver_get_values(ctx, 3, var_ids, out)

    for i in range(3):
        expected = lib.solver_get_value(ctx, i)
        assert out[i] == expected, f"var {i}: batch={out[i]} != single={expected}"

    lib.zsp_block_alloc_destroy(ba)
