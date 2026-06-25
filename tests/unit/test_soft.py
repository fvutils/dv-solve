"""Unit tests for soft constraints (Sprint 6).

Tests assumption-based soft constraint relaxation.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL     = 0xFFFF_FFFF
PROP_OK       = 0
PROP_CONFLICT = 1
SOLVE_OK      = 0
SOLVE_UNSAT   = 1

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
    lib.problem_add_constraint.restype  = ctypes.c_uint32
    lib.problem_add_constraint.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.problem_add_soft_constraint.restype  = ctypes.c_uint32
    lib.problem_add_soft_constraint.argtypes = [ctypes.c_void_p,
                                                ctypes.c_uint32, ctypes.c_uint32]
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

    lib.zsp_var_lo64.restype  = ctypes.c_int64
    lib.zsp_var_lo64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_var_hi64.restype  = ctypes.c_int64
    lib.zsp_var_hi64.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.solver_propagate.restype  = ctypes.c_int
    lib.solver_propagate.argtypes = [ctypes.c_void_p]

    lib.solver_soft_active.restype  = ctypes.c_int
    lib.solver_soft_active.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

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
    lib.solver_reset.restype  = None
    lib.solver_reset.argtypes = [ctypes.c_void_p]
    lib.solver_get_value.restype  = ctypes.c_int64
    lib.solver_get_value.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


BIN_EQ  = 10
BIN_LTE = 13
BIN_GT  = 14
BIN_GTE = 15


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

def test_soft_all_satisfiable(libzsp):
    """Hard: x in [0,10]. Soft: x == 5. Solution should be x=5."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 10]
    lib.problem_add_var(sp, 0, 8, 0, 0, 10)

    # Soft: x == 5 (priority 0 = highest)
    v_x = lib.expr_var(sp, 0)
    c5 = lib.expr_const(sp, 5, 0)
    eq_e = lib.expr_binary(sp, BIN_EQ, v_x, c5)
    lib.problem_add_soft_constraint(sp, eq_e, 0)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x1234)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    x = lib.solver_get_value(ctx, 0)
    assert x == 5, f"x={x}, expected 5"

    # Soft should be active (not relaxed)
    assert lib.solver_soft_active(ctx, 0) == 1

    lib.zsp_block_alloc_destroy(ba)


def test_soft_one_relaxed(libzsp):
    """Hard: x > 7. Soft: x == 5. Soft must be relaxed. Solution: x > 7."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 20]
    lib.problem_add_var(sp, 0, 8, 0, 0, 20)

    # Hard: x > 7  (i.e. x >= 8)
    v_x = lib.expr_var(sp, 0)
    c7 = lib.expr_const(sp, 7, 0)
    gt_e = lib.expr_binary(sp, BIN_GT, v_x, c7)
    lib.problem_add_constraint(sp, gt_e)

    # Soft: x == 5 (conflicts with hard)
    c5 = lib.expr_const(sp, 5, 0)
    v_x2 = lib.expr_var(sp, 0)
    eq_e = lib.expr_binary(sp, BIN_EQ, v_x2, c5)
    lib.problem_add_soft_constraint(sp, eq_e, 0)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x5678)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    x = lib.solver_get_value(ctx, 0)
    assert x > 7, f"x={x}, expected > 7"

    # Soft should be relaxed
    assert lib.solver_soft_active(ctx, 0) == 0

    lib.zsp_block_alloc_destroy(ba)


def test_soft_priority_ordering(libzsp):
    """Two conflicting soft constraints with different priorities.
    Lower priority (higher number) is relaxed first."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 20]
    lib.problem_add_var(sp, 0, 8, 0, 0, 20)

    # Hard: x >= 10
    v_x = lib.expr_var(sp, 0)
    c10 = lib.expr_const(sp, 10, 0)
    gte_e = lib.expr_binary(sp, BIN_GTE, v_x, c10)
    lib.problem_add_constraint(sp, gte_e)

    # Soft 0: x == 3 (priority 0 = high, should try to keep)
    v_x2 = lib.expr_var(sp, 0)
    c3 = lib.expr_const(sp, 3, 0)
    eq3_e = lib.expr_binary(sp, BIN_EQ, v_x2, c3)
    lib.problem_add_soft_constraint(sp, eq3_e, 0)

    # Soft 1: x == 5 (priority 10 = low, should be relaxed first)
    v_x3 = lib.expr_var(sp, 0)
    c5 = lib.expr_const(sp, 5, 0)
    eq5_e = lib.expr_binary(sp, BIN_EQ, v_x3, c5)
    lib.problem_add_soft_constraint(sp, eq5_e, 10)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0xABCD)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    x = lib.solver_get_value(ctx, 0)
    assert x >= 10, f"x={x}, expected >= 10"

    # Both soft constraints conflict with hard (x >= 10), both must be relaxed.
    # Assumption indices are reversed from add order (prepended list):
    # assumption 0 = soft 1 (x==5, pri 10) -> relaxed
    # assumption 1 = soft 0 (x==3, pri 0)  -> relaxed
    assert lib.solver_soft_active(ctx, 0) == 0  # both relaxed
    assert lib.solver_soft_active(ctx, 1) == 0

    lib.zsp_block_alloc_destroy(ba)


