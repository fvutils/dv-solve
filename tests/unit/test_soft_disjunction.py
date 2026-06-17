"""DSE-0 diagnostic + DSE-1 acceptance: disjunction / guarded soft constraints.

A pyvsc guarded soft ``if (g) soft(e)`` lowers to the *disjunction* soft
``(!g) || e``.  The engine honors simple-predicate softs (see test_soft.py) but
historically did NOT steer the value picker toward a *kept* disjunction soft
(GAP-1 in dv_solve_soft_constraints_engine_plan.md).  These tests measure the
honor rate across seeds to pin the mechanism, and become the DSE-1 acceptance
gate once the soft value-bias lands.

Op codes (zsp_problem.h BinOp/UnaryOp):
  BIN_EQ=10  BIN_LTE=13  BIN_GT=14  BIN_GTE=15  BIN_OR=17 ; UN_NOT=1
"""
from __future__ import annotations

import ctypes
import pytest

SOLVE_OK = 0

_SP_BUF_SIZE = 65536
_CTX_BUF_SIZE = 524288

BIN_EQ = 10
BIN_LTE = 13
BIN_GT = 14
BIN_GTE = 15
BIN_OR = 17
UN_NOT = 1


def _setup(lib: ctypes.CDLL):
    lib.zsp_block_alloc_create.restype = ctypes.c_void_p
    lib.zsp_block_alloc_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.zsp_block_alloc_destroy.restype = None
    lib.zsp_block_alloc_destroy.argtypes = [ctypes.c_void_p]

    lib.solve_problem_init.restype = ctypes.c_void_p
    lib.solve_problem_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.problem_add_var.restype = ctypes.c_uint32
    lib.problem_add_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_uint8, ctypes.c_uint8,
                                    ctypes.c_int64, ctypes.c_int64]
    lib.problem_add_constraint.restype = ctypes.c_uint32
    lib.problem_add_constraint.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.problem_add_soft_constraint.restype = ctypes.c_uint32
    lib.problem_add_soft_constraint.argtypes = [ctypes.c_void_p,
                                                ctypes.c_uint32, ctypes.c_uint32]
    lib.expr_var.restype = ctypes.c_uint32
    lib.expr_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.expr_const.restype = ctypes.c_uint32
    lib.expr_const.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_uint8]
    lib.expr_binary.restype = ctypes.c_uint32
    lib.expr_binary.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                ctypes.c_uint32, ctypes.c_uint32]
    lib.expr_unary.restype = ctypes.c_uint32
    lib.expr_unary.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]

    lib.solver_create.restype = ctypes.c_void_p
    lib.solver_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p]
    lib.solver_compile.restype = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    class SolveOpts(ctypes.Structure):
        _fields_ = [
            ("seed", ctypes.c_uint64),
            ("max_conflicts", ctypes.c_uint32),
            ("max_restarts", ctypes.c_uint32),
            ("use_phase_save", ctypes.c_uint8),
            ("_pad", ctypes.c_uint8 * 3),
            ("max_shave_iters", ctypes.c_uint32),
        ]
    lib._SolveOpts = SolveOpts
    lib.solver_solve.restype = ctypes.c_int
    lib.solver_solve.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.solver_get_value.restype = ctypes.c_int64
    lib.solver_get_value.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


def _run_once(lib, seed, *, hard_guard_true, not_wrapped=False):
    """Build: a,d in [0,100]; optional hard a>10; soft !(a>10)||(d==40).

    Returns (a, d).  When hard_guard_true is set, a>10 forces the !(a>10)
    disjunct definitely-false, so a kept soft MUST satisfy d==40.

    not_wrapped selects how the guard's negation is expressed:
      * False -> pre-negated inline comparison ``a<=10`` (what a direct C
        caller would build);
      * True  -> ``UN_NOT(a>10)`` (what the pyvsc implies translator emits via
        ``expr_unary(UN_NOT, cond)``).  This is the discriminator for whether
        the DisjClause sees through a NOT-wrapped comparison.
    """
    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    lib.problem_add_var(sp, 0, 8, 0, 0, 100)   # var 0 = a
    lib.problem_add_var(sp, 1, 8, 0, 0, 100)   # var 1 = d

    if hard_guard_true:
        # Hard: a > 10
        gt = lib.expr_binary(sp, BIN_GT, lib.expr_var(sp, 0),
                             lib.expr_const(sp, 10, 0))
        lib.problem_add_constraint(sp, gt)

    # Soft: !(a>10) || (d==40)
    if not_wrapped:
        guard = lib.expr_binary(sp, BIN_GT, lib.expr_var(sp, 0),
                                lib.expr_const(sp, 10, 0))
        notguard = lib.expr_unary(sp, UN_NOT, guard)
    else:
        notguard = lib.expr_binary(sp, BIN_LTE, lib.expr_var(sp, 0),
                                   lib.expr_const(sp, 10, 0))
    d_eq_40 = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 1),
                              lib.expr_const(sp, 40, 0))
    disj = lib.expr_binary(sp, BIN_OR, notguard, d_eq_40)
    lib.problem_add_soft_constraint(sp, disj, 0)

    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0, f"compile rc={rc}"

    opts = lib._SolveOpts(seed=seed, use_phase_save=1)
    res = lib.solver_solve(ctx, ctypes.byref(opts))
    assert res == SOLVE_OK
    a = lib.solver_get_value(ctx, 0)
    d = lib.solver_get_value(ctx, 1)
    lib.zsp_block_alloc_destroy(ba)
    return a, d


