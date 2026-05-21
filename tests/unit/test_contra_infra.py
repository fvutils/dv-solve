"""Unit tests for contradiction analysis infrastructure (Sprint 0).

Tests constraint_id tracking, prop_constraint_id mapping, and
contra_ctx/contra_hooks initialization.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL     = 0xFFFF_FFFF
SOLVE_OK      = 0
SOLVE_UNSAT   = 1

_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 524288

# BinOp values
BIN_ADD = 0
BIN_EQ  = 10
BIN_LTE = 13
BIN_GTE = 15


def _setup(lib: ctypes.CDLL):
    """Declare ctypes argtypes/restype for functions used by these tests."""
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
    lib.expr_sum.restype  = ctypes.c_uint32
    lib.expr_sum.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                             ctypes.c_uint32, ctypes.c_void_p]

    lib.solver_create.restype  = ctypes.c_void_p
    lib.solver_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p]
    lib.solver_compile.restype  = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.zsp_prop_constraint_id.restype  = ctypes.c_uint32
    lib.zsp_prop_constraint_id.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


def _make_ctx(lib):
    """Create a block_alloc and solver context; return (ctx_buf, ctx, ba)."""
    ba = lib.zsp_block_alloc_create(None, 4096)
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx
    return ctx_buf, ctx, ba


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

def test_constraint_id_auto(libzsp):
    """Constraints get auto-assigned sequential IDs from 1."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # a in [0, 100], b in [0, 100], r in [0, 200]
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)  # a
    lib.problem_add_var(sp, 1, 8, 0, 0, 100)  # b
    lib.problem_add_var(sp, 2, 8, 0, 0, 200)  # r

    # Constraint 1: a <= b  (var-var -> creates propagator)
    va = lib.expr_var(sp, 0)
    vb = lib.expr_var(sp, 1)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, va, vb))

    # Constraint 2: r == a (var-var eq -> creates propagator)
    vr = lib.expr_var(sp, 2)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_EQ, vr,
                                                    lib.expr_var(sp, 0)))

    # Compile
    ctx_buf, ctx, ba = _make_ctx(lib)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0, f"solver_compile failed with rc={rc}"

    # Each propagator should have a constraint_id in {1, 2}
    seen_ids = set()
    for pi in range(100):
        cid = lib.zsp_prop_constraint_id(ctx, pi)
        if cid == 0:
            break
        seen_ids.add(cid)

    assert len(seen_ids) >= 1, f"Expected at least 1 constraint ID, got {seen_ids}"
    assert all(1 <= cid <= 2 for cid in seen_ids), f"Unexpected IDs: {seen_ids}"

    lib.zsp_block_alloc_destroy(ba)


def test_constraint_id_roundtrip(libzsp):
    """Compile a multi-constraint problem and verify prop_constraint_id
    maps propagators to the correct constraint IDs."""
    lib = libzsp
    _setup(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # a in [0, 100], b in [0, 100], r in [0, 200]
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    lib.problem_add_var(sp, 1, 8, 0, 0, 100)
    lib.problem_add_var(sp, 2, 8, 0, 0, 200)

    # Constraint: r == a + b via expr_sum (creates BoundsAdd propagator)
    vr = lib.expr_var(sp, 2)
    vrefs = (ctypes.c_uint32 * 2)()
    vrefs[0] = lib.expr_var(sp, 0)
    vrefs[1] = lib.expr_var(sp, 1)
    esum = lib.expr_sum(sp, vr, 2, vrefs)
    lib.problem_add_constraint(sp, esum)

    ctx_buf, ctx, ba = _make_ctx(lib)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    # All propagators should map to constraint_id == 1
    found_any = False
    for pi in range(100):
        cid = lib.zsp_prop_constraint_id(ctx, pi)
        if cid == 0:
            break
        assert cid == 1, f"Propagator {pi} has constraint_id {cid}, expected 1"
        found_any = True

    assert found_any, "No propagators were created"
    lib.zsp_block_alloc_destroy(ba)


def test_contra_hooks_null(libzsp):
    """contra_ctx and contra_hooks are NULL after solver_create;
    UNSAT at compile time does not crash."""
    lib = libzsp
    _setup(lib)

    ctx_buf, ctx, ba = _make_ctx(lib)

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp

    # x in [10, 20], constraint x <= 5 -> UNSAT at compile time
    lib.problem_add_var(sp, 0, 8, 0, 10, 20)
    vx = lib.expr_var(sp, 0)
    c5 = lib.expr_const(sp, 5, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx, c5))

    rc = lib.solver_compile(ctx, sp)
    assert rc == -2, "Expected UNSAT at compile time"

    lib.zsp_block_alloc_destroy(ba)


def test_contra_api_available(libzsp_debug):
    """Contradiction analysis API is available in the debug library."""
    lib = libzsp_debug
    _setup(lib)

    lib.contra_analyze_unsat.restype = ctypes.c_int
    lib.contra_analyze_unsat.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.c_void_p
    ]
    lib.contra_quick_core.restype = ctypes.c_int
    lib.contra_quick_core.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.c_void_p
    ]
    lib.contra_result_free.restype = None
    lib.contra_result_free.argtypes = [ctypes.c_void_p]

    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    assert sp
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)

    ctx_buf, ctx, ba = _make_ctx(lib)
    rc = lib.solver_compile(ctx, sp)
    assert rc >= 0

    # contra_analyze_unsat should be callable (returns 0 for a SAT problem)
    class ContraResult(ctypes.Structure):
        _fields_ = [
            ("mus_constraint_ids", ctypes.c_void_p),
            ("mus_size", ctypes.c_uint32),
            ("proof_text", ctypes.c_char_p),
            ("proof_json", ctypes.c_char_p),
            ("core_size", ctypes.c_uint32),
            ("n_solver_calls", ctypes.c_uint32),
            ("elapsed_sec", ctypes.c_double),
            ("relaxations", ctypes.c_void_p),
            ("n_relaxations", ctypes.c_uint32),
            ("unconfirmed", ctypes.c_uint8),
            ("_res_pad", ctypes.c_uint8 * 7),
        ]
    result = ContraResult()
    rc = lib.contra_analyze_unsat(ctx, sp, None, ctypes.byref(result))
    assert rc == 0, f"contra_analyze_unsat returned {rc}"

    # contra_quick_core should be callable
    out_ids = (ctypes.c_uint32 * 10)()
    out_n = ctypes.c_uint32(10)
    rc = lib.contra_quick_core(ctx, sp, out_ids, ctypes.byref(out_n))
    assert rc == 0

    lib.zsp_block_alloc_destroy(ba)