def test_soft_multiple_relaxed(libzsp):
    """Three soft constraints, two conflict with hard. Both relaxed,
    third remains active."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 20], y in [0, 20]
    lib.problem_add_var(sp, 0, 8, 0, 0, 20)
    lib.problem_add_var(sp, 1, 8, 0, 0, 20)

    # Hard: x >= 10
    v_x = lib.expr_var(sp, 0)
    c10 = lib.expr_const(sp, 10, 0)
    gte_e = lib.expr_binary(sp, BIN_GTE, v_x, c10)
    lib.problem_add_constraint(sp, gte_e)

    # Soft 0: x == 3 (priority 5, conflicts with hard)
    v_x2 = lib.expr_var(sp, 0)
    c3 = lib.expr_const(sp, 3, 0)
    eq3_e = lib.expr_binary(sp, BIN_EQ, v_x2, c3)
    lib.problem_add_soft_constraint(sp, eq3_e, 5)

    # Soft 1: x == 5 (priority 10, conflicts with hard)
    v_x3 = lib.expr_var(sp, 0)
    c5 = lib.expr_const(sp, 5, 0)
    eq5_e = lib.expr_binary(sp, BIN_EQ, v_x3, c5)
    lib.problem_add_soft_constraint(sp, eq5_e, 10)

    # Soft 2: y == 7 (priority 1, does NOT conflict, should be active)
    v_y = lib.expr_var(sp, 1)
    c7 = lib.expr_const(sp, 7, 0)
    eq7_e = lib.expr_binary(sp, BIN_EQ, v_y, c7)
    lib.problem_add_soft_constraint(sp, eq7_e, 1)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x9999)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    x = lib.solver_get_value(ctx, 0)
    y = lib.solver_get_value(ctx, 1)
    assert x >= 10, f"x={x}, expected >= 10"
    assert y == 7, f"y={y}, expected 7"

    # Soft constraints are stored in reverse add order (prepended list):
    # assumption 0 = soft 2 (y==7, pri 1) -> should be active
    # assumption 1 = soft 1 (x==5, pri 10) -> should be relaxed
    # assumption 2 = soft 0 (x==3, pri 5) -> should be relaxed
    assert lib.solver_soft_active(ctx, 0) == 1   # y==7, active
    assert lib.solver_soft_active(ctx, 1) == 0   # x==5, relaxed
    assert lib.solver_soft_active(ctx, 2) == 0   # x==3, relaxed

    lib.zsp_block_alloc_destroy(ba)


# NOTE (DSE-3 follow-up): the *serve* path (zsp_bbsolver_check_maxsat) uses additive
# greedy and is collateral-free — locked by
# test_soft_maxsat_serve.test_serve_soft_no_collateral_drop. The *primary* path
# (solver_solve) keeps the subtractive relaxation and can shed a satisfiable
# lower-preference soft as collateral when a higher-preference sibling conflicts; an
# additive primary rewrite is blocked on a deeper engine quirk (pinning all assumption
# vars to 0 + re-solve spuriously reports UNSAT for >1 soft). Tracked in
# dv_solve_soft_constraints_engine_plan.md DSE-3. Real soft-heavy/conflicting RandSets
# force-serve and take the corrected serve path; no primary collateral test is asserted
# here until that quirk is fixed.


def test_soft_priority_ladder_three_primary(libzsp):
    """DSE-3 ladder (primary path). Three mutually-exclusive softs at distinct
    priorities; all conflict pairwise so only one survives. C priority is
    `lower value = keep-harder` (0 = highest preference), so x==10 @ pri0 is kept
    and the pri5/pri10 softs relax. The matched serve-path test
    (`test_soft_maxsat_serve.test_serve_soft_priority_ladder_three`) asserts the
    SAME kept value — locking primary order == serve order."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 8, 0, 0, 100)   # x in [0,100]

    # Soft 0: x == 10, priority 0 (keep-hardest)
    s0 = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 0), lib.expr_const(sp, 10, 0))
    lib.problem_add_soft_constraint(sp, s0, 0)
    # Soft 1: x == 20, priority 5
    s1 = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 0), lib.expr_const(sp, 20, 0))
    lib.problem_add_soft_constraint(sp, s1, 5)
    # Soft 2: x == 30, priority 10 (relax-first)
    s2 = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 0), lib.expr_const(sp, 30, 0))
    lib.problem_add_soft_constraint(sp, s2, 10)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0xC0DE)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    x = lib.solver_get_value(ctx, 0)
    assert x == 10, f"x={x}, expected 10 (highest-preference soft kept)"

    lib.zsp_block_alloc_destroy(ba)


