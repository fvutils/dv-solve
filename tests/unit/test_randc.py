"""Unit tests for randc (cyclic-random) support via dvs_solver_exclude_value (Sprint 8).

Tests exclusion of individual values from a variable's domain and
verifies full-cycle randc behaviour.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL     = 0xFFFF_FFFF
SOLVE_OK      = 0
SOLVE_UNSAT   = 1

_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 524288


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
    lib.dvs_block_alloc_create.restype  = ctypes.c_void_p
    lib.dvs_block_alloc_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.dvs_block_alloc_destroy.restype  = None
    lib.dvs_block_alloc_destroy.argtypes = [ctypes.c_void_p]

    lib.solve_problem_init.restype  = ctypes.c_void_p
    lib.solve_problem_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.problem_add_var.restype  = ctypes.c_uint32
    lib.problem_add_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_uint8, ctypes.c_uint8,
                                    ctypes.c_int64, ctypes.c_int64]

    lib.dvs_solver_create.restype  = ctypes.c_void_p
    lib.dvs_solver_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p]
    lib.dvs_solver_compile.restype  = ctypes.c_int
    lib.dvs_solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.dvs_solver_solve.restype  = ctypes.c_int
    lib.dvs_solver_solve.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.dvs_solver_get_value.restype  = ctypes.c_int64
    lib.dvs_solver_get_value.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.dvs_solver_reset.restype  = None
    lib.dvs_solver_reset.argtypes = [ctypes.c_void_p]
    lib.dvs_solver_set_seed.restype  = None
    lib.dvs_solver_set_seed.argtypes = [ctypes.c_void_p, ctypes.c_uint64]

    lib.dvs_solver_exclude_value.restype  = ctypes.c_int
    lib.dvs_solver_exclude_value.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                         ctypes.c_int64]

    lib.dvs_var_lo64.restype  = ctypes.c_int64
    lib.dvs_var_lo64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.dvs_var_hi64.restype  = ctypes.c_int64
    lib.dvs_var_hi64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


def _compile_single_var(lib, width, lo, hi):
    """Create a problem with one unconstrained variable and compile it."""
    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp
    lib.problem_add_var(sp, 0, width, 0, lo, hi)
    ba = lib.dvs_block_alloc_create(None, 0)
    ctx = lib.dvs_solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.dvs_solver_compile(ctx, sp)
    assert rc >= 0
    return ctx, ba, sp_buf, ctx_buf


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

def test_exclude_boundary_lo(libdvs):
    """Exclude lo value, verify new lo = old_lo+1 after tightening."""
    lib = libdvs
    _setup(lib)

    ctx, ba, _, _ = _compile_single_var(lib, 8, 0, 10)

    rc = lib.dvs_solver_exclude_value(ctx, 0, 0)
    assert rc == 0

    # After excluding 0, the lower bound should be tightened to 1
    lo = lib.dvs_var_lo64(ctx, 0)
    assert lo >= 1, f"lo={lo}, expected >= 1 after excluding 0"

    # Solve should produce a value >= 1
    opts = SolveOpts(seed=0x1111)
    result = lib.dvs_solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK
    x = lib.dvs_solver_get_value(ctx, 0)
    assert x >= 1, f"x={x}, expected >= 1"

    lib.dvs_block_alloc_destroy(ba)


def test_exclude_boundary_hi(libdvs):
    """Exclude hi value, verify new hi = old_hi-1."""
    lib = libdvs
    _setup(lib)

    ctx, ba, _, _ = _compile_single_var(lib, 8, 0, 10)

    rc = lib.dvs_solver_exclude_value(ctx, 0, 10)
    assert rc == 0

    hi = lib.dvs_var_hi64(ctx, 0)
    assert hi <= 9, f"hi={hi}, expected <= 9 after excluding 10"

    opts = SolveOpts(seed=0x2222)
    result = lib.dvs_solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK
    x = lib.dvs_solver_get_value(ctx, 0)
    assert x <= 9, f"x={x}, expected <= 9"

    lib.dvs_block_alloc_destroy(ba)


def test_exclude_middle(libdvs):
    """Exclude middle value, verify it's never picked over 100 solves."""
    lib = libdvs
    _setup(lib)

    ctx, ba, _, _ = _compile_single_var(lib, 8, 0, 10)

    rc = lib.dvs_solver_exclude_value(ctx, 0, 5)
    assert rc == 0

    for i in range(100):
        lib.dvs_solver_reset(ctx)
        lib.dvs_solver_set_seed(ctx, 3000 + i)
        # Re-exclude after reset since boundary tightening is undone
        # (but hole list persists, so _pick_value avoids it)
        opts = SolveOpts(seed=3000 + i)
        result = lib.dvs_solver_solve(ctx, ctypes.byref(opts))
        assert result == SOLVE_OK
        x = lib.dvs_solver_get_value(ctx, 0)
        assert x != 5, f"iteration {i}: x=5, but 5 was excluded"

    lib.dvs_block_alloc_destroy(ba)


