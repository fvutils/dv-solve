"""Unit tests for the search loop (Phase 7).

Tests:
1. Unconstrained 2-variable problem: solution found, values in domain.
2. x+y=7, x∈[0,10], y∈[0,10]: solution satisfies constraint.
3. Unsatisfiable (x≥5 AND x≤3): SOLVE_UNSAT.
4. Restart: max_conflicts=1 forces restarts; solution still found.
5. Randc: same domain, different seeds → different values seen.
6. Seeded: same seed produces same solution.
"""
from __future__ import annotations

import ctypes
import pytest

# ------------------------------------------------------------------ #
# Constants                                                            #
# ------------------------------------------------------------------ #

EXPR_NULL   = 0xFFFF_FFFF
PROP_OK       = 0
PROP_CONFLICT = 1

SOLVE_OK      = 0
SOLVE_UNSAT   = 1
SOLVE_TIMEOUT = 2

VAR_SIGNED = 0x01

_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 1048576   # 1 MiB — extra headroom for decisions[]


# ------------------------------------------------------------------ #
# Library setup                                                        #
# ------------------------------------------------------------------ #

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

    lib.solver_create.restype  = ctypes.c_void_p
    lib.solver_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p]
    lib.solver_destroy.restype  = None
    lib.solver_destroy.argtypes = [ctypes.c_void_p]
    lib.solver_compile.restype  = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.zsp_var_lo32.restype  = ctypes.c_int32
    lib.zsp_var_lo32.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_hi32.restype  = ctypes.c_int32
    lib.zsp_var_hi32.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_lo64.restype  = ctypes.c_int64
    lib.zsp_var_lo64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_hi64.restype  = ctypes.c_int64
    lib.zsp_var_hi64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.prop_add_bounds_add_32.restype  = ctypes.c_uint32
    lib.prop_add_bounds_add_32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                           ctypes.c_uint32, ctypes.c_uint32,
                                           ctypes.c_uint8]
    lib.prop_add_bounds_le_32.restype  = ctypes.c_uint32
    lib.prop_add_bounds_le_32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                          ctypes.c_uint32, ctypes.c_uint8]
    lib.ctx_tighten_lb32.restype  = ctypes.c_int
    lib.ctx_tighten_lb32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int32]
    lib.ctx_tighten_ub32.restype  = ctypes.c_int
    lib.ctx_tighten_ub32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int32]
    lib.solver_propagate.restype  = ctypes.c_int
    lib.solver_propagate.argtypes = [ctypes.c_void_p]

    # SolveOpts layout: seed(8) + max_conflicts(4) + max_restarts(4) +
    #                   use_phase_save(1) + _pad(3)
    class SolveOpts(ctypes.Structure):
        _fields_ = [
            ("seed",           ctypes.c_uint64),
            ("max_conflicts",  ctypes.c_uint32),
            ("max_restarts",   ctypes.c_uint32),
            ("use_phase_save", ctypes.c_uint8),
            ("_pad",           ctypes.c_uint8 * 3),
        ]

    lib._SolveOpts = SolveOpts

    lib.solver_solve.restype  = ctypes.c_int
    lib.solver_solve.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.solver_get_value.restype  = ctypes.c_int64
    lib.solver_get_value.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


# ------------------------------------------------------------------ #
# Helpers                                                              #
# ------------------------------------------------------------------ #

def _make_ctx(lib, var_specs):
    """Create a compiled SolveCtx.  var_specs = [(width, signed, lo, hi), ...]
    Returns (sp_buf, ctx_buf, ba, sp, ctx).
    """
    n = len(var_specs)
    sp_buf  = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()

    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    for i, (width, is_signed, lo, hi) in enumerate(var_specs):
        ref = lib.problem_add_var(sp, i, width, is_signed, lo, hi)
        assert ref != EXPR_NULL

    ba  = lib.zsp_block_alloc_create(None, 0)
    assert ba
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx

    rc = lib.solver_compile(ctx, sp)
    assert rc == 0

    return sp_buf, ctx_buf, ba, sp, ctx


def _solve(lib, ctx, seed=0x1234, max_conflicts=0, max_restarts=0,
           use_phase_save=0):
    """Call solver_solve with given options."""
    SolveOpts = lib._SolveOpts
    opts = SolveOpts(seed=seed, max_conflicts=max_conflicts,
                     max_restarts=max_restarts, use_phase_save=use_phase_save)
    return lib.solver_solve(ctx, ctypes.byref(opts))


def _val(lib, ctx, var_id):
    return lib.solver_get_value(ctx, var_id)


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

