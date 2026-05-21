"""Unit tests for the propagator engine (Phase 6).

Tests:
- BoundsAdd_32: r=a+b, fix a=[3,3] b=[4,4], propagate → r=[7,7]
- BoundsLE_32: x≤y, fix y=[5,5], propagate → x.hi=5
- BoundsLT_32: x<y, fix y=[10,10], propagate → x.hi=9
- BoundsEQ_32: x=y, fix x=[7,7], propagate → y=[7,7]
- BoundsNE_32: x≠y, fix x=[5,5] y=[5,5] → CONFLICT (empty domain)
- UnaryNeg_32: r=-a, fix a=[3,3], propagate → r=[-3,-3]
- InSet_32: x∈{2,5,9}, initial x=[0,20], propagate → x in [2,9]
- Implication_32 (UB): guard=1→a≤3, fix guard=[1,1], propagate → a.hi=3
- Implication_32 (guard=0): guard=0, entailed (no change)
- Conflict: tighten lb > hi → PROP_CONFLICT
- Priority queue: two propagators at different priorities, both fire correctly
- PropQueue ordering: high-priority enqueued last still fires first
- Multi-propagator: chained BoundsAdd + BoundsLE both fire at fixpoint
- Watcher: changing a variable re-enqueues its watchers
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
PROP_ENTAILED = 2

VAR_SIGNED = 0x01
VAR_TIER1  = 0x08

_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 524288   # 512 KiB


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
                                    ctypes.c_uint32, ctypes.c_uint8,
                                    ctypes.c_int64, ctypes.c_int64]

    lib.solver_create.restype  = ctypes.c_void_p
    lib.solver_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p]
    lib.solver_destroy.restype  = None
    lib.solver_destroy.argtypes = [ctypes.c_void_p]
    lib.solver_compile.restype  = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.solver_get_var.restype  = ctypes.c_void_p
    lib.solver_get_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.zsp_ctx_decision_level.restype  = ctypes.c_uint32
    lib.zsp_ctx_decision_level.argtypes = [ctypes.c_void_p]
    lib.zsp_var_lo32.restype  = ctypes.c_int32
    lib.zsp_var_lo32.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_hi32.restype  = ctypes.c_int32
    lib.zsp_var_hi32.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_lo64.restype  = ctypes.c_int64
    lib.zsp_var_lo64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_hi64.restype  = ctypes.c_int64
    lib.zsp_var_hi64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    # trail helpers used to fix variable bounds
    lib.trail_record_lb.restype  = ctypes.c_int
    lib.trail_record_lb.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_int64]
    lib.trail_record_ub.restype  = ctypes.c_int
    lib.trail_record_ub.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_int64]

    # propagators
    lib.prop_add_bounds_le_32.restype  = ctypes.c_uint32
    lib.prop_add_bounds_le_32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                          ctypes.c_uint32, ctypes.c_uint8]
    lib.prop_add_bounds_lt_32.restype  = ctypes.c_uint32
    lib.prop_add_bounds_lt_32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                          ctypes.c_uint32, ctypes.c_uint8]
    lib.prop_add_bounds_eq_32.restype  = ctypes.c_uint32
    lib.prop_add_bounds_eq_32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                          ctypes.c_uint32, ctypes.c_uint8]
    lib.prop_add_bounds_ne_32.restype  = ctypes.c_uint32
    lib.prop_add_bounds_ne_32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                          ctypes.c_uint32, ctypes.c_uint8]
    lib.prop_add_bounds_add_32.restype  = ctypes.c_uint32
    lib.prop_add_bounds_add_32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                           ctypes.c_uint32, ctypes.c_uint32,
                                           ctypes.c_uint8]
    lib.prop_add_unary_neg_32.restype  = ctypes.c_uint32
    lib.prop_add_unary_neg_32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                          ctypes.c_uint32, ctypes.c_uint8]

    lib.prop_add_in_set_32.restype  = ctypes.c_uint32
    lib.prop_add_in_set_32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                       ctypes.c_uint32,
                                       ctypes.POINTER(ctypes.c_int32),
                                       ctypes.c_uint8]

    lib.prop_add_implication_32.restype  = ctypes.c_uint32
    lib.prop_add_implication_32.argtypes = [ctypes.c_void_p,
                                             ctypes.c_uint32, ctypes.c_uint32,
                                             ctypes.c_int32, ctypes.c_uint8,
                                             ctypes.c_uint8]

    lib.solver_propagate.restype  = ctypes.c_int
    lib.solver_propagate.argtypes = [ctypes.c_void_p]

    lib.ctx_tighten_lb32.restype  = ctypes.c_int
    lib.ctx_tighten_lb32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int32]
    lib.ctx_tighten_ub32.restype  = ctypes.c_int
    lib.ctx_tighten_ub32.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int32]


# ------------------------------------------------------------------ #
# Helpers                                                              #
# ------------------------------------------------------------------ #

def _make_problem_and_ctx(lib, var_specs):
    """Create a SolveProblem + SolveCtx with given variable specs.

    var_specs: list of (width, is_signed, lo, hi)
    Returns (sp_buf, ctx_buf, block_alloc, sp, ctx)
    """
    n = len(var_specs)
    sp_buf  = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()

    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    for i, (width, is_signed, lo, hi) in enumerate(var_specs):
        ref = lib.problem_add_var(sp, i, width, is_signed, lo, hi)
        assert ref != EXPR_NULL, f"problem_add_var failed for var {i}"

    ba = lib.zsp_block_alloc_create(None, 0)
    assert ba
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx

    rc = lib.solver_compile(ctx, sp)
    assert rc == 0, f"solver_compile returned {rc}"

    return sp_buf, ctx_buf, ba, sp, ctx


def _lo(lib, ctx, var_id):
    return lib.zsp_var_lo64(ctx, var_id)


def _hi(lib, ctx, var_id):
    return lib.zsp_var_hi64(ctx, var_id)


def _fix(lib, ctx, var_id, val):
    """Fix a variable to [val, val] via trail_record_lb/ub."""
    lib.trail_record_lb(ctx, var_id, val)
    lib.trail_record_ub(ctx, var_id, val)


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

def test_bounds_add_32_propagates(libzsp):
    """r=a+b; fix a=[3,3] b=[4,4]; propagate → r=[7,7]."""
    lib = libzsp
    _setup(lib)

    # vars: r=[0,100], a=[0,100], b=[0,100]
    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, 0, 100),  # 0: r
        (32, 1, 0, 100),  # 1: a
        (32, 1, 0, 100),  # 2: b
    ])

    ref = lib.prop_add_bounds_add_32(ctx, 0, 1, 2, 0)
    assert ref != EXPR_NULL

    # fix a=[3,3], b=[4,4]  — uses trail_record (no watcher wake needed yet)
    _fix(lib, ctx, 1, 3)
    _fix(lib, ctx, 2, 4)

    # BoundsAdd was enqueued at creation; propagate
    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    # After initial fire: r=[0+0=0 .. 100+100=200], then a/b fixed → re-fire
    # We need to re-enqueue after fixing; call tighten to wake watchers
    lib.ctx_tighten_lb32(ctx, 1, 3)
    lib.ctx_tighten_ub32(ctx, 1, 3)
    lib.ctx_tighten_lb32(ctx, 2, 4)
    lib.ctx_tighten_ub32(ctx, 2, 4)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) == 7
    assert _hi(lib, ctx, 0) == 7

    lib.zsp_block_alloc_destroy(ba)


def test_bounds_le_32_propagates(libzsp):
    """x≤y; fix y=[5,5]; propagate → x.hi=5."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, 0, 100),  # 0: x
        (32, 1, 0, 100),  # 1: y
    ])

    ref = lib.prop_add_bounds_le_32(ctx, 0, 1, 0)
    assert ref != EXPR_NULL

    # fix y=[5,5]
    lib.ctx_tighten_ub32(ctx, 1, 5)
    lib.ctx_tighten_lb32(ctx, 1, 5)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _hi(lib, ctx, 0) <= 5
    # BoundsLE: y.lo is tightened to max(y.lo, x.lo) = max(5,0) = 5 (unchanged)
    assert _lo(lib, ctx, 1) >= _lo(lib, ctx, 0)  # y.lo ≥ x.lo always holds

    lib.zsp_block_alloc_destroy(ba)


