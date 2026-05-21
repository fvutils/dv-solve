"""Unit tests for distribution constraints (Sprint 7).

Tests weighted value selection via dist constraints.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL     = 0xFFFF_FFFF
SOLVE_OK      = 0

_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 524288


class DistEntry(ctypes.Structure):
    """ctypes mirror of the C DistEntry struct."""
    _fields_ = [
        ("lo",           ctypes.c_int64),
        ("hi",           ctypes.c_int64),
        ("weight",       ctypes.c_uint32),
        ("is_per_value", ctypes.c_uint8),
        ("_dpad",        ctypes.c_uint8 * 3),
    ]


class SolveOpts(ctypes.Structure):
    _fields_ = [
        ("seed",            ctypes.c_uint64),
        ("max_conflicts",   ctypes.c_uint32),
        ("max_restarts",    ctypes.c_uint32),
        ("use_phase_save",  ctypes.c_uint8),
        ("_pad",            ctypes.c_uint8 * 3),
        ("max_shave_iters", ctypes.c_uint32),
    ]


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
    lib.problem_add_dist.restype  = ctypes.c_uint32
    lib.problem_add_dist.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_uint32, ctypes.c_void_p]

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
    lib.solver_compile.restype  = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.solver_solve.restype  = ctypes.c_int
    lib.solver_solve.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.solver_get_value.restype  = ctypes.c_int64
    lib.solver_get_value.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.solver_reset.restype  = None
    lib.solver_reset.argtypes = [ctypes.c_void_p]
    lib.solver_set_seed.restype  = None
    lib.solver_set_seed.argtypes = [ctypes.c_void_p, ctypes.c_uint64]


def _make_dist_entries(*specs):
    """Build a ctypes DistEntry array from (lo, hi, weight, is_per_value) tuples."""
    arr = (DistEntry * len(specs))()
    for i, (lo, hi, w, ipv) in enumerate(specs):
        arr[i].lo = lo
        arr[i].hi = hi
        arr[i].weight = w
        arr[i].is_per_value = 1 if ipv else 0
    return arr


def _create_solver(lib, sp):
    """Create a solver context, compile the problem, return (ctx, ba)."""
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0, f"solver_compile failed: {rc}"
    return ctx, ba, ctx_buf


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

def test_dist_domain_restriction(libzsp):
    """dist {[1:3], [5:6]}. Verify solutions only in those ranges."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 10]
    lib.problem_add_var(sp, 0, 8, 0, 0, 10)

    # dist: [1,3] weight=1 per-range, [5,6] weight=1 per-range
    entries = _make_dist_entries(
        (1, 3, 1, False),
        (5, 6, 1, False),
    )
    ref = lib.problem_add_dist(sp, 0, 2, entries)
    assert ref != EXPR_NULL

    ctx, ba, _ = _create_solver(lib, sp)

    allowed = set(range(1, 4)) | set(range(5, 7))  # {1,2,3,5,6}
    for i in range(200):
        lib.solver_reset(ctx)
        lib.solver_set_seed(ctx, 1000 + i)
        opts = SolveOpts(seed=1000 + i)
        result = lib.solver_solve(ctx, ctypes.byref(opts))
        assert result == SOLVE_OK
        x = lib.solver_get_value(ctx, 0)
        assert x in allowed, f"iteration {i}: x={x}, not in {allowed}"

    lib.zsp_block_alloc_destroy(ba)


def test_dist_zero_weight_excluded(libzsp):
    """dist {0 := 0, 1 := 1}. Verify 0 never appears."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 1]
    lib.problem_add_var(sp, 0, 8, 0, 0, 1)

    # dist: value 0 weight=0 (excluded), value 1 weight=1
    entries = _make_dist_entries(
        (0, 0, 0, True),   # weight 0 -> excluded
        (1, 1, 1, True),   # weight 1 -> always picked
    )
    lib.problem_add_dist(sp, 0, 2, entries)

    ctx, ba, _ = _create_solver(lib, sp)

    for i in range(100):
        lib.solver_reset(ctx)
        lib.solver_set_seed(ctx, 2000 + i)
        opts = SolveOpts(seed=2000 + i)
        result = lib.solver_solve(ctx, ctypes.byref(opts))
        assert result == SOLVE_OK
        x = lib.solver_get_value(ctx, 0)
        assert x == 1, f"iteration {i}: x={x}, expected 1 (0 should be excluded)"

    lib.zsp_block_alloc_destroy(ba)


def test_dist_weighted_bias(libzsp):
    """dist {0 := 1, 255 := 3}. Over 1000 solves, verify ~75% hit 255."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 255]
    lib.problem_add_var(sp, 0, 8, 0, 0, 255)

    # dist: value 0 := weight 1, value 255 := weight 3
    # Expected: P(0)=1/4=25%, P(255)=3/4=75%
    entries = _make_dist_entries(
        (0, 0, 1, True),
        (255, 255, 3, True),
    )
    lib.problem_add_dist(sp, 0, 2, entries)

    ctx, ba, _ = _create_solver(lib, sp)

    count_255 = 0
    n_trials = 1000
    for i in range(n_trials):
        lib.solver_reset(ctx)
        lib.solver_set_seed(ctx, 3000 + i)
        opts = SolveOpts(seed=3000 + i)
        result = lib.solver_solve(ctx, ctypes.byref(opts))
        assert result == SOLVE_OK
        x = lib.solver_get_value(ctx, 0)
        assert x in (0, 255), f"iteration {i}: x={x}, expected 0 or 255"
        if x == 255:
            count_255 += 1

    ratio = count_255 / n_trials
    # Expect ~75% with reasonable tolerance (60%-90%)
    assert 0.60 <= ratio <= 0.90, (
        f"Expected ~75% hits on 255, got {ratio*100:.1f}% "
        f"({count_255}/{n_trials})"
    )

    lib.zsp_block_alloc_destroy(ba)


def test_dist_range_divided(libzsp):
    """dist {[0:9] :/ 1, [10:19] :/ 3}. Over 1000 solves, verify ~75% in [10:19]."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 19]
    lib.problem_add_var(sp, 0, 8, 0, 0, 19)

    # dist: [0,9] :/ weight 1, [10,19] :/ weight 3
    # :/ means the weight is shared across the range.
    # Effective weight: entry 0 = 1, entry 1 = 3. Total = 4.
    # P([0:9]) = 1/4 = 25%, P([10:19]) = 3/4 = 75%
    entries = _make_dist_entries(
        (0, 9, 1, False),    # :/ 1
        (10, 19, 3, False),  # :/ 3
    )
    lib.problem_add_dist(sp, 0, 2, entries)

    ctx, ba, _ = _create_solver(lib, sp)

    count_high = 0
    n_trials = 1000
    for i in range(n_trials):
        lib.solver_reset(ctx)
        lib.solver_set_seed(ctx, 4000 + i)
        opts = SolveOpts(seed=4000 + i)
        result = lib.solver_solve(ctx, ctypes.byref(opts))
        assert result == SOLVE_OK
        x = lib.solver_get_value(ctx, 0)
        assert 0 <= x <= 19, f"iteration {i}: x={x}, out of range"
        if x >= 10:
            count_high += 1

    ratio = count_high / n_trials
    # Expect ~75% with tolerance (60%-90%)
    assert 0.60 <= ratio <= 0.90, (
        f"Expected ~75% in [10:19], got {ratio*100:.1f}% "
        f"({count_high}/{n_trials})"
    )

    lib.zsp_block_alloc_destroy(ba)