def test_soft_no_collateral_drop_primary(libzsp):
    """Primary-path collateral-shedding regression — the root cause of the
    intermittent `ve/unit/test_constraint_soft.py::test_soft_nested` failure.

    Hard x==20. Three softs (add order / priority): a==11@0, x==5@1, d==40@2.
    Only x==5 conflicts (with hard x==20); a==11 and d==40 are independent and
    trivially satisfiable, so a maximal priority-respecting soft set MUST keep
    both. The bare subtractive relaxation drops the lowest-preference (highest
    priority *value*) active soft on each UNSAT, so to reach the conflicting x==5
    (pri 1) it first sheds d==40 (pri 2) as collateral and returns SOLVE_OK with
    d != 40. The additive re-add refinement recovers d==40 (re-adding it keeps the
    problem SAT). Mirrors the serve-path lock
    `test_soft_maxsat_serve.test_serve_soft_no_collateral_drop` → primary == serve.

    EQ-only softs deliberately: they compile to guard-gated implication
    propagators that relax cleanly. (A `!=` soft falls to the generic
    compile-with-guard path whose compile-time tightening does not fully undo on
    relaxation — a separate, escalation-covered limitation, see zsp_compile.c."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    for vid in range(3):                       # 0=a 1=x 2=d
        lib.problem_add_var(sp, vid, 8, 0, 0, 100)
    lib.problem_add_constraint(sp, lib.expr_binary(
        sp, BIN_EQ, lib.expr_var(sp, 1), lib.expr_const(sp, 20, 0)))   # hard x==20
    for vid, k, pri in [(0, 11, 0), (1, 5, 1), (2, 40, 2)]:           # a==11,x==5,d==40
        e = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, vid), lib.expr_const(sp, k, 0))
        lib.problem_add_soft_constraint(sp, e, pri)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0xBEEF)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    a = lib.solver_get_value(ctx, 0)
    x = lib.solver_get_value(ctx, 1)
    d = lib.solver_get_value(ctx, 2)
    assert x == 20, f"hard must hold: x={x}"
    assert a == 11, f"a={a}: a==11 dropped as collateral"
    assert d == 40, f"d={d}: d==40 dropped as collateral (the test_soft_nested bug)"

    lib.zsp_block_alloc_destroy(ba)


def test_soft_resolve_reuse_keeps_set(libzsp):
    """Regression: solving the SAME ctx twice (the backend's plan-reuse path:
    solver_reset + solver_solve) must keep the same maximal soft set both times.
    Previously solver_solve did not re-activate assumption_active_mask at entry,
    so the second solve inherited the first solve's relaxations, then relaxed the
    remaining kept soft on the conflict and dropped the WHOLE set (the var came
    back unconstrained). Here a 3-soft conflicting ladder must keep x==10 on every
    solve of the reused ctx."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 8, 0, 0, 100)   # x in [0,100]
    for val, pri in ((10, 0), (20, 5), (30, 10)):   # all conflict; pri 0 kept
        e = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 0),
                            lib.expr_const(sp, val, 0))
        lib.problem_add_soft_constraint(sp, e, pri)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    for attempt in range(4):
        if attempt > 0:
            lib.solver_reset(ctx)
        opts = lib._SolveOpts(seed=0x1000 + attempt)
        result = lib.solver_solve(ctx, ctypes.byref(opts))
        assert result == SOLVE_OK, f"attempt {attempt}: solve failed"
        x = lib.solver_get_value(ctx, 0)
        assert x == 10, f"attempt {attempt}: x={x}, expected 10 (kept soft)"

    lib.zsp_block_alloc_destroy(ba)


