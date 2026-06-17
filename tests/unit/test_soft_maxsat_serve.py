"""DSE-2: soft-aware MaxSAT on the BV-SAT serve path (zsp_bbsolver_check_maxsat).

The primary engine honors softs; the BV-SAT serve path historically did not
(bit-blast ignored softs_head). zsp_bbsolver_check_maxsat keeps the maximal
priority-respecting soft set on the serve path, mirroring solver_solve's
relaxation policy. These tests drive the C API directly.

Op codes (zsp_problem.h): BIN_EQ=10 BIN_GT=14 BIN_GTE=15.
"""
from __future__ import annotations

import ctypes
import pytest

ZSP_BB_SAT = 10
ZSP_BB_UNSAT = 20

_SP_BUF_SIZE = 65536

BIN_EQ = 10
BIN_GT = 14
BIN_GTE = 15


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

    lib.zsp_bbsolver_check_maxsat.restype = ctypes.c_int
    lib.zsp_bbsolver_check_maxsat.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                              ctypes.c_uint64,
                                              ctypes.POINTER(ctypes.c_void_p),
                                              ctypes.c_void_p, ctypes.c_uint32]
    lib.zsp_bbsolver_value.restype = ctypes.c_int
    lib.zsp_bbsolver_value.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                       ctypes.POINTER(ctypes.c_int64)]
    lib.zsp_bbsolver_free.restype = None
    lib.zsp_bbsolver_free.argtypes = [ctypes.c_void_p]


def _maxsat(lib, sp, seed=0x1234):
    out = ctypes.c_void_p()
    rc = lib.zsp_bbsolver_check_maxsat(None, sp, seed, ctypes.byref(out), None, 0)
    return rc, out


def _val(lib, bb, vid):
    v = ctypes.c_int64()
    assert lib.zsp_bbsolver_value(bb, vid, ctypes.byref(v)) == 0
    return v.value


def test_serve_soft_satisfiable_honored(libzsp):
    """Hard a>10; soft d==40 (no conflict). MaxSAT keeps it → d==40."""
    lib = libzsp
    _setup(lib)
    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)   # a
    lib.problem_add_var(sp, 1, 8, 0, 0, 100)   # d
    gt = lib.expr_binary(sp, BIN_GT, lib.expr_var(sp, 0),
                         lib.expr_const(sp, 10, 0))
    lib.problem_add_constraint(sp, gt)
    soft = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 1),
                           lib.expr_const(sp, 40, 0))
    lib.problem_add_soft_constraint(sp, soft, 0)

    rc, bb = _maxsat(lib, sp)
    assert rc == ZSP_BB_SAT
    assert _val(lib, bb, 0) > 10
    assert _val(lib, bb, 1) == 40, "serve-path soft d==40 not honored"
    lib.zsp_bbsolver_free(bb)


def test_serve_soft_conflicting_relaxed(libzsp):
    """Hard a>10 AND a==5-soft conflicts; the soft must be relaxed, model valid."""
    lib = libzsp
    _setup(lib)
    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)   # a
    gt = lib.expr_binary(sp, BIN_GT, lib.expr_var(sp, 0),
                         lib.expr_const(sp, 10, 0))
    lib.problem_add_constraint(sp, gt)
    soft = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 0),
                           lib.expr_const(sp, 5, 0))   # conflicts with a>10
    lib.problem_add_soft_constraint(sp, soft, 0)

    rc, bb = _maxsat(lib, sp)
    assert rc == ZSP_BB_SAT
    assert _val(lib, bb, 0) > 10, "hard must hold; conflicting soft relaxed"
    lib.zsp_bbsolver_free(bb)


def test_serve_soft_priority_order(libzsp):
    """Two mutually-exclusive softs at distinct priorities + a hard that forces
    dropping at least one. Lower preference (higher priority value) is dropped
    first → the higher-preference soft (priority 0) is kept."""
    lib = libzsp
    _setup(lib)
    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)   # x
    # Hard: x >= 0 (trivial; the two softs are the conflict).
    # Soft A: x == 20, priority 0 (keep-hardest)
    a = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 0),
                        lib.expr_const(sp, 20, 0))
    lib.problem_add_soft_constraint(sp, a, 0)
    # Soft B: x == 30, priority 10 (relax first)
    b = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 0),
                        lib.expr_const(sp, 30, 0))
    lib.problem_add_soft_constraint(sp, b, 10)

    rc, bb = _maxsat(lib, sp)
    assert rc == ZSP_BB_SAT
    assert _val(lib, bb, 0) == 20, "higher-preference soft (x==20) must be kept"
    lib.zsp_bbsolver_free(bb)