def _run_simple(lib, seed):
    """Control: soft(d==40) on a free d (no disjunction). Should be honored."""
    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)   # d
    d_eq_40 = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 0),
                              lib.expr_const(sp, 40, 0))
    lib.problem_add_soft_constraint(sp, d_eq_40, 0)
    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert lib.solver_compile(ctx, sp) >= 0
    opts = lib._SolveOpts(seed=seed, use_phase_save=1)
    assert lib.solver_solve(ctx, ctypes.byref(opts)) == SOLVE_OK
    d = lib.solver_get_value(ctx, 0)
    lib.zsp_block_alloc_destroy(ba)
    return d


_SEEDS = [0x1000 + i * 0x9E37 for i in range(32)]


def test_control_simple_soft_honored(libzsp):
    """Baseline: a simple non-disjunction soft is honored (sanity)."""
    lib = libzsp
    _setup(lib)
    hits = sum(1 for s in _SEEDS if _run_simple(lib, s) == 40)
    assert hits == len(_SEEDS), f"simple soft honored {hits}/{len(_SEEDS)}"


def test_disjunction_soft_guard_forced_true(libzsp):
    """DSE-1 ACCEPTANCE: hard a>10 forces (a<=10) false, so a kept soft must
    satisfy d==40. Pre-DSE-1 this was honored ~0/N (the gap)."""
    lib = libzsp
    _setup(lib)
    results = [_run_once(lib, s, hard_guard_true=True) for s in _SEEDS]
    # Every model must satisfy the disjunction (a>10 is hard, so d==40).
    honored = sum(1 for (a, d) in results if d == 40)
    # Soundness floor: the disjunction must at least hold in every model
    # (a<=10 is impossible under the hard constraint).
    for (a, d) in results:
        assert a > 10, f"hard a>10 violated: a={a}"
    assert honored == len(_SEEDS), (
        f"guarded soft honored {honored}/{len(_SEEDS)} "
        f"(sample: {results[:5]})")


def test_disjunction_soft_NOTwrapped_guard_forced_true(libzsp):
    """DSE-0 DISCRIMINATOR: same as above but the guard negation is
    ``UN_NOT(a>10)`` (the pyvsc-emitted shape) rather than the pre-negated
    ``a<=10``. If this fails while the pre-negated form passes, the gap is the
    DisjClause not seeing through UN_NOT(comparison) — a translation/compile
    issue, not a value-picker one."""
    lib = libzsp
    _setup(lib)
    results = [_run_once(lib, s, hard_guard_true=True, not_wrapped=True)
               for s in _SEEDS]
    for (a, d) in results:
        assert a > 10, f"hard a>10 violated: a={a}"
    honored = sum(1 for (a, d) in results if d == 40)
    assert honored == len(_SEEDS), (
        f"NOT-wrapped guarded soft honored {honored}/{len(_SEEDS)} "
        f"(sample: {results[:5]})")


def test_disjunction_soft_guard_free_holds(libzsp):
    """Soundness: with the guard free, the disjunction must still HOLD in
    every model (either a<=10 or d==40) — never violated."""
    lib = libzsp
    _setup(lib)
    results = [_run_once(lib, s, hard_guard_true=False) for s in _SEEDS]
    for (a, d) in results:
        assert (a <= 10) or (d == 40), (
            f"kept disjunction soft violated: a={a}, d={d}")