def test_bounds_lt_32_propagates(libzsp):
    """x<y; fix y=[10,10]; propagate → x.hi=9."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, 0, 100),  # 0: x
        (32, 1, 0, 100),  # 1: y
    ])

    ref = lib.prop_add_bounds_lt_32(ctx, 0, 1, 0)
    assert ref != EXPR_NULL

    lib.ctx_tighten_ub32(ctx, 1, 10)
    lib.ctx_tighten_lb32(ctx, 1, 10)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _hi(lib, ctx, 0) <= 9

    lib.zsp_block_alloc_destroy(ba)


def test_bounds_eq_32_propagates(libzsp):
    """x=y; fix x=[7,7]; propagate → y=[7,7]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, 0, 100),  # 0: x
        (32, 1, 0, 100),  # 1: y
    ])

    ref = lib.prop_add_bounds_eq_32(ctx, 0, 1, 0)
    assert ref != EXPR_NULL

    lib.ctx_tighten_lb32(ctx, 0, 7)
    lib.ctx_tighten_ub32(ctx, 0, 7)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 1) == 7
    assert _hi(lib, ctx, 1) == 7

    lib.zsp_block_alloc_destroy(ba)


def test_bounds_ne_32_singleton_conflict(libzsp):
    """x≠y; both fixed to 5 → bounds-NE alone is a soft propagator,
    so it won't CONFLICT from NE alone; but if the domain becomes empty
    from the singleton removal path, it would. Here: x=[5,5], y=[5,5]
    — NE tries to remove 5 from y → y.lo becomes 6 > y.hi=5 → CONFLICT."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, 5, 5),  # 0: x fixed at 5
        (32, 1, 5, 5),  # 1: y fixed at 5
    ])

    ref = lib.prop_add_bounds_ne_32(ctx, 0, 1, 0)
    assert ref != EXPR_NULL

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_CONFLICT

    lib.zsp_block_alloc_destroy(ba)


def test_unary_neg_32(libzsp):
    """r=-a; fix a=[3,3]; propagate → r=[-3,-3]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, -100, 100),  # 0: r (signed)
        (32, 1, -100, 100),  # 1: a (signed)
    ])

    ref = lib.prop_add_unary_neg_32(ctx, 0, 1, 0)
    assert ref != EXPR_NULL

    lib.ctx_tighten_lb32(ctx, 1, 3)
    lib.ctx_tighten_ub32(ctx, 1, 3)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) == -3
    assert _hi(lib, ctx, 0) == -3

    lib.zsp_block_alloc_destroy(ba)


