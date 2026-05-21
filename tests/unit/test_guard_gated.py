"""Unit tests for guard-gated propagator infrastructure (Sprint 2.1).

Verifies that propagators with a guard variable:
- Fire normally when guard is pinned to 1
- Are entailed (skipped permanently) when guard is pinned to 0
- Are deferred when guard is undecided [0,1]
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL     = 0xFFFF_FFFF
PROP_OK       = 0
PROP_CONFLICT = 1
PROP_ENTAILED = 2
SOLVE_OK      = 0

_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 524288


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

    lib.zsp_var_lo64.restype  = ctypes.c_int64
    lib.zsp_var_lo64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_hi64.restype  = ctypes.c_int64
    lib.zsp_var_hi64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.solver_propagate.restype  = ctypes.c_int
    lib.solver_propagate.argtypes = [ctypes.c_void_p]

    lib.ctx_tighten_lb64.restype  = ctypes.c_int
    lib.ctx_tighten_lb64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int64]
    lib.ctx_tighten_ub64.restype  = ctypes.c_int
    lib.ctx_tighten_ub64.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int64]
    lib.ctx_tighten_lb32.restype  = ctypes.c_int
    lib.ctx_tighten_lb32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int32]
    lib.ctx_tighten_ub32.restype  = ctypes.c_int
    lib.ctx_tighten_ub32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int32]

    lib.prop_add_bounds_le_32.restype  = ctypes.c_uint32
    lib.prop_add_bounds_le_32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                          ctypes.c_uint32, ctypes.c_uint8]

    lib.prop_set_guard.restype  = None
    lib.prop_set_guard.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                   ctypes.c_uint32]

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


def _make_ctx(lib, var_specs):
    sp_buf  = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp
    for i, (width, is_signed, lo, hi) in enumerate(var_specs):
        ref = lib.problem_add_var(sp, i, width, is_signed, lo, hi)
        assert ref != EXPR_NULL
    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0
    return sp_buf, ctx_buf, ba, sp, ctx


def test_guard_true_fires(libzsp):
    """LE propagator with guard=1 fires normally: x <= y, y=5 -> x.hi=5."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (32, 1, 0, 100),  # 0: x (signed, tier-0)
        (32, 1, 5,   5),  # 1: y = 5
        (1,  1, 0,   1),  # 2: guard (boolean)
    ])

    # Add LE propagator: x <= y
    ref = lib.prop_add_bounds_le_32(ctx, 0, 1, 0)
    assert ref != EXPR_NULL

    # Set guard to var 2
    lib.prop_set_guard(ctx, ref, 2)

    # Pin guard to 1
    lib.ctx_tighten_lb32(ctx, 2, 1)
    lib.ctx_tighten_ub32(ctx, 2, 1)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    # Propagator should have fired: x.hi <= 5
    assert lib.zsp_var_hi64(ctx, 0) <= 5

    lib.zsp_block_alloc_destroy(ba)


def test_guard_false_entails(libzsp):
    """LE propagator with guard=0 is entailed: x unchanged."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (32, 1, 0, 100),  # 0: x
        (32, 1, 5,   5),  # 1: y = 5
        (1,  1, 0,   1),  # 2: guard
    ])

    ref = lib.prop_add_bounds_le_32(ctx, 0, 1, 0)
    assert ref != EXPR_NULL
    lib.prop_set_guard(ctx, ref, 2)

    # Pin guard to 0
    lib.ctx_tighten_lb32(ctx, 2, 0)
    lib.ctx_tighten_ub32(ctx, 2, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    # Propagator should NOT have fired: x.hi still 100
    assert lib.zsp_var_hi64(ctx, 0) == 100

    lib.zsp_block_alloc_destroy(ba)


def test_guard_undecided_skips(libzsp):
    """LE propagator with guard in [0,1] does not fire yet."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (32, 1, 0, 100),  # 0: x
        (32, 1, 5,   5),  # 1: y = 5
        (1,  1, 0,   1),  # 2: guard (undecided)
    ])

    ref = lib.prop_add_bounds_le_32(ctx, 0, 1, 0)
    assert ref != EXPR_NULL
    lib.prop_set_guard(ctx, ref, 2)

    # Don't pin guard -- it stays [0,1]
    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    # Propagator should not have fired: x.hi still 100
    assert lib.zsp_var_hi64(ctx, 0) == 100

    lib.zsp_block_alloc_destroy(ba)


def test_guard_becomes_true_during_search(libzsp):
    """Guard starts undecided, gets pinned to 1, propagator then fires."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_ctx(lib, [
        (32, 1, 0, 100),  # 0: x
        (32, 1, 5,   5),  # 1: y = 5
        (1,  1, 0,   1),  # 2: guard
    ])

    ref = lib.prop_add_bounds_le_32(ctx, 0, 1, 0)
    assert ref != EXPR_NULL
    lib.prop_set_guard(ctx, ref, 2)

    # Initial propagation: guard undecided, LE should not fire
    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK
    assert lib.zsp_var_hi64(ctx, 0) == 100

    # Now pin guard to 1
    lib.ctx_tighten_lb32(ctx, 2, 1)
    lib.ctx_tighten_ub32(ctx, 2, 1)

    # Propagate again: LE should now fire
    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK
    assert lib.zsp_var_hi64(ctx, 0) <= 5

    lib.zsp_block_alloc_destroy(ba)