def test_unconstrained_two_vars(libzsp):
    """Unconstrained 2-var problem: solution found, both values in domain."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (32, 1, 0, 9),   # x in [0,9]
        (32, 1, 0, 9),   # y in [0,9]
    ])

    rc = _solve(lib, ctx)
    assert rc == SOLVE_OK, f"Expected SOLVE_OK, got {rc}"

    x = _val(lib, ctx, 0)
    y = _val(lib, ctx, 1)
    assert 0 <= x <= 9, f"x={x} out of domain [0,9]"
    assert 0 <= y <= 9, f"y={y} out of domain [0,9]"

    lib.zsp_block_alloc_destroy(ba)


def test_add_constraint_x_plus_y_eq_7(libzsp):
    """x+y=7; x∈[0,10], y∈[0,10]: solution satisfies x+y=7."""
    lib = libzsp
    _setup(lib)

    # vars: r=result of x+y, x, y
    # constraint: r == 7 AND r = x + y
    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (32, 1, 0, 20),  # 0: r = x+y
        (32, 1, 0, 10),  # 1: x
        (32, 1, 0, 10),  # 2: y
    ])

    # Add propagator: r = x + y
    lib.prop_add_bounds_add_32(ctx, 0, 1, 2, 0)

    # Fix r = 7
    lib.ctx_tighten_lb32(ctx, 0, 7)
    lib.ctx_tighten_ub32(ctx, 0, 7)

    rc = _solve(lib, ctx)
    assert rc == SOLVE_OK, f"Expected SOLVE_OK, got {rc}"

    x = _val(lib, ctx, 1)
    y = _val(lib, ctx, 2)
    r = _val(lib, ctx, 0)

    assert r == 7, f"r={r} should be 7"
    assert x + y == 7, f"x={x} + y={y} != 7"
    assert 0 <= x <= 10
    assert 0 <= y <= 10

    lib.zsp_block_alloc_destroy(ba)


def test_unsatisfiable(libzsp):
    """x in [0,10]; constrain x≥5 AND x≤3 → SOLVE_UNSAT."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (32, 1, 0, 10),  # x in [0,10]
    ])

    # Impose conflicting bounds before solve
    lib.ctx_tighten_lb32(ctx, 0, 5)   # x >= 5
    lib.ctx_tighten_ub32(ctx, 0, 3)   # x <= 3  → domain empty

    # Propagation at level 0 should detect conflict
    rc = _solve(lib, ctx)
    assert rc == SOLVE_UNSAT, f"Expected SOLVE_UNSAT, got {rc}"

    lib.zsp_block_alloc_destroy(ba)


def test_restart_fires_and_finds_solution(libzsp):
    """Force restarts with max_conflicts=1; solution still found."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (32, 1, 0, 9),
        (32, 1, 0, 9),
    ])

    # Very low conflict budget → many restarts; max_restarts=0 means unlimited
    rc = _solve(lib, ctx, seed=0xABCD, max_conflicts=1, max_restarts=0)
    # Should still find a solution eventually
    assert rc == SOLVE_OK, f"Expected SOLVE_OK with restarts, got {rc}"

    x = _val(lib, ctx, 0)
    y = _val(lib, ctx, 1)
    assert 0 <= x <= 9
    assert 0 <= y <= 9

    lib.zsp_block_alloc_destroy(ba)


def test_seeded_reproducible(libzsp):
    """Same seed → same solution on two independent runs."""
    lib = libzsp
    _setup(lib)

    SEED = 0xDEADBEEF_CAFEBABE

    def run_with_seed(seed):
        sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
            (32, 1, 0, 99),
            (32, 1, 0, 99),
            (32, 1, 0, 99),
        ])
        rc = _solve(lib, ctx, seed=seed)
        assert rc == SOLVE_OK
        vals = tuple(_val(lib, ctx, i) for i in range(3))
        lib.zsp_block_alloc_destroy(ba)
        return vals

    v1 = run_with_seed(SEED)
    v2 = run_with_seed(SEED)
    assert v1 == v2, f"Same seed produced different values: {v1} vs {v2}"


def test_different_seeds_different_values(libzsp):
    """Different seeds produce at least one different solution across 8 runs."""
    lib = libzsp
    _setup(lib)

    results = set()
    for seed in range(1, 9):
        sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
            (32, 1, 0, 99),
        ])
        rc = _solve(lib, ctx, seed=seed * 0x1111111111111111)
        assert rc == SOLVE_OK
        results.add(_val(lib, ctx, 0))
        lib.zsp_block_alloc_destroy(ba)

    # With 8 different seeds on a 100-value domain we expect > 1 distinct value
    assert len(results) > 1, \
        f"All 8 seeds produced the same value {results} — RNG may be broken"


def test_randc_all_values_seen(libzsp):
    """x∈[0,3] (4 values): run many times with different seeds; all 4 values seen."""
    lib = libzsp
    _setup(lib)

    seen = set()
    for i in range(64):
        # Use a wide spread of seeds to exercise the full domain
        seed = (i + 1) * 0x9E3779B97F4A7C15  # Fibonacci hashing multiplier
        sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
            (32, 1, 0, 3),   # x in {0,1,2,3}
        ])
        rc = _solve(lib, ctx, seed=seed)
        assert rc == SOLVE_OK
        seen.add(_val(lib, ctx, 0))
        lib.zsp_block_alloc_destroy(ba)
        if seen == {0, 1, 2, 3}:
            break  # early exit once all values covered

    assert seen == {0, 1, 2, 3}, \
        f"Not all values seen after 64 runs: {seen}"