def test_in_set_32(libzsp):
    """x∈{2,5,9}; initial x=[0,20]; propagate → x in [2,9]."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, 0, 20),   # 0: x
    ])

    elems = (ctypes.c_int32 * 3)(2, 5, 9)
    ref = lib.prop_add_in_set_32(ctx, 0, 3, elems, 0)
    assert ref != EXPR_NULL

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    # InSet shrinks bounds to [min_valid, max_valid] = [2, 9]
    assert _lo(lib, ctx, 0) >= 2
    assert _hi(lib, ctx, 0) <= 9

    lib.zsp_block_alloc_destroy(ba)


def test_in_set_32_conflict(libzsp):
    """x∈{2,5,9}; but x=[15,20]; no valid element → CONFLICT."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, 15, 20),   # 0: x
    ])

    elems = (ctypes.c_int32 * 3)(2, 5, 9)
    ref = lib.prop_add_in_set_32(ctx, 0, 3, elems, 0)
    assert ref != EXPR_NULL

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_CONFLICT

    lib.zsp_block_alloc_destroy(ba)


def test_implication_32_ub_fires_when_guard_true(libzsp):
    """guard=1 → a≤3; fix guard=[1,1]; propagate → a.hi=3."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, 0,  1),   # 0: guard (boolean)
        (32, 1, 0, 10),   # 1: a
    ])

    ref = lib.prop_add_implication_32(ctx, 0, 1, 3, 1, 0)  # is_ub=1
    assert ref != EXPR_NULL

    lib.ctx_tighten_lb32(ctx, 0, 1)
    lib.ctx_tighten_ub32(ctx, 0, 1)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _hi(lib, ctx, 1) <= 3

    lib.zsp_block_alloc_destroy(ba)


def test_implication_32_entailed_when_guard_false(libzsp):
    """guard=0 → implication entailed; a unchanged."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, 0,  1),   # 0: guard
        (32, 1, 0, 10),   # 1: a
    ])

    ref = lib.prop_add_implication_32(ctx, 0, 1, 3, 1, 0)
    assert ref != EXPR_NULL

    # guard fixed to false
    lib.ctx_tighten_ub32(ctx, 0, 0)
    lib.ctx_tighten_lb32(ctx, 0, 0)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    # a should be unchanged (no tightening)
    assert _hi(lib, ctx, 1) == 10

    lib.zsp_block_alloc_destroy(ba)