def test_serve_hard_unsat(libzsp):
    """Hard core UNSAT (x in [0,5] but x>10) → UNSAT even after dropping softs."""
    lib = libzsp
    _setup(lib)
    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 8, 0, 0, 5)   # x in [0,5]
    gt = lib.expr_binary(sp, BIN_GT, lib.expr_var(sp, 0),
                         lib.expr_const(sp, 10, 0))
    lib.problem_add_constraint(sp, gt)
    soft = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 0),
                           lib.expr_const(sp, 3, 0))
    lib.problem_add_soft_constraint(sp, soft, 0)

    out = ctypes.c_void_p()
    rc = lib.zsp_bbsolver_check_maxsat(None, sp, 0x99, ctypes.byref(out), None, 0)
    assert rc == ZSP_BB_UNSAT
    assert not out  # NULL out_bb on UNSAT


def test_serve_soft_no_collateral_drop(libzsp):
    """Serve-path twin of `test_soft.test_soft_no_collateral_drop_primary`. The
    additive-greedy MaxSAT must keep the satisfiable softs (a==11, d==40, e==50)
    and drop only the conflicting x!=20, y!=30 — NOT shed satisfiable softs as
    collateral. The subtractive greedy this replaced dropped d==40 here (the
    test_soft_nested failure). Same kept set as the primary => primary == serve."""
    lib = libzsp
    _setup(lib)
    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    for vid in range(5):                       # 0=a 1=x 2=d 3=y 4=e
        lib.problem_add_var(sp, vid, 8, 0, 0, 100)
    lib.problem_add_constraint(sp, lib.expr_binary(
        sp, BIN_EQ, lib.expr_var(sp, 1), lib.expr_const(sp, 20, 0)))   # x==20
    lib.problem_add_constraint(sp, lib.expr_binary(
        sp, BIN_EQ, lib.expr_var(sp, 3), lib.expr_const(sp, 30, 0)))   # y==30
    BIN_NE = 11
    for vid, op, k, pri in [(0, BIN_EQ, 11, 0), (1, BIN_NE, 20, 1),
                            (2, BIN_EQ, 40, 2), (3, BIN_NE, 30, 3),
                            (4, BIN_EQ, 50, 4)]:
        e = lib.expr_binary(sp, op, lib.expr_var(sp, vid), lib.expr_const(sp, k, 0))
        lib.problem_add_soft_constraint(sp, e, pri)

    rc, bb = _maxsat(lib, sp)
    assert rc == ZSP_BB_SAT
    assert _val(lib, bb, 0) == 11, "a==11 dropped as collateral"
    assert _val(lib, bb, 2) == 40, "d==40 dropped as collateral (the test_soft_nested bug)"
    assert _val(lib, bb, 4) == 50, "e==50 dropped as collateral"
    assert _val(lib, bb, 1) == 20 and _val(lib, bb, 3) == 30, "hard must hold"
    lib.zsp_bbsolver_free(bb)


def test_serve_soft_priority_ladder_three(libzsp):
    """DSE-3 ladder (serve path) — the matched twin of
    `test_soft.test_soft_priority_ladder_three_primary`. Identical problem: three
    mutually-exclusive softs at priorities 0/5/10. The serve-path MaxSAT must keep
    the SAME soft the primary keeps (x==10 @ pri0) → primary order == serve order."""
    lib = libzsp
    _setup(lib)
    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)   # x in [0,100]
    s0 = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 0), lib.expr_const(sp, 10, 0))
    lib.problem_add_soft_constraint(sp, s0, 0)   # keep-hardest
    s1 = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 0), lib.expr_const(sp, 20, 0))
    lib.problem_add_soft_constraint(sp, s1, 5)
    s2 = lib.expr_binary(sp, BIN_EQ, lib.expr_var(sp, 0), lib.expr_const(sp, 30, 0))
    lib.problem_add_soft_constraint(sp, s2, 10)  # relax-first

    rc, bb = _maxsat(lib, sp)
    assert rc == ZSP_BB_SAT
    assert _val(lib, bb, 0) == 10, "serve path must keep the same soft as primary (x==10)"
    lib.zsp_bbsolver_free(bb)