def test_soft_active_query(libzsp):
    """After solve, verify solver_soft_active() returns correct status."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 8, 0, 0, 20)

    # Soft 0: x == 15 (satisfiable, priority 0)
    v_x = lib.expr_var(sp, 0)
    c15 = lib.expr_const(sp, 15, 0)
    eq_e = lib.expr_binary(sp, BIN_EQ, v_x, c15)
    lib.problem_add_soft_constraint(sp, eq_e, 0)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    opts = lib._SolveOpts(seed=0x4444)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK

    assert lib.solver_soft_active(ctx, 0) == 1
    # Out of range returns -1
    assert lib.solver_soft_active(ctx, 99) == -1

    lib.zsp_block_alloc_destroy(ba)


def test_soft_with_hard_unsat(libzsp):
    """Hard constraints alone are UNSAT. Returns SOLVE_UNSAT
    (soft relaxation can't help)."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [0, 5]
    lib.problem_add_var(sp, 0, 8, 0, 0, 5)

    # Hard: x > 10 (impossible with domain [0, 5])
    v_x = lib.expr_var(sp, 0)
    c10 = lib.expr_const(sp, 10, 0)
    gt_e = lib.expr_binary(sp, BIN_GT, v_x, c10)
    lib.problem_add_constraint(sp, gt_e)

    # Soft: x == 3 (doesn't matter, hard is UNSAT)
    v_x2 = lib.expr_var(sp, 0)
    c3 = lib.expr_const(sp, 3, 0)
    eq_e = lib.expr_binary(sp, BIN_EQ, v_x2, c3)
    lib.problem_add_soft_constraint(sp, eq_e, 0)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)

    # Compile might detect UNSAT directly (returns -2)
    if rc == -2:
        # UNSAT detected at compile time - that's fine
        lib.zsp_block_alloc_destroy(ba)
        return

    opts = lib._SolveOpts(seed=0x7777)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    # Should be UNSAT since hard constraint alone is impossible
    assert result != SOLVE_OK

    lib.zsp_block_alloc_destroy(ba)