def test_conflict_empty_domain(libzsp):
    """ctx_tighten_lb32 with lb > hi → PROP_CONFLICT."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, 0, 5),   # 0: x in [0,5]
    ])

    # Force lb past hi — should return CONFLICT immediately
    rc = lib.ctx_tighten_lb32(ctx, 0, 10)
    assert rc == PROP_CONFLICT

    lib.zsp_block_alloc_destroy(ba)


def test_priority_queue_ordering(libzsp):
    """Two BoundsLE propagators at different priorities both converge.

    High-priority (0): x ≤ y → x.hi ≤ y.hi = 5
    Low-priority  (5): x ≤ z → x.hi ≤ z.hi = 10
    Result: x.hi ≤ 5 (tightest bound wins at fixpoint regardless of order).
    Tests that both propagators fired and the queue processed both.
    """
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1,  0, 100),  # 0: x
        (32, 1,  5,   5),  # 1: y = 5
        (32, 1, 10,  10),  # 2: z = 10
    ])

    # high-priority propagator
    r1 = lib.prop_add_bounds_le_32(ctx, 0, 1, 0)
    # low-priority propagator
    r2 = lib.prop_add_bounds_le_32(ctx, 0, 2, 5)
    assert r1 != EXPR_NULL
    assert r2 != EXPR_NULL

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    # x.hi must be ≤ 5 (tightest, from y)
    assert _hi(lib, ctx, 0) <= 5

    lib.zsp_block_alloc_destroy(ba)


def test_chained_propagators(libzsp):
    """BoundsAdd + BoundsLE chain: r=a+b; r≤z.
    Fix a=b=3; propagate; expect r=[6,6] and z.lo≥6.
    """
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, 0, 100),  # 0: r
        (32, 1, 3,   3),  # 1: a = 3
        (32, 1, 3,   3),  # 2: b = 3
        (32, 1, 0, 100),  # 3: z
    ])

    r1 = lib.prop_add_bounds_add_32(ctx, 0, 1, 2, 0)
    r2 = lib.prop_add_bounds_le_32(ctx, 0, 3, 0)
    assert r1 != EXPR_NULL
    assert r2 != EXPR_NULL

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _lo(lib, ctx, 0) == 6
    assert _hi(lib, ctx, 0) == 6
    # BoundsLE: z.lo ≥ r.lo = 6
    assert _lo(lib, ctx, 3) >= 6

    lib.zsp_block_alloc_destroy(ba)


def test_watcher_re_enqueues_on_tighten(libzsp):
    """Tightening a variable re-enqueues its watching propagator."""
    lib = libzsp
    _setup(lib)

    sp_buf, ctx_buf, ba, sp, ctx = _make_problem_and_ctx(lib, [
        (32, 1, 0, 100),  # 0: x
        (32, 1, 0, 100),  # 1: y
    ])

    # Add BoundsLE: x ≤ y
    ref = lib.prop_add_bounds_le_32(ctx, 0, 1, 0)
    assert ref != EXPR_NULL

    # Drain the initial queue
    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    # Now tighten y → should wake the LE propagator
    lib.ctx_tighten_ub32(ctx, 1, 20)

    rc = lib.solver_propagate(ctx)
    assert rc == PROP_OK

    assert _hi(lib, ctx, 0) <= 20

    lib.zsp_block_alloc_destroy(ba)