def test_full_cycle(libdvs):
    """Variable with domain [0,7]. Exclude all 8 values one-by-one.
    Verify each value appears exactly once."""
    lib = libdvs
    _setup(lib)

    ctx, ba, _, _ = _compile_single_var(lib, 8, 0, 7)

    seen = set()
    for cycle_step in range(8):
        lib.dvs_solver_reset(ctx)
        lib.dvs_solver_set_seed(ctx, 5000 + cycle_step)
        opts = SolveOpts(seed=5000 + cycle_step)
        result = lib.dvs_solver_solve(ctx, ctypes.byref(opts))
        assert result == SOLVE_OK
        x = lib.dvs_solver_get_value(ctx, 0)
        assert 0 <= x <= 7, f"step {cycle_step}: x={x} out of range"
        assert x not in seen, (
            f"step {cycle_step}: x={x} already seen in {seen}"
        )
        seen.add(x)

        # Exclude this value for subsequent solves
        rc = lib.dvs_solver_exclude_value(ctx, 0, x)
        if cycle_step < 7:
            assert rc == 0, f"step {cycle_step}: exclude_value returned {rc}"

    assert seen == set(range(8)), f"Incomplete cycle: seen={seen}"

    lib.dvs_block_alloc_destroy(ba)


def test_exclude_all_returns_error(libdvs):
    """Excluding all values from a 2-element domain returns -1."""
    lib = libdvs
    _setup(lib)

    ctx, ba, _, _ = _compile_single_var(lib, 8, 0, 1)

    rc = lib.dvs_solver_exclude_value(ctx, 0, 0)
    assert rc == 0
    # After excluding 0, domain is [1,1] (singleton)
    rc = lib.dvs_solver_exclude_value(ctx, 0, 1)
    assert rc == -1, "Expected -1 when excluding would empty domain"

    lib.dvs_block_alloc_destroy(ba)


def test_exclude_outside_domain(libdvs):
    """Excluding a value outside the domain is a no-op (returns 0)."""
    lib = libdvs
    _setup(lib)

    ctx, ba, _, _ = _compile_single_var(lib, 8, 5, 10)

    rc = lib.dvs_solver_exclude_value(ctx, 0, 0)
    assert rc == 0, "Excluding value below domain should succeed (no-op)"

    rc = lib.dvs_solver_exclude_value(ctx, 0, 100)
    assert rc == 0, "Excluding value above domain should succeed (no-op)"

    lib.dvs_block_alloc_destroy(ba)


def test_exclude_duplicate(libdvs):
    """Excluding the same value twice is a no-op."""
    lib = libdvs
    _setup(lib)

    ctx, ba, _, _ = _compile_single_var(lib, 8, 0, 10)

    rc = lib.dvs_solver_exclude_value(ctx, 0, 5)
    assert rc == 0
    rc = lib.dvs_solver_exclude_value(ctx, 0, 5)
    assert rc == 0, "Duplicate exclude should be a no-op"

    lib.dvs_block_alloc_destroy(ba)
